/*
 * bench/bench_context_switch.c - context switch round-trip latency.
 *
 * Measures:
 *   single-fiber yield: one fiber yields in a tight loop, reports ns/switch.
 *   two-fiber ping-pong: two fibers alternate yields through the scheduler.
 *
 * Methodology:
 *   Each strand_fiber_yield call is one "switch out" plus one "switch in"
 *   (the fiber parks and then resumes).  Reported ns/switch = elapsed_ns
 *   divided by (iters * 2).
 *
 *   In the ping-pong case the root fiber measures elapsed time for its own
 *   ITERS yields.  The partner fiber is interleaved, so each root yield
 *   round-trips through the scheduler while the partner also runs.  The
 *   measured time reflects realistic two-fiber interleaving.
 *
 * See REPOSITORY_STRUCTURE.md 5 (bench_context_switch.c description).
 * See DEVELOPMENT.md 7.2.
 */

#include <errno.h>
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

#define CS_ITERS 2000000UL

/* -------------------------------------------------------------------------
 * Internal helpers - same pattern as tests/test_integration.c.
 * push_root_fiber: inject a fiber into the scheduler's run queue from
 * the host thread, bypassing the WRONGCTX guard.
 * drive_to_idle: advance the scheduler until the run queue is empty.
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
 * Single-fiber yield benchmark
 * -------------------------------------------------------------------------
 */

typedef struct {
	strand_scheduler_t *sched;
	uint64_t            t0;
	uint64_t            t1;
} cs_single_t;

static void
cs_single_fiber(void *arg)
{
	cs_single_t *a = arg;
	uint64_t     i;

	a->t0 = bench_now_ns();
	for (i = 0; i < CS_ITERS; i++)
		strand_fiber_yield(a->sched);
	a->t1 = bench_now_ns();
}

static void
bench_single_yield(strand_scheduler_t *sched)
{
	cs_single_t a;
	uint64_t    elapsed;

	memset(&a, 0, sizeof(a));
	a.sched = sched;

	if (push_root_fiber(sched, cs_single_fiber, &a) != 0) {
		fprintf(stderr, "push_root_fiber failed\n");
		return;
	}
	drive_to_idle(sched);
	elapsed = a.t1 - a.t0;
	/* Each yield = 2 switches (fiber→sched, sched→fiber) */
	bench_print_result("single-fiber yield (ns/switch)",
	    CS_ITERS * 2, elapsed);
}

/* -------------------------------------------------------------------------
 * Two-fiber ping-pong benchmark
 * -------------------------------------------------------------------------
 */

typedef struct {
	strand_scheduler_t *sched;
	uint64_t            t0;
	uint64_t            t1;
} cs_pp_t;

static void
cs_partner_fiber(void *arg)
{
	cs_pp_t *a = arg;
	uint64_t i;

	for (i = 0; i < CS_ITERS; i++)
		strand_fiber_yield(a->sched);
}

static void
cs_root_fiber(void *arg)
{
	cs_pp_t *a = arg;
	uint64_t i;

	strand_fiber_spawn(a->sched, cs_partner_fiber, a, 0, NULL);

	a->t0 = bench_now_ns();
	for (i = 0; i < CS_ITERS; i++)
		strand_fiber_yield(a->sched);
	a->t1 = bench_now_ns();
}

static void
bench_ping_pong(strand_scheduler_t *sched)
{
	cs_pp_t  a;
	uint64_t elapsed;

	memset(&a, 0, sizeof(a));
	a.sched = sched;

	if (push_root_fiber(sched, cs_root_fiber, &a) != 0) {
		fprintf(stderr, "push_root_fiber failed\n");
		return;
	}
	drive_to_idle(sched);
	elapsed = a.t1 - a.t0;
	/*
	 * Root fiber measures its own CS_ITERS yields.  Each yield in the root
	 * includes the scheduler dispatching the partner, so the measured time
	 * captures realistic two-fiber scheduling overhead.
	 * ns/switch = elapsed / (CS_ITERS * 2).
	 */
	bench_print_result("two-fiber ping-pong (ns/switch)",
	    CS_ITERS * 2, elapsed);
}

/* -------------------------------------------------------------------------
 * errno save/restore benchmark
 *
 * Each iteration sets errno to a non-zero value before yielding so that the
 * errno save/restore path in strand_context_switch is exercised with a live
 * value.  The cost delta vs bench_single_yield shows the errno preservation
 * overhead (in practice near-zero since errno is an integer store/load).
 * See REPOSITORY_STRUCTURE.md §5 bench_context_switch.c.
 * -------------------------------------------------------------------------
 */

static void
cs_errno_fiber(void *arg)
{
	cs_single_t *a = arg;
	uint64_t     i;

	a->t0 = bench_now_ns();
	for (i = 0; i < CS_ITERS; i++) {
		errno = (int)(i & 0x7f) + 1; /* non-zero errno each iteration */
		strand_fiber_yield(a->sched);
	}
	a->t1 = bench_now_ns();
}

static void
bench_errno_switch(strand_scheduler_t *sched)
{
	cs_single_t a;
	uint64_t    elapsed;

	memset(&a, 0, sizeof(a));
	a.sched = sched;

	if (push_root_fiber(sched, cs_errno_fiber, &a) != 0) {
		fprintf(stderr, "push_root_fiber failed\n");
		return;
	}
	drive_to_idle(sched);
	elapsed = a.t1 - a.t0;
	bench_print_result("switch with errno save/restore (ns/switch)",
	    CS_ITERS * 2, elapsed);
}

/* -------------------------------------------------------------------------
 * MXCSR / FPCR save/restore benchmark
 *
 * On x86_64 each iteration modifies the MXCSR register (FP control/status)
 * before yielding so that the MXCSR save/restore path is exercised with a
 * non-default value.  On AArch64 FPCR is modified instead.  On other
 * architectures this simply falls back to the errno benchmark path.
 *
 * The cost delta vs bench_single_yield shows the FP-state preservation
 * overhead per context switch.
 * See REPOSITORY_STRUCTURE.md §5 bench_context_switch.c.
 * -------------------------------------------------------------------------
 */

#if defined(__x86_64__)
static inline void modify_fp_ctrl(void) {
	unsigned int mxcsr;
	/* Read MXCSR, toggle FZ (flush-to-zero, bit 15), write back.
	 * Uses inline asm to avoid <immintrin.h> which may not be universally
	 * available (e.g. without explicit -msse flags on some platforms).
	 * The context switch wrapper saves/restores MXCSR on every switch. */
	__asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
	mxcsr ^= (1u << 15);
	__asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
}
#elif defined(__aarch64__)
static inline void modify_fp_ctrl(void) {
	uint64_t fpcr;
	__asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
	/* Toggle bit 24 (FZ - flush-to-zero). */
	__asm__ volatile("msr fpcr, %0" :: "r"(fpcr ^ (1ULL << 24)));
}
#else
static inline void modify_fp_ctrl(void) { (void)0; }
#endif

static void
cs_fpcr_fiber(void *arg)
{
	cs_single_t *a = arg;
	uint64_t     i;

	a->t0 = bench_now_ns();
	for (i = 0; i < CS_ITERS; i++) {
		modify_fp_ctrl();
		strand_fiber_yield(a->sched);
	}
	a->t1 = bench_now_ns();
}

static void
bench_fpcr_switch(strand_scheduler_t *sched)
{
	cs_single_t a;
	uint64_t    elapsed;

	memset(&a, 0, sizeof(a));
	a.sched = sched;

	if (push_root_fiber(sched, cs_fpcr_fiber, &a) != 0) {
		fprintf(stderr, "push_root_fiber failed\n");
		return;
	}
	drive_to_idle(sched);
	elapsed = a.t1 - a.t0;
#if defined(__x86_64__)
	bench_print_result("switch with MXCSR save/restore (ns/switch)",
	    CS_ITERS * 2, elapsed);
#elif defined(__aarch64__)
	bench_print_result("switch with FPCR save/restore (ns/switch)",
	    CS_ITERS * 2, elapsed);
#else
	bench_print_result("switch with FP-ctrl save/restore (ns/switch)",
	    CS_ITERS * 2, elapsed);
#endif
}

/* -------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------
 */

int
main(void)
{
	strand_scheduler_t *sched;

	printf("=== bench_context_switch ===\n");
	bench_print_hw();

	sched = strand_scheduler_create(NULL);
	if (sched == NULL) {
		fprintf(stderr, "strand_scheduler_create failed\n");
		return (1);
	}

	bench_single_yield(sched);
	bench_ping_pong(sched);
	bench_errno_switch(sched);
	bench_fpcr_switch(sched);

	strand_scheduler_destroy(sched);
	printf("\n");
	return (0);
}

