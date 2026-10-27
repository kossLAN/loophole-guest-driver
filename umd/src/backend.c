/* SPDX-License-Identifier: MIT */

#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <string.h>

#include "npt_env.h"
#include "virtio-gpu/virgl_hw.h"
#include "tritonSharedBridge.h"
#include "shared.h"
/* After shared.h, which picks the Vulkan platform. */
#include <vulkan/vk_icd.h>

/* vkd3d-proton's d3d12core.dll entry. */
typedef struct IVKD3DCoreInterface IVKD3DCoreInterface;
typedef struct IVKD3DCoreInterfaceVtbl {
   HRESULT(STDMETHODCALLTYPE *CreateDevice)(IVKD3DCoreInterface *, IUnknown *, D3D_FEATURE_LEVEL, REFIID,
                                            void **);
   HRESULT(STDMETHODCALLTYPE *CreateRootSignatureDeserializer)(IVKD3DCoreInterface *, const void *, SIZE_T,
                                                               REFIID, void **);
   HRESULT(STDMETHODCALLTYPE *SerializeRootSignature)(IVKD3DCoreInterface *, const D3D12_ROOT_SIGNATURE_DESC *,
                                                      D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
   HRESULT(STDMETHODCALLTYPE *CreateVersionedRootSignatureDeserializer)(IVKD3DCoreInterface *, const void *,
                                                                        SIZE_T, REFIID, void **);
   HRESULT(STDMETHODCALLTYPE *SerializeVersionedRootSignature)(IVKD3DCoreInterface *,
                                                               const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *,
                                                               ID3DBlob **, ID3DBlob **);
} IVKD3DCoreInterfaceVtbl;
struct IVKD3DCoreInterface {
   const IVKD3DCoreInterfaceVtbl *lpVtbl;
};
static const GUID CLSID_VKD3DCore = {0xed53efad, 0xda21, 0x4d96, {0xa1, 0xbc, 0xe7, 0x34, 0xe0, 0x78, 0x87, 0x9c}};
static const GUID IID_IVKD3DCoreInterface = {0x18da885c, 0xa7b5, 0x464e,
                                             {0xa1, 0x21, 0xcc, 0xb7, 0x5d, 0x4d, 0xfc, 0x31}};

typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY1)(REFIID, void **);
typedef HRESULT(WINAPI *PFN_D3D12_GET_INTERFACE)(REFCLSID, REFIID, void **);
typedef void(WINAPI *PFN_SET_VULKAN_LOADER)(PFN_vkGetInstanceProcAddr);

static INIT_ONCE log_once = INIT_ONCE_STATIC_INIT;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION log_lock;

static BOOL
log_dir(char *dir, DWORD size)
{
   DWORD n = GetEnvironmentVariableA("LOOPHOLE_UMD_LOG", dir, size);
   if (n && n < size)
      return TRUE;
   return RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\loophole", "UmdLog", RRF_RT_REG_SZ, NULL, dir, &size) ==
             ERROR_SUCCESS &&
          dir[0];
}

static BOOL CALLBACK
log_open(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
   (void)once, (void)param, (void)ctx;
   InitializeCriticalSection(&log_lock);
   char dir[MAX_PATH], exe[MAX_PATH], path[MAX_PATH];
   if (!log_dir(dir, sizeof(dir)))
      return TRUE;
   GetModuleFileNameA(NULL, exe, sizeof(exe));
   const char *base = strrchr(exe, '\\');
   base = base ? base + 1 : exe;
   snprintf(path, sizeof(path), "%s\\umd-%s-%lu.log", dir, base, GetCurrentProcessId());
   log_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
   return TRUE;
}

void
loophole_umd_log(const char *line)
{
   OutputDebugStringA(line);
   InitOnceExecuteOnce(&log_once, log_open, NULL, NULL);
   if (log_file == INVALID_HANDLE_VALUE)
      return;
   char buf[640];
   SYSTEMTIME t;
   GetLocalTime(&t);
   int len = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u %5lu %s", t.wHour, t.wMinute, t.wSecond,
                      t.wMilliseconds, GetCurrentThreadId(), line);
   if (len < 0)
      return;
   if (len >= (int)sizeof(buf))
      len = sizeof(buf) - 1;
   DWORD written;
   EnterCriticalSection(&log_lock);
   WriteFile(log_file, buf, (DWORD)len, &written, NULL);
   LeaveCriticalSection(&log_lock);
}

#define LOG(fmt, ...)                                                                                        \
   do {                                                                                                      \
      char _b[512];                                                                                          \
      snprintf(_b, sizeof(_b), "loophole: " fmt "\n", ##__VA_ARGS__);                                       \
      loophole_umd_log(_b);                                                                                 \
   } while (0)

static INIT_ONCE layers_once = INIT_ONCE_STATIC_INIT;
static HMODULE dxgi_module, d3d11_module, d3d12core_module;
static PFN_D3D11_CREATE_DEVICE dxvk_create_device;
static IDXGIAdapter1 *dxvk_adapter;
static IVKD3DCoreInterface *vkd3d_core;
static PFN_vkGetInstanceProcAddr vulkan_driver;

static void
module_dir(wchar_t *dir, DWORD size)
{
   HMODULE self = NULL;
   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      (LPCWSTR)(void *)module_dir, &self);
   DWORD n = GetModuleFileNameW(self, dir, size);
   wchar_t *slash = n && n < size ? wcsrchr(dir, L'\\') : NULL;
   if (slash)
      slash[1] = 0;
   else
      dir[0] = 0;
}

static HMODULE
load_beside(const wchar_t *dir, const wchar_t *name)
{
   wchar_t path[MAX_PATH];
   _snwprintf(path, MAX_PATH, L"%ls%ls", dir, name);
   path[MAX_PATH - 1] = 0;
   HMODULE m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
   if (!m)
      LOG("cannot load %ls (error %lu)", path, GetLastError());
   return m;
}

#ifdef _WIN64
#define LAYER(name) L"loophole_" name L".dll"
#define VULKAN_DRIVER L"vulkan_radeon.dll"
#else
#define LAYER(name) L"loophole32_" name L".dll"
#define VULKAN_DRIVER L"vulkan_radeon32.dll"
#endif

static void
quiet_layer_logs(void)
{
   char dir[MAX_PATH];
   const char *path = log_dir(dir, sizeof(dir)) ? dir : "none";
   if (!GetEnvironmentVariableA("DXVK_LOG_PATH", NULL, 0))
      SetEnvironmentVariableA("DXVK_LOG_PATH", path);
}

static void
vulkan_driver_load(const wchar_t *dir)
{
   HMODULE icd = load_beside(dir, VULKAN_DRIVER);
   if (!icd)
      return;
   PFN_vkNegotiateLoaderICDInterfaceVersion negotiate =
      (PFN_vkNegotiateLoaderICDInterfaceVersion)(void *)GetProcAddress(icd,
                                                                       "vk_icdNegotiateLoaderICDInterfaceVersion");
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(icd, "vk_icdGetInstanceProcAddr");
   uint32_t version = CURRENT_LOADER_ICD_INTERFACE_VERSION;
   /* From version 3 the driver makes its own surfaces, which a caller without
    * a loader needs. */
   if (!negotiate || !gipa || negotiate(&version) != VK_SUCCESS || version < 3) {
      LOG("%ls: no driver interface to use", VULKAN_DRIVER);
      return;
   }
   vulkan_driver = gipa;
}

static void
set_vulkan_loader(HMODULE layer, const char *name)
{
   PFN_SET_VULKAN_LOADER set = layer ? (PFN_SET_VULKAN_LOADER)(void *)GetProcAddress(layer, name) : NULL;
   if (set)
      set(vulkan_driver);
   else if (layer)
      LOG("no %s: the layer uses the Vulkan loader", name);
}

PFN_vkGetInstanceProcAddr
wh_vk_gipa(void)
{
   if (vulkan_driver)
      return vulkan_driver;
   HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
   return loader ? (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(loader, "vkGetInstanceProcAddr") : NULL;
}

static BOOL CALLBACK
layers_load(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
   (void)once, (void)param, (void)ctx;
   wchar_t dir[MAX_PATH];
   module_dir(dir, MAX_PATH);
   quiet_layer_logs();

   vulkan_driver_load(dir);
   dxgi_module = load_beside(dir, LAYER("dxgi"));
   d3d11_module = load_beside(dir, LAYER("d3d11"));
   d3d12core_module = load_beside(dir, LAYER("d3d12core"));
   if (vulkan_driver) {
      set_vulkan_loader(dxgi_module, "DXVKSetVulkanLoader");
      set_vulkan_loader(d3d12core_module, "VKD3DSetVulkanLoader");
   }

   PFN_CREATE_DXGI_FACTORY1 create_factory =
      dxgi_module ? (PFN_CREATE_DXGI_FACTORY1)(void *)GetProcAddress(dxgi_module, "CreateDXGIFactory1") : NULL;
   IDXGIFactory1 *factory = NULL;
   if (create_factory && SUCCEEDED(create_factory(&IID_IDXGIFactory1, (void **)&factory))) {
      if (FAILED(IDXGIFactory1_EnumAdapters1(factory, 0, &dxvk_adapter)))
         dxvk_adapter = NULL;
      IDXGIFactory1_Release(factory);
   }
   if (dxvk_adapter) {
      DXGI_ADAPTER_DESC1 d;
      if (SUCCEEDED(IDXGIAdapter1_GetDesc1(dxvk_adapter, &d)))
         LOG("Vulkan adapter: %ls", d.Description);
   } else {
      LOG("no Vulkan adapter from DXVK");
   }

   if (d3d11_module)
      dxvk_create_device = (PFN_D3D11_CREATE_DEVICE)(void *)GetProcAddress(d3d11_module, "D3D11CreateDevice");

   PFN_D3D12_GET_INTERFACE get_interface =
      d3d12core_module ? (PFN_D3D12_GET_INTERFACE)(void *)GetProcAddress(d3d12core_module, "D3D12GetInterface")
                       : NULL;
   if (!get_interface || FAILED(get_interface(&CLSID_VKD3DCore, &IID_IVKD3DCoreInterface, (void **)&vkd3d_core)))
      vkd3d_core = NULL;
   return TRUE;
}

static void
layers_init(void)
{
   InitOnceExecuteOnce(&layers_once, layers_load, NULL, NULL);
}

HRESULT
npt_d3d11_create_device_internal(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
                                 const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
                                 ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel,
                                 ID3D11DeviceContext **ppImmediateContext)
{
   (void)pAdapter, (void)DriverType, (void)Software;
   layers_init();
   if (!dxvk_create_device || !dxvk_adapter)
      return DXGI_ERROR_UNSUPPORTED;
   HRESULT hr = dxvk_create_device((IDXGIAdapter *)dxvk_adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, Flags,
                                   pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel,
                                   ppImmediateContext);
   if (FAILED(hr))
      LOG("DXVK D3D11CreateDevice failed 0x%08lx", hr);
   return hr;
}

HRESULT
npt_d3d12_create_device_internal(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid,
                                 void **ppDevice)
{
   (void)pAdapter;
   layers_init();
   if (!vkd3d_core || !dxvk_adapter)
      return DXGI_ERROR_UNSUPPORTED;
   HRESULT hr = vkd3d_core->lpVtbl->CreateDevice(vkd3d_core, (IUnknown *)dxvk_adapter, MinimumFeatureLevel, riid,
                                                 ppDevice);
   if (FAILED(hr))
      LOG("vkd3d-proton CreateDevice failed 0x%08lx", hr);
   return hr;
}

HRESULT __stdcall
npt_D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pRootSignature,
                                         ID3DBlob **ppBlob, ID3DBlob **ppErrorBlob)
{
   layers_init();
   if (!vkd3d_core)
      return E_NOTIMPL;
   return vkd3d_core->lpVtbl->SerializeVersionedRootSignature(vkd3d_core, pRootSignature, ppBlob, ppErrorBlob);
}

/* Hooks for Neptune's remoted host backend. A failed gate arm makes the queue
 * order on the CPU side. */
BOOL
npt_d3d12_host_takes_row_major_texture(void *device)
{
   (void)device;
   return FALSE;
}

struct gate_wait {
   ID3D12Fence *fence;
   HANDLE event;
   HANDLE wait;
   UINT64 token;
};

static void CALLBACK
gate_ready(PVOID param, BOOLEAN fired)
{
   (void)fired;
   struct gate_wait *g = param;
   wh_gate_complete(g->token);
   UnregisterWaitEx(g->wait, NULL);
   CloseHandle(g->event);
   ID3D12Fence_Release(g->fence);
   HeapFree(GetProcessHeap(), 0, g);
}

static void
gate_abandon(struct gate_wait *g)
{
   if (g->wait)
      UnregisterWaitEx(g->wait, INVALID_HANDLE_VALUE);
   if (g->token)
      wh_gate_complete(g->token);
   if (g->event)
      CloseHandle(g->event);
   if (g->fence)
      ID3D12Fence_Release(g->fence);
   HeapFree(GetProcessHeap(), 0, g);
}

BOOL
npt_d3d12_ecl_gate_arm(void *fence, UINT64 value, UINT64 *token)
{
   struct gate_wait *g = HeapAlloc(GetProcessHeap(), 0, sizeof(*g));
   if (!g)
      return FALSE;
   *g = (struct gate_wait){.fence = fence};
   ID3D12Fence_AddRef(g->fence);
   g->event = CreateEventW(NULL, FALSE, FALSE, NULL);
   if (!g->event || !wh_gate_arm_deferred(&g->token)) {
      gate_abandon(g);
      return FALSE;
   }
   if (!RegisterWaitForSingleObject(&g->wait, g->event, gate_ready, g, INFINITE, WT_EXECUTEONLYONCE)) {
      gate_abandon(g);
      return FALSE;
   }
   const UINT64 kmdToken = g->token;
   HRESULT hr = ID3D12Fence_SetEventOnCompletion(g->fence, value, g->event);
   if (FAILED(hr)) {
      static LONG failures;
      if (InterlockedIncrement(&failures) <= 4)
         LOG("gate: SetEventOnCompletion(%llu) failed 0x%08lx", (unsigned long long)value, hr);
      gate_abandon(g);
      return FALSE;
   }
   *token = kmdToken;
   return TRUE;
}

struct npt_ring;

struct npt_ring *
npt_com_object_ring_seqno(void *self, uint32_t *seqno)
{
   (void)self;
   if (seqno)
      *seqno = 0;
   return NULL;
}

void
npt_ring_order_after_ring(struct npt_ring *ring, const struct npt_ring *target, uint32_t seqno)
{
   (void)ring, (void)target, (void)seqno;
}

void
npt_tls_unload(void)
{
}

void
npt_ring_unload(void)
{
}

uint32_t
npt_host_workaround_flags(void)
{
   return 0;
}

struct npt_env npt_env;
static INIT_ONCE env_once = INIT_ONCE_STATIC_INIT;

static uint64_t
parse_tokens(const char *var, const char *const *names, unsigned count)
{
   char buf[512];
   DWORD n = GetEnvironmentVariableA(var, buf, sizeof(buf));
   if (!n || n >= sizeof(buf))
      return 0;
   uint64_t bits = 0;
   for (char *tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
      for (unsigned i = 0; i < count; i++) {
         if (names[i] && !strcmp(tok, names[i]))
            bits |= 1ull << i;
      }
   }
   return bits;
}

static uint32_t
parse_u32(const char *var, uint32_t fallback)
{
   char buf[32];
   DWORD n = GetEnvironmentVariableA(var, buf, sizeof(buf));
   return n && n < sizeof(buf) ? (uint32_t)strtoul(buf, NULL, 10) : fallback;
}

static BOOL CALLBACK
env_load(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
   (void)once, (void)param, (void)ctx;
   /* Bit order of enum npt_perf / enum npt_debug. */
   static const char *const perf[] = {"multi_ring", "no_fence_feedback"};
   static const char *const debug[] = {
      "profile",       "force_vtest",          "present_timing",        "expose_all_modes",     "no_wddm2_ddi",
      "wddm2_0_only",  "d3d12_list_migration", "no_d3d12_xqueue_order", "d3d12_claim_atomic64",
   };
   npt_env.perf = parse_tokens("NPT_PERF", perf, ARRAYSIZE(perf));
   npt_env.debug = parse_tokens("NPT_DEBUG", debug, ARRAYSIZE(debug));
   npt_env.profile_period_ms = parse_u32("NPT_PROFILE_PERIOD_MS", 1000);
   npt_env.stutter_threshold_ms = parse_u32("NPT_STUTTER_MS", 50);
   npt_env.present_gate_trace_ms = parse_u32("NPT_PRESENT_GATE_TRACE", 0);
   npt_env.event_waiter_trace_ms = parse_u32("NPT_EVENT_WAITER_TRACE", 0);
   return TRUE;
}

void
npt_env_init(void)
{
   InitOnceExecuteOnce(&env_once, env_load, NULL, NULL);
}

uint32_t
npt_shared_texture_host_format(uint32_t format)
{
   switch (format) {
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
   default:
      return format;
   }
}

uint32_t
npt_shared_texture_virgl_format(uint32_t format)
{
   switch (format) {
   case DXGI_FORMAT_B8G8R8X8_UNORM:
      return VIRGL_FORMAT_B8G8R8X8_UNORM;
   case DXGI_FORMAT_R8G8B8A8_UNORM:
      return VIRGL_FORMAT_R8G8B8A8_UNORM;
   default:
      return VIRGL_FORMAT_B8G8R8A8_UNORM;
   }
}

uint32_t
npt_shared_texture_virgl_copy_format(uint32_t format)
{
   switch (format) {
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      return VIRGL_FORMAT_B8G8R8A8_UNORM;
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8X8_TYPELESS:
      return VIRGL_FORMAT_B8G8R8X8_UNORM;
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return VIRGL_FORMAT_R8G8B8A8_UNORM;
   case DXGI_FORMAT_R10G10B10A2_UNORM:
   case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return VIRGL_FORMAT_R10G10B10A2_UNORM;
   case DXGI_FORMAT_R16G16B16A16_FLOAT:
   case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return VIRGL_FORMAT_R16G16B16A16_FLOAT;
   case DXGI_FORMAT_B5G6R5_UNORM:
      return VIRGL_FORMAT_B5G6R5_UNORM;
   case DXGI_FORMAT_B5G5R5A1_UNORM:
      return VIRGL_FORMAT_B5G5R5A1_UNORM;
   case DXGI_FORMAT_B4G4R4A4_UNORM:
      return VIRGL_FORMAT_B4G4R4A4_UNORM;
   default:
      return 0;
   }
}

void
tritonSharedBridgeAdapterProbe(struct triton_adapter_probe *out)
{
   npt_env_init();
   memset(out, 0, sizeof(*out));
   /* The runtime loads this DLL only for viogpu3d, which runs in WDDM2
    * (GpuMmu) mode. */
   out->viogpu = true;
   out->wddm2 = true;
   out->host_ok = true;
   out->caps_flags = TRITON_HOSTCAP_D3D12 | TRITON_HOSTCAP_DXIL;
   out->no_wddm2_ddi = NPT_DEBUG(NO_WDDM2_DDI);
   out->wddm2_0_only = NPT_DEBUG(WDDM2_0_ONLY);
}

bool
tritonSharedBridgeCanExport(void)
{
   return false;
}

bool
tritonSharedBridgeExportBlob(void *texture, struct triton_shared_texture_desc *desc)
{
   (void)texture, (void)desc;
   return false;
}

bool
tritonSharedBridgeImportRes(void *device, uint32_t res_id, uint64_t size, uint32_t *alloc, uint32_t *res_kmt)
{
   (void)device, (void)res_id, (void)size, (void)alloc, (void)res_kmt;
   return false;
}

void
tritonSharedBridgeReleaseImportRes(void *device, uint32_t alloc, uint32_t res_kmt)
{
   (void)device, (void)alloc, (void)res_kmt;
}

void *
tritonSharedBridgeOpenRes(void *device, uint32_t res_id, const struct triton_shared_texture_desc *desc)
{
   (void)device, (void)res_id, (void)desc;
   return NULL;
}

HRESULT APIENTRY OpenAdapter10_2(void *args);
HRESULT APIENTRY OpenAdapter12(void *args);

static BOOL
listed(const char *value, const char *exe)
{
   char list[4096];
   DWORD size = sizeof(list) - 2;
   memset(list, 0, sizeof(list));
   if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\loophole", value, RRF_RT_REG_MULTI_SZ, NULL, list, &size) !=
       ERROR_SUCCESS)
      return FALSE;
   for (const char *p = list; *p; p += strlen(p) + 1) {
      if (!strcmp(p, "*") || !_stricmp(p, exe))
         return TRUE;
   }
   return FALSE;
}

/* Serves every process but those UmdDeny names. A denied one sees the adapter
 * fail to open. */
static BOOL
app_enabled(void)
{
   static LONG state; /* 0 unknown, 1 on, 2 off */
   LONG s = state;
   if (s)
      return s == 1;
   char exe[MAX_PATH];
   GetModuleFileNameA(NULL, exe, sizeof(exe));
   const char *base = strrchr(exe, '\\');
   base = base ? base + 1 : exe;
   const BOOL on = !listed("UmdDeny", base);
   LOG("%s: %s", base, on ? "served" : "not served (HKLM\\SOFTWARE\\loophole UmdDeny)");
   InterlockedCompareExchange(&state, on ? 1 : 2, 0);
   return on;
}

HRESULT APIENTRY
loophole_OpenAdapter10_2(void *args)
{
   return app_enabled() ? OpenAdapter10_2(args) : E_FAIL;
}

HRESULT APIENTRY
loophole_OpenAdapter12(void *args)
{
   return app_enabled() ? OpenAdapter12(args) : E_FAIL;
}
