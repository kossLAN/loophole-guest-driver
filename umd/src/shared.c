/* SPDX-License-Identifier: MIT */

/*
 * Each texture is a Vulkan image on DXVK's device in memory RADV exports behind
 * a KMT handle. Its KM allocation is a residency-only viogpu3d blob naming that
 * memory. Primaries and bitblt back buffers, which the KMD reads, are linear.
 * The rest are optimal.
 */

#include "triton.h"
#include "triton_log.h"
#include "tritonKm.h"
#include "virtio-gpu/virgl_hw.h"

#include <d3d11_3.h>
#include <d3dkmthk.h>

#include "shared.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#endif

/* DXVK's interop interfaces, as C vtables. */
typedef struct IDXGIVkInteropDevice1 IDXGIVkInteropDevice1;
typedef struct IDXGIVkInteropDevice1Vtbl {
   HRESULT(STDMETHODCALLTYPE *QueryInterface)(IDXGIVkInteropDevice1 *, REFIID, void **);
   ULONG(STDMETHODCALLTYPE *AddRef)(IDXGIVkInteropDevice1 *);
   ULONG(STDMETHODCALLTYPE *Release)(IDXGIVkInteropDevice1 *);
   void(STDMETHODCALLTYPE *GetVulkanHandles)(IDXGIVkInteropDevice1 *, VkInstance *, VkPhysicalDevice *,
                                             VkDevice *);
   void(STDMETHODCALLTYPE *GetSubmissionQueue)(IDXGIVkInteropDevice1 *, VkQueue *, uint32_t *);
   void(STDMETHODCALLTYPE *TransitionSurfaceLayout)(IDXGIVkInteropDevice1 *, void *, const VkImageSubresourceRange *,
                                                    VkImageLayout, VkImageLayout);
   void(STDMETHODCALLTYPE *FlushRenderingCommands)(IDXGIVkInteropDevice1 *);
   void(STDMETHODCALLTYPE *LockSubmissionQueue)(IDXGIVkInteropDevice1 *);
   void(STDMETHODCALLTYPE *ReleaseSubmissionQueue)(IDXGIVkInteropDevice1 *);
   void(STDMETHODCALLTYPE *GetSubmissionQueue1)(IDXGIVkInteropDevice1 *, VkQueue *, uint32_t *, uint32_t *);
   HRESULT(STDMETHODCALLTYPE *CreateTexture2DFromVkImage)(IDXGIVkInteropDevice1 *, const D3D11_TEXTURE2D_DESC1 *,
                                                          VkImage, ID3D11Texture2D **);
} IDXGIVkInteropDevice1Vtbl;
struct IDXGIVkInteropDevice1 {
   const IDXGIVkInteropDevice1Vtbl *lpVtbl;
};
static const GUID IID_IDXGIVkInteropDevice1 = {0xe2ef5fa5, 0xdc21, 0x4af7,
                                               {0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x24}};

#define BGRA8_VIEWS 2, {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB}
#define RGBA8_VIEWS                                                                                          \
   5, {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SNORM, \
       VK_FORMAT_R8G8B8A8_SINT}
#define RGB10A2_VIEWS 2, {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32}

/* Must match the Vulkan formats DXVK and vkd3d-proton pick. The views cover
 * every format D3D lets a view of the texture take. */
static const struct wh_format formats[] = {
   {DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VIRGL_FORMAT_B8G8R8X8_UNORM, BGRA8_VIEWS},
   {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VIRGL_FORMAT_B8G8R8X8_UNORM, BGRA8_VIEWS},
   {DXGI_FORMAT_B8G8R8A8_TYPELESS, VK_FORMAT_B8G8R8A8_UNORM, VIRGL_FORMAT_B8G8R8X8_UNORM, BGRA8_VIEWS},
   {DXGI_FORMAT_B8G8R8X8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VIRGL_FORMAT_B8G8R8X8_UNORM, BGRA8_VIEWS},
   {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VIRGL_FORMAT_B8G8R8X8_UNORM, BGRA8_VIEWS},
   {DXGI_FORMAT_B8G8R8X8_TYPELESS, VK_FORMAT_B8G8R8A8_UNORM, VIRGL_FORMAT_B8G8R8X8_UNORM, BGRA8_VIEWS},
   {DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VIRGL_FORMAT_R8G8B8X8_UNORM, RGBA8_VIEWS},
   {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VIRGL_FORMAT_R8G8B8X8_UNORM, RGBA8_VIEWS},
   {DXGI_FORMAT_R8G8B8A8_TYPELESS, VK_FORMAT_R8G8B8A8_UNORM, VIRGL_FORMAT_R8G8B8X8_UNORM, RGBA8_VIEWS},
   {DXGI_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UINT, 0, RGBA8_VIEWS},
   {DXGI_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, 0, RGBA8_VIEWS},
   {DXGI_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SINT, 0, RGBA8_VIEWS},
   {DXGI_FORMAT_R10G10B10A2_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VIRGL_FORMAT_R10G10B10A2_UNORM,
    RGB10A2_VIEWS},
   {DXGI_FORMAT_R10G10B10A2_UINT, VK_FORMAT_A2B10G10R10_UINT_PACK32, 0, RGB10A2_VIEWS},
   {DXGI_FORMAT_R10G10B10A2_TYPELESS, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VIRGL_FORMAT_R10G10B10A2_UNORM,
    RGB10A2_VIEWS},
   {DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VIRGL_FORMAT_R16G16B16A16_FLOAT, 1,
    {VK_FORMAT_R16G16B16A16_SFLOAT}},
};

const struct wh_format *
wh_format_lookup(DXGI_FORMAT f)
{
   for (unsigned i = 0; i < ARRAYSIZE(formats); i++) {
      if (formats[i].dxgi == f)
         return &formats[i];
   }
   return NULL;
}

BOOL
wh_vk_load(struct wh_vk *vk, VkInstance inst, VkPhysicalDevice phys, VkDevice dev)
{
   memset(vk, 0, sizeof(*vk));
   vk->phys = phys;
   vk->dev = dev;
   PFN_vkGetInstanceProcAddr gipa = wh_vk_gipa();
   PFN_vkGetDeviceProcAddr gdpa = gipa ? (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr") : NULL;
   if (!gdpa)
      return FALSE;
#define INST(name) vk->name = (PFN_vk##name)gipa(inst, "vk" #name)
#define DEV(name) vk->name = (PFN_vk##name)gdpa(dev, "vk" #name)
   INST(GetPhysicalDeviceImageFormatProperties2);
   INST(GetPhysicalDeviceMemoryProperties);
   DEV(CreateImage);
   DEV(DestroyImage);
   DEV(GetImageMemoryRequirements2);
   DEV(GetImageSubresourceLayout);
   DEV(AllocateMemory);
   DEV(FreeMemory);
   DEV(BindImageMemory);
   DEV(GetMemoryWin32HandleKHR);
   DEV(CmdPipelineBarrier);
   DEV(CmdCopyImage);
#undef INST
#undef DEV
   if (!vk->GetPhysicalDeviceImageFormatProperties2 || !vk->GetPhysicalDeviceMemoryProperties ||
       !vk->CreateImage || !vk->DestroyImage || !vk->GetImageMemoryRequirements2 || !vk->GetImageSubresourceLayout ||
       !vk->AllocateMemory || !vk->FreeMemory || !vk->BindImageMemory || !vk->GetMemoryWin32HandleKHR ||
       !vk->CmdPipelineBarrier || !vk->CmdCopyImage) {
      TR_LOG("shared: Vulkan entry points missing (VK_KHR_external_memory_win32 not enabled?)");
      return FALSE;
   }
   return TRUE;
}

static void
image_info(const struct wh_shared_desc *d, VkImageCreateInfo *ci, VkImageFormatListCreateInfo *list,
           VkExternalMemoryImageCreateInfo *ext)
{
   *ext = (VkExternalMemoryImageCreateInfo){
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
   };
   *list = (VkImageFormatListCreateInfo){
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
      .pNext = ext,
      .viewFormatCount = d->view_format_count,
      .pViewFormats = (const VkFormat *)d->view_formats,
   };
   *ci = (VkImageCreateInfo){
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = list,
      .flags = d->flags,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = (VkFormat)d->format,
      .extent = {d->width, d->height, 1},
      .mipLevels = d->mip_levels,
      .arrayLayers = d->array_layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = (VkImageTiling)d->tiling,
      .usage = d->usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
}

static BOOL
image_supported(struct wh_vk *vk, const struct wh_shared_desc *d)
{
   VkImageCreateInfo ci;
   VkImageFormatListCreateInfo list;
   VkExternalMemoryImageCreateInfo ext;
   image_info(d, &ci, &list, &ext);
   VkPhysicalDeviceExternalImageFormatInfo ext_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
      .pNext = &list,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
   };
   list.pNext = NULL;
   VkPhysicalDeviceImageFormatInfo2 info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .pNext = &ext_info,
      .format = ci.format,
      .type = ci.imageType,
      .tiling = ci.tiling,
      .usage = ci.usage,
      .flags = ci.flags,
   };
   VkExternalImageFormatProperties ext_props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
   VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ext_props};
   if (vk->GetPhysicalDeviceImageFormatProperties2(vk->phys, &info, &props) != VK_SUCCESS)
      return FALSE;
   const VkExternalMemoryFeatureFlags need =
      VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
   return (ext_props.externalMemoryProperties.externalMemoryFeatures & need) == need &&
          props.imageFormatProperties.maxMipLevels >= d->mip_levels &&
          props.imageFormatProperties.maxArrayLayers >= d->array_layers &&
          props.imageFormatProperties.maxExtent.width >= d->width &&
          props.imageFormatProperties.maxExtent.height >= d->height;
}

BOOL
wh_desc_init(struct wh_vk *vk, struct wh_shared_desc *d, const struct wh_format *f, UINT width, UINT height,
             UINT mip_levels, UINT array_layers, BOOL linear, UINT bind_flags, UINT misc_flags)
{
   *d = (struct wh_shared_desc){
      .magic = WH_SHARED_MAGIC,
      .version = WH_SHARED_VERSION,
      .format = f->vk,
      .tiling = linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .width = width,
      .height = height,
      .mip_levels = mip_levels ? mip_levels : 1,
      .array_layers = array_layers ? array_layers : 1,
      .view_format_count = f->view_count,
      .dxgi_format = f->dxgi,
      .bind_flags = bind_flags,
      /* DXVK exports memory of its own for the sharing flags. The high bits
       * are Triton's (TRITON_D3D11_MISC_LINEAR_EXPORT). */
      .misc_flags = misc_flags & 0xffff &
                    ~(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                      D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX),
   };
   memcpy(d->view_formats, f->views, sizeof(d->view_formats));
   if (f->view_count > 1)
      d->flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
   if (misc_flags & D3D11_RESOURCE_MISC_TEXTURECUBE)
      d->flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
   if (bind_flags & D3D11_BIND_UNORDERED_ACCESS) {
      d->usage |= VK_IMAGE_USAGE_STORAGE_BIT;
      if (!image_supported(vk, d))
         d->usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
   }
   if (!image_supported(vk, d)) {
      TR_LOG("shared: %ux%u fmt=%d mips=%u layers=%u tiling=%u not exportable", d->width, d->height,
             (int)f->dxgi, d->mip_levels, d->array_layers, d->tiling);
      return FALSE;
   }
   return TRUE;
}

static uint32_t km_res_id(uint32_t kmt);

BOOL
wh_image_create(struct wh_vk *vk, struct wh_shared_desc *d, BOOL import, struct wh_image *img)
{
   memset(img, 0, sizeof(*img));
   VkImageCreateInfo ci;
   VkImageFormatListCreateInfo list;
   VkExternalMemoryImageCreateInfo ext;
   image_info(d, &ci, &list, &ext);
   VkResult res = vk->CreateImage(vk->dev, &ci, NULL, &img->image);
   if (res != VK_SUCCESS) {
      TR_LOG("shared: vkCreateImage failed %d", (int)res);
      return FALSE;
   }

   VkMemoryDedicatedRequirements dedicated_req = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
   VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated_req};
   VkImageMemoryRequirementsInfo2 req_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
      .image = img->image,
   };
   vk->GetImageMemoryRequirements2(vk->dev, &req_info, &req);

   VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = img->image,
   };
   VkExportMemoryAllocateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
   };
   VkImportMemoryWin32HandleInfoKHR import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
      .pNext = &dedicated,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
      .handle = (HANDLE)(uintptr_t)d->kmt,
   };
   VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = import ? (const void *)&import_info : (const void *)&export_info,
      .allocationSize = import ? d->memory_size : req.memoryRequirements.size,
      .memoryTypeIndex = d->memory_type,
   };
   if (!import) {
      VkPhysicalDeviceMemoryProperties mp;
      vk->GetPhysicalDeviceMemoryProperties(vk->phys, &mp);
      alloc.memoryTypeIndex = UINT32_MAX;
      for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
         if ((req.memoryRequirements.memoryTypeBits & (1u << i)) &&
             (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc.memoryTypeIndex = i;
            break;
         }
      }
      if (alloc.memoryTypeIndex == UINT32_MAX) {
         TR_LOG("shared: no device-local memory type in 0x%x", req.memoryRequirements.memoryTypeBits);
         goto fail;
      }
   } else if (req.memoryRequirements.size > d->memory_size ||
              !(req.memoryRequirements.memoryTypeBits & (1u << d->memory_type))) {
      TR_LOG("shared: image needs %llu bytes of types 0x%x, the memory is %llu bytes of type %u",
             (unsigned long long)req.memoryRequirements.size, req.memoryRequirements.memoryTypeBits,
             (unsigned long long)d->memory_size, d->memory_type);
      goto fail;
   }
   res = vk->AllocateMemory(vk->dev, &alloc, NULL, &img->memory);
   if (res != VK_SUCCESS) {
      TR_LOG("shared: vkAllocateMemory (%s) failed %d", import ? "import" : "export", (int)res);
      goto fail;
   }
   res = vk->BindImageMemory(vk->dev, img->image, img->memory, 0);
   if (res != VK_SUCCESS) {
      TR_LOG("shared: vkBindImageMemory failed %d", (int)res);
      goto fail;
   }
   if (d->tiling == VK_IMAGE_TILING_LINEAR) {
      VkImageSubresource sub = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
      vk->GetImageSubresourceLayout(vk->dev, img->image, &sub, &img->layout);
   }
   if (!import) {
      d->memory_size = alloc.allocationSize;
      d->memory_type = alloc.memoryTypeIndex;
      VkMemoryGetWin32HandleInfoKHR get = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
         .memory = img->memory,
         .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,
      };
      HANDLE h = NULL;
      res = vk->GetMemoryWin32HandleKHR(vk->dev, &get, &h);
      if (res != VK_SUCCESS || !h) {
         TR_LOG("shared: vkGetMemoryWin32HandleKHR failed %d", (int)res);
         goto fail;
      }
      d->kmt = (uint32_t)(uintptr_t)h;
      d->res_id = km_res_id(d->kmt);
      if (d->tiling == VK_IMAGE_TILING_LINEAR && !d->res_id) {
         TR_LOG("shared: linear image without a host resource");
         goto fail;
      }
   }
   return TRUE;

fail:
   wh_image_destroy(vk, img);
   return FALSE;
}

void
wh_image_destroy(struct wh_vk *vk, struct wh_image *img)
{
   if (img->image)
      vk->DestroyImage(vk->dev, img->image, NULL);
   if (img->memory)
      vk->FreeMemory(vk->dev, img->memory, NULL);
   img->image = VK_NULL_HANDLE;
   img->memory = VK_NULL_HANDLE;
}

static struct {
   INIT_ONCE once;
   PFND3DKMT_QUERYRESOURCEINFO QueryResourceInfo;
   PFND3DKMT_OPENRESOURCE2 OpenResource2;
   PFND3DKMT_DESTROYALLOCATION DestroyAllocation;
   PFND3DKMT_ESCAPE Escape;
   PFND3DKMT_CREATEDEVICE CreateDevice;
   D3DKMT_HANDLE adapter;
   D3DKMT_HANDLE device;
   CRITICAL_SECTION lock;
} km = {INIT_ONCE_STATIC_INIT};

static BOOL CALLBACK
km_open(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
   (void)once, (void)param, (void)ctx;
   InitializeCriticalSection(&km.lock);
   HMODULE gdi = LoadLibraryW(L"gdi32.dll");
   if (!gdi)
      return TRUE;
   PFND3DKMT_ENUMADAPTERS2 EnumAdapters2 = (void *)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
   PFND3DKMT_QUERYADAPTERINFO QueryAdapterInfo = (void *)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
   PFND3DKMT_CLOSEADAPTER CloseAdapter = (void *)GetProcAddress(gdi, "D3DKMTCloseAdapter");
   km.CreateDevice = (void *)GetProcAddress(gdi, "D3DKMTCreateDevice");
   km.QueryResourceInfo = (void *)GetProcAddress(gdi, "D3DKMTQueryResourceInfo");
   km.OpenResource2 = (void *)GetProcAddress(gdi, "D3DKMTOpenResource2");
   km.DestroyAllocation = (void *)GetProcAddress(gdi, "D3DKMTDestroyAllocation");
   km.Escape = (void *)GetProcAddress(gdi, "D3DKMTEscape");
   if (!EnumAdapters2 || !QueryAdapterInfo || !CloseAdapter || !km.CreateDevice || !km.QueryResourceInfo ||
       !km.OpenResource2 || !km.DestroyAllocation || !km.Escape)
      return TRUE;

   D3DKMT_ENUMADAPTERS2 en = {0};
   if (!NT_SUCCESS(EnumAdapters2(&en)) || !en.NumAdapters)
      return TRUE;
   D3DKMT_ADAPTERINFO *infos = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, en.NumAdapters * sizeof(*infos));
   if (!infos)
      return TRUE;
   en.pAdapters = infos;
   if (NT_SUCCESS(EnumAdapters2(&en))) {
      for (ULONG i = 0; i < en.NumAdapters; i++) {
         VIOGPU_ADAPTERINFO vi = {0};
         D3DKMT_QUERYADAPTERINFO q = {
            .hAdapter = infos[i].hAdapter,
            .Type = KMTQAITYPE_UMDRIVERPRIVATE,
            .pPrivateDriverData = &vi,
            .PrivateDriverDataSize = sizeof(vi),
         };
         if (!km.adapter && NT_SUCCESS(QueryAdapterInfo(&q)) && vi.IamVioGPU == VIOGPU_IAM) {
            km.adapter = infos[i].hAdapter;
         } else {
            D3DKMT_CLOSEADAPTER c = {.hAdapter = infos[i].hAdapter};
            NTSTATUS closed = CloseAdapter(&c);
            (void)closed;
         }
      }
   }
   HeapFree(GetProcessHeap(), 0, infos);
   if (!km.adapter)
      TR_LOG("shared: no loophole GPU adapter");
   return TRUE;
}

static uint32_t
km_res_id(uint32_t kmt)
{
   InitOnceExecuteOnce(&km.once, km_open, NULL, NULL);
   if (!km.adapter)
      return 0;
   EnterCriticalSection(&km.lock);
   uint32_t res_id = 0;
   if (!km.device) {
      D3DKMT_CREATEDEVICE cd = {.hAdapter = km.adapter};
      if (!NT_SUCCESS(km.CreateDevice(&cd))) {
         TR_LOG("shared: no D3DKMT device on the GPU");
         goto out;
      }
      km.device = cd.hDevice;
   }
   D3DKMT_QUERYRESOURCEINFO q = {.hDevice = km.device, .hGlobalShare = kmt};
   NTSTATUS st = km.QueryResourceInfo(&q);
   if (!NT_SUCCESS(st) || q.NumAllocations != 1) {
      TR_LOG("shared: D3DKMTQueryResourceInfo(0x%x) failed 0x%lx (%u allocations)", kmt, st, q.NumAllocations);
      goto out;
   }
   const UINT total = q.TotalPrivateDriverDataSize, resource = q.ResourcePrivateDriverDataSize,
              runtime = q.PrivateRuntimeDataSize;
   BYTE *mem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total + resource + runtime + 1);
   if (!mem)
      goto out;
   D3DDDI_OPENALLOCATIONINFO2 ai = {0};
   D3DKMT_OPENRESOURCE o = {
      .hDevice = km.device,
      .hGlobalShare = kmt,
      .NumAllocations = 1,
      .pOpenAllocationInfo2 = &ai,
      .pPrivateRuntimeData = mem + total + resource,
      .PrivateRuntimeDataSize = runtime,
      .pResourcePrivateDriverData = mem + total,
      .ResourcePrivateDriverDataSize = resource,
      .pTotalPrivateDriverDataBuffer = mem,
      .TotalPrivateDriverDataBufferSize = total,
   };
   st = km.OpenResource2(&o);
   HeapFree(GetProcessHeap(), 0, mem);
   if (!NT_SUCCESS(st)) {
      TR_LOG("shared: D3DKMTOpenResource2(0x%x) failed 0x%lx", kmt, st);
      goto out;
   }
   VIOGPU_ESCAPE esc;
   memset(&esc, 0, sizeof(esc));
   esc.Type = VIOGPU_RES_INFO;
   esc.DataLength = sizeof(esc.ResourceInfo);
   esc.ResourceInfo.ResHandle = ai.hAllocation;
   D3DKMT_ESCAPE e = {
      .hAdapter = km.adapter,
      .hDevice = km.device,
      .Type = D3DKMT_ESCAPE_DRIVERPRIVATE,
      .pPrivateDriverData = &esc,
      .PrivateDriverDataSize = sizeof(esc),
   };
   st = km.Escape(&e);
   if (NT_SUCCESS(st) && esc.ResourceInfo.IsCreated)
      res_id = esc.ResourceInfo.Id;
   else
      TR_LOG("shared: RES_INFO failed 0x%lx (created %d)", st, esc.ResourceInfo.IsCreated);
   D3DKMT_DESTROYALLOCATION d = {.hDevice = km.device, .hResource = o.hResource};
   NTSTATUS closed = km.DestroyAllocation(&d);
   (void)closed;
out:
   LeaveCriticalSection(&km.lock);
   return res_id;
}

/* Unique across processes and loads of this DLL. Bits 61-63 are reserved for
 * RADV's shared blobs and the KMD. */
static uint64_t
carrier_cookie(void)
{
   static LONG next;
   if (!next) {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      InterlockedCompareExchange(&next, (LONG)(now.LowPart | 1), 0);
   }
   return (1ull << 60) | ((uint64_t)((GetCurrentProcessId() >> 2) & 0x0fffffff) << 32) |
          (uint32_t)InterlockedIncrement(&next);
}

void
wh_carrier_fill(struct wh_carrier *c, const struct wh_shared_desc *d, const struct wh_format *f,
                const struct wh_image *img, BOOL linear, BOOL primary)
{
   memset(c, 0, sizeof(*c));
   VIOGPU_CREATE_ALLOCATION_EXCHANGE *x = &c->ex.Base;
   x->Type = VIOGPU_RESOURCE_TYPE_BLOB;
   x->OptionsBlob.blob_mem = VIOGPU_BLOB_MEM_GUEST;
   x->OptionsBlob.blob_flags = VIOGPU_BLOB_FLAG_RESIDENCY_ONLY;
   x->OptionsBlob.surface_width = d->width;
   x->OptionsBlob.surface_height = d->height;
   x->OptionsBlob.surface_format = d->dxgi_format;
   x->OptionsBlob.primary = primary;
   if (linear) {
      x->OptionsBlob.scanout_res_id = d->res_id;
      x->OptionsBlob.scanout.width = d->width;
      x->OptionsBlob.scanout.height = d->height;
      x->OptionsBlob.scanout.format = f->virgl;
      x->OptionsBlob.scanout.strides[0] = (ULONG)img->layout.rowPitch;
      x->OptionsBlob.scanout.offsets[0] = (ULONG)img->layout.offset;
   }
   x->Size = 4096;
   c->ex.LookupCookie = carrier_cookie();
   c->desc = *d;
}

struct shared {
   struct wh_image img;
};

struct dxvk {
   IDXGIVkInteropDevice1 *interop;
   struct wh_vk vk;
};

static void
dxvk_fini(struct dxvk *x)
{
   if (x->interop)
      x->interop->lpVtbl->Release(x->interop);
   x->interop = NULL;
}

static BOOL
dxvk_init(PTRITON_DEVICE pD, struct dxvk *x)
{
   memset(x, 0, sizeof(*x));
   if (!pD->pDev1 ||
       FAILED(ID3D11Device1_QueryInterface(pD->pDev1, &IID_IDXGIVkInteropDevice1, (void **)&x->interop)))
      return FALSE;
   VkInstance inst;
   VkPhysicalDevice phys;
   VkDevice dev;
   x->interop->lpVtbl->GetVulkanHandles(x->interop, &inst, &phys, &dev);
   if (!wh_vk_load(&x->vk, inst, phys, dev)) {
      dxvk_fini(x);
      return FALSE;
   }
   return TRUE;
}

static ID3D11Texture2D *
image_wrap(struct dxvk *x, const struct wh_shared_desc *d, VkImage image)
{
   D3D11_TEXTURE2D_DESC1 td = {
      .Width = d->width,
      .Height = d->height,
      .MipLevels = d->mip_levels,
      .ArraySize = d->array_layers,
      .Format = (DXGI_FORMAT)d->dxgi_format,
      .SampleDesc = {.Count = 1},
      .Usage = D3D11_USAGE_DEFAULT,
      .BindFlags = d->bind_flags,
      .MiscFlags = d->misc_flags,
      .TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED,
   };
   ID3D11Texture2D *tex = NULL;
   HRESULT hr = x->interop->lpVtbl->CreateTexture2DFromVkImage(x->interop, &td, image, &tex);
   if (FAILED(hr)) {
      TR_LOG("shared: CreateTexture2DFromVkImage failed 0x%08lx", (unsigned long)hr);
      return NULL;
   }
   return tex;
}

static BOOL
carrier_allocate(PTRITON_DEVICE pD, PTRITON_RESOURCE r, struct wh_carrier *c, BOOL primary)
{
   D3DDDI_ALLOCATIONINFO2 ai;
   memset(&ai, 0, sizeof(ai));
   ai.pPrivateDriverData = c;
   ai.PrivateDriverDataSize = sizeof(*c);
   if (primary) {
      ai.Flags.Primary = 1;
      ai.VidPnSourceId = 0;
   }
   D3DDDICB_ALLOCATE cb;
   memset(&cb, 0, sizeof(cb));
   cb.hResource = r->hRTResource.handle;
   cb.NumAllocations = 1;
   cb.pAllocationInfo2 = &ai;
   HRESULT hr = pD->KTCallbacks.pfnAllocateCb(pD->hRTDevice.handle, &cb);
   if (FAILED(hr) || !ai.hAllocation) {
      TR_LOG("shared: pfnAllocateCb failed 0x%08lx", (unsigned long)hr);
      return FALSE;
   }
   r->hKMAllocation = ai.hAllocation;
   /* WDDM2 allocations start out evicted. */
   tritonKmRequestResidency(&pD->Km, ai.hAllocation);
   return TRUE;
}

static void
lock_context(PTRITON_DEVICE pD)
{
   if (pD->presentLockInit)
      EnterCriticalSection(&pD->presentLock);
}

static void
unlock_context(PTRITON_DEVICE pD)
{
   if (pD->presentLockInit)
      LeaveCriticalSection(&pD->presentLock);
}

ID3D11Texture2D *
tritonSharedCreate(PTRITON_DEVICE pD, PTRITON_RESOURCE r, const D3D11_TEXTURE2D_DESC *desc,
                   const D3D11_SUBRESOURCE_DATA *init, UINT flags)
{
   const BOOL primary = (flags & TRITON_SHARED_PRIMARY) != 0;
   const BOOL linear = primary || (flags & TRITON_SHARED_LINEAR);
   const struct wh_format *f = wh_format_lookup(desc->Format);
   if (!f || desc->SampleDesc.Count > 1 || desc->Usage != D3D11_USAGE_DEFAULT || desc->CPUAccessFlags ||
       !pD->KTCallbacks.pfnAllocateCb || (linear && !f->virgl))
      return NULL;
   struct dxvk x;
   if (!dxvk_init(pD, &x))
      return NULL;

   struct wh_shared_desc d;
   struct shared *s = NULL;
   ID3D11Texture2D *tex = NULL;
   struct wh_carrier c;
   if (!wh_desc_init(&x.vk, &d, f, desc->Width, desc->Height, desc->MipLevels, desc->ArraySize, linear,
                     desc->BindFlags, desc->MiscFlags) ||
       !(s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s))) ||
       !wh_image_create(&x.vk, &d, FALSE, &s->img))
      goto fail;
   tex = image_wrap(&x, &d, s->img.image);
   if (!tex)
      goto fail;
   wh_carrier_fill(&c, &d, f, &s->img, linear, primary);
   if (!carrier_allocate(pD, r, &c, primary))
      goto fail;
   dxvk_fini(&x);

   if (init) {
      lock_context(pD);
      for (UINT layer = 0; layer < d.array_layers; layer++) {
         for (UINT mip = 0; mip < d.mip_levels; mip++) {
            const D3D11_SUBRESOURCE_DATA *i = &init[layer * d.mip_levels + mip];
            ID3D11DeviceContext1_UpdateSubresource(pD->pCtx1, (ID3D11Resource *)tex, layer * d.mip_levels + mip,
                                                   NULL, i->pSysMem, i->SysMemPitch, i->SysMemSlicePitch);
         }
      }
      unlock_context(pD);
   }

   r->pShared = s;
   TR_LOG("shared: %s %ux%u fmt=%d kmt=0x%x res_id=%u alloc=0x%x pitch=%llu",
          primary  ? "primary"
          : linear ? "linear texture"
                   : "texture",
          d.width, d.height, (int)desc->Format, d.kmt, d.res_id, r->hKMAllocation,
          (unsigned long long)s->img.layout.rowPitch);
   return tex;

fail:
   if (tex)
      ID3D11Texture2D_Release(tex);
   if (s)
      wh_image_destroy(&x.vk, &s->img);
   HeapFree(GetProcessHeap(), 0, s);
   dxvk_fini(&x);
   return NULL;
}

/* Stands in when the memory died with its producer. */
static ID3D11Texture2D *
blank_texture(PTRITON_DEVICE pD, const struct wh_shared_desc *d)
{
   const UINT bpp = d->dxgi_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
   const SIZE_T pitch = (SIZE_T)d->width * bpp;
   void *zero = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, pitch * d->height);
   if (!zero || d->mip_levels != 1 || d->array_layers != 1) {
      HeapFree(GetProcessHeap(), 0, zero);
      return NULL;
   }
   D3D11_TEXTURE2D_DESC td = {
      .Width = d->width,
      .Height = d->height,
      .MipLevels = 1,
      .ArraySize = 1,
      .Format = (DXGI_FORMAT)d->dxgi_format,
      .SampleDesc = {.Count = 1},
      .Usage = D3D11_USAGE_DEFAULT,
      .BindFlags = d->bind_flags,
   };
   D3D11_SUBRESOURCE_DATA init = {.pSysMem = zero, .SysMemPitch = (UINT)pitch};
   ID3D11Texture2D *tex = NULL;
   if (FAILED(ID3D11Device1_CreateTexture2D(pD->pDev1, &td, &init, &tex)))
      tex = NULL;
   HeapFree(GetProcessHeap(), 0, zero);
   return tex;
}

BOOL
tritonSharedOpen(PTRITON_DEVICE pD, PTRITON_RESOURCE r, const void *priv, UINT size)
{
   struct wh_carrier c;
   if (!priv || size < sizeof(c))
      return FALSE;
   memcpy(&c, priv, sizeof(c));
   if (c.desc.magic != WH_SHARED_MAGIC)
      return FALSE;
   if (c.desc.version != WH_SHARED_VERSION || c.desc.view_format_count > WH_MAX_VIEW_FORMATS) {
      TR_LOG("shared: open of version %u", c.desc.version);
      tritonSetCreateError(pD, E_FAIL);
      return TRUE;
   }

   struct dxvk x;
   struct shared *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
   ID3D11Texture2D *tex = NULL;
   if (s && dxvk_init(pD, &x)) {
      if (wh_image_create(&x.vk, &c.desc, TRUE, &s->img)) {
         tex = image_wrap(&x, &c.desc, s->img.image);
         if (!tex)
            wh_image_destroy(&x.vk, &s->img);
      }
      dxvk_fini(&x);
   }
   if (!tex) {
      HeapFree(GetProcessHeap(), 0, s);
      s = NULL;
      tex = blank_texture(pD, &c.desc);
      TR_LOG("shared: open of kmt=0x%x %ux%u failed; %s", c.desc.kmt, c.desc.width, c.desc.height,
             tex ? "a blank texture stands in" : "no blank texture either");
      if (!tex) {
         tritonSetCreateError(pD, E_FAIL);
         return TRUE;
      }
   }

   r->Format = (DXGI_FORMAT)c.desc.dxgi_format;
   r->Width = c.desc.width;
   r->Height = c.desc.height;
   r->Depth = 1;
   r->MipLevels = c.desc.mip_levels;
   r->ArraySize = c.desc.array_layers;
   r->BindFlags = c.desc.bind_flags;
   r->MiscFlags = c.desc.misc_flags;
   r->SampleDesc.Count = 1;
   r->SampleDesc.Quality = 0;
   r->IsShared = TRUE;
   r->pResource = (ID3D11Resource *)tex;
   r->pShared = s;
   TR_LOG("shared: opened kmt=0x%x %ux%u fmt=%u", c.desc.kmt, c.desc.width, c.desc.height, c.desc.dxgi_format);
   return TRUE;
}

static void
wait_idle(PTRITON_DEVICE pD)
{
   D3D11_QUERY_DESC qd = {.Query = D3D11_QUERY_EVENT};
   ID3D11Query *q = NULL;
   if (!pD->pCtx1 || FAILED(ID3D11Device1_CreateQuery(pD->pDev1, &qd, &q)))
      return;
   lock_context(pD);
   ID3D11DeviceContext1_End(pD->pCtx1, (ID3D11Asynchronous *)q);
   ID3D11DeviceContext1_Flush(pD->pCtx1);
   unlock_context(pD);
   const ULONGLONG deadline = GetTickCount64() + 5000;
   for (unsigned tries = 0;; tries++) {
      BOOL done = FALSE;
      lock_context(pD);
      HRESULT hr = ID3D11DeviceContext1_GetData(pD->pCtx1, (ID3D11Asynchronous *)q, &done, sizeof(done), 0);
      unlock_context(pD);
      if (hr != S_FALSE || GetTickCount64() > deadline)
         break;
      Sleep(tries < 16 ? 0 : 1);
   }
   ID3D11Query_Release(q);
}

void
tritonSharedDestroy(PTRITON_DEVICE pD, PTRITON_RESOURCE r)
{
   struct shared *s = r->pShared;
   r->pShared = NULL;
   if (!s)
      return;
   wait_idle(pD);
   struct dxvk x;
   if (dxvk_init(pD, &x)) {
      wh_image_destroy(&x.vk, &s->img);
      dxvk_fini(&x);
   }
   HeapFree(GetProcessHeap(), 0, s);
}
