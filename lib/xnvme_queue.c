// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <errno.h>
#include <libxnvme.h>
#include <xnvme_be.h>
#include <xnvme_cmd.h>
#include <xnvme_dev.h>
#include <xnvme_queue.h>

static int
queue_bind_mem_policy(struct xnvme_queue *queue, const struct xnvme_queue_attr *attr)
{
	const struct xnvme_be_config *cfg = NULL;
	const struct xnvme_be_mem *mem = NULL;
	const char *mem_name = NULL;

	/*
	 * Queue memory policy falls back in two steps:
	 *
	 * 1) explicit queue attributes
	 * 2) the currently bound device memory backend id
	 *
	 * This keeps queue-init consistent with device defaults while still
	 * allowing per-queue memory overrides.
	 */
	mem_name = attr ? attr->mem : NULL;
	if (!mem_name || !strcmp(mem_name, queue->base.dev->be.mem.id)) {
		queue->base.mem = &queue->base.dev->be.mem;
		return 0;
	}

	cfg = xnvme_be_config_from_be(&queue->base.dev->be);
	mem = xnvme_be_config_get_mem(cfg, mem_name);
	if (!mem) {
		XNVME_DEBUG("FAILED: no mem backend matching '%s'", mem_name);
		return -ENOSYS;
	}

	queue->base.mem = mem;
	return 0;
}

const struct xnvme_be_mem *
xnvme_queue_get_mem_ops(const struct xnvme_queue *queue)
{
	if (!queue || !queue->base.dev) {
		return NULL;
	}

	return queue->base.mem ? queue->base.mem : &queue->base.dev->be.mem;
}

int
xnvme_queue_term(struct xnvme_queue *queue)
{
	int err;

	if (!queue) {
		XNVME_DEBUG("FAILED: !queue");
		return -EINVAL;
	}

	err = queue->base.dev ? queue->base.dev->be.async.term(queue) : 0;
	if (err) {
		XNVME_DEBUG("FAILED: backend queue-termination failed with err: %d", err);
	}

	free(queue);

	return err;
}

static void
callback_noop(struct xnvme_cmd_ctx *XNVME_UNUSED(ctx), void *XNVME_UNUSED(cb_arg))
{
	return;
}

int
xnvme_queue_init(struct xnvme_dev *dev, uint16_t capacity, int opts, struct xnvme_queue **queue)
{
	const struct xnvme_queue_attr attr = {
		.opts = opts,
	};

	return xnvme_queue_init_with_attr(dev, capacity, &attr, queue);
}

int
xnvme_queue_init_with_attr(struct xnvme_dev *dev, uint16_t capacity,
			   const struct xnvme_queue_attr *attr,
			   struct xnvme_queue **queue)
{
	size_t queue_nbytes;
	int err;

	if (!dev) {
		XNVME_DEBUG("FAILED: !dev");
		return -EINVAL;
	}
	if (!(xnvme_is_pow2(capacity) && (capacity < 4096))) {
		XNVME_DEBUG("EINVAL: capacity: %u", capacity);
		return -EINVAL;
	}

	queue_nbytes = sizeof(**queue) + (capacity + 1) * sizeof(*((*queue)->pool_storage));

	*queue = calloc(1, queue_nbytes);
	if (!*queue) {
		XNVME_DEBUG("FAILED: calloc(queue), err: %s", strerror(errno));
		return -errno;
	}
	(*queue)->base.capacity = capacity;
	(*queue)->base.dev = dev;

	SLIST_INIT(&(*queue)->base.pool);

	err = queue_bind_mem_policy(*queue, attr);
	if (err) {
		free(*queue);
		*queue = NULL;
		return err;
	}

	for (uint32_t i = 0; i <= (*queue)->base.capacity; ++i) {
		(*queue)->pool_storage[i].dev = dev;
		(*queue)->pool_storage[i].async.queue = *queue;
		(*queue)->pool_storage[i].async.cb = callback_noop;
		(*queue)->pool_storage[i].async.cb_arg = NULL;
		(*queue)->pool_storage[i].opts = XNVME_CMD_ASYNC;
		(*queue)->pool_storage[i].id = i;

		SLIST_INSERT_HEAD(&(*queue)->base.pool, &((*queue)->pool_storage[i]), link);
	}

	err = dev->be.async.init(*queue, attr ? attr->opts : 0);
	if (err) {
		XNVME_DEBUG("FAILED: backend-queue initialization with err: %d", err);
		free(*queue);
		*queue = NULL;
		return err;
	}

	return 0;
}

int
xnvme_queue_set_cb(struct xnvme_queue *queue, xnvme_queue_cb cb, void *cb_arg)
{
	for (uint32_t i = 0; i <= queue->base.capacity; ++i) {
		queue->pool_storage[i].async.cb = cb;
		queue->pool_storage[i].async.cb_arg = cb_arg;
	}

	return 0;
}

int
xnvme_queue_poke(struct xnvme_queue *queue, uint32_t max)
{
	if (!queue->base.outstanding) {
		return 0;
	}

	return queue->base.dev->be.async.poke(queue, max);
}

int
xnvme_queue_wait(struct xnvme_queue *queue)
{
	printf("ERR: USING DEPRECATED FUNCTION: xnvme_queue_wait(*queue) use "
	       "xnvme_queue_drain(*queue) instead\n");
	return xnvme_queue_drain(queue);
}

int
xnvme_queue_drain(struct xnvme_queue *queue)
{
	int acc = 0;

	while (queue->base.outstanding) {
		int err;

		err = xnvme_queue_poke(queue, 0);
		if (err < 0) {
			XNVME_DEBUG("FAILED: xnvme_queue_poke(), err: %d", err);
			return err;
		}

		acc += err;
	}

	return acc;
}

uint32_t
xnvme_queue_get_capacity(struct xnvme_queue *queue)
{
	return queue->base.capacity;
}

uint32_t
xnvme_queue_get_outstanding(struct xnvme_queue *queue)
{
	return queue->base.outstanding;
}

struct xnvme_cmd_ctx *
xnvme_queue_get_cmd_ctx(struct xnvme_queue *queue)
{
	struct xnvme_cmd_ctx *ctx = (struct xnvme_cmd_ctx *)SLIST_FIRST(&queue->base.pool);

	if (!ctx) {
		errno = ENOMEM;
		return ctx;
	}

	SLIST_REMOVE_HEAD(&queue->base.pool, link);

	return ctx;
}

int
xnvme_queue_put_cmd_ctx(struct xnvme_queue *queue, struct xnvme_cmd_ctx *ctx)
{
	SLIST_INSERT_HEAD(&queue->base.pool, (struct xnvme_cmd_ctx_entry *)ctx, link);

	return 0;
}

int
xnvme_queue_get_completion_fd(struct xnvme_queue *queue)
{
	return queue->base.dev->be.async.get_completion_fd(queue);
}

const char *
xnvme_queue_get_mem_id(const struct xnvme_queue *queue)
{
	const struct xnvme_be_mem *mem = xnvme_queue_get_mem_ops(queue);

	return mem ? mem->id : NULL;
}

void *
xnvme_queue_buf_phys_alloc(const struct xnvme_queue *queue, size_t nbytes, uint64_t *phys)
{
	const struct xnvme_be_mem *mem = xnvme_queue_get_mem_ops(queue);

	if (!mem) {
		errno = ENOSYS;
		return NULL;
	}

	return mem->buf_alloc(queue->base.dev, nbytes, phys);
}

void *
xnvme_queue_buf_phys_realloc(const struct xnvme_queue *queue, void *buf, size_t nbytes,
			     uint64_t *phys)
{
	const struct xnvme_be_mem *mem = xnvme_queue_get_mem_ops(queue);

	if (!mem) {
		errno = ENOSYS;
		return NULL;
	}

	return mem->buf_realloc(queue->base.dev, buf, nbytes, phys);
}

void
xnvme_queue_buf_phys_free(const struct xnvme_queue *queue, void *buf)
{
	const struct xnvme_be_mem *mem = xnvme_queue_get_mem_ops(queue);

	if (!mem) {
		return;
	}

	mem->buf_free(queue->base.dev, buf);
}

int
xnvme_queue_buf_vtophys(const struct xnvme_queue *queue, void *buf, uint64_t *phys)
{
	const struct xnvme_be_mem *mem = xnvme_queue_get_mem_ops(queue);

	return mem ? mem->buf_vtophys(queue->base.dev, buf, phys) : -ENOSYS;
}

void *
xnvme_queue_buf_alloc(const struct xnvme_queue *queue, size_t nbytes)
{
	return xnvme_queue_buf_phys_alloc(queue, nbytes, NULL);
}

void *
xnvme_queue_buf_realloc(const struct xnvme_queue *queue, void *buf, size_t nbytes)
{
	return xnvme_queue_buf_phys_realloc(queue, buf, nbytes, NULL);
}

void
xnvme_queue_buf_free(const struct xnvme_queue *queue, void *buf)
{
	xnvme_queue_buf_phys_free(queue, buf);
}
