/*
 * tests/test_layer4.c — Layer 4 (Phase 5) test suite.
 *
 * Task 5.1: bounded MPSC inject ring buffer tests.
 *
 * Tests covered here:
 *   - Single-threaded push/pop correctness (basic round-trip)
 *   - Push fills queue to capacity; drain clears all items; FIFO order
 *   - inject_queue_discard_all leaves queue empty
 *   - inject_queue capacity is rounded to next power of 2
 *   - inject_queue_drain delivers INJECT_CANCEL items to the scheduler
 *   - test_inject_queue_overflow_backoff: producer fills queue then blocks
 *     on the (capacity+1)th push until consumer frees a slot; no item lost
 *
 * Tests that require the runtime (Tasks 5.2+) will be added in later tasks.
 *
 * See DEVELOPMENT.md §"Tests for Phase 5", TESTING.md §2.4.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "test_harness.h"
#include "../include/strand.h"
#include "../src/strand_internal.h"
#include "../src/strand_inject.h"
#include "../src/strand_sched.h"
#include "../src/strand_fiber.h"

/* -------------------------------------------------------------------------
 * Shared helpers
 * -------------------------------------------------------------------------
 */

static strand_scheduler_t *
make_test_scheduler(void)
{
	strand_sched_config_t cfg = {
	    .budget     = 64,
	    .inject_cap = 16,
	    .cache_cap  = 8,
	    .idle_floor = 2,
	};
	return (strand_scheduler_create(&cfg));
}

/*
 * t5_push_fiber -- bootstrap a fiber into the run queue from the host thread.
 *
 * Mirrors the internals of strand_fiber_spawn but skips the WRONGCTX check
 * (current_fiber == NULL), allowing the host thread to enqueue fibers before
 * the first strand_scheduler_advance call.
 *
 * Uses strand_fiber_entry_start as the context entry point so fibers can
 * return normally.  If out != NULL, *out receives an ABA-safe handle.
 * Returns 0 on success, -1 on allocation failure.
 */
static int
t5_push_fiber(strand_scheduler_t *sched, strand_fiber_fn_t fn, void *arg,
    strand_fiber_handle_t *out)
{
	strand_fiber_t *f;
	void           *base;
	unsigned long   vg_id;
	size_t          sz = STRAND_DEFAULT_STACK_SIZE;

	f = fiber_alloc(&sched->dead_pool);
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
	    (char *)f->stack_base + sz,
	    strand_fiber_entry_start, f);
	strand_fiber_tsan_init(f);

	atomic_store_explicit(&f->state, FIBER_RUNNABLE, memory_order_relaxed);
	run_queue_push(sched, f);

	if (out != NULL) {
		out->ptr        = f;
		out->generation = f->generation;
	}
	return (0);
}

/* -------------------------------------------------------------------------
 * test_inject_push_pop_basic
 * Push one INJECT_CANCEL item; pop it back; verify fields match.
 * -------------------------------------------------------------------------
 */
static int
test_inject_push_pop_basic(void)
{
	strand_inject_queue_t q;
	inject_item_t         push_item, pop_item;
	strand_fiber_t        dummy;
	int                   rc;

	memset(&dummy, 0, sizeof(dummy));
	dummy.generation = 42;

	if (inject_queue_init(&q, 4) != 0)
		return (1);

	push_item.type                       = INJECT_CANCEL;
	push_item.u.cancel_handle.ptr        = &dummy;
	push_item.u.cancel_handle.generation = 42;

	inject_queue_push_release(&q, &push_item);

	memset(&pop_item, 0, sizeof(pop_item));
	rc = inject_queue_pop_acquire(&q, &pop_item);
	if (rc != 1) {
		inject_queue_destroy(&q);
		return (1);
	}
	if ((int)pop_item.type != (int)INJECT_CANCEL ||
	    pop_item.u.cancel_handle.ptr != &dummy ||
	    pop_item.u.cancel_handle.generation != 42) {
		inject_queue_destroy(&q);
		return (1);
	}

	/* Queue now empty — next pop must return 0. */
	rc = inject_queue_pop_acquire(&q, &pop_item);
	inject_queue_destroy(&q);
	return (rc == 0) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * test_inject_fill_and_drain
 * Fill the queue to capacity; pop every item; verify FIFO order.
 * -------------------------------------------------------------------------
 */
static int
test_inject_fill_and_drain(void)
{
	strand_inject_queue_t q;
	inject_item_t         item;
	strand_fiber_t        dummies[8];
	size_t                cap = 8;
	size_t                i;
	int                   rc;
	int                   failed = 0;

	memset(dummies, 0, sizeof(dummies));
	for (i = 0; i < cap; i++)
		dummies[i].generation = (uint64_t)i;

	if (inject_queue_init(&q, cap) != 0)
		return (1);

	for (i = 0; i < cap; i++) {
		item.type                    = INJECT_CANCEL;
		item.u.cancel_handle.ptr        = &dummies[i];
		item.u.cancel_handle.generation = (uint64_t)i;
		inject_queue_push_release(&q, &item);
	}

	for (i = 0; i < cap; i++) {
		memset(&item, 0, sizeof(item));
		rc = inject_queue_pop_acquire(&q, &item);
		if (rc != 1 ||
		    item.u.cancel_handle.ptr != &dummies[i] ||
		    item.u.cancel_handle.generation != (uint64_t)i) {
			failed = 1;
			break;
		}
	}

	if (!failed) {
		rc = inject_queue_pop_acquire(&q, &item);
		if (rc != 0)
			failed = 1;
	}

	inject_queue_destroy(&q);
	return (failed ? 1 : 0);
}

/* -------------------------------------------------------------------------
 * test_inject_discard_all
 * Push several items; call discard_all; verify queue is empty afterwards.
 * -------------------------------------------------------------------------
 */
static int
test_inject_discard_all(void)
{
	strand_inject_queue_t q;
	inject_item_t         item;
	strand_fiber_t        dummy;
	int                   rc;
	int                   i;

	memset(&dummy, 0, sizeof(dummy));

	if (inject_queue_init(&q, 4) != 0)
		return (1);

	item.type                    = INJECT_CANCEL;
	item.u.cancel_handle.ptr        = &dummy;
	item.u.cancel_handle.generation = 0;

	for (i = 0; i < 3; i++)
		inject_queue_push_release(&q, &item);

	inject_queue_discard_all(&q);

	rc = inject_queue_pop_acquire(&q, &item);
	inject_queue_destroy(&q);
	return (rc == 0) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * test_inject_capacity_is_power_of_two
 * Verify inject_queue_init rounds non-power-of-2 capacity up correctly.
 * -------------------------------------------------------------------------
 */
static int
test_inject_capacity_is_power_of_two(void)
{
	strand_inject_queue_t q;
	int                   failed = 0;

	/* Request 5 — should round up to 8. */
	if (inject_queue_init(&q, 5) != 0)
		return (1);
	if (q.capacity != 8 || q.mask != 7)
		failed = 1;
	inject_queue_destroy(&q);
	if (failed)
		return (1);

	/* Request 1 — stays 1. */
	if (inject_queue_init(&q, 1) != 0)
		return (1);
	if (q.capacity != 1)
		failed = 1;
	inject_queue_destroy(&q);
	if (failed)
		return (1);

	/* Request 0 — uses default. */
	if (inject_queue_init(&q, 0) != 0)
		return (1);
	if (q.capacity != STRAND_DEFAULT_INJECT_CAP)
		failed = 1;
	inject_queue_destroy(&q);
	return (failed ? 1 : 0);
}

/* -------------------------------------------------------------------------
 * test_inject_drain_delivers_cancel
 *
 * Push an INJECT_CANCEL for a timer-parked fiber directly into the
 * scheduler's inject queue (simulating a cross-worker cancel); call
 * strand_scheduler_advance; verify the fiber observes STRAND_CANCELLED.
 * -------------------------------------------------------------------------
 */

typedef struct {
	strand_scheduler_t *sched;
	_Atomic int         cancel_seen;
} drain_cancel_args_t;

static void
drain_cancel_fiber(void *arg)
{
	drain_cancel_args_t *a = arg;
	int rc;

	/*
	 * Sleep with a far-future deadline.  The inject-delivered cancel
	 * will interrupt this before the timer fires.
	 */
	rc = strand_fiber_sleep_until(a->sched,
	    (uint64_t)0xFFFFFFFFFFFFFFFFULL);
	if (rc == STRAND_CANCELLED)
		atomic_store(&a->cancel_seen, 1);
}

static int
test_inject_drain_delivers_cancel(void)
{
	strand_scheduler_t   *sched;
	drain_cancel_args_t   args;
	strand_fiber_handle_t handle;
	inject_item_t         item;
	int                   rc;

	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	atomic_init(&args.cancel_seen, 0);
	args.sched = sched;

	if (t5_push_fiber(sched, drain_cancel_fiber, &args, &handle) != 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * Run once: fiber enters drain_cancel_fiber, calls sleep_until,
	 * and parks on the timer heap (FIBER_PARKED_TIMER).
	 */
	strand_scheduler_advance(sched, NULL);

	/*
	 * Push a cancel item directly into the inject queue to simulate a
	 * cross-worker cancel bypassing the same-worker fast path.
	 */
	item.type            = INJECT_CANCEL;
	item.u.cancel_handle = handle;
	inject_queue_push_release(&sched->inject_queue, &item);

	/*
	 * Advance twice: first pass drains inject queue (Step 1) and delivers
	 * the cancel so the fiber moves to the run queue; second pass runs the
	 * fiber to completion.
	 */
	strand_scheduler_advance(sched, NULL);
	strand_scheduler_advance(sched, NULL);

	rc = atomic_load(&args.cancel_seen);
	strand_scheduler_destroy(sched);
	return (rc == 1) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * test_inject_queue_overflow_backoff
 *
 * Producer: pushes OVERFLOW_CAP + 1 items.  The first OVERFLOW_CAP fill
 * the queue; the last one blocks until the consumer frees a slot.
 * Consumer: sleeps 20 ms to let the producer block, then drains all items.
 *
 * Verification: after join, exactly OVERFLOW_CAP + 1 items were consumed.
 * Confirms no item is dropped and the exponential backoff path executes.
 *
 * See DEVELOPMENT.md §"Tests for Phase 5" — test_inject_queue_overflow_backoff.
 * -------------------------------------------------------------------------
 */

#define OVERFLOW_CAP 16

typedef struct {
	strand_inject_queue_t *q;
	_Atomic int            items_consumed;
} overflow_args_t;

static void *
overflow_producer(void *arg)
{
	overflow_args_t       *a = arg;
	inject_item_t          item;
	int                    i;

	item.type                    = INJECT_CANCEL;
	item.u.cancel_handle.ptr        = NULL;
	item.u.cancel_handle.generation = 0;

	for (i = 0; i < OVERFLOW_CAP + 1; i++) {
		item.u.cancel_handle.generation = (uint64_t)i;
		inject_queue_push_release(a->q, &item);
	}
	return (NULL);
}

static void *
overflow_consumer(void *arg)
{
	overflow_args_t  *a = arg;
	inject_item_t     item;
	struct timespec   ts;
	int               got;

	/* Sleep to allow producer to fill queue and block on overflow. */
	ts.tv_sec  = 0;
	ts.tv_nsec = 20000000L; /* 20 ms */
	nanosleep(&ts, NULL);

	got = 0;
	while (got < OVERFLOW_CAP + 1) {
		if (inject_queue_pop_acquire(a->q, &item)) {
			got++;
			atomic_fetch_add(&a->items_consumed, 1);
		}
	}
	return (NULL);
}

static int
test_inject_queue_overflow_backoff(void)
{
	strand_inject_queue_t  q;
	overflow_args_t        args;
	pthread_t              producer, consumer;
	int                    total;

	if (inject_queue_init(&q, OVERFLOW_CAP) != 0)
		return (1);

	atomic_init(&args.items_consumed, 0);
	args.q = &q;

	pthread_create(&producer, NULL, overflow_producer, &args);
	pthread_create(&consumer, NULL, overflow_consumer, &args);

	pthread_join(producer, NULL);
	pthread_join(consumer, NULL);

	total = atomic_load(&args.items_consumed);
	inject_queue_destroy(&q);
	return (total == OVERFLOW_CAP + 1) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * Suite runner
 * -------------------------------------------------------------------------
 */
void
run_layer4_tests(void)
{
	RUN("test_inject_push_pop_basic",        test_inject_push_pop_basic);
	RUN("test_inject_fill_and_drain",        test_inject_fill_and_drain);
	RUN("test_inject_discard_all",           test_inject_discard_all);
	RUN("test_inject_capacity_is_power_of_two",
	    test_inject_capacity_is_power_of_two);
	RUN("test_inject_drain_delivers_cancel", test_inject_drain_delivers_cancel);
	RUN("test_inject_queue_overflow_backoff",
	    test_inject_queue_overflow_backoff);
}

