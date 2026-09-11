// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include <libxnvme.h>
#include <xnvme_be.h>
#include <xnvme_be_nosys.h>
#ifdef XNVME_BE_UPCIE_CUDA_ENABLED
#include <stdlib.h>
#include <pthread.h>
#include <xnvme_dev.h>
#include <xnvme_be_upcie_cuda.h>

/** Serializes bring-up/tear-down of the shared CUDA runtime (g_upcie_cuda_rte) */
static pthread_mutex_t g_cuda_rte_lock = PTHREAD_MUTEX_INITIALIZER;

/** CUDA RTE references held by open devices; only read or written under g_cuda_rte_lock */
static int g_cuda_ctrlr_count;

static int
_cuda_rte_init_locked(size_t heap_size, uint32_t gpu_id);

/** Drop a reference taken by _cuda_rte_get(); the last one tears the runtime down */
static void
_cuda_rte_put(void)
{
	pthread_mutex_lock(&g_cuda_rte_lock);

	if ((--g_cuda_ctrlr_count == 0) && g_upcie_cuda_rte.is_initialized) {
		/* The cuMemFree inside the heap teardown needs the context too, and the
		 * thread closing the last device need not be the one that opened it. */
		xnvme_be_upcie_cuda_ctx_bind();

		dmamem_destroy(&g_upcie_cuda_rte.dmem);
		cudamem_heap_term(&g_upcie_cuda_rte.cuda_heap);
		cuDevicePrimaryCtxRelease(g_upcie_cuda_rte.cu_dev);

		g_upcie_cuda_rte.cu_ctx = NULL;
		g_upcie_cuda_rte.is_initialized = 0;
	}

	pthread_mutex_unlock(&g_cuda_rte_lock);
}

/** Bring the shared CUDA runtime up, or join one already up, and take a reference */
static int
_cuda_rte_get(size_t heap_size, uint32_t gpu_id)
{
	int err;

	pthread_mutex_lock(&g_cuda_rte_lock);

	if (g_upcie_cuda_rte.is_initialized) {
		err = 0;
	} else {
		err = _cuda_rte_init_locked(heap_size, gpu_id);
	}

	if (!err) {
		g_cuda_ctrlr_count++;
	}

	pthread_mutex_unlock(&g_cuda_rte_lock);

	return err;
}

int
xnvme_be_upcie_cuda_ctx_bind(void)
{
	CUcontext cur = NULL;

	if ((cuCtxGetCurrent(&cur) == CUDA_SUCCESS) && (cur == g_upcie_cuda_rte.cu_ctx)) {
		return 0;
	}

	if (cuCtxSetCurrent(g_upcie_cuda_rte.cu_ctx) != CUDA_SUCCESS) {
		XNVME_DEBUG("FAILED: cuCtxSetCurrent()");
		return -EIO;
	}

	return 0;
}

static int
_cuda_rte_init_locked(size_t heap_size, uint32_t gpu_id)
{
	CUdevice cu_dev;
	int err;

	if (!heap_size) {
		heap_size = XNVME_BE_UPCIE_DEFAULT_HEAP_SIZE;
	}

	err = cuInit(0);
	if (err) {
		XNVME_DEBUG("FAILED: cuInit(); err(%d)", err);
		return -ENODEV;
	}

	err = cuDeviceGet(&cu_dev, gpu_id);
	if (err) {
		XNVME_DEBUG("FAILED: cuDeviceGet(); err(%d)", err);
		return -ENODEV;
	}

	/* The primary context is the one the CUDA runtime API binds, so making it
	 * current on a caller's thread does not displace the context that caller
	 * already works in. A private cuCtxCreate() context would. */
	err = cuDevicePrimaryCtxRetain(&g_upcie_cuda_rte.cu_ctx, cu_dev);
	if (err) {
		XNVME_DEBUG("FAILED: cuDevicePrimaryCtxRetain(); err(%d)", err);
		return -EIO;
	}
	g_upcie_cuda_rte.cu_dev = cu_dev;

	/* Retaining does not make it current; the allocations below need it. */
	err = xnvme_be_upcie_cuda_ctx_bind();
	if (err) {
		cuDevicePrimaryCtxRelease(cu_dev);
		return err;
	}

	err = cudamem_config_init(&g_upcie_cuda_rte.cuda_config, 0);
	if (err) {
		XNVME_DEBUG("FAILED: cudamem_config_init(); err(%d)", err);
		cuDevicePrimaryCtxRelease(cu_dev);
		return err;
	}

	// align to the dma-buf page granularity used by the cudamem heap
	heap_size = ((heap_size + g_upcie_cuda_rte.cuda_config.device_pagesize - 1) /
		     g_upcie_cuda_rte.cuda_config.device_pagesize) *
		    g_upcie_cuda_rte.cuda_config.device_pagesize;

	err = cudamem_heap_init(&g_upcie_cuda_rte.cuda_heap, heap_size,
				&g_upcie_cuda_rte.cuda_config);
	if (err) {
		XNVME_DEBUG("FAILED: cudamem_heap_init(); err(%d)", err);
		cuDevicePrimaryCtxRelease(cu_dev);
		return -ENOMEM;
	}

	/* Physical addresses read the same from every controller, so one table
	 * serves them all; per-domain IOVAs do not. */
	if (!xnvme_be_upcie_gpu_map_required()) {
		err = dmamem_from_cuda_registry(&g_upcie_cuda_rte.dmem,
						&g_upcie_cuda_rte.cuda_heap,
						xnvme_be_upcie_va_bits());
		if (err) {
			XNVME_DEBUG("FAILED: dmamem_from_cuda_registry(); err(%d)", err);
			cudamem_heap_term(&g_upcie_cuda_rte.cuda_heap);
			cuDevicePrimaryCtxRelease(cu_dev);
			return err;
		}
	}

	g_upcie_cuda_rte.is_initialized = 1;

	return 0;
}

/** Heap bytes to map, rounded as the registry rounds a registration */
static uint64_t
_cuda_slice_span(const struct cudamem_heap *heap)
{
	const uint64_t gran = DMAMEM_CUDA_REGISTRY_GRANULARITY;

	return ((heap->size + gran - 1) & ~(gran - 1)) + gran;
}

/** Point the device at the runtime's table, or build it one of its own */
static int
_cuda_dev_dmem_init(struct xnvme_dev *dev)
{
	struct xnvme_be_upcie_state *state = (void *)dev->be.state;
	struct xnvme_be_upcie_gpu_dmem *gpu;
	int err;

	if (!xnvme_be_upcie_gpu_map_required()) {
		state->dmem = &g_upcie_cuda_rte.dmem;
		return 0;
	}

	gpu = calloc(1, sizeof(*gpu));
	if (!gpu) {
		return -ENOMEM;
	}

	err = xnvme_be_upcie_gpu_map_open(&gpu->map, dev->ident.uri,
					  _cuda_slice_span(&g_upcie_cuda_rte.cuda_heap));
	if (err) {
		XNVME_DEBUG("FAILED: xnvme_be_upcie_gpu_map_open(%s); err(%d)", dev->ident.uri,
			    err);
		free(gpu);
		return err;
	}

	err = dmamem_from_cuda_iommu_map_pa(&gpu->dmem, &g_upcie_cuda_rte.cuda_heap,
					    xnvme_be_upcie_va_bits(), &gpu->map.imp);
	if (err) {
		XNVME_DEBUG("FAILED: dmamem_from_cuda_iommu_map_pa(); err(%d)", err);
		xnvme_be_upcie_gpu_map_close(&gpu->map);
		free(gpu);
		return err;
	}

	state->gpu = gpu;
	state->dmem = &gpu->dmem;

	return 0;
}

static void
_cuda_dev_dmem_term(struct xnvme_dev *dev)
{
	struct xnvme_be_upcie_state *state = (void *)dev->be.state;

	state->dmem = NULL;

	if (!state->gpu) {
		return;
	}

	/* Unmap before ctrlr_term detaches and replaces the domain. */
	dmamem_destroy(&state->gpu->dmem);
	xnvme_be_upcie_gpu_map_close(&state->gpu->map);

	free(state->gpu);
	state->gpu = NULL;
}

/**
 * Open a uPCIe CUDA device handle.
 *
 * Memory layout
 * -------------
 * This backend uses a hybrid memory model for PCIe P2P DMA:
 *
 *  - NVMe control structures (SQ, CQ, PRP lists) are allocated from the host
 *    hugepage heap (g_upcie_rte).  The CPU writes these structures and the
 *    NVMe controller reads them; host hugepages are required because the
 *    controller cannot DMA-read GPU DRAM through BAR1 for the control path.
 *
 *  - Data buffers (xnvme_buf_alloc) are allocated from the CUDA device heap
 *    (g_upcie_cuda_rte).  The NVMe controller accesses these directly via
 *    PCIe P2P DMA, bypassing host DRAM entirely.
 *
 * Consequently, both the host hugepage runtime (256 MiB) and the CUDA heap
 * (1 GiB) are initialized when the first upcie-cuda device is opened.
 */
static int
xnvme_be_upcie_cuda_dev_open(struct xnvme_dev *dev)
{
	int err;

	err = xnvme_be_upcie_dev_open(dev);
	if (err) {
		return err;
	}

	err = _cuda_rte_get(dev->opts.device_heap_size, dev->opts.gpu_id);
	if (err) {
		XNVME_DEBUG("FAILED: _cuda_rte_get(); err(%d)", err);
		return err;
	}

	/* Everything below, and every later cu*() this thread makes through the
	 * backend, resolves against the context bound here. */
	err = xnvme_be_upcie_cuda_ctx_bind();
	if (err) {
		_cuda_rte_put();
		return err;
	}

	/* Data buffers live in device memory for this backend; the control path
	 * (queues, PRP lists) stays on the host heap set by the base dev_open. */
	err = _cuda_dev_dmem_init(dev);
	if (err) {
		XNVME_DEBUG("FAILED: _cuda_dev_dmem_init(); err(%d)", err);
		_cuda_rte_put();
		return err;
	}

	return 0;
}

static void
xnvme_be_upcie_cuda_dev_close(struct xnvme_dev *dev)
{
	_cuda_dev_dmem_term(dev);

	_cuda_rte_put();
	xnvme_be_upcie_dev_close(dev);
}

#endif

struct xnvme_be_dev g_xnvme_be_upcie_cuda_dev = {
#ifdef XNVME_BE_UPCIE_CUDA_ENABLED
	.dev_open = xnvme_be_upcie_cuda_dev_open,
	.dev_close = xnvme_be_upcie_cuda_dev_close,
	.id = "upcie-cuda",
	.ctrlr_init = xnvme_be_upcie_ctrlr_init,
	.ctrlr_term = xnvme_be_upcie_ctrlr_term,
#else
	.dev_open = xnvme_be_nosys_dev_open,
	.dev_close = xnvme_be_nosys_dev_close,
#endif
};
