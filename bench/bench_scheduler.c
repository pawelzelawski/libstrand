/*
 * bench/bench_scheduler.c - scheduler throughput.
 *
 * Measures:
 *   fibers run per second with a fixed fiber count and varying scheduler
 *   budgets (8, 16, 32, 64, 128, 256).
 *
 * Methodology:
 *   N_FIBERS fibers each yield ITERS_PER_FIBER times.  The root fiber
 *   spawns all N_FIBERS children, then the scheduler drives them to
 *   completion.  Total fiber-run events = N_FIBERS * ITERS_PER_FIBER.
 *   Elapsed time is measured from just before drive_to_idle to just after.
 *
 *   Budget sensitivity: each run re-creates the scheduler with a specific
 *   budget value.  Budget controls how many fibers run per
 *   strand_scheduler_advance call before returning to the host.
 *
 * See REPOSITORY_STRUCTURE.md 5 (bench_scheduler.c description).
 * See DEVELOPMENT.md 7.2.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/strand.h"
#include "../src/strand_context.h"
#include "../src/strand_fiber.h"
#include "../src/strand_internal.h"
#include "../src/strand_sched.h"
#include "bench_common.h"

#define N_FIBERS        64UL
#define ITERS_PER_FIBER 1000UL  /* yields per fiber; total ops = N*I */

/* -------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------
 */

static int
push_root_fiber(strand_scheduler_t *sched, strand_fiber_fn_t fn, void *arg)
{
	strand_fiber_t *f;
	void           *base;
	unsigned long   vg_id;
	size_t          sz;

	sz   = STRAND_DEFAULT_STACK_SIZE;
	f    = fiber_alloc(&sched->dead_pool);
	if (f == NULL)
		return (-1);
	base = sched_stack_alloc(sched, sz, &vg_id);
	if (base == NULL) {
		fiber_free(&sched->dead_pool, f);
		return (-1);
	}
	f->stack_base        = (char *)base + page_size();
	f->stack_size        = sz;
	f->valgrind_stack_id = vg_id;
	f->entry_fn          = fn;
	f->entry_arg         = arg;
	f->home_sched        = sched;
	strand_context_init(&f->context,
	    (char *)f->stack_base + sz, strand_fiber_entry_start, f);
	strand_fiber_tsan_init(f);
	atomic_store_explicit(&f->state, FIBER_RUNNABLE, memory_order_relaxed);
	run_queue_push(sched, f);
	return (0);
}

static void
drive_to_idle(strand_scheduler_t *sched)
{
	uint64_t dl;
	while (strand_scheduler_advance(sched, &dl) == STRAND_SCHED_PROGRESS)
		;
}

/* -------------------------------------------------------------------------
 * Yielding child fiber
 * -------------------------------------------------------------------------
 */

typedef struct {
	strand_scheduler_t *sched;
	uint64_t            iters;
} child_args_t;

static void
yielding_child(void *arg)
{
	child_args_t *a = arg;
	uint64_t      i;

	for (i = 0; i < a->iters; i++)
		strand_fiber_yield(a->sched);
}

/* -------------------------------------------------------------------------
 * Root fiber: spawns N_FIBERS children.
 * -------------------------------------------------------------------------
 */

typedef struct {
	strand_scheduler_t *sched;
	child_args_t        child_arg; /* same arg for all children */
} root_args_t;

static void
sched_root_fiber(void *arg)
{
	root_args_t *a = arg;
	uint64_t     i;

	for (i = 0; i < N_FIBERS; i++)
		strand_fiber_spawn(a->sched, yielding_child, &a->child_arg,
		    0, NULL);
}

/* -------------------------------------------------------------------------
 * Run one budget configuration
 * -------------------------------------------------------------------------
 */

static void
run_budget(size_t budget)
{
	strand_sched_config_t cfg;
	strand_scheduler_t   *sched;
	root_args_t           root;
	uint64_t              t0, t1, elapsed;
	uint64_t              total_ops;
	char                  label[64];

	memset(&cfg, 0, sizeof(cfg));
	cfg.budget = budget;

	sched = strand_scheduler_create(&cfg);
	if (sched == NULL) {
		fprintf(stderr, "strand_scheduler_create failed\n");
		return;
	}

	memset(&root, 0, sizeof(root));
	root.sched           = sched;
	root.child_arg.sched = sched;
	root.child_arg.iters = ITERS_PER_FIBER;

	if (push_root_fiber(sched, sched_root_fiber, &root) != 0) {
		fprintf(stderr, "push_root_fiber failed\n");
		strand_scheduler_destroy(sched);
		return;
	}

	t0 = bench_now_ns();
	drive_to_idle(sched);
	t1 = bench_now_ns();

	elapsed   = t1 - t0;
	total_ops = N_FIBERS * ITERS_PER_FIBER;

	snprintf(label, sizeof(label), "scheduler budget=%zu (ns/fiber-run)",
	    budget);
	bench_print_result(label, total_ops, elapsed);

	strand_scheduler_destroy(sched);
}

/* -------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------
 */

int
main(void)
{
	static const size_t budgets[] = { 8, 16, 32, 64, 128, 256 };
	size_t              i;

	printf("=== bench_scheduler ===\n");
	printf("    %zu fibers x %zu yields each = %zu total fiber-run events\n",
	    N_FIBERS, ITERS_PER_FIBER, N_FIBERS * ITERS_PER_FIBER);
	printf("\n");
	bench_print_hw();

	for (i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++)
		run_budget(budgets[i]);

	printf("\n");
	return (0);
}

