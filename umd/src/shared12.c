/* SPDX-License-Identifier: MIT */

/* Each D3D12 back buffer gets a linear companion, named by a carrier
 * allocation, for the KMD's blts. */

#include "triton12.h"
#include "triton_log.h"
#include "tritonKm.h"

#include <d3d11.h>

#include "shared.h"

typedef struct ID3D12DXVKInteropDevice1 ID3D12DXVKInteropDevice1;
typedef struct ID3D12DXVKInteropDevice1Vtbl {
   HRESULT(STDMETHODCALLTYPE *QueryInterface)(ID3D12DXVKInteropDevice1 *, REFIID, void **);
   ULONG(STDMETHODCALLTYPE *AddRef)(ID3D12DXVKInteropDevice1 *);
   ULONG(STDMETHODCALLTYPE *Release)(ID3D12DXVKInteropDevice1 *);
   HRESULT(STDMETHODCALLTYPE *GetDXGIAdapter)(ID3D12DXVKInteropDevice1 *, REFIID, void **);
   HRESULT(STDMETHODCALLTYPE *GetInstanceExtensions)(ID3D12DXVKInteropDevice1 *, UINT *, const char **);
   HRESULT(STDMETHODCALLTYPE *GetDeviceExtensions)(ID3D12DXVKInteropDevice1 *, UINT *, const char **);
   HRESULT(STDMETHODCALLTYPE *GetDeviceFeatures)(ID3D12DXVKInteropDevice1 *, const VkPhysicalDeviceFeatures2 **);
   HRESULT(STDMETHODCALLTYPE *GetVulkanHandles)(ID3D12DXVKInteropDevice1 *, VkInstance *, VkPhysicalDevice *,
                                                VkDevice *);
   HRESULT(STDMETHODCALLTYPE *GetVulkanQueueInfo)(ID3D12DXVKInteropDevice1 *, ID3D12CommandQueue *, VkQueue *,
                                                  UINT32 *);
   void(STDMETHODCALLTYPE *GetVulkanImageLayout)(ID3D12DXVKInteropDevice1 *, ID3D12Resource *,
                                                 D3D12_RESOURCE_STATES, VkImageLayout *);
   HRESULT(STDMETHODCALLTYPE *GetVulkanResourceInfo)(ID3D12DXVKInteropDevice1 *, ID3D12Resource *, UINT64 *,
                                                     UINT64 *);
   HRESULT(STDMETHODCALLTYPE *LockCommandQueue)(ID3D12DXVKInteropDevice1 *, ID3D12CommandQueue *);
   HRESULT(STDMETHODCALLTYPE *UnlockCommandQueue)(ID3D12DXVKInteropDevice1 *, ID3D12CommandQueue *);
   HRESULT(STDMETHODCALLTYPE *GetVulkanResourceInfo1)(ID3D12DXVKInteropDevice1 *, ID3D12Resource *, UINT64 *,
                                                      UINT64 *, VkFormat *);
   HRESULT(STDMETHODCALLTYPE *CreateInteropCommandQueue)(ID3D12DXVKInteropDevice1 *,
                                                         const D3D12_COMMAND_QUEUE_DESC *, UINT32,
                                                         ID3D12CommandQueue **);
   HRESULT(STDMETHODCALLTYPE *CreateInteropCommandAllocator)(ID3D12DXVKInteropDevice1 *, D3D12_COMMAND_LIST_TYPE,
                                                             UINT32, ID3D12CommandAllocator **);
   HRESULT(STDMETHODCALLTYPE *BeginVkCommandBufferInterop)(ID3D12DXVKInteropDevice1 *, ID3D12CommandList *,
                                                           VkCommandBuffer *);
   HRESULT(STDMETHODCALLTYPE *EndVkCommandBufferInterop)(ID3D12DXVKInteropDevice1 *, ID3D12CommandList *);
} ID3D12DXVKInteropDevice1Vtbl;
struct ID3D12DXVKInteropDevice1 {
   const ID3D12DXVKInteropDevice1Vtbl *lpVtbl;
};
static const GUID IID_ID3D12DXVKInteropDevice1 = {0x902d8115, 0x59eb, 0x4406,
                                                  {0x95, 0x18, 0xfe, 0x00, 0xf9, 0x91, 0xee, 0x65}};

struct shared12 {
   struct wh_image img;
   UINT width, height;
};

struct vkd3d {
   ID3D12DXVKInteropDevice1 *interop;
   struct wh_vk vk;
};

static void
vkd3d_fini(struct vkd3d *x)
{
   if (x->interop)
      x->interop->lpVtbl->Release(x->interop);
   x->interop = NULL;
}

static BOOL
vkd3d_init(PTRITON12_DEVICE p, struct vkd3d *x)
{
   memset(x, 0, sizeof(*x));
   if (!p->pDev ||
       FAILED(ID3D12Device_QueryInterface(p->pDev, &IID_ID3D12DXVKInteropDevice1, (void **)&x->interop)))
      return FALSE;
   VkInstance inst;
   VkPhysicalDevice phys;
   VkDevice dev;
   if (FAILED(x->interop->lpVtbl->GetVulkanHandles(x->interop, &inst, &phys, &dev)) ||
       !wh_vk_load(&x->vk, inst, phys, dev)) {
      vkd3d_fini(x);
      return FALSE;
   }
   return TRUE;
}

HRESULT
tritonShared12CreateCompanion(PTRITON12_DEVICE p, PTRITON12_RESOURCE r, const D3D12_RESOURCE_DESC *desc)
{
   const struct wh_format *f = wh_format_lookup(desc->Format);
   if (!f || !f->virgl || desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc->MipLevels > 1 ||
       desc->DepthOrArraySize > 1 || desc->SampleDesc.Count > 1 || desc->Width > UINT32_MAX)
      return E_INVALIDARG;
   if (!p->pUMCallbacks || !p->pUMCallbacks->pfnAllocateCb)
      return E_FAIL;
   struct vkd3d x;
   if (!vkd3d_init(p, &x))
      return E_FAIL;

   HRESULT hr = E_FAIL;
   struct wh_shared_desc d;
   struct shared12 *s = NULL;
   if (!wh_desc_init(&x.vk, &d, f, (UINT)desc->Width, desc->Height, 1, 1, TRUE,
                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0)) {
      hr = E_INVALIDARG;
      goto out;
   }
   s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
   if (!s || !wh_image_create(&x.vk, &d, FALSE, &s->img))
      goto out;
   s->width = d.width;
   s->height = d.height;

   struct wh_carrier c;
   wh_carrier_fill(&c, &d, f, &s->img, TRUE, FALSE);
   D3D12DDI_ALLOCATION_INFO_0022 ai;
   memset(&ai, 0, sizeof(ai));
   ai.pPrivateDriverData = &c;
   ai.PrivateDriverDataSize = sizeof(c);
   D3D12DDICB_ALLOCATE_0022 cb;
   memset(&cb, 0, sizeof(cb));
   cb.hResource = r->hRTResource.handle;
   cb.NumAllocations = 1;
   cb.pAllocationInfo = &ai;
   hr = p->pUMCallbacks->pfnAllocateCb(p->hRTDevice, &cb);
   if (FAILED(hr) || !ai.hAllocation) {
      TR_LOG("12.shared: pfnAllocateCb failed 0x%08lx", (unsigned long)hr);
      hr = FAILED(hr) ? hr : E_FAIL;
      wh_image_destroy(&x.vk, &s->img);
      goto out;
   }
   /* A residency-only placeholder gives way to the companion. */
   triton12DeallocateKM(p, &r->hKMAllocation);
   r->hKMAllocation = ai.hAllocation;
   tritonKmRequestResidency(&p->Km, ai.hAllocation);
   TR_LOG("12.shared: companion %ux%u fmt=%d kmt=0x%x res_id=%u alloc=0x%x pitch=%llu", d.width, d.height,
          (int)desc->Format, d.kmt, d.res_id, r->hKMAllocation, (unsigned long long)s->img.layout.rowPitch);
   r->pShared = s;
   s = NULL;
   hr = S_OK;
out:
   HeapFree(GetProcessHeap(), 0, s);
   vkd3d_fini(&x);
   return hr;
}

BOOL
tritonShared12RecordCopy(PTRITON12_DEVICE p, ID3D12GraphicsCommandList *list, PTRITON12_RESOURCE r)
{
   struct shared12 *s = r->pShared;
   struct vkd3d x;
   if (!s || !r->pResource || !vkd3d_init(p, &x))
      return FALSE;
   BOOL ok = FALSE;
   UINT64 handle = 0, offset = 0;
   VkFormat format;
   VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   if (FAILED(x.interop->lpVtbl->GetVulkanResourceInfo1(x.interop, r->pResource, &handle, &offset, &format)) ||
       !handle) {
      TR_LOG("12.shared: no Vulkan image behind the back buffer");
      goto out;
   }
   x.interop->lpVtbl->GetVulkanImageLayout(x.interop, r->pResource, D3D12_RESOURCE_STATE_PRESENT, &layout);
   if (FAILED(x.interop->lpVtbl->BeginVkCommandBufferInterop(x.interop, (ID3D12CommandList *)list, &cmd)))
      goto out;

   /* A non-dispatchable handle: a pointer on 64-bit builds, a uint64_t on
    * 32-bit ones, 64 bits either way. */
   VkImage src;
   memcpy(&src, &handle, sizeof(src));
   const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageMemoryBarrier before[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = layout,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = src,
         .subresourceRange = range,
      },
      {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = 0,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = s->img.image,
         .subresourceRange = range,
      },
   };
   x.vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                           NULL, 2, before);
   const VkImageCopy region = {
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .extent = {s->width, s->height, 1},
   };
   x.vk.CmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->img.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
   VkImageMemoryBarrier after[2] = {before[0], before[1]};
   after[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   after[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
   after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   after[0].newLayout = layout;
   after[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   after[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
   after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   after[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
   x.vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                           NULL, 2, after);
   ok = SUCCEEDED(x.interop->lpVtbl->EndVkCommandBufferInterop(x.interop, (ID3D12CommandList *)list));
out:
   vkd3d_fini(&x);
   return ok;
}

/* The runtime destroys swapchain buffers only after the queue that presents
 * them is idle, and the companion's only GPU use is that queue's copy. */
void
tritonShared12Destroy(PTRITON12_DEVICE p, PTRITON12_RESOURCE r)
{
   struct shared12 *s = r->pShared;
   r->pShared = NULL;
   if (!s)
      return;
   struct vkd3d x;
   if (vkd3d_init(p, &x)) {
      wh_image_destroy(&x.vk, &s->img);
      vkd3d_fini(&x);
   }
   HeapFree(GetProcessHeap(), 0, s);
}
