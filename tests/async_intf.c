// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include <errno.h>
#include <string.h>
#include <libxnvme.h>

#define XNVME_TESTS_QDEPTH_MAX 512
#define XNVME_TESTS_NQUEUE_MAX 1024

static int
test_init_term(struct xnvme_cli *cli)
{
	struct xnvme_dev *dev = cli->args.dev;
	uint64_t count = cli->args.count;
	uint64_t qd = cli->args.qdepth;
	int err = 0;

	struct xnvme_queue *queue[XNVME_TESTS_NQUEUE_MAX] = {0};

	if (count > XNVME_TESTS_NQUEUE_MAX) {
		XNVME_DEBUG("FAILED: count(%zu) out-of-bounds for test", count);
		return 1;
	}
	if (qd > XNVME_TESTS_QDEPTH_MAX) {
		XNVME_DEBUG("FAILED: qd(%zu) out-of-bounds for test", qd);
		return 1;
	}

	xnvme_cli_pinf("count: %zu", count);
	xnvme_cli_pinf("qdepth: %zu", qd);
	xnvme_cli_pinf("clear: %d", cli->args.clear);

	if (!cli->args.clear) {
		// Ask how many queues are supported
		struct xnvme_cmd_ctx ctx = xnvme_cmd_ctx_from_dev(dev);
		struct xnvme_spec_feat feat = {.val = 0};

		err = xnvme_adm_gfeat(&ctx, 0x0, XNVME_SPEC_FEAT_NQUEUES,
				      XNVME_SPEC_FEAT_SEL_CURRENT, NULL, 0);
		if (err || xnvme_cmd_ctx_cpl_status(&ctx)) {
			xnvme_cli_perr("xnvme_adm_gfeat()", err);
			xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
			err = err ? err : -EIO;
			return err;
		}

		feat.val = ctx.cpl.cdw0;

		xnvme_spec_feat_pr(XNVME_SPEC_FEAT_NQUEUES, feat, XNVME_PR_DEF);

		if (count >= (uint64_t)(feat.nqueues.nsqa + 1)) {
			xnvme_cli_pinf("skipping -- count: %zu > (nsqa + 1): %u", count,
				       feat.nqueues.nsqa);
			return 0;
		}
	}

	// Initialize and check capacity of asynchronous contexts
	for (uint64_t qn = 0; qn < count; ++qn) {
		err = xnvme_queue_init(dev, qd, 0, &queue[qn]);
		if (err) {
			XNVME_DEBUG("FAILED: init qn: %zu, qd: %zu, err: %d", qn, qd, err);

			if (!((err == -ENOMEM) && (qn == count - 1))) {
				goto exit;
			}

			XNVME_DEBUG("INFO: expected failure => OK");
			err = 0;
			continue;
		}

		if (xnvme_queue_get_capacity(queue[qn]) != qd) {
			XNVME_DEBUG("FAILED: xnvme_queue_get_capacity() != qd(%zu)", qd);
			err = -EIO;
			goto exit;
		}
	}

exit:
	// Tear down queues
	for (uint64_t qn = 0; qn < count; ++qn) {
		XNVME_DEBUG("INFO: qn: %zu", qn);
		if (!queue[qn]) {
			continue;
		}
		if (xnvme_queue_term(queue[qn])) {
			XNVME_DEBUG("FAILED: xnvme_queue_term, qn(%zu)", qn);
			err = -EIO;
		}
	}

	return err;
}

static int
test_init_term_ex(struct xnvme_cli *cli)
{
	struct xnvme_dev *dev = cli->args.dev;
	const struct xnvme_opts *dev_opts = xnvme_dev_get_opts(dev);
	struct xnvme_queue_attr fallback_attr = {
		.opts = 0,
	};
	struct xnvme_queue_attr attr = {
		.opts = 0,
		.mem = dev_opts->mem,
	};
	struct xnvme_queue *queue_default = NULL;
	struct xnvme_queue *queue_fallback = NULL;
	struct xnvme_queue *queue = NULL;
	int err;

	err = xnvme_queue_init(dev, cli->args.qdepth, 0, &queue_default);
	if (err) {
		xnvme_cli_perr("xnvme_queue_init()", err);
		return err;
	}

	err = xnvme_queue_init_with_attr(dev, cli->args.qdepth, &attr, &queue);
	if (err) {
		xnvme_cli_perr("xnvme_queue_init_with_attr()", err);
		xnvme_queue_term(queue_default);
		return err;
	}

	if (!xnvme_queue_get_mem_id(queue) ||
	    strcmp(xnvme_queue_get_mem_id(queue), xnvme_queue_get_mem_id(queue_default))) {
		xnvme_cli_pinf("unexpected mem queue policy");
		err = -EIO;
		goto exit;
	}

	err = xnvme_queue_init_with_attr(dev, cli->args.qdepth, &fallback_attr, &queue_fallback);
	if (err) {
		xnvme_cli_perr("xnvme_queue_init_with_attr(fallback)", err);
		goto exit;
	}

	if (!xnvme_queue_get_mem_id(queue_fallback) ||
	    strcmp(xnvme_queue_get_mem_id(queue_fallback), xnvme_queue_get_mem_id(queue_default))) {
		xnvme_cli_pinf("unexpected mem fallback policy");
		err = -EIO;
		goto exit;
	}

	err = 0;

exit:
	xnvme_queue_term(queue_fallback);
	xnvme_queue_term(queue);
	xnvme_queue_term(queue_default);
	return err;
}

static int
test_init_term_ex_invalid(struct xnvme_cli *cli)
{
	struct xnvme_dev *dev = cli->args.dev;
	struct xnvme_queue *queue = NULL;
	struct xnvme_queue_attr attr = {
		.opts = 0,
		.mem = "__xnvme_no_such_mem__",
	};
	int err;

	err = xnvme_queue_init_with_attr(dev, cli->args.qdepth, &attr, &queue);
	if (err != -ENOSYS) {
		xnvme_cli_pinf("unexpected result for invalid mem override: %d", err);
		return -EIO;
	}

	return 0;
}

//
// Command-Line Interface (CLI) definition
//
static struct xnvme_cli_sub g_subs[] = {
	{
		"init_term",
		"Create 'count' contexts with given 'qdepth'",
		"Create 'count' contexts with given 'qdepth'",
		test_init_term,
		{
			{XNVME_CLI_OPT_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_URI, XNVME_CLI_POSA},

			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_COUNT, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_QDEPTH, XNVME_CLI_LREQ},
			{XNVME_CLI_OPT_CLEAR, XNVME_CLI_LFLG},

			XNVME_CLI_ASYNC_OPTS,
		},
	},
	{
		"init_term_ex",
		"Create a queue with explicit queue memory policy",
		"Create a queue with explicit queue memory policy",
		test_init_term_ex,
		{
			{XNVME_CLI_OPT_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_URI, XNVME_CLI_POSA},

			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_QDEPTH, XNVME_CLI_LREQ},

			XNVME_CLI_ASYNC_OPTS,
		},
	},
	{
		"init_term_ex_invalid",
		"Reject invalid queue memory overrides",
		"Reject invalid queue memory overrides",
		test_init_term_ex_invalid,
		{
			{XNVME_CLI_OPT_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_URI, XNVME_CLI_POSA},

			{XNVME_CLI_OPT_NON_POSA_TITLE, XNVME_CLI_SKIP},
			{XNVME_CLI_OPT_QDEPTH, XNVME_CLI_LREQ},

			XNVME_CLI_ASYNC_OPTS,
		},
	},
};

static struct xnvme_cli g_cli = {
	.title = "Test xNVMe Asynchronous Interface",
	.descr_short = "Test xNVMe Asynchronous Interface",
	.subs = g_subs,
	.nsubs = sizeof g_subs / sizeof(*g_subs),
};

int
main(int argc, char **argv)
{
	return xnvme_cli_run(&g_cli, argc, argv, XNVME_CLI_INIT_DEV_OPEN);
}
