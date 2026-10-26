/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Device-level kernel context for the present path.  Presentable resources themselves are ordinary
 * exportable textures bound to virtio-gpu blob resources
 * (tritonResource.c); the KMD scans them out directly, so there is no
 * host swapchain or per-present host command.
 */

#ifndef TRITON_PRESENT_H_INCLUDED
#define TRITON_PRESENT_H_INCLUDED

#include "triton.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the kernel context on demand: deferred out of
 * tritonCreateDevice because creating it during the runtime's device
 * probe leads the runtime to reject the adapter. */
void tritonPresentEnsureKernelContext(PTRITON_DEVICE pD);

#ifdef __cplusplus
}
#endif

#endif /* TRITON_PRESENT_H_INCLUDED */
