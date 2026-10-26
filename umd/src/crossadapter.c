/* SPDX-License-Identifier: MIT */

#include "triton.h"
#include "triton_log.h"
#include "virtio/virtio-gpu/wddm_hw.h"

struct cross_adapter {
   ID3D11Texture2D *staging;
   UINT bytes_per_pixel;
   UINT pitch;
};

static UINT
bytes_per_pixel(DXGI_FORMAT format)
{
   switch (format) {
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8X8_TYPELESS:
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
   case DXGI_FORMAT_R10G10B10A2_TYPELESS:
   case DXGI_FORMAT_R10G10B10A2_UNORM:
   case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
      return 4;
   case DXGI_FORMAT_R16G16B16A16_TYPELESS:
   case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return 8;
   default:
      return 0;
   }
}

static UINT
align(UINT v, UINT a)
{
   return (v + a - 1) / a * a;
}

BOOL
tritonCrossAdapterCreate(PTRITON_DEVICE pD, PTRITON_RESOURCE r)
{
   const UINT bpp = bytes_per_pixel(r->Format);
   if (!bpp || r->MipLevels > 1 || r->ArraySize > 1 || r->SampleDesc.Count > 1 ||
       !pD->KTCallbacks.pfnAllocateCb || !pD->KTCallbacks.pfnLock2Cb) {
      TR_LOG("cross-adapter: unsupported %ux%u fmt=%d mips=%u array=%u samples=%u", r->Width, r->Height,
             (int)r->Format, r->MipLevels, r->ArraySize, r->SampleDesc.Count);
      return FALSE;
   }
   struct cross_adapter *x = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*x));
   if (!x)
      return FALSE;
   x->bytes_per_pixel = bpp;
   x->pitch = align(r->Width * bpp, D3DKMT_CROSS_ADAPTER_RESOURCE_PITCH_ALIGNMENT);
   const UINT rows = align(r->Height, D3DKMT_CROSS_ADAPTER_RESOURCE_HEIGHT_ALIGNMENT);

   D3D11_TEXTURE2D_DESC sd = {
      .Width = r->Width,
      .Height = r->Height,
      .MipLevels = 1,
      .ArraySize = 1,
      .Format = r->Format,
      .SampleDesc = {.Count = 1},
      .Usage = D3D11_USAGE_STAGING,
      .CPUAccessFlags = D3D11_CPU_ACCESS_READ,
   };
   HRESULT hr = ID3D11Device1_CreateTexture2D(pD->pDev1, &sd, NULL, &x->staging);
   if (FAILED(hr)) {
      TR_LOG("cross-adapter: staging texture failed 0x%08lx", (unsigned long)hr);
      HeapFree(GetProcessHeap(), 0, x);
      return FALSE;
   }

   VIOGPU_CREATE_ALLOCATION_EXCHANGE ax;
   memset(&ax, 0, sizeof(ax));
   ax.Type = VIOGPU_RESOURCE_TYPE_BLOB;
   ax.OptionsBlob.blob_mem = VIOGPU_BLOB_MEM_GUEST;
   ax.OptionsBlob.blob_flags = VIOGPU_BLOB_FLAG_RESIDENCY_ONLY;
   ax.OptionsBlob.surface_width = r->Width;
   ax.OptionsBlob.surface_height = r->Height;
   ax.OptionsBlob.surface_format = r->Format;
   ax.Size = ((ULONGLONG)x->pitch * rows + 4095) & ~4095ull;

   D3DDDI_ALLOCATIONINFO2 ai;
   memset(&ai, 0, sizeof(ai));
   ai.pPrivateDriverData = &ax;
   ai.PrivateDriverDataSize = sizeof(ax);
   D3DDDICB_ALLOCATE cb;
   memset(&cb, 0, sizeof(cb));
   cb.hResource = r->hRTResource.handle;
   cb.NumAllocations = 1;
   cb.pAllocationInfo2 = &ai;
   hr = pD->KTCallbacks.pfnAllocateCb(pD->hRTDevice.handle, &cb);
   if (FAILED(hr) || !ai.hAllocation) {
      TR_LOG("cross-adapter: pfnAllocateCb failed 0x%08lx (%ux%u fmt=%d)", (unsigned long)hr, r->Width, r->Height,
             (int)r->Format);
      ID3D11Texture2D_Release(x->staging);
      HeapFree(GetProcessHeap(), 0, x);
      return FALSE;
   }
   r->hKMAllocation = ai.hAllocation;
   r->pCrossAdapter = x;
   /* WDDM2 allocations start out evicted. */
   tritonKmRequestResidency(&pD->Km, ai.hAllocation);
   TR_LOG("cross-adapter: %ux%u fmt=%d pitch=%u alloc=0x%x", r->Width, r->Height, (int)r->Format, x->pitch,
          ai.hAllocation);
   return TRUE;
}

void
tritonCrossAdapterSync(PTRITON_DEVICE pD, PTRITON_RESOURCE r)
{
   struct cross_adapter *x = r->pCrossAdapter;
   if (!x || !r->pResource || !pD->pCtx1)
      return;
   if (pD->presentLockInit)
      EnterCriticalSection(&pD->presentLock);

   ID3D11DeviceContext1_CopySubresourceRegion(pD->pCtx1, (ID3D11Resource *)x->staging, 0, 0, 0, 0, r->pResource, 0,
                                              NULL);
   D3D11_MAPPED_SUBRESOURCE m;
   HRESULT hr = ID3D11DeviceContext1_Map(pD->pCtx1, (ID3D11Resource *)x->staging, 0, D3D11_MAP_READ, 0, &m);
   if (SUCCEEDED(hr)) {
      D3DDDICB_LOCK2 lock;
      memset(&lock, 0, sizeof(lock));
      lock.hAllocation = r->hKMAllocation;
      hr = pD->KTCallbacks.pfnLock2Cb(pD->hRTDevice.handle, &lock);
      if (SUCCEEDED(hr) && lock.pData) {
         const UINT row = r->Width * x->bytes_per_pixel;
         for (UINT y = 0; y < r->Height; y++)
            memcpy((BYTE *)lock.pData + (SIZE_T)y * x->pitch, (const BYTE *)m.pData + (SIZE_T)y * m.RowPitch, row);
         D3DDDICB_UNLOCK2 unlock = {.hAllocation = r->hKMAllocation};
         HRESULT unlocked = pD->KTCallbacks.pfnUnlock2Cb(pD->hRTDevice.handle, &unlock);
         (void)unlocked;
      } else {
         static LONG failures;
         if (InterlockedIncrement(&failures) <= 8)
            TR_LOG("cross-adapter: pfnLock2Cb failed 0x%08lx", (unsigned long)hr);
      }
      ID3D11DeviceContext1_Unmap(pD->pCtx1, (ID3D11Resource *)x->staging, 0);
   } else {
      static LONG failures;
      if (InterlockedIncrement(&failures) <= 8)
         TR_LOG("cross-adapter: staging Map failed 0x%08lx", (unsigned long)hr);
   }

   if (pD->presentLockInit)
      LeaveCriticalSection(&pD->presentLock);
}

void
tritonCrossAdapterDestroy(PTRITON_DEVICE pD, PTRITON_RESOURCE r)
{
   (void)pD;
   struct cross_adapter *x = r->pCrossAdapter;
   if (!x)
      return;
   ID3D11Texture2D_Release(x->staging);
   HeapFree(GetProcessHeap(), 0, x);
   r->pCrossAdapter = NULL;
}
