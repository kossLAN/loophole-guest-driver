/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Device-level kernel context for the present path.  See tritonPresent.h.
 */

#include "tritonPresent.h"
#include "triton_log.h"

void
tritonPresentEnsureKernelContext(PTRITON_DEVICE pD)
{
   if (pD->hKMContext || !pD->KTCallbacks.pfnCreateContextCb)
      return;
   /* Serialise the check-and-create under kmCtxLock (the HCONTEXT-domain
    * lock, see triton.h): concurrent present threads — and a primary-create
    * on another thread — could each see hKMContext==NULL and create a
    * duplicate kernel context. */
   if (!pD->presentLockInit)
      return;
   EnterCriticalSection(&pD->kmCtxLock);
   if (!pD->hKMContext) {
      /* Prefer a VIRTUAL-ADDRESSING context (WDDM2 GpuMmu).  On a legacy
       * (non-virtual) context, the redirected blt-model present is the
       * first submission whose DMA buffer carries content, and dxgmms2
       * never makes the non-virtual DMA pool resident -- the submit dies
       * with VidSchErrorSubmittingRenderForNonResidentAllocation ("Failed
       * to reference DMA buffer: Allocation is not requested to be
       * resident") and the device reads back DXGI_ERROR_DEVICE_HUNG
       * on the first present of any DISCARD swapchain.  A virtual
       * context's DMA pool is GPU-VA mapped and resident by construction
       * (the D3D12 path submits through it constantly).  Fall back to the
       * legacy callback on failure so WDDM 1.3 keeps its old context. */
      HRESULT hr = E_NOTIMPL;
      if (pD->KTCallbacks.pfnCreateContextVirtualCb) {
         D3DDDICB_CREATECONTEXTVIRTUAL vctx;
         memset(&vctx, 0, sizeof(vctx));
         hr = pD->KTCallbacks.pfnCreateContextVirtualCb(pD->hRTDevice.handle,
                                                        &vctx);
         if (SUCCEEDED(hr)) {
            pD->hKMContext = vctx.hContext;
            TR_LOG("present: kernel context created (virtual)");
         }
      }
      if (!pD->hKMContext) {
         D3DDDICB_CREATECONTEXT ctx;
         memset(&ctx, 0, sizeof(ctx));
         HRESULT lhr = pD->KTCallbacks.pfnCreateContextCb(pD->hRTDevice.handle,
                                                          &ctx);
         if (SUCCEEDED(lhr)) {
            pD->hKMContext = ctx.hContext;
            TR_LOG("present: kernel context created (LEGACY; virtual hr=0x%08lx)",
                   (unsigned long)hr);
         } else
            TR_LOG("present: CreateContext failed (virtual 0x%08lx, "
                   "legacy 0x%08lx)", (unsigned long)hr, (unsigned long)lhr);
      }
   }
   LeaveCriticalSection(&pD->kmCtxLock);
}
