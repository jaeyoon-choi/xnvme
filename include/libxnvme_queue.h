/**
 * SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * @headerfile libxnvme_adm.h
 */

/**
 * Opaque Command Queue Handle to be initialized by xnvme_queue_init()
 *
 * @see xnvme_queue_init
 * @see xnvme_queue_term
 *
 * @struct xnvme_queue
 */
struct xnvme_queue;

/**
 * Queue initialization attributes
 *
 * The queue owns a single memory backend. When `mem` is omitted, the queue
 * uses the device-default memory backend selected when the device was opened.
 * The asynchronous I/O path remains the device-default path.
 *
 * @struct xnvme_queue_attr
 */
struct xnvme_queue_attr {
	int opts;        ///< Queue options, see ::xnvme_queue_opts
	const char *mem; ///< Optional memory backend override
};

/**
 * Command Queue initialization options
 *
 * @enum xnvme_queue_opts
 */
enum xnvme_queue_opts {
	XNVME_QUEUE_IOPOLL = 0x1,      ///< XNVME_QUEUE_IOPOLL: queue. is polled for completions
	XNVME_QUEUE_SQPOLL = 0x1 << 1, ///< XNVME_QUEUE_SQPOLL: queue. is polled for submissions
};

/**
 * Allocate a Command Queue for asynchronous command submission and completion
 *
 * The queue inherits the device-default asynchronous I/O path and memory
 * backend selected for the given device.
 *
 * @param dev Device handle (::xnvme_dev) obtained with xnvme_dev_open()
 * @param capacity Maximum number of outstanding commands on the initialized queue, note that it
 * must be a power of 2 within the range [1,4096]
 * @param opts Queue options
 * @param queue Pointer-pointer to the ::xnvme_queue to initialize
 *
 * @return On success, 0 is returned. On error, negative `errno` is returned.
 */
int
xnvme_queue_init(struct xnvme_dev *dev, uint16_t capacity, int opts, struct xnvme_queue **queue);

/**
 * Allocate a Command Queue with explicit queue policy
 *
 * The queue still operates on the given device, but the queue attributes may
 * override the device-default memory backend selected for the device when it
 * was opened. The asynchronous I/O path remains the device-default path.
 *
 * @param dev Device handle (::xnvme_dev) obtained with xnvme_dev_open()
 * @param capacity Maximum number of outstanding commands on the initialized queue, note that it
 * must be a power of 2 within the range [1,4096]
 * @param attr Queue attributes; when NULL, device defaults are used
 * @param queue Pointer-pointer to the ::xnvme_queue to initialize
 *
 * @return On success, 0 is returned. On error, negative `errno` is returned.
 */
int
xnvme_queue_init_with_attr(struct xnvme_dev *dev, uint16_t capacity,
			   const struct xnvme_queue_attr *attr,
			   struct xnvme_queue **queue);

/**
 * Get the capacity of the given ::xnvme_queue
 *
 * @param queue Pointer to the ::xnvme_queue to query for capacity
 *
 * @return On success, capacity of given ::xnvme_queue text is returned. On error, 0 is returned
 * e.g. errors are silent
 */
uint32_t
xnvme_queue_get_capacity(struct xnvme_queue *queue);

/**
 * Get the number of outstanding commands on the given ::xnvme_queue
 *
 * @param queue Pointer to the ::xnvme_queue to query for outstanding commands
 *
 * @return On success, number of outstanding commands are returned. On error, 0 is returned e.g.
 * errors are silent
 */
uint32_t
xnvme_queue_get_outstanding(struct xnvme_queue *queue);

/**
 * Tear down the given ::xnvme_queue
 *
 * @param queue Pointer to the ::xnvme_queue to tear down
 *
 * @return On success, 0 is returned. On error, negative `errno` is returned.
 */
int
xnvme_queue_term(struct xnvme_queue *queue);

/**
 * Process completions of commands on the given ::xnvme_queue
 *
 * Set process 'max' to limit number of completions, 0 means no max.
 *
 * @param queue Pointer to the ::xnvme_queue to poke for completions
 * @param max The max number of completions to complete
 *
 * @return On success, number of completions processed, may be 0. On error, negative `errno` is
 * returned.
 */
int
xnvme_queue_poke(struct xnvme_queue *queue, uint32_t max);

/**
 * Process outstanding commands on the given ::xnvme_queue until it is empty
 *
 * @param queue Pointer to the ::xnvme_queue to wait/process commands on
 *
 * @return On success, number of commands processed, may be 0. On error, negative `errno` is
 * returned.
 */
int
xnvme_queue_drain(struct xnvme_queue *queue);

/**
 * DEPRECATED: expect that this function will be removed in an upcoming release
 *
 * @param queue Pointer to the ::xnvme_queue to wait/process commands on
 *
 * @return On success, number of commands processed, may be 0. On error, negative `errno` is
 * returned.
 */
int
xnvme_queue_wait(struct xnvme_queue *queue);

/**
 * Retrieve a command-context from the given queue for async. command execution with the queue
 *
 * @note The command-context is managed by the queue, thus, return it to the queue via
 * ::xnvme_queue_put_cmd_ctx
 *
 * @note This is not thread-safe
 *
 * @param queue Pointer to the ::xnvme_queue to retrieve a command-context for
 *
 * @return On success, a command-context is returned. On error, NULL is returned and `errno` is set
 * to indicate the error.
 */
struct xnvme_cmd_ctx *
xnvme_queue_get_cmd_ctx(struct xnvme_queue *queue);

/**
 * Hand back a command-context previously retrieve using ::xnvme_queue_get_cmd_ctx
 *
 * @note This function is not thread-safe
 *
 * @param queue Pointer to the ::xnvme_queue to hand back the command-context to
 * @param ctx Pointer to command context (::xnvme_cmd_ctx)
 *
 * @return On success, 0 is returned. On error, negative `errno` is returned.
 */
int
xnvme_queue_put_cmd_ctx(struct xnvme_queue *queue, struct xnvme_cmd_ctx *ctx);

/**
 * Signature of function used with Command Queues for async. callback upon command-completion
 */
typedef void (*xnvme_queue_cb)(struct xnvme_cmd_ctx *ctx, void *opaque);

/**
 * Assign a callback-function and argument to be used with the ::xnvme_cmd_ctx of the queue
 *
 * @param queue The ::xnvme_queue to assign default callback function for
 * @param cb The callback function to use
 * @param cb_arg The callback argument to use
 *
 * @return On success, 0 is returned. On error, negative `errno` is returned.
 */
int
xnvme_queue_set_cb(struct xnvme_queue *queue, xnvme_queue_cb cb, void *cb_arg);

/**
 * Get the completion event fd on the given ::xnvme_queue
 *
 * @param queue Pointer to the ::xnvme_queue to query for outstanding commands
 *
 * @return On success, an eventfd() file descriptor is returned. On error, negative `errno`
 * is returned.
 */
int
xnvme_queue_get_completion_fd(struct xnvme_queue *queue);

/**
 * Retrieve the memory backend identifier bound to the queue
 *
 * @param queue Pointer to the ::xnvme_queue to query
 *
 * @return On success, a backend identifier string is returned. On error, NULL is returned.
 */
const char *
xnvme_queue_get_mem_id(const struct xnvme_queue *queue);

/**
 * Allocate a buffer for I/O with the given queue policy
 *
 * @param queue Queue handle obtained with ::xnvme_queue_init() or
 *              ::xnvme_queue_init_with_attr()
 * @param nbytes The size of the allocated buffer in bytes
 *
 * @return On success, a pointer to the allocated memory is returned. On error, NULL is returned
 * and `errno` set to indicate the error.
 */
void *
xnvme_queue_buf_alloc(const struct xnvme_queue *queue, size_t nbytes);

/**
 * Reallocate a buffer for I/O with the given queue policy
 *
 * @param queue Queue handle obtained with ::xnvme_queue_init() or
 *              ::xnvme_queue_init_with_attr()
 * @param buf The buffer to reallocate
 * @param nbytes The size of the allocated buffer in bytes
 *
 * @return On success, a pointer to the allocated memory is returned. On error, NULL is returned
 * and `errno` set to indicate the error.
 */
void *
xnvme_queue_buf_realloc(const struct xnvme_queue *queue, void *buf, size_t nbytes);

/**
 * Free the given I/O buffer allocated with ::xnvme_queue_buf_alloc()
 *
 * @param queue Queue handle obtained with ::xnvme_queue_init() or
 *              ::xnvme_queue_init_with_attr()
 * @param buf Pointer to a buffer allocated with ::xnvme_queue_buf_alloc()
 */
void
xnvme_queue_buf_free(const struct xnvme_queue *queue, void *buf);

/**
 * Allocate a buffer and optionally return its physical address
 *
 * @param queue Queue handle obtained with ::xnvme_queue_init() or
 *              ::xnvme_queue_init_with_attr()
 * @param nbytes The size of the allocated buffer in bytes
 * @param phys Physical address output; may be NULL
 *
 * @return On success, a pointer to the allocated memory is returned. On error, NULL is returned
 * and `errno` set to indicate the error.
 */
void *
xnvme_queue_buf_phys_alloc(const struct xnvme_queue *queue, size_t nbytes, uint64_t *phys);

/**
 * Reallocate a physical buffer and optionally return its physical address
 *
 * @param queue Queue handle obtained with ::xnvme_queue_init() or
 *              ::xnvme_queue_init_with_attr()
 * @param buf The buffer to reallocate
 * @param nbytes The size of the allocated buffer in bytes
 * @param phys Physical address output; may be NULL
 *
 * @return On success, a pointer to the allocated memory is returned. On error, NULL is returned
 * and `errno` set to indicate the error.
 */
void *
xnvme_queue_buf_phys_realloc(const struct xnvme_queue *queue, void *buf, size_t nbytes,
			     uint64_t *phys);

/**
 * Free a physical buffer allocated with ::xnvme_queue_buf_phys_alloc()
 *
 * @param queue Queue handle obtained with ::xnvme_queue_init() or
 *              ::xnvme_queue_init_with_attr()
 * @param buf Pointer to a buffer allocated with ::xnvme_queue_buf_phys_alloc()
 */
void
xnvme_queue_buf_phys_free(const struct xnvme_queue *queue, void *buf);

/**
 * Retrieve the physical address of the given buffer using the queue policy
 *
 * @param queue Queue handle obtained with ::xnvme_queue_init() or
 *              ::xnvme_queue_init_with_attr()
 * @param buf Pointer to a buffer allocated with ::xnvme_queue_buf_alloc()
 * @param phys Physical address output
 *
 * @return On success, 0 is returned. On error, negative `errno` is returned.
 */
int
xnvme_queue_buf_vtophys(const struct xnvme_queue *queue, void *buf, uint64_t *phys);
