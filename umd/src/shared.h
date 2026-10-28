/* SPDX-License-Identifier: MIT */

#ifndef LOOPHOLE_SHARED_H
#define LOOPHOLE_SHARED_H

#include <windows.h>
/* NTSTATUS, which d3dkmthk.h (through wddm_hw.h) needs. */
#include <winternl.h>
#include <dxgiformat.h>
#include <stdint.h>

#include "virtio/virtio-gpu/wddm_hw.h"

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

/* Follows the exchange in the KM allocation's private data, which dxgkrnl hands
 * every opener unchanged: all an opener needs to recreate the image. */
#define WH_SHARED_MAGIC 0x54534857 /* "WHST" */
#define WH_SHARED_VERSION 1
#define WH_MAX_VIEW_FORMATS 5

#pragma pack(push, 1)
struct wh_shared_desc {
   uint32_t magic;
   uint32_t version;
   uint32_t kmt;    /* global D3DKMT handle of the memory */
   uint32_t res_id; /* the memory's host resource */
   uint64_t memory_size;
   uint32_t memory_type;
   uint32_t flags;
   uint32_t format;
   uint32_t tiling;
   uint32_t usage;
   uint32_t width;
   uint32_t height;
   uint32_t mip_levels;
   uint32_t array_layers;
   uint32_t view_format_count;
   uint32_t view_formats[WH_MAX_VIEW_FORMATS];
   uint32_t dxgi_format;
   uint32_t bind_flags;
   uint32_t misc_flags;
};

struct wh_carrier {
   VIOGPU_CREATE_ALLOCATION_EXCHANGE_EX ex;
   struct wh_shared_desc desc;
};
#pragma pack(pop)

struct wh_format {
   DXGI_FORMAT dxgi;
   VkFormat vk;
   uint32_t virgl; /* the format the KMD shows or copies it as, 0: none */
   uint32_t view_count;
   VkFormat views[WH_MAX_VIEW_FORMATS];
};

const struct wh_format *wh_format_lookup(DXGI_FORMAT f);

struct wh_vk {
   VkPhysicalDevice phys;
   VkDevice dev;
   PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2;
   PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
   PFN_vkCreateImage CreateImage;
   PFN_vkDestroyImage DestroyImage;
   PFN_vkGetImageMemoryRequirements2 GetImageMemoryRequirements2;
   PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
   PFN_vkAllocateMemory AllocateMemory;
   PFN_vkFreeMemory FreeMemory;
   PFN_vkBindImageMemory BindImageMemory;
   PFN_vkGetMemoryWin32HandleKHR GetMemoryWin32HandleKHR;
   PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
   PFN_vkCmdCopyImage CmdCopyImage;
};

PFN_vkGetInstanceProcAddr wh_vk_gipa(void);

BOOL wh_vk_load(struct wh_vk *vk, VkInstance inst, VkPhysicalDevice phys, VkDevice dev);

struct wh_image {
   VkImage image;
   VkDeviceMemory memory;
   VkSubresourceLayout layout; /* linear images */
};

BOOL wh_desc_init(struct wh_vk *vk, struct wh_shared_desc *d, const struct wh_format *f, UINT width, UINT height,
                  UINT mip_levels, UINT array_layers, BOOL linear, UINT bind_flags, UINT misc_flags);

/* import FALSE: new memory, exported to d->kmt with its host resource in
 * d->res_id. import TRUE: over the memory d->kmt names. */
BOOL wh_image_create(struct wh_vk *vk, struct wh_shared_desc *d, BOOL import, struct wh_image *img);
void wh_image_destroy(struct wh_vk *vk, struct wh_image *img);

void wh_carrier_fill(struct wh_carrier *c, const struct wh_shared_desc *d, const struct wh_format *f,
                     const struct wh_image *img, BOOL linear, BOOL primary);

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct wh_companion;

struct wh_companion *wh_companion_create(struct ID3D12Device *d3d12, DXGI_FORMAT format, UINT width, UINT height,
                                         struct wh_carrier *c);
BOOL wh_companion_record_copy(struct ID3D12Device *d3d12, struct ID3D12GraphicsCommandList *list,
                              struct ID3D12Resource *src_resource, UINT src_state, struct wh_companion *s);
void wh_companion_destroy(struct ID3D12Device *d3d12, struct wh_companion *s);

BOOL wh_gate_arm_deferred(uint64_t *token);
BOOL wh_gate_complete(uint64_t token);

#endif
