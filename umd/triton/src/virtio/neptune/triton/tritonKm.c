/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Kernel-mode plumbing shared by the D3D10/11 and D3D12 UMDs; see
 * tritonKm.h.
 */

#include "triton.h"
#include "triton_log.h"
#include "tritonSharedBridge.h"
#include "npt_shared_texture.h"

/* The KMD pins the same sizes (viogpu_adapter.cpp); a drift on either side
 * is an ABI break that would otherwise only show up as garbage escapes. */
_Static_assert(sizeof(VIOGPU_ADAPTERINFO) == 32, "VIOGPU_ADAPTERINFO");
_Static_assert(sizeof(VIOGPU_ESCAPE) == 108, "VIOGPU_ESCAPE");
_Static_assert(sizeof(VIOGPU_RES_INFO_REQ) == 104, "VIOGPU_RES_INFO_REQ");
_Static_assert(sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE) == 204,
               "VIOGPU_CREATE_ALLOCATION_EXCHANGE");
_Static_assert(sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX) == 212,
               "VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX");
_Static_assert(sizeof(VIOGPU_BLIT_PRESENT) == 348, "VIOGPU_BLIT_PRESENT");
_Static_assert(sizeof(VIOGPU_DMA_PRIVATE) == 1040, "VIOGPU_DMA_PRIVATE");
_Static_assert(offsetof(VIOGPU_DMA_PRIVATE, cmdRaw) == 0,
               "VIOGPU_DMA_PRIVATE.cmdRaw");

void
tritonKmInit(TRITON_KM *km, HANDLE hRTAdapter, HANDLE hRTDevice,
             const D3DDDI_DEVICECALLBACKS *pCallbacks)
{
   memset(km, 0, sizeof(*km));
   km->hRTAdapter = hRTAdapter;
   km->hRTDevice  = hRTDevice;
   km->pCallbacks = pCallbacks;
}

void
tritonKmDestroy(TRITON_KM *km)
{
   if (km->hPagingQueue && km->pCallbacks &&
       km->pCallbacks->pfnDestroyPagingQueueCb) {
      D3DDDI_DESTROYPAGINGQUEUE dpq;
      memset(&dpq, 0, sizeof(dpq));
      dpq.hPagingQueue = km->hPagingQueue;
      km->pCallbacks->pfnDestroyPagingQueueCb(km->hRTDevice, &dpq);
   }
   km->hPagingQueue  = 0;
   km->PagingFenceVa = NULL;
   km->pLock         = NULL;
}

HRESULT
tritonKmEscape(const TRITON_KM *km, VIOGPU_ESCAPE *esc)
{
   if (!km->pCallbacks || !km->pCallbacks->pfnEscapeCb)
      return E_NOTIMPL;
   D3DDDICB_ESCAPE cb;
   memset(&cb, 0, sizeof(cb));
   cb.hDevice               = km->hRTDevice;
   cb.pPrivateDriverData    = esc;
   cb.PrivateDriverDataSize = sizeof(*esc);
   return km->pCallbacks->pfnEscapeCb(km->hRTAdapter, &cb);
}

/* Give the runtime's kernel device a virtio context, so the KMD's deferred
 * host bindings (RESOURCE_CREATE_BLOB / CTX_ATTACH_RESOURCE on this device's
 * context) reach a valid host context.  Under the lock: racing first uses
 * would otherwise double-issue VIOGPU_CTX_INIT. */
void
tritonKmEnsureRuntimeCtx(TRITON_KM *km)
{
   if (km->RuntimeCtxInited || !km->pLock || !tritonSharedBridgeCanExport())
      return;
   EnterCriticalSection(km->pLock);
   if (!km->RuntimeCtxInited) {
      VIOGPU_ESCAPE esc;
      memset(&esc, 0, sizeof(esc));
      esc.Type             = VIOGPU_CTX_INIT;
      esc.DataLength       = sizeof(esc.CtxInit);
      esc.CtxInit.CapsetID = 7;
      esc.CtxInit.NumRings = 1;
      HRESULT hr = tritonKmEscape(km, &esc);
      if (SUCCEEDED(hr))
         km->RuntimeCtxInited = TRUE;
      else
         TR_LOG("km: VIOGPU_CTX_INIT failed 0x%08lx", (unsigned long)hr);
   }
   LeaveCriticalSection(km->pLock);
}

/* The paging fence value is a monitored-fence UINT64 the kernel advances in
 * a page mapped read-only into the process, so an interlocked
 * compare-exchange (a locked write cycle) would fault.  A plain 64-bit load
 * is atomic on 64-bit targets; the 32-bit UMD re-reads the high half until
 * it is stable, which is enough for a monotonic value. */
static UINT64
tritonKmLoadFence(volatile const UINT64 *va)
{
#if defined(_WIN64)
   return *va;
#else
   volatile const UINT32 *half = (volatile const UINT32 *)va;
   UINT32 hi, lo;
   do {
      hi = half[1];
      lo = half[0];
   } while (hi != half[1]);
   return ((UINT64)hi << 32) | lo;
#endif
}

/* WDDM2 explicit residency.  GpuMmu allocations are not resident at create,
 * and VidMm refuses any DMA buffer that references an allocation holding no
 * residency request ("Failed to reference DMA buffer: Allocation is not
 * requested to be resident") and marks the device in error.  Rendering rides
 * the npt ring and references no KM allocation, but the OS builds such a
 * reference for every blt-model present (the back buffer is its source), so
 * the request exists for the OS's present packet.  The runtime makes the
 * allocations it owns resident, never the ones pfnAllocateCb hands back to
 * the driver.  Requests are never revoked; the reference dies with the
 * allocation. */
void
tritonKmRequestResidency(TRITON_KM *km, D3DKMT_HANDLE hAllocation)
{
   if (!hAllocation || !km->pCallbacks)
      return;
   if (!km->PagingQueueTried) {
      if (!km->pLock)
         return;
      EnterCriticalSection(km->pLock);
      if (!km->PagingQueueTried) {
         if (km->pCallbacks->pfnCreatePagingQueueCb &&
             km->pCallbacks->pfnMakeResidentCb) {
            D3DDDICB_CREATEPAGINGQUEUE pq;
            memset(&pq, 0, sizeof(pq)); /* PRIORITY_NORMAL, adapter 0 */
            HRESULT hr = km->pCallbacks->pfnCreatePagingQueueCb(km->hRTDevice,
                                                                 &pq);
            if (SUCCEEDED(hr) && pq.hPagingQueue) {
               km->hPagingQueue  = pq.hPagingQueue;
               km->PagingFenceVa = (volatile const UINT64 *)
                  pq.FenceValueCPUVirtualAddress;
            } else {
               TR_LOG("km: CreatePagingQueue failed 0x%08lx -- legacy "
                      "(implicit) residency", (unsigned long)hr);
            }
         }
         km->PagingQueueTried = TRUE;
      }
      LeaveCriticalSection(km->pLock);
   }
   if (!km->hPagingQueue)
      return;

   D3DDDI_MAKERESIDENT mr;
   memset(&mr, 0, sizeof(mr));
   mr.hPagingQueue   = km->hPagingQueue;
   mr.NumAllocations = 1;
   mr.AllocationList = &hAllocation;
   HRESULT hr = km->pCallbacks->pfnMakeResidentCb(km->hRTDevice, &mr);
   if (hr == E_PENDING && km->PagingFenceVa) {
      /* Queued: a present built against the allocation before the paging
       * fence reaches mr.PagingFenceValue still dies as above.  Creation-time
       * waits are rare and short, so poll the plain monotonic fence value
       * with a yield and a loud timeout. */
      const ULONGLONG deadline = GetTickCount64() + 5000;
      while (tritonKmLoadFence(km->PagingFenceVa) < mr.PagingFenceValue) {
         if (GetTickCount64() > deadline) {
            TR_LOG("km: paging fence wait TIMED OUT (alloc=0x%x fence=%llu < %llu)",
                   hAllocation, (unsigned long long)tritonKmLoadFence(km->PagingFenceVa),
                   (unsigned long long)mr.PagingFenceValue);
            return;
         }
         Sleep(0);
      }
   } else if (FAILED(hr)) {
      TR_LOG("km: MakeResident(alloc=0x%x) FAILED 0x%08lx -- a blt-model "
             "present referencing it will remove the device",
             hAllocation, (unsigned long)hr);
   }
}

BOOL
tritonKmFillSharedExchange(VIOGPU_CREATE_ALLOCATION_EXCHANGE *ax,
                           void *pResourceWrapper,
                           const TRITON_KM_SHARED_DESC *d, BOOL primary)
{
   memset(ax, 0, sizeof(*ax));
   ax->Type = VIOGPU_RESOURCE_TYPE_SHARED;
   VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *o = &ax->OptionsShared;

   struct triton_shared_texture_desc exp;
   memset(&exp, 0, sizeof(exp));
   if (!tritonSharedBridgeExportBlob(pResourceWrapper, &exp)) {
      TR_LOG("km: shared export failed %llux%u fmt=%d",
             (unsigned long long)d->Width, d->Height, (int)d->Format);
      return FALSE;
   }
   o->blob_id         = exp.blob_id;
   o->create_ctx_id   = exp.create_ctx_id;
   o->plane_count     = exp.plane_count;
   o->texture_layout  = exp.texture_layout;
   o->modifier        = exp.modifier;
   o->allocation_size = exp.allocation_size;
   for (UINT i = 0; i < exp.plane_count && i < TRITON_SHARED_MAX_PLANES; i++) {
      o->planes[i].offset = exp.planes[i].offset;
      o->planes[i].pitch  = exp.planes[i].pitch;
   }

   o->primary          = primary ? 1u : 0u;
   o->width            = (ULONG)d->Width;
   o->height           = d->Height;
   o->mip_levels       = d->MipLevels ? d->MipLevels : 1;
   o->array_size       = d->ArraySize ? d->ArraySize : 1;
   o->format           = (ULONG)d->Format;
   o->sample_count     = d->SampleCount ? d->SampleCount : 1;
   o->usage            = d->Usage;
   o->bind_flags       = d->BindFlags;
   o->cpu_access_flags = d->CpuAccessFlags;
   o->misc_flags       = d->MiscFlags;

   /* Every shared blob carries a texture description: the KMD keys its
    * blob-info-valid flag off ScanoutInfo.width, and without it
    * DescribeAllocation returns STATUS_INVALID_PARAMETER and a windowed blt
    * present types the source with width/height/stride 0, which the host
    * rejects along with the copy.  A single-sample 2D non-primary is typed
    * with its exact copy format, so the blt reads it in the layout it was
    * rendered in; anything else (or a primary) takes the scanout mapping.
    * Claiming the scanout stays gated on `primary`. */
   const uint32_t copyFmt = (!primary && d->Is2D && d->SampleCount <= 1)
      ? npt_shared_texture_virgl_copy_format(d->Format) : 0;
   o->ScanoutInfo.width      = (ULONG)d->Width;
   o->ScanoutInfo.height     = d->Height;
   o->ScanoutInfo.format     =
      copyFmt ? copyFmt : npt_shared_texture_virgl_format(d->Format);
   o->ScanoutInfo.strides[0] = (ULONG)o->planes[0].pitch;
   o->ScanoutInfo.offsets[0] = (ULONG)o->planes[0].offset;

   ax->Size = (o->allocation_size + 4095ull) & ~4095ull;
   if (!ax->Size)
      ax->Size = ((d->Width * d->Height * 4) + 4095) & ~4095ull;
   return TRUE;
}
