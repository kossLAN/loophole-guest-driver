#pragma once
#include "helper.h"
#include "viogpum.h"

// Completion counters for the fenced ring submits of one virtio context,
// on a page shared read-only with the process that owns the context.
//
// The owning VioGpuContext holds one reference and every ring submit in
// flight holds one more. The user mapping is removed at context teardown
// (PASSIVE_LEVEL, owner's address space). The last Release frees the page
// and may run in the completion DPC.
class VioGpuRingFences
{
  public:
    static VioGpuRingFences *Create();

    // PASSIVE_LEVEL.
    NTSTATUS MapIntoCurrentProcess(PVOID *pUserVa);
    // PASSIVE_LEVEL.
    void UnmapUser();

    void AddRef();
    // <= DISPATCH_LEVEL.
    void Release();

    // CtrlQueue completion callback. ctx is a VioGpuRingFences reference, which it releases.
    static void SubmitCompleteCb(void *ctx, void *cmd, void *resp);

  private:
    VioGpuRingFences() = default;

    VIOGPU_RING_FENCE_PAGE *m_Page = NULL;
    PMDL m_Mdl = NULL;
    PVOID m_UserVa = NULL;
    PEPROCESS m_Process = NULL;
    FAST_MUTEX m_MapLock;
    volatile LONG m_Refs = 1;
};
