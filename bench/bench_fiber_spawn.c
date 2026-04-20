/*
 * bench/bench_fiber_spawn.c - fiber creation and teardown throughput.
 *
 * Measures:
 *   cold path: spawn one fiber, drive it to completion, repeat N times.
 *              Uses a non-default stack size (128 KB) that never matches
 *              cache entries, forcing mmap+mprotect on every spawn.
 *   warm path: spawn one fiber, drive it to completion, repeat N times.
 *              Uses the default stack size and the prior iteration's
 *              completion returns the stack to the cache, so every spawn
 *              after the first is a cache hit.
 *
 * Each spawned fiber simply returns immediately (zero-work body).
 * The measurement covers: fiber descriptor allocation, stack allocation
 * (cache hit or mmap), context fabrication, run-queue insertion, fiber
 * dispatch, execution, teardown, and stack return to cache (or munmap).
 *
 * See REPOSITORY_STRUCTURE.md 5 (bench_fiber_spawn.c description).
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

#define SPAWN_ITERS 50000UL

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
 * Trivial child fiber: returns immediately.
 * -------------------------------------------------------------------------
 */

static void
trivial_child(void *arg)
{
	(void)arg;
}

/* -------------------------------------------------------------------------
 * Spawn-one-drive-one fiber: spawns a single child with the given stack
 * size, then yields so the child runs to completion (returning its stack
 * to the cache).  Repeats N times.
 * -------------------------------------------------------------------------
 */

typedef struct {
	strand_scheduler_t *sched;
	uint64_t            iters;
	size_t              stack_sz;
	uint64_t            t0;
	uint64_t            t1;
} spawner_args_t;

static void
spawner_fiber(void *arg)
{
	spawner_args_t *a = arg;
	uint64_t        i;

	/* Warmup: one untimed iteration to prime the dead pool. */
	strand_fiber_spawn(a->sched, trivial_child, NULL, a->stack_sz, NULL);
	strand_fiber_yield(a->sched);

	a->t0 = bench_now_ns();
	for (i = 0; i < a->iters; i++) {
		strand_fiber_spawn(a->sched, trivial_child, NULL,
		    a->stack_sz, NULL);
		/*
		 * Yield once so the scheduler runs the trivial child to
		 * completion.  Its stack is returned to the cache (warm) or
		 * munmap'd (cold, size mismatch).  The next spawn iteration
		 * then exercises the intended path.
		 */
		strand_fiber_yield(a->sched);
	}
	a->t1 = bench_now_ns();
}

/* -------------------------------------------------------------------------
 * Benchmark: cold spawn (cache miss every iteration via size mismatch).
 * -------------------------------------------------------------------------
 */

static void
bench_spawn_cold(void)
{
	strand_scheduler_t *sched;
	spawner_args_t      a;
	uint64_t            elapsed;

	sched = strand_scheduler_create(NULL);
	if (sched == NULL) {
		fprintf(stderr, "strand_scheduler_create failed\n");
		return;
	}

	/*
	 * Disable the stack cache entirely for the cold path.  This forces
	 * every spawn to call mmap+mprotect and every teardown to call
	 * munmap, measuring the true OS allocation cost.
	 */
	sched->cache_cap = 0;

	memset(&a, 0, sizeof(a));
	a.sched    = sched;
	a.iters    = SPAWN_ITERS;
	a.stack_sz = 0; /* default size - same as warm, only cache is disabled */

	if (push_root_fiber(sched, spawner_fiber, &a) != 0) {
		fprintf(stderr, "push_root_fiber failed\n");
		strand_scheduler_destroy(sched);
		return;
	}
	drive_to_idle(sched);
	elapsed = a.t1 - a.t0;
	bench_print_result("fiber spawn cold (mmap+munmap, ns/spawn)",
	    SPAWN_ITERS, elapsed);

	strand_scheduler_destroy(sched);
}

/* -------------------------------------------------------------------------
 * Benchmark: warm spawn (cache hit every iteration).
 * -------------------------------------------------------------------------
 */

static void
bench_spawn_warm(void)
{
	strand_scheduler_t *sched;
	spawner_args_t      a;
	uint64_t            elapsed;

	sched = strand_scheduler_create(NULL);
	if (sched == NULL) {
		fprintf(stderr, "strand_scheduler_create failed\n");
		return;
	}

	memset(&a, 0, sizeof(a));
	a.sched    = sched;
	a.iters    = SPAWN_ITERS;
	a.stack_sz = 0; /* default size - matches cache entries */

	if (push_root_fiber(sched, spawner_fiber, &a) != 0) {
		fprintf(stderr, "push_root_fiber failed\n");
		strand_scheduler_destroy(sched);
		return;
	}
	drive_to_idle(sched);
	elapsed = a.t1 - a.t0;
	bench_print_result("fiber spawn warm (cache hit, ns/spawn)",
	    SPAWN_ITERS, elapsed);

	strand_scheduler_destroy(sched);
}

/* -------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------
 */

int
main(void)
{
	printf("=== bench_fiber_spawn ===\n");
	bench_print_hw();

	bench_spawn_cold();
	bench_spawn_warm();

	printf("\n");
	return (0);
}

