// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

/**
 * Concurrency tests for the parts of xNVMe that several threads may touch
 *
 * The queue submit/complete path is deliberately lock-free and a queue is owned
 * by one thread, so it is not covered here. What is covered is the shared state
 * behind it: the buffer allocator, which backends may implement over a heap of
 * their own.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <libxnvme.h>

#define ROUNDS 256 ///< Alloc/free cycles per thread
#define LIVE 8     ///< Allocations a thread holds at once
#define MAX_THREADS 64

/** One live allocation, as the overlap check sees it */
struct extent {
	uintptr_t start;
	size_t nbytes;
};

static struct {
	pthread_mutex_t lock; ///< Guards this table only, never the code under test
	struct extent live[MAX_THREADS * LIVE];
	int nlive;
	int noverlap;
	int nalloc_fail;
	uint64_t nalloc;
} g_reg;

/**
 * Record an allocation and report whether it overlaps one already live
 *
 * Two disjoint allocations cannot overlap, so a hit here is a freelist that
 * handed the same block to two threads.
 */
static int
_extent_insert(uintptr_t start, size_t nbytes)
{
	int overlap = 0;

	pthread_mutex_lock(&g_reg.lock);

	for (int i = 0; i < g_reg.nlive; ++i) {
		const struct extent *e = &g_reg.live[i];

		if ((start < e->start + e->nbytes) && (e->start < start + nbytes)) {
			overlap = 1;
			g_reg.noverlap += 1;
			break;
		}
	}

	if (g_reg.nlive < (int)(sizeof(g_reg.live) / sizeof(*g_reg.live))) {
		g_reg.live[g_reg.nlive].start = start;
		g_reg.live[g_reg.nlive].nbytes = nbytes;
		g_reg.nlive += 1;
	}
	g_reg.nalloc += 1;

	pthread_mutex_unlock(&g_reg.lock);

	return overlap;
}

static void
_extent_remove(uintptr_t start)
{
	pthread_mutex_lock(&g_reg.lock);

	for (int i = 0; i < g_reg.nlive; ++i) {
		if (g_reg.live[i].start != start) {
			continue;
		}
		g_reg.live[i] = g_reg.live[g_reg.nlive - 1];
		g_reg.nlive -= 1;
		break;
	}

	pthread_mutex_unlock(&g_reg.lock);
}

struct worker {
	struct xnvme_dev *dev;
	unsigned int seed;
};

/**
 * Hold LIVE buffers at a time, cycling them ROUNDS times
 *
 * The varying sizes are what make the freelist split and coalesce, which is
 * where an unguarded freelist comes apart.
 */
static void *
_worker_fn(void *arg)
{
	struct worker *w = arg;
	void *bufs[LIVE] = {0};

	for (int round = 0; round < ROUNDS; ++round) {
		for (int i = 0; i < LIVE; ++i) {
			size_t nbytes = 512ULL << (rand_r(&w->seed) % 4);

			bufs[i] = xnvme_buf_alloc(w->dev, nbytes);
			if (!bufs[i]) {
				pthread_mutex_lock(&g_reg.lock);
				g_reg.nalloc_fail += 1;
				pthread_mutex_unlock(&g_reg.lock);
				continue;
			}
			_extent_insert((uintptr_t)bufs[i], nbytes);
		}

		for (int i = 0; i < LIVE; ++i) {
			if (!bufs[i]) {
				continue;
			}
			_extent_remove((uintptr_t)bufs[i]);
			xnvme_buf_free(w->dev, bufs[i]);
			bufs[i] = NULL;
		}
	}

	return NULL;
}

static int
test_buf_alloc_free_mt(struct xnvme_cli *cli)
{
	uint64_t nthreads = cli->args.count;
	pthread_t tids[MAX_THREADS];
	struct worker workers[MAX_THREADS];
	uint64_t started = 0;

	if (!nthreads || nthreads > MAX_THREADS) {
		xnvme_cli_perr("count must be within [1, 64]", -EINVAL);
		return -EINVAL;
	}

	memset(&g_reg, 0, sizeof(g_reg));
	pthread_mutex_init(&g_reg.lock, NULL);

	xnvme_cli_pinf("threads: %zu, rounds: %d, live-per-thread: %d", nthreads, ROUNDS, LIVE);

	for (uint64_t i = 0; i < nthreads; ++i) {
		workers[i].dev = cli->args.dev;
		workers[i].seed = (unsigned int)(i + 1);

		if (pthread_create(&tids[i], NULL, _worker_fn, &workers[i])) {
			xnvme_cli_perr("pthread_create()", -errno);
			break;
		}
		started += 1;
	}

	for (uint64_t i = 0; i < started; ++i) {
		pthread_join(tids[i], NULL);
	}

	pthread_mutex_destroy(&g_reg.lock);

	xnvme_cli_pinf("allocations: %" PRIu64 ", failures: %d, overlaps: %d", g_reg.nalloc,
		       g_reg.nalloc_fail, g_reg.noverlap);

	if (started != nthreads) {
		return -EAGAIN;
	}
	if (g_reg.noverlap) {
		xnvme_cli_pinf("--={[ freelist handed the same block to two threads ]}=--");
		return -EFAULT;
	}
	if (g_reg.nalloc_fail) {
		xnvme_cli_pinf("--={[ allocation failed under contention ]}=--");
		return -ENOMEM;
	}
	if (g_reg.nlive) {
		xnvme_cli_pinf("--={[ %d extents left live ]}=--", g_reg.nlive);
		return -EFAULT;
	}

	xnvme_cli_pinf("LGMT: xnvme_buf_{alloc,free} across threads");

	return 0;
}

//
// Command-Line Interface (CLI) definition
//
static struct xnvme_cli_sub g_subs[] = {
	{
		"buf_alloc_free_mt",
		"Allocate and free buffers from 'count' threads at once",
		"Allocate and free buffers from 'count' threads at once, failing on "
		"overlapping allocations",
		test_buf_alloc_free_mt,
		{
			{XNVME_CLI_OPT_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_URI, XNVME_CLI_POSA},

			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_COUNT, XNVME_CLI_LREQ},

			XNVME_CLI_ADMIN_OPTS,
		},
	},
};

static struct xnvme_cli g_cli = {
	.title = "Test xNVMe under concurrent use",
	.descr_short = "Test xNVMe under concurrent use",
	.subs = g_subs,
	.nsubs = sizeof g_subs / sizeof(*g_subs),
};

int
main(int argc, char **argv)
{
	return xnvme_cli_run(&g_cli, argc, argv, XNVME_CLI_INIT_DEV_OPEN);
}
