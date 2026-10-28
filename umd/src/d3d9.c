/* SPDX-License-Identifier: MIT */

#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <d3d9types.h>
#include <d3dumddi.h>
#include <d3d12.h>

#include "d3d9on12ddi.h"
#include "shared.h"
#include "triton_log.h"
#include "tritonKm.h"

HRESULT npt_d3d12_create_device_internal(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid,
                                         void **ppDevice);

struct adapter {
   HANDLE runtime;
   HANDLE driver;
   ID3D12Device *dev;
   ID3D12CommandQueue *queue;
   struct adapter *next;
};

struct surface {
   HANDLE driver;
   HANDLE runtime;
   struct wh_companion *companion;
   D3DKMT_HANDLE alloc;
   struct surface *next;
};

struct device {
   HANDLE runtime;
   HANDLE driver;
   D3DDDI_DEVICECALLBACKS callbacks;
   PFND3DDDI_PRESENTCB runtime_present;
   const D3DDDIARG_PRESENT1 *present;
   HANDLE context;
   TRITON_KM km;
   CRITICAL_SECTION km_lock;

   ID3D12Device *dev;
   ID3D12CommandQueue *queue;
   ID3D12CommandAllocator *alloc;
   ID3D12GraphicsCommandList *list;
   ID3D12Fence *fence;
   UINT64 fence_value;
   HANDLE event;

   struct surface *surfaces;

   struct device *next;
};

static D3D9ON12_PRIVATE_DDI_TABLE_VERSIONED d9;
static PFND3DDDI_CREATEDEVICE d9_create_device;
static PFND3DDDI_CLOSEADAPTER d9_close_adapter;
static PFND3DDDI_DESTROYDEVICE d9_destroy_device;
static PFND3DDDI_PRESENT1 d9_present1;
static PFND3DDDI_FLUSH1 d9_flush1;
static PFND3DDDI_CREATERESOURCE2 d9_create_resource2;
static PFND3DDDI_DESTROYRESOURCE d9_destroy_resource;
static SRWLOCK lock = SRWLOCK_INIT;
static struct adapter *adapters;
static struct device *devices;

static BOOL CALLBACK
d9_load(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
   (void)once, (void)param, (void)ctx;
   HMODULE m = LoadLibraryExW(L"d3d9on12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
   PFND3D9ON12_GETPRIVATEDDITABLEVERSIONED get =
      m ? (PFND3D9ON12_GETPRIVATEDDITABLEVERSIONED)(void *)GetProcAddress(m, "GetPrivateDDITableVersioned") : NULL;
   if (!get || FAILED(get(&d9, sizeof(d9))) || !d9.pfnOpenAdapter2 || !d9.pfnGetD3D12Resource ||
       !d9.pfnTransitionResource) {
      TR_LOG("d3d9: no D3D9On12 with interface version 2 (error %lu)", GetLastError());
      memset(&d9, 0, sizeof(d9));
   }
   return TRUE;
}

static struct device *
device_by_runtime(HANDLE runtime)
{
   AcquireSRWLockShared(&lock);
   struct device *d = devices;
   while (d && d->runtime != runtime)
      d = d->next;
   ReleaseSRWLockShared(&lock);
   return d;
}

static struct device *
device_by_driver(HANDLE driver)
{
   AcquireSRWLockShared(&lock);
   struct device *d = devices;
   while (d && d->driver != driver)
      d = d->next;
   ReleaseSRWLockShared(&lock);
   return d;
}

static struct surface *
surface_find(struct device *d, HANDLE driver)
{
   AcquireSRWLockShared(&lock);
   struct surface *s = d->surfaces;
   while (s && s->driver != driver)
      s = s->next;
   ReleaseSRWLockShared(&lock);
   return s;
}

static void
surface_free(struct device *d, struct surface *s)
{
   if (s->alloc) {
      D3DDDICB_DEALLOCATE da = {.hResource = s->runtime};
      HRESULT freed = d->callbacks.pfnDeallocateCb(d->runtime, &da);
      (void)freed;
   }
   wh_companion_destroy((struct ID3D12Device *)d->dev, s->companion);
   HeapFree(GetProcessHeap(), 0, s);
}

static HRESULT
companion_create(struct device *d, struct surface *s, ID3D12Resource *src)
{
   D3D12_RESOURCE_DESC desc;
   src->lpVtbl->GetDesc(src, &desc);
   struct wh_carrier c;
   s->companion = wh_companion_create((struct ID3D12Device *)d->dev, desc.Format, (UINT)desc.Width, desc.Height, &c);
   if (!s->companion) {
      TR_LOG("d3d9: no companion for a %llux%u back buffer of format %d", (unsigned long long)desc.Width,
             desc.Height, desc.Format);
      return E_FAIL;
   }
   D3DDDI_ALLOCATIONINFO ai = {.pPrivateDriverData = &c, .PrivateDriverDataSize = sizeof(c)};
   D3DDDICB_ALLOCATE a = {.hResource = s->runtime, .NumAllocations = 1, .pAllocationInfo = &ai};
   HRESULT hr = d->callbacks.pfnAllocateCb(d->runtime, &a);
   if (FAILED(hr)) {
      TR_LOG("d3d9: companion allocation failed 0x%08lx", hr);
      wh_companion_destroy((struct ID3D12Device *)d->dev, s->companion);
      s->companion = NULL;
      return hr;
   }
   s->alloc = ai.hAllocation;
   tritonKmRequestResidency(&d->km, s->alloc);
   return S_OK;
}

static HRESULT
companion_copy(struct device *d, struct surface *s, ID3D12Resource *src)
{
   HRESULT hr = S_OK;
   if (!d->list) {
      d->event = CreateEventW(NULL, FALSE, FALSE, NULL);
      if (!d->event)
         return E_OUTOFMEMORY;
      hr = ID3D12Device_CreateCommandAllocator(d->dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                               (void **)&d->alloc);
      if (SUCCEEDED(hr))
         hr = ID3D12Device_CreateFence(d->dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&d->fence);
      if (SUCCEEDED(hr))
         hr = ID3D12Device_CreateCommandList(d->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, d->alloc, NULL,
                                             &IID_ID3D12GraphicsCommandList, (void **)&d->list);
      if (FAILED(hr))
         return hr;
   } else {
      ID3D12CommandAllocator_Reset(d->alloc);
      ID3D12GraphicsCommandList_Reset(d->list, d->alloc, NULL);
   }
   BOOL recorded = wh_companion_record_copy((struct ID3D12Device *)d->dev, (struct ID3D12GraphicsCommandList *)d->list,
                                            (struct ID3D12Resource *)src, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                            s->companion);
   ID3D12GraphicsCommandList_Close(d->list);
   if (!recorded)
      return E_FAIL;
   ID3D12CommandQueue_ExecuteCommandLists(d->queue, 1, (ID3D12CommandList **)&d->list);
   hr = ID3D12CommandQueue_Signal(d->queue, d->fence, ++d->fence_value);
   if (SUCCEEDED(hr))
      hr = ID3D12Fence_SetEventOnCompletion(d->fence, d->fence_value, d->event);
   if (SUCCEEDED(hr))
      WaitForSingleObject(d->event, INFINITE);
   return hr;
}

static HRESULT APIENTRY
present_cb(HANDLE runtime, D3DDDICB_PRESENT *args)
{
   (void)args;
   struct device *d = device_by_runtime(runtime);
   if (!d || !d->present)
      return E_INVALIDARG;
   HANDLE src = d->present->phSrcResources[0].hResource;
   struct surface *s = surface_find(d, src);
   if (!s) {
      TR_LOG("d3d9: presenting a resource that is not a render target");
      return E_INVALIDARG;
   }
   d9.pfnTransitionResource(d->driver, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
   HRESULT hr = d9_flush1(d->driver, 0);
   ID3D12Resource *res = NULL;
   if (SUCCEEDED(hr))
      hr = d9.pfnGetD3D12Resource(src, &IID_ID3D12Resource, (void **)&res);
   if (SUCCEEDED(hr) && !s->companion)
      hr = companion_create(d, s, res);
   if (SUCCEEDED(hr))
      hr = companion_copy(d, s, res);
   if (res)
      ID3D12Resource_Release(res);
   if (FAILED(hr)) {
      TR_LOG("d3d9: copying the back buffer failed 0x%08lx", hr);
      return hr;
   }
   D3DDDICB_PRESENT present = {.hSrcAllocation = s->alloc, .hContext = d->context};
   hr = d->runtime_present(runtime, &present);
   if (FAILED(hr))
      TR_LOG("d3d9: present of allocation 0x%x failed 0x%08lx", s->alloc, hr);
   return hr;
}

static HRESULT APIENTRY
present1(HANDLE driver, D3DDDIARG_PRESENT1 *args)
{
   struct device *d = device_by_driver(driver);
   if (d)
      d->present = args;
   HRESULT hr = d9_present1(driver, args);
   if (d)
      d->present = NULL;
   if (FAILED(hr))
      TR_LOG("d3d9: D3D9On12 Present1 failed 0x%08lx", hr);
   return hr;
}

static HRESULT
context_create(struct device *d)
{
   D3DDDICB_CREATECONTEXTVIRTUAL v = {0};
   if (d->callbacks.pfnCreateContextVirtualCb &&
       SUCCEEDED(d->callbacks.pfnCreateContextVirtualCb(d->runtime, &v))) {
      d->context = v.hContext;
      return S_OK;
   }
   D3DDDICB_CREATECONTEXT c = {0};
   HRESULT hr = d->callbacks.pfnCreateContextCb(d->runtime, &c);
   if (SUCCEEDED(hr))
      d->context = c.hContext;
   return hr;
}

static HRESULT APIENTRY
create_resource2(HANDLE driver, D3DDDIARG_CREATERESOURCE2 *args)
{
   HANDLE runtime = args->hResource;
   HRESULT hr = d9_create_resource2(driver, args);
   struct device *d = device_by_driver(driver);
   if (FAILED(hr) || !d || !(args->Flags.RenderTarget || args->Flags.Primary))
      return hr;
   struct surface *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
   if (!s)
      return hr;
   s->driver = args->hResource;
   s->runtime = runtime;
   AcquireSRWLockExclusive(&lock);
   s->next = d->surfaces;
   d->surfaces = s;
   ReleaseSRWLockExclusive(&lock);
   return hr;
}

static HRESULT APIENTRY
destroy_resource(HANDLE driver, HANDLE resource)
{
   struct device *d = device_by_driver(driver);
   struct surface *s = NULL;
   if (d) {
      AcquireSRWLockExclusive(&lock);
      struct surface **link = &d->surfaces;
      while (*link && (*link)->driver != resource)
         link = &(*link)->next;
      s = *link;
      if (s)
         *link = s->next;
      ReleaseSRWLockExclusive(&lock);
   }
   if (s)
      surface_free(d, s);
   return d9_destroy_resource(driver, resource);
}

static void
surfaces_free(struct device *d)
{
   while (d->surfaces) {
      struct surface *s = d->surfaces;
      d->surfaces = s->next;
      surface_free(d, s);
   }
}

static void
device_free(struct device *d)
{
   tritonKmDestroy(&d->km);
   DeleteCriticalSection(&d->km_lock);
   if (d->context) {
      D3DDDICB_DESTROYCONTEXT c = {.hContext = d->context};
      HRESULT destroyed = d->callbacks.pfnDestroyContextCb(d->runtime, &c);
      (void)destroyed;
   }
   if (d->list)
      ID3D12GraphicsCommandList_Release(d->list);
   if (d->alloc)
      ID3D12CommandAllocator_Release(d->alloc);
   if (d->fence)
      ID3D12Fence_Release(d->fence);
   if (d->event)
      CloseHandle(d->event);
   if (d->queue)
      ID3D12CommandQueue_Release(d->queue);
   if (d->dev)
      ID3D12Device_Release(d->dev);
   HeapFree(GetProcessHeap(), 0, d);
}

static HRESULT APIENTRY
destroy_device(HANDLE driver)
{
   AcquireSRWLockExclusive(&lock);
   struct device **link = &devices;
   while (*link && (*link)->driver != driver)
      link = &(*link)->next;
   struct device *d = *link;
   if (d)
      *link = d->next;
   ReleaseSRWLockExclusive(&lock);
   if (d)
      surfaces_free(d);
   HRESULT hr = d9_destroy_device(driver);
   if (d)
      device_free(d);
   return hr;
}

static HRESULT APIENTRY
create_device(HANDLE adapter, D3DDDIARG_CREATEDEVICE *args)
{
   AcquireSRWLockShared(&lock);
   struct adapter *a = adapters;
   while (a && a->driver != adapter)
      a = a->next;
   ReleaseSRWLockShared(&lock);
   struct device *d = a ? HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d)) : NULL;
   if (!d)
      return E_OUTOFMEMORY;
   d->runtime = args->hDevice;
   d->callbacks = *args->pCallbacks;
   d->runtime_present = d->callbacks.pfnPresentCb;
   d->callbacks.pfnPresentCb = present_cb;
   tritonKmInit(&d->km, a->runtime, d->runtime, &d->callbacks);
   InitializeCriticalSection(&d->km_lock);
   d->km.pLock = &d->km_lock;
   d->dev = a->dev;
   ID3D12Device_AddRef(d->dev);
   d->queue = a->queue;
   ID3D12CommandQueue_AddRef(d->queue);

   const D3DDDI_DEVICECALLBACKS *runtime_callbacks = args->pCallbacks;
   args->pCallbacks = &d->callbacks;
   HRESULT hr = context_create(d);
   if (SUCCEEDED(hr))
      hr = d9_create_device(adapter, args);
   args->pCallbacks = runtime_callbacks;
   if (FAILED(hr)) {
      TR_LOG("d3d9: creating the device failed 0x%08lx", hr);
      device_free(d);
      return hr;
   }
   d->driver = args->hDevice;
   d9_destroy_device = args->pDeviceFuncs->pfnDestroyDevice;
   d9_present1 = args->pDeviceFuncs->pfnPresent1;
   d9_flush1 = args->pDeviceFuncs->pfnFlush1;
   d9_create_resource2 = args->pDeviceFuncs->pfnCreateResource2;
   d9_destroy_resource = args->pDeviceFuncs->pfnDestroyResource;
   args->pDeviceFuncs->pfnDestroyDevice = destroy_device;
   args->pDeviceFuncs->pfnPresent1 = present1;
   args->pDeviceFuncs->pfnCreateResource2 = create_resource2;
   args->pDeviceFuncs->pfnDestroyResource = destroy_resource;
   AcquireSRWLockExclusive(&lock);
   d->next = devices;
   devices = d;
   ReleaseSRWLockExclusive(&lock);
   return hr;
}

static HRESULT APIENTRY
close_adapter(HANDLE adapter)
{
   AcquireSRWLockExclusive(&lock);
   struct adapter **link = &adapters;
   while (*link && (*link)->driver != adapter)
      link = &(*link)->next;
   struct adapter *a = *link;
   if (a)
      *link = a->next;
   ReleaseSRWLockExclusive(&lock);
   HRESULT hr = d9_close_adapter(adapter);
   if (a) {
      ID3D12CommandQueue_Release(a->queue);
      ID3D12Device_Release(a->dev);
      HeapFree(GetProcessHeap(), 0, a);
   }
   return hr;
}

HRESULT APIENTRY
OpenAdapter9(D3DDDIARG_OPENADAPTER *args)
{
   static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
   InitOnceExecuteOnce(&once, d9_load, NULL, NULL);
   if (!d9.pfnOpenAdapter2)
      return E_FAIL;

   VIOGPU_ADAPTERINFO info = {0};
   D3DDDICB_QUERYADAPTERINFO q = {.pPrivateDriverData = &info, .PrivateDriverDataSize = sizeof(info)};
   HRESULT hr = args->pAdapterCallbacks->pfnQueryAdapterInfoCb(args->hAdapter, &q);
   if (FAILED(hr) || info.IamVioGPU != VIOGPU_IAM)
      return FAILED(hr) ? hr : E_FAIL;

   struct adapter *a = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*a));
   if (!a)
      return E_OUTOFMEMORY;
   a->runtime = args->hAdapter;
   hr = npt_d3d12_create_device_internal(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&a->dev);
   D3D12_COMMAND_QUEUE_DESC qd = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
   if (SUCCEEDED(hr))
      hr = ID3D12Device_CreateCommandQueue(a->dev, &qd, &IID_ID3D12CommandQueue, (void **)&a->queue);
   if (SUCCEEDED(hr)) {
      D3D9ON12_CREATE_DEVICE_ARGS2 create = {
         .pD3D12Device = (IUnknown *)a->dev,
         .ppD3D12Queues = (IUnknown **)&a->queue,
         .NumQueues = 1,
         .D3D9On12InterfaceVersion = 1,
      };
      hr = d9.pfnOpenAdapter2(args, &info.AdapterLuid, &create);
   }
   if (FAILED(hr)) {
      TR_LOG("d3d9: opening the adapter on D3D9On12 failed 0x%08lx", hr);
      if (a->queue)
         ID3D12CommandQueue_Release(a->queue);
      if (a->dev)
         ID3D12Device_Release(a->dev);
      HeapFree(GetProcessHeap(), 0, a);
      return hr;
   }
   a->driver = args->hAdapter;
   d9_create_device = args->pAdapterFuncs->pfnCreateDevice;
   d9_close_adapter = args->pAdapterFuncs->pfnCloseAdapter;
   args->pAdapterFuncs->pfnCreateDevice = create_device;
   args->pAdapterFuncs->pfnCloseAdapter = close_adapter;
   AcquireSRWLockExclusive(&lock);
   a->next = adapters;
   adapters = a;
   ReleaseSRWLockExclusive(&lock);
   return hr;
}
