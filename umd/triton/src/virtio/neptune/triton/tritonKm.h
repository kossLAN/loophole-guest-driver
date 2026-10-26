/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Kernel-mode plumbing shared by the D3D10/11 and D3D12 halves of Triton:
 * VIOGPU escapes, the one-shot VIOGPU_CTX_INIT on the runtime's kernel
 * device, WDDM2 residency requests for the KM allocations the driver mints,
 * and the allocation private data that describes a shared-blob texture.
 * Each UMD embeds one TRITON_KM in its device and keeps only its own
 * pfnAllocateCb call (D3DDDICB_ALLOCATE vs D3D12DDICB_ALLOCATE_0022).
 *
 * Include after the d3dumddi.h family (triton.h does).
 */

#ifndef TRITON_KM_H_INCLUDED
#define TRITON_KM_H_INCLUDED

#include "virtio/virtio-gpu/wddm_hw.h"   /* VIOGPU escape / allocation ABI */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TRITON_KM {
    HANDLE                         hRTAdapter;
    HANDLE                         hRTDevice;
    const D3DDDI_DEVICECALLBACKS  *pCallbacks;
    /* Serialises the lazy context init and paging-queue create.  NULL until
     * the owning device has initialised it (and again once it is deleted);
     * the lazy inits are skipped while it is NULL, since no second thread
     * can reach the device yet. */
    CRITICAL_SECTION              *pLock;

    /* Set only once VIOGPU_CTX_INIT succeeded: every later blob create and
     * context attach depends on that context, so a failure stays
     * retryable. */
    BOOL                           RuntimeCtxInited;

    /* WDDM2 paging queue for explicit residency requests, created on the
     * first request.  hPagingQueue == 0 with PagingQueueTried set means the
     * runtime/KMD pair has no GpuMmu residency model (WDDM 1.3) and requests
     * are no-ops.  PagingFenceVa is the queue's monitored-fence value CPU VA,
     * polled when MakeResident answers E_PENDING. */
    BOOL                           PagingQueueTried;
    D3DKMT_HANDLE                  hPagingQueue;
    volatile const UINT64         *PagingFenceVa;
} TRITON_KM;

void    tritonKmInit(TRITON_KM *km, HANDLE hRTAdapter, HANDLE hRTDevice,
                     const D3DDDI_DEVICECALLBACKS *pCallbacks);
/* Destroys the paging queue.  Call before the owning lock is deleted. */
void    tritonKmDestroy(TRITON_KM *km);

HRESULT tritonKmEscape(const TRITON_KM *km, VIOGPU_ESCAPE *esc);
void    tritonKmEnsureRuntimeCtx(TRITON_KM *km);
void    tritonKmRequestResidency(TRITON_KM *km, D3DKMT_HANDLE hAllocation);

/* The D3D11 description of a texture being exported as a shared blob. */
typedef struct TRITON_KM_SHARED_DESC {
    UINT64       Width;
    UINT         Height;
    UINT         MipLevels;
    UINT         ArraySize;
    DXGI_FORMAT  Format;         /* host format the texture was created with */
    UINT         SampleCount;
    BOOL         Is2D;
    UINT         Usage;          /* D3D11_USAGE */
    UINT         BindFlags;      /* D3D11_BIND_* */
    UINT         CpuAccessFlags;
    UINT         MiscFlags;      /* D3D11_RESOURCE_MISC_* */
} TRITON_KM_SHARED_DESC;

/* Export the host texture behind pResourceWrapper (a Neptune COM wrapper)
 * as this context's pending blob and fill the VIOGPU_RESOURCE_TYPE_SHARED
 * private data the caller hands to its pfnAllocateCb.  primary marks a
 * flippable scanout primary.  Returns FALSE when the export fails. */
BOOL tritonKmFillSharedExchange(VIOGPU_CREATE_ALLOCATION_EXCHANGE *ax,
                                void *pResourceWrapper,
                                const TRITON_KM_SHARED_DESC *d,
                                BOOL primary);

#ifdef __cplusplus
}
#endif

#endif /* TRITON_KM_H_INCLUDED */
