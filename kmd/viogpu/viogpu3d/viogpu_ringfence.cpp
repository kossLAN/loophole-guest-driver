// ntifs.h must precede the ntddk.h that helper.h includes.
#include <ntifs.h>
#include "viogpu_ringfence.h"
#include "baseobj.h"
#include "viogpu.h"

#pragma code_seg(push)
#pragma code_seg()

VioGpuRingFences *VioGpuRingFences::Create()
{
    VioGpuRingFences *f = new (NonPagedPoolNx) VioGpuRingFences();
    if (f == NULL)
    {
        return NULL;
    }
    ExInitializeFastMutex(&f->m_MapLock);
    static_assert(sizeof(VIOGPU_RING_FENCE_PAGE) <= PAGE_SIZE, "ring fence page too large");
    f->m_Page = (VIOGPU_RING_FENCE_PAGE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, VIOGPUTAG);
    if (f->m_Page != NULL)
    {
        f->m_Mdl = IoAllocateMdl(f->m_Page, PAGE_SIZE, FALSE, FALSE, NULL);
    }
    if (f->m_Mdl == NULL)
    {
        if (f->m_Page != NULL)
        {
            ExFreePoolWithTag(f->m_Page, VIOGPUTAG);
        }
        delete f;
        return NULL;
    }
    MmBuildMdlForNonPagedPool(f->m_Mdl);
    return f;
}

void VioGpuRingFences::AddRef()
{
    InterlockedIncrement(&m_Refs);
}

void VioGpuRingFences::Release()
{
    if (InterlockedDecrement(&m_Refs) != 0)
    {
        return;
    }
    NT_ASSERT(m_UserVa == NULL);
    IoFreeMdl(m_Mdl);
    ExFreePoolWithTag(m_Page, VIOGPUTAG);
    delete this;
}

void VioGpuRingFences::SubmitCompleteCb(void *ctx, void *cmd, void *resp)
{
    UNREFERENCED_PARAMETER(resp);
    VioGpuRingFences *f = reinterpret_cast<VioGpuRingFences *>(ctx);
    PGPU_CTRL_HDR hdr = reinterpret_cast<PGPU_CTRL_HDR>(cmd);
    // An error response from the host also retires the fence.
    if (hdr->ring_idx < VIOGPU_MAX_RINGS)
    {
        InterlockedIncrement64(reinterpret_cast<volatile LONG64 *>(&f->m_Page->Completed[hdr->ring_idx]));
    }
    f->Release();
}

#pragma code_seg(pop)

PAGED_CODE_SEG_BEGIN

NTSTATUS VioGpuRingFences::MapIntoCurrentProcess(PVOID *pUserVa)
{
    PAGED_CODE();
    PEPROCESS self = PsGetCurrentProcess();
    NTSTATUS status = STATUS_SUCCESS;

    ExAcquireFastMutex(&m_MapLock);
    if (m_UserVa != NULL)
    {
        status = m_Process == self ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
    }
    else
    {
        PVOID va = NULL;
        __try
        {
            va = MmMapLockedPagesSpecifyCache(m_Mdl, UserMode, MmCached, NULL, FALSE,
                                              (MM_PAGE_PRIORITY)(NormalPagePriority | MdlMappingNoExecute));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            status = GetExceptionCode();
        }
        if (NT_SUCCESS(status) && va == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        if (NT_SUCCESS(status))
        {
            ObReferenceObject(self);
            m_Process = self;
            m_UserVa = va;
        }
    }
    if (NT_SUCCESS(status))
    {
        *pUserVa = m_UserVa;
    }
    ExReleaseFastMutex(&m_MapLock);
    return status;
}

void VioGpuRingFences::UnmapUser()
{
    PAGED_CODE();
    ExAcquireFastMutex(&m_MapLock);
    if (m_UserVa != NULL)
    {
        if (PsGetCurrentProcess() == m_Process)
        {
            MmUnmapLockedPages(m_UserVa, m_Mdl);
        }
        else
        {
            KAPC_STATE apc;
            KeStackAttachProcess(m_Process, &apc);
            MmUnmapLockedPages(m_UserVa, m_Mdl);
            KeUnstackDetachProcess(&apc);
        }
        ObDereferenceObject(m_Process);
        m_UserVa = NULL;
        m_Process = NULL;
    }
    ExReleaseFastMutex(&m_MapLock);
}

PAGED_CODE_SEG_END
