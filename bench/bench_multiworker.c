/*
 * bench/bench_multiworker.c - multi-worker scaling.
 *
 * Measures total fiber throughput (fibers completed per second) as the
 * number of workers scales from 1 to N_WORKERS_MAX.
 *
 * Methodology:
 *   For each worker count W:
 *     - Initialise a runtime with W workers.
 *     - Spawn TOTAL_FIBERS fibers (round-robin across workers).
 *     - Each fiber yields YIELDS_PER_FIBER times then completes.
 *     - Wait for all fibers to complete (atomic counter → condvar).
 *     - Report fibers/second and ns/fiber.
 *
 *   Per-worker pinned model: no fiber ever migrates between workers.
 *   Cross-worker interaction is inject-queue based only.
 *
 * See REPOSITORY_STRUCTURE.md 5 (bench_multiworker.c description).
 * See DEVELOPMENT.md 7.2, ARCHITECTURE.md 6.1.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/strand.h"
#include "../src/strand_sched.h"
#include "bench_common.h"

#define N_WORKERS_MAX    8UL
#define TOTAL_FIBERS     10000UL
#define YIELDS_PER_FIBER 10UL

/* -------------------------------------------------------------------------
 * Shared state for all fibers in one benchmark run
 * -------------------------------------------------------------------------
 */

typedef struct {
	_Atomic uint64_t remaining;
	bench_done_t     done;
} mw_shared_t;

/* -------------------------------------------------------------------------
 * Per-fiber argument
 * -------------------------------------------------------------------------
 */

typedef struct {
	mw_shared_t *shared;
} mw_fiber_args_t;

/* -------------------------------------------------------------------------
 * Worker fiber: yields YIELDS_PER_FIBER times then decrements counter.
 * -------------------------------------------------------------------------
 */

static void
mw_fiber(void *arg)
{
	mw_fiber_args_t    *a    = arg;
	strand_scheduler_t *sched = strand_sched_current_tls;
	uint64_t            i;

	for (i = 0; i < YIELDS_PER_FIBER; i++)
		strand_fiber_yield(sched);

	if (atomic_fetch_sub_explicit(&a->shared->remaining, 1,
	    memory_order_acq_rel) == 1)
		bench_done_signal(&a->shared->done);
}

/* -------------------------------------------------------------------------
 * Run one worker-count configuration
 * -------------------------------------------------------------------------
 */

static void
run_workers(size_t nworkers)
{
	strand_runtime_t *rt;
	mw_shared_t       shared;
	mw_fiber_args_t  *args;   /* TOTAL_FIBERS arg structs */
	uint64_t          t0, t1, elapsed;
	uint64_t          i;
	char              label[64];
	int               rc;

	args = calloc(TOTAL_FIBERS, sizeof(*args));
	if (args == NULL) {
		fprintf(stderr, "calloc failed\n");
		return;
	}

	atomic_init(&shared.remaining, TOTAL_FIBERS);
	bench_done_init(&shared.done);

	rt = strand_runtime_init(NULL);
	if (rt == NULL) {
		fprintf(stderr, "strand_runtime_init failed\n");
		free(args);
		bench_done_destroy(&shared.done);
		return;
	}

	for (i = 0; i < nworkers; i++) {
		if (strand_worker_start(rt, NULL) == NULL) {
			fprintf(stderr, "strand_worker_start failed\n");
			strand_runtime_destroy(rt);
			free(args);
			bench_done_destroy(&shared.done);
			return;
		}
	}

	t0 = bench_now_ns();

	for (i = 0; i < TOTAL_FIBERS; i++) {
		args[i].shared = &shared;
		rc = strand_runtime_spawn(rt, mw_fiber, &args[i],
		    STRAND_DEFAULT_STACK_SIZE, NULL, NULL);
		if (rc != STRAND_OK) {
			fprintf(stderr, "strand_runtime_spawn failed: %d\n", rc);
			break;
		}
	}

	bench_done_wait(&shared.done);
	t1 = bench_now_ns();

	elapsed = t1 - t0;
	snprintf(label, sizeof(label), "multi-worker %zu worker(s) (ns/fiber)",
	    nworkers);
	bench_print_result(label, TOTAL_FIBERS, elapsed);

	strand_runtime_destroy(rt);
	bench_done_destroy(&shared.done);
	free(args);
}

/* -------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------
 */

int
main(void)
{
	size_t w;

	printf("=== bench_multiworker ===\n");
	printf("    %zu fibers x %zu yields each\n",
	    TOTAL_FIBERS, YIELDS_PER_FIBER);
	printf("\n");
	bench_print_hw();

	for (w = 1; w <= N_WORKERS_MAX; w++)
		run_workers(w);

	printf("\n");
	return (0);
}

