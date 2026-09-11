// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __INTERNAL_XNVME_BE_UPCIE_CUDA_H
#define __INTERNAL_XNVME_BE_UPCIE_CUDA_H
#ifdef XNVME_BE_UPCIE_CUDA_ENABLED
#include <xnvme_be.h>

#include <xnvme_be_upcie.h>
#include <upcie/upcie_cuda.h>

struct xnvme_cuda_queue {
	struct nvme_qpair_cuda qpair;
};

/**
 * State used across multiple instances of controllers/namespaces
 */
struct xnvme_be_upcie_cuda_rte {
	CUdevice cu_dev;
	CUcontext cu_ctx; ///< The device's primary context, retained for the RTE's lifetime
	struct cudamem_config cuda_config;
	struct cudamem_heap cuda_heap;
	struct dmamem dmem; ///< Shared translation; unused where each controller needs its own
	int is_initialized;
};

extern struct xnvme_be_upcie_cuda_rte g_upcie_cuda_rte;

extern struct xnvme_be_mem g_xnvme_be_upcie_cuda_mem;
extern struct xnvme_be_dev g_xnvme_be_upcie_cuda_dev;

/**
 * Make the runtime's CUDA context current on the calling thread
 *
 * The driver API resolves against the calling thread's current context, and a
 * thread that did not bring the runtime up has none. Call this before any
 * cu*() in a path a caller can reach from a thread of its own.
 *
 * @return 0 on success, negative errno on failure.
 */
int
xnvme_be_upcie_cuda_ctx_bind(void);

#endif /* XNVME_BE_UPCIE_CUDA_ENABLED */
#endif /* __INTERNAL_XNVME_BE_UPCIE_CUDA_H */
