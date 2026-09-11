// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __INTERNAL_XNVME_BE_UPCIE_HIP_H
#define __INTERNAL_XNVME_BE_UPCIE_HIP_H
#ifdef XNVME_BE_UPCIE_HIP_ENABLED
#include <xnvme_be.h>

#include <xnvme_be_upcie.h>
#include <upcie/upcie_hip.h>

/**
 * State used across multiple instances of controllers/namespaces
 */
struct xnvme_be_upcie_hip_rte {
	int gpu_id; ///< The device the heap lives on, as hipSetDevice() takes it
	struct hipmem_config hip_config;
	struct hipmem_heap hip_heap;
	struct dmamem dmem; ///< Shared translation; unused where each controller needs its own
	int is_initialized;
};

extern struct xnvme_be_upcie_hip_rte g_upcie_hip_rte;

extern struct xnvme_be_mem g_xnvme_be_upcie_hip_mem;
extern struct xnvme_be_dev g_xnvme_be_upcie_hip_dev;

/**
 * Make the runtime's GPU current on the calling thread
 *
 * HIP resolves allocations against the calling thread's current device, and a
 * thread that did not bring the runtime up is still on device 0. Call this
 * before any hip*() in a path a caller can reach from a thread of its own.
 *
 * @return 0 on success, negative errno on failure.
 */
int
xnvme_be_upcie_hip_dev_bind(void);

#endif /* XNVME_BE_UPCIE_HIP_ENABLED */
#endif /* __INTERNAL_XNVME_BE_UPCIE_HIP_H */
