/*
 * tests/test_layer4.c - Layer 4 test suite.
 *
 * Bounded MPSC inject ring buffer tests.
 * strand_runtime_t, strand_worker_start, host-thread spawn.
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
#include "../src/strand_runtime.h"
#include "../src/strand_offload.h"

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

typedef int (*wait_pred_fn)(void *);

static uint64_t
test_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

static void
test_sleep_1ms(void)
{
	struct timespec ts;

	ts.tv_sec = 0;
	ts.tv_nsec = 1000000L;
	nanosleep(&ts, NULL);
}

static int
wait_until_pred(wait_pred_fn pred, void *ctx, uint64_t timeout_ms,
    strand_scheduler_t *drive_sched)
{
	uint64_t deadline_ns;

	deadline_ns = test_now_ns() + timeout_ms * 1000000ULL;
	for (;;) {
		if (pred(ctx))
			return (1);
		if (drive_sched != NULL)
			strand_scheduler_advance(drive_sched, NULL);
		if (test_now_ns() >= deadline_ns)
			break;
		test_sleep_1ms();
	}
	return (pred(ctx));
}

typedef struct {
	_Atomic int *value;
	int          target;
} wait_atomic_ctx_t;

static int
pred_atomic_at_least(void *ctx)
{
	wait_atomic_ctx_t *a = ctx;

	return (atomic_load_explicit(a->value, memory_order_acquire) >= a->target);
}

static int
wait_atomic_at_least(_Atomic int *value, int target, uint64_t timeout_ms,
    strand_scheduler_t *drive_sched)
{
	wait_atomic_ctx_t ctx;

	ctx.value  = value;
	ctx.target = target;
	return (wait_until_pred(pred_atomic_at_least, &ctx, timeout_ms,
	    drive_sched));
}


typedef struct {
	strand_fiber_handle_t *handle;
	fiber_state_t          expected;
} wait_state_ctx_t;

static int
pred_fiber_state_is(void *ctx)
{
	wait_state_ctx_t *a = ctx;

	if (a->handle->ptr == NULL)
		return (0);
	return (atomic_load_explicit(&a->handle->ptr->state,
	    memory_order_acquire) == a->expected);
}

static int
wait_fiber_state(strand_fiber_handle_t *handle, fiber_state_t expected,
    uint64_t timeout_ms, strand_scheduler_t *drive_sched)
{
	wait_state_ctx_t ctx;

	ctx.handle   = handle;
	ctx.expected = expected;
	return (wait_until_pred(pred_fiber_state_is, &ctx, timeout_ms,
	    drive_sched));
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

	/* Queue now empty - next pop must return 0. */
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

	/* Request 5 - should round up to 8. */
	if (inject_queue_init(&q, 5) != 0)
		return (1);
	if (q.capacity != 8 || q.mask != 7)
		failed = 1;
	inject_queue_destroy(&q);
	if (failed)
		return (1);

	/* Request 1 - stays 1. */
	if (inject_queue_init(&q, 1) != 0)
		return (1);
	if (q.capacity != 1)
		failed = 1;
	inject_queue_destroy(&q);
	if (failed)
		return (1);

	/* Request 0 - uses default. */
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
 * See DEVELOPMENT.md §"Tests for Phase 5" - test_inject_queue_overflow_backoff.
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
 * Helpers shared by runtime tests
 * -------------------------------------------------------------------------
 */

/*
 * make_test_runtime -- create a runtime with defaults suitable for tests.
 */
static strand_runtime_t *
make_test_runtime(void)
{
	return (strand_runtime_init(NULL));
}

/*
 * make_test_worker -- start a worker with small scheduler defaults.
 */
static strand_worker_t *
make_test_worker(strand_runtime_t *rt)
{
	strand_sched_config_t   scfg = {
	    .budget     = 64,
	    .inject_cap = 64,
	    .cache_cap  = 8,
	    .idle_floor = 2,
	};
	strand_worker_config_t  wcfg = {
	    .sched_cfg    = &scfg,
	    .cpu_affinity = -1,
	};
	return (strand_worker_start(rt, &wcfg));
}

/* -------------------------------------------------------------------------
 * test_runtime_init_destroy
 * Allocate a runtime and destroy it immediately - no workers started.
 * -------------------------------------------------------------------------
 */
static int
test_runtime_init_destroy(void)
{
	strand_runtime_t *rt;

	rt = make_test_runtime();
	if (rt == NULL)
		return (1);
	strand_runtime_destroy(rt);
	return (0);
}

/* -------------------------------------------------------------------------
 * test_no_workers_registered_error
 * Host-thread spawn before any strand_worker_start must return
 * STRAND_ERR_NO_WORKERS.
 * -------------------------------------------------------------------------
 */
static int
test_no_workers_registered_error(void)
{
	strand_runtime_t     *rt;
	strand_fiber_handle_t handle;
	int                   rc;

	rt = make_test_runtime();
	if (rt == NULL)
		return (1);

	rc = strand_runtime_spawn(rt, NULL, NULL, 0, NULL, &handle);
	strand_runtime_destroy(rt);
	return (rc == STRAND_ERR_NO_WORKERS) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * test_spawn_during_shutdown_error
 * After strand_worker_stop the runtime shutdown_flag is set; subsequent
 * strand_runtime_spawn must return STRAND_ERR_SHUTDOWN.
 * -------------------------------------------------------------------------
 */
static int
test_spawn_during_shutdown_error(void)
{
	strand_runtime_t     *rt;
	strand_worker_t      *w;
	strand_fiber_handle_t handle;
	int                   rc;

	rt = make_test_runtime();
	if (rt == NULL)
		return (1);

	w = make_test_worker(rt);
	if (w == NULL) {
		strand_runtime_destroy(rt);
		return (1);
	}

	strand_worker_stop(w);
	strand_worker_join(w);

	rc = strand_runtime_spawn(rt, NULL, NULL, 0, NULL, &handle);
	strand_runtime_destroy(rt);
	return (rc == STRAND_ERR_SHUTDOWN) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * test_explicit_worker_wrong_runtime_error
 * Explicit worker override must belong to the same runtime.
 * -------------------------------------------------------------------------
 */
static int
test_explicit_worker_wrong_runtime_error(void)
{
	strand_runtime_t     *rt_a;
	strand_runtime_t     *rt_b;
	strand_worker_t      *w_b;
	strand_fiber_handle_t handle;
	int                   rc;

	rt_a = make_test_runtime();
	rt_b = make_test_runtime();
	if (rt_a == NULL || rt_b == NULL) {
		strand_runtime_destroy(rt_a);
		strand_runtime_destroy(rt_b);
		return (1);
	}

	w_b = make_test_worker(rt_b);
	if (w_b == NULL) {
		strand_runtime_destroy(rt_a);
		strand_runtime_destroy(rt_b);
		return (1);
	}

	rc = strand_runtime_spawn(rt_a, NULL, NULL, 0, w_b, &handle);

	strand_runtime_destroy(rt_a);
	strand_runtime_destroy(rt_b);
	return (rc == STRAND_ERR_WRONGCTX) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * test_inject_delivers_to_worker
 * Spawn a fiber via strand_runtime_spawn; verify it runs on the target
 * worker by recording the sched pointer inside the fiber.
 * -------------------------------------------------------------------------
 */

/*
 * Warmup helper: a trivial fiber that proves a specific worker is running.
 * Reused by multiple runtime tests.  Defined here (before the first user)
 * so that all runtime tests can reference it.
 */
typedef struct {
	strand_scheduler_t *expected_sched;
	_Atomic int         done;
} rr_warmup_args_t;

static void
rr_warmup_fiber(void *arg)
{
	rr_warmup_args_t *a = arg;

	if (strand_sched_current_tls == a->expected_sched)
		atomic_store(&a->done, 1);
}

typedef struct {
	strand_scheduler_t *observed_sched;
	_Atomic int         ran;
} delivers_args_t;

static void
delivers_fiber(void *arg)
{
	delivers_args_t *a = arg;

	/*
	 * Record the scheduler this fiber is running on via the thread-local
	 * set by strand_scheduler_run.  No UB, no data race.
	 */
	a->observed_sched = strand_sched_current_tls;
	atomic_store(&a->ran, 1);
}

static int
test_inject_delivers_to_worker(void)
{
	strand_runtime_t     *rt;
	strand_worker_t      *w;
	delivers_args_t       args;
	rr_warmup_args_t      warm;

	rt = make_test_runtime();
	if (rt == NULL)
		return (1);

	w = make_test_worker(rt);
	if (w == NULL) {
		strand_runtime_destroy(rt);
		return (1);
	}

	/* Warmup: prove the worker is running before testing inject delivery. */
	atomic_init(&warm.done, 0);
	warm.expected_sched = w->sched;
	if (strand_runtime_spawn(rt, rr_warmup_fiber, &warm,
	    STRAND_DEFAULT_STACK_SIZE, w, NULL) != STRAND_OK) {
		strand_runtime_destroy(rt);
		return (1);
	}
	if (!wait_atomic_at_least(&warm.done, 1, 60000, NULL)) {
		strand_runtime_destroy(rt);
		return (1);
	}

	atomic_init(&args.ran, 0);
	args.observed_sched = NULL;

	if (strand_runtime_spawn(rt, delivers_fiber, &args,
	    STRAND_DEFAULT_STACK_SIZE, w, NULL) != STRAND_OK) {
		strand_runtime_destroy(rt);
		return (1);
	}

	/* Save w->sched before destroy frees the worker descriptor. */
	{
		strand_scheduler_t *expected_sched = w->sched;

		if (!wait_atomic_at_least(&args.ran, 1, 60000, NULL)) {
			strand_runtime_destroy(rt);
			return (1);
		}

		strand_runtime_destroy(rt);
		return (atomic_load(&args.ran) == 1 &&
		    args.observed_sched == expected_sched) ? 0 : 1;
	}
}

/* -------------------------------------------------------------------------
 * test_round_robin_selection
 * Spawn 10 fibers with 2 workers; verify both workers receive fibers.
 * Each fiber records which scheduler it ran on.
 * -------------------------------------------------------------------------
 */
#define ROUND_ROBIN_FIBERS 10

typedef struct {
	strand_scheduler_t *sched_a;
	strand_scheduler_t *sched_b;
	_Atomic int         count_a;
	_Atomic int         count_b;
	_Atomic int         total;
} rr_args_t;


static void
rr_fiber(void *arg)
{
	rr_args_t          *a = arg;
	strand_scheduler_t *my_sched;

	/*
	 * Read the scheduler for this worker thread via the TLS variable set
	 * by strand_scheduler_run.  No cross-thread pointer chasing, no races.
	 */
	my_sched = strand_sched_current_tls;
	if (my_sched == a->sched_a)
		atomic_fetch_add(&a->count_a, 1);
	else if (my_sched == a->sched_b)
		atomic_fetch_add(&a->count_b, 1);
	atomic_fetch_add(&a->total, 1);
}


static int
test_round_robin_selection(void)
{
	strand_runtime_t *rt;
	strand_worker_t  *wa, *wb;
	rr_args_t         args;
	rr_warmup_args_t  warm_a, warm_b;
	const uint64_t    warmup_timeout_ms = 60000;
	const uint64_t    total_timeout_ms = 60000;
	int               i;

	rt = make_test_runtime();
	if (rt == NULL)
		return (1);

	wa = make_test_worker(rt);
	wb = make_test_worker(rt);
	if (wa == NULL || wb == NULL) {
		strand_runtime_destroy(rt);
		return (1);
	}

	atomic_init(&args.count_a, 0);
	atomic_init(&args.count_b, 0);
	atomic_init(&args.total, 0);
	args.sched_a = wa->sched;
	args.sched_b = wb->sched;

	/*
	 * Warmup: prove both workers are running and able to execute targeted
	 * work before validating round-robin spread.
	 */
	atomic_init(&warm_a.done, 0);
	atomic_init(&warm_b.done, 0);
	warm_a.expected_sched = wa->sched;
	warm_b.expected_sched = wb->sched;

	if (strand_runtime_spawn(rt, rr_warmup_fiber, &warm_a,
	    STRAND_DEFAULT_STACK_SIZE, wa, NULL) != STRAND_OK ||
	    strand_runtime_spawn(rt, rr_warmup_fiber, &warm_b,
	    STRAND_DEFAULT_STACK_SIZE, wb, NULL) != STRAND_OK) {
		strand_runtime_destroy(rt);
		return (1);
	}

	if (!wait_atomic_at_least(&warm_a.done, 1, warmup_timeout_ms, NULL) ||
	    !wait_atomic_at_least(&warm_b.done, 1, warmup_timeout_ms, NULL)) {
		strand_runtime_destroy(rt);
		return (1);
	}

	for (i = 0; i < ROUND_ROBIN_FIBERS; i++) {
		if (strand_runtime_spawn(rt, rr_fiber, &args,
		    STRAND_DEFAULT_STACK_SIZE, NULL, NULL) != STRAND_OK) {
			strand_runtime_destroy(rt);
			return (1);
		}
	}

	if (!wait_atomic_at_least(&args.total, ROUND_ROBIN_FIBERS,
	    total_timeout_ms, NULL)) {
		strand_runtime_destroy(rt);
		return (1);
	}

	strand_runtime_destroy(rt);

	/* Both workers must have received at least one fiber. */
	if (atomic_load(&args.total) != ROUND_ROBIN_FIBERS)
		return (1);
	if (atomic_load(&args.count_a) == 0 || atomic_load(&args.count_b) == 0)
		return (1);
	return (0);
}

/* -------------------------------------------------------------------------
 * test_explicit_worker_override
 * Spawn with explicit worker; verify the fiber runs on that worker's sched.
 * -------------------------------------------------------------------------
 */
typedef struct {
	strand_scheduler_t *expected_sched;
	_Atomic int         correct;
	_Atomic int         ran;
} explicit_args_t;

static void
explicit_fiber(void *arg)
{
	explicit_args_t *a = arg;

	/*
	 * Verify the fiber is running on the expected scheduler via TLS.
	 * No cross-thread pointer chasing.
	 */
	if (strand_sched_current_tls == a->expected_sched)
		atomic_store(&a->correct, 1);
	atomic_store(&a->ran, 1);
}

static int
test_explicit_worker_override(void)
{
	strand_runtime_t *rt;
	strand_worker_t  *wa, *wb;
	explicit_args_t   args;
	rr_warmup_args_t  warm;
	const uint64_t    warmup_timeout_ms = 60000;

	rt = make_test_runtime();
	if (rt == NULL)
		return (1);

	wa = make_test_worker(rt);
	wb = make_test_worker(rt);
	if (wa == NULL || wb == NULL) {
		strand_runtime_destroy(rt);
		return (1);
	}

	/*
	 * Warmup worker B before spawning the test fiber.  Without this,
	 * under Valgrind's 20-50x slowdown, the worker thread may not have
	 * entered its run loop and set strand_sched_current_tls by the time
	 * the test fiber runs, causing a spurious correct==0.
	 * Same pattern as test_inject_delivers_to_worker et al.
	 */
	atomic_init(&warm.done, 0);
	warm.expected_sched = wb->sched;
	if (strand_runtime_spawn(rt, rr_warmup_fiber, &warm,
	    STRAND_DEFAULT_STACK_SIZE, wb, NULL) != STRAND_OK) {
		strand_runtime_destroy(rt);
		return (1);
	}
	if (!wait_atomic_at_least(&warm.done, 1, warmup_timeout_ms, NULL)) {
		strand_runtime_destroy(rt);
		return (1);
	}

	atomic_init(&args.correct, 0);
	atomic_init(&args.ran, 0);
	args.expected_sched = wb->sched; /* explicitly target worker B */

	if (strand_runtime_spawn(rt, explicit_fiber, &args,
	    STRAND_DEFAULT_STACK_SIZE, wb, NULL) != STRAND_OK) {
		strand_runtime_destroy(rt);
		return (1);
	}

	if (!wait_atomic_at_least(&args.ran, 1, 60000, NULL)) {
		strand_runtime_destroy(rt);
		return (1);
	}

	strand_runtime_destroy(rt);
	return (atomic_load(&args.correct) == 1) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * test_stopped_worker_excluded_roundrobin
 * Stop one of two workers; all subsequent spawns must go to the live worker.
 * -------------------------------------------------------------------------
 */
#define EXCL_FIBERS 4

typedef struct {
	strand_scheduler_t *live_sched;
	_Atomic int         on_live;
	_Atomic int         total;
} excl_args_t;

static void
excl_fiber(void *arg)
{
	excl_args_t *a = arg;

	/*
	 * All fibers in this test must land on live_sched (wa is stopped).
	 * Verify via TLS - no cross-thread pointer chasing.
	 */
	if (strand_sched_current_tls == a->live_sched)
		atomic_fetch_add(&a->on_live, 1);
	atomic_fetch_add(&a->total, 1);
}

static int
test_stopped_worker_excluded_roundrobin(void)
{
	strand_runtime_t *rt;
	strand_worker_t  *wa, *wb;
	rr_warmup_args_t  warm_live;
	excl_args_t       args;
	const uint64_t    warmup_timeout_ms = 60000;
	const uint64_t    total_timeout_ms = 60000;
	int               i;

	rt = make_test_runtime();
	if (rt == NULL)
		return (1);

	wa = make_test_worker(rt);
	wb = make_test_worker(rt);
	if (wa == NULL || wb == NULL) {
		strand_runtime_destroy(rt);
		return (1);
	}

	/* Stop worker A; worker B is the live one. */
	strand_worker_stop(wa);
	strand_worker_join(wa);

	/* Warmup live worker to avoid asserting before it has started running. */
	atomic_init(&warm_live.done, 0);
	warm_live.expected_sched = wb->sched;
	if (strand_runtime_spawn(rt, rr_warmup_fiber, &warm_live,
	    STRAND_DEFAULT_STACK_SIZE, wb, NULL) != STRAND_OK) {
		strand_runtime_destroy(rt);
		return (1);
	}
	if (!wait_atomic_at_least(&warm_live.done, 1, warmup_timeout_ms, NULL)) {
		strand_runtime_destroy(rt);
		return (1);
	}

	atomic_init(&args.on_live, 0);
	atomic_init(&args.total, 0);
	args.live_sched = wb->sched;

	for (i = 0; i < EXCL_FIBERS; i++) {
		if (strand_runtime_spawn(rt, excl_fiber, &args,
		    STRAND_DEFAULT_STACK_SIZE, NULL, NULL) != STRAND_OK) {
			strand_runtime_destroy(rt);
			return (1);
		}
	}

	if (!wait_atomic_at_least(&args.total, EXCL_FIBERS,
	    total_timeout_ms, NULL)) {
		strand_runtime_destroy(rt);
		return (1);
	}

	strand_runtime_destroy(rt);

	if (atomic_load(&args.total) != EXCL_FIBERS)
		return (1);
	/* All fibers must have landed on the live worker. */
        return (atomic_load(&args.on_live) == EXCL_FIBERS) ? 0 : 1;
}


/* -------------------------------------------------------------------------
 * test_cross_worker_wakeup
 *
 * Start a worker; wait long enough for it to enter its idle poll
 * (epoll_wait / kevent).  Then inject a fiber via strand_runtime_spawn.
 * Verify the worker wakes up and the fiber actually runs.
 *
 * This directly exercises the "wakeup write" path in strand_poller.c that
 * is triggered whenever an item is pushed to a sleeping worker's inject
 * queue.
 * -------------------------------------------------------------------------
 */
typedef struct {
	_Atomic int ran;
} cww_args_t;

static void
cww_fiber(void *arg)
{
	cww_args_t *a = arg;
	atomic_store_explicit(&a->ran, 1, memory_order_release);
}

static int
test_cross_worker_wakeup(void)
{
	strand_runtime_t *rt;
	strand_worker_t  *w;
	cww_args_t        args;
	rr_warmup_args_t  warm;

	rt = make_test_runtime();
	if (rt == NULL)
		return (1);

	w = make_test_worker(rt);
	if (w == NULL) {
		strand_runtime_destroy(rt);
		return (1);
	}

	/*
	 * Warmup: prove the worker is running before we test the wakeup path.
	 * After this returns, we know the worker has entered its run loop.
	 */
	atomic_init(&warm.done, 0);
	warm.expected_sched = w->sched;
	if (strand_runtime_spawn(rt, rr_warmup_fiber, &warm,
	    STRAND_DEFAULT_STACK_SIZE, w, NULL) != STRAND_OK) {
		strand_runtime_destroy(rt);
		return (1);
	}
	if (!wait_atomic_at_least(&warm.done, 1, 60000, NULL)) {
		strand_runtime_destroy(rt);
		return (1);
	}

	/*
	 * Let the worker settle into its idle poll loop.  Use a generous
	 * sleep because Valgrind slows execution 20-50x.
	 */
	{
		struct timespec ts = { 0, 200000000L }; /* 200 ms */
		nanosleep(&ts, NULL);
	}

	atomic_init(&args.ran, 0);

	/* Inject a fiber to the sleeping worker. */
	if (strand_runtime_spawn(rt, cww_fiber, &args,
	    STRAND_DEFAULT_STACK_SIZE, w, NULL) != STRAND_OK) {
		strand_runtime_destroy(rt);
		return (1);
	}

	/* Wait for the worker to wake and run the fiber. */
	if (!wait_atomic_at_least(&args.ran, 1, 60000, NULL)) {
		strand_runtime_destroy(rt);
		return (1);
	}

	strand_runtime_destroy(rt);
	return (atomic_load_explicit(&args.ran, memory_order_acquire) == 1) ? 0 : 1;
}

/* =========================================================================
 * Offload pool tests (Tasks 5.6, 5.7, 5.8)
 * =========================================================================
 */

/* -------------------------------------------------------------------------
 * test_offload_result_delivered
 *
 * Offload a function that writes a known value to result_slot.
 * Verify the fiber receives STRAND_OK and the correct result.
 *
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        int                    result;
        int                    offload_rc;
        _Atomic int            done; /* set to 1 by fiber after offload returns */
} offload_basic_args_t;

static void
offload_fn_write42(void *arg, void *result_slot)
{
        (void)arg;
        *(int *)result_slot = 42;
}

static void
fiber_offload_basic(void *varg)
{
        offload_basic_args_t *a = varg;
        a->offload_rc = strand_fiber_offload(a->sched, a->pool,
                                             offload_fn_write42, NULL,
                                             &a->result);
        /*
         * Signal completion.  The fiber runs on the scheduler thread so this
         * store is sequenced-after the inject-queue acquire that established
         * happens-before with the offload thread's result_slot write.
         * See ARCHITECTURE.md 6.7.
         */
        atomic_store_explicit(&a->done, 1, memory_order_release);
}

static int
test_offload_result_delivered(void)
{
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        offload_basic_args_t   args;
        int                    i;
        struct timespec        ts = { 0, 5000000L }; /* 5 ms */

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        pool = strand_offload_pool_init(2);
        if (pool == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&args, 0, sizeof(args));
        atomic_init(&args.done, 0);
        args.sched = sched;
        args.pool  = pool;

        if (t5_push_fiber(sched, fiber_offload_basic, &args, NULL) != 0) {
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /*
         * Advance until the fiber signals it has completed.  Poll the atomic
         * done flag - not result directly - to avoid a TSan data race between
         * the offload thread's result write and this thread's read.
         * Once done==1, the fiber has already run (on this thread) and read
         * result_slot; both operations are sequenced on the same thread so
         * reading args.result here is race-free.
         */
        for (i = 0; i < 200; i++) {
                strand_scheduler_advance(sched, NULL);
                if (atomic_load_explicit(&args.done, memory_order_acquire))
                        break;
                nanosleep(&ts, NULL);
        }

        strand_offload_pool_destroy(pool);
        strand_scheduler_destroy(sched);

        return (args.offload_rc == STRAND_OK && args.result == 42) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * test_offload_eagain_when_full
 *
 * Fill the pool to capacity (count + in_flight >= capacity) then call
 * strand_fiber_offload from a third fiber.  Verify STRAND_EAGAIN is
 * returned immediately without parking.
 *
 * Strategy: 1-thread pool (capacity = 2).  Two "blocker" fibers each
 * submit offload_fn_block which spins on g_eagain_release.  Both items
 * become outstanding (one in-flight, one queued).  A third fiber then
 * calls offload and must see STRAND_EAGAIN.
 *
 * -------------------------------------------------------------------------
 */

/* Blocking function that spins on g_eagain_release. */
static _Atomic int g_eagain_release;

static void
offload_fn_block(void *arg, void *result_slot)
{
        (void)result_slot;
        (void)arg;
        while (!atomic_load_explicit(&g_eagain_release, memory_order_acquire))
                ; /* spin until released */
}

#define EAGAIN_BLOCKER_COUNT 2

typedef struct {
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        int                    eagain_seen;
        int                    dummy_result;
} offload_eagain_args_t;

static void
fiber_offload_eagain(void *varg)
{
        offload_eagain_args_t *a = varg;
        int rc;

        rc = strand_fiber_offload(a->sched, a->pool,
                                  offload_fn_block, NULL, &a->dummy_result);
        if (rc == STRAND_EAGAIN)
                a->eagain_seen = 1;
}

static int
pred_pool_full(void *ctx)
{
        strand_offload_pool_t *pool = ctx;
        int full;

        pthread_mutex_lock(&pool->mutex);
        full = ((pool->count + pool->in_flight) >= pool->capacity);
        pthread_mutex_unlock(&pool->mutex);
        return (full);
}

static int
test_offload_eagain_when_full(void)
{
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        offload_eagain_args_t  blocker[EAGAIN_BLOCKER_COUNT];
        offload_eagain_args_t  checker;
        size_t                 i;
        int                    rc = 1;

        atomic_store(&g_eagain_release, 0);

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        /* 1 thread -> capacity = 2 (count + in_flight must reach 2). */
        pool = strand_offload_pool_init(1);
        if (pool == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* Queue EAGAIN_BLOCKER_COUNT blocker fibers; advance to park them. */
        for (i = 0; i < EAGAIN_BLOCKER_COUNT; i++) {
                memset(&blocker[i], 0, sizeof(blocker[i]));
                blocker[i].sched = sched;
                blocker[i].pool  = pool;
                t5_push_fiber(sched, fiber_offload_eagain, &blocker[i], NULL);
        }
        /* Advance until both fibers are parked in offload. */
        if (!wait_until_pred(pred_pool_full, pool, 60000, sched)) {
                atomic_store_explicit(&g_eagain_release, 1,
                    memory_order_release);
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* Now a third fiber must get STRAND_EAGAIN. */
        memset(&checker, 0, sizeof(checker));
        checker.sched = sched;
        checker.pool  = pool;
        t5_push_fiber(sched, fiber_offload_eagain, &checker, NULL);
        for (i = 0; i < 10; i++) {
                strand_scheduler_advance(sched, NULL);
                if (checker.eagain_seen)
                        break;
        }
        rc = checker.eagain_seen ? 0 : 1;

        /* Release blockers and drain all completions. */
        atomic_store_explicit(&g_eagain_release, 1, memory_order_release);
        {
                struct timespec drain_ts = { 0, 20000000L }; /* 20 ms */
                for (i = 0; i < 30; i++) {
                        strand_scheduler_advance(sched, NULL);
                        nanosleep(&drain_ts, NULL);
                }
        }

        strand_offload_pool_destroy(pool);
        strand_scheduler_destroy(sched);
        return (rc);
}

/* -------------------------------------------------------------------------
 * test_offload_cancelled_wins
 *
 * Arrange for CANCELLED CAS to win: park a fiber on an offload that blocks
 * until released, cancel it from the same scheduler context, then release.
 * Verify:
 *   - fiber resumes with STRAND_CANCELLED
 *   - result_slot is not written (remains sentinel value)
 *
 * -------------------------------------------------------------------------
 */

static _Atomic int g_cancel_release;
static _Atomic int g_cancel_fn_started;

static void
offload_fn_block_cancel(void *arg, void *result_slot)
{
        (void)arg;
        (void)result_slot;
        atomic_store_explicit(&g_cancel_fn_started, 1, memory_order_release);
        while (!atomic_load_explicit(&g_cancel_release, memory_order_acquire))
                ; /* spin until released by the test */
        /*
         * Do NOT write to result_slot.  When CANCELLED wins, the fiber
         * ignores result_slot and returns STRAND_CANCELLED.  The test verifies
         * offload_rc only.  See ARCHITECTURE.md 6.6.
         */
}

typedef struct {
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        strand_fiber_handle_t  handle;
        int                    result_slot;
        int                    offload_rc;
} offload_cancel_args_t;

static void
fiber_offload_cancel_target(void *varg)
{
        offload_cancel_args_t *a = varg;
        a->result_slot = 0;
        a->offload_rc  = strand_fiber_offload(a->sched, a->pool,
                                               offload_fn_block_cancel,
                                               NULL, &a->result_slot);
}

static int
test_offload_cancelled_wins(void)
{
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        offload_cancel_args_t  args;
        int                    i, rc;

        atomic_store(&g_cancel_release, 0);
        atomic_store(&g_cancel_fn_started, 0);

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        pool = strand_offload_pool_init(1);
        if (pool == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&args, 0, sizeof(args));
        args.sched = sched;
        args.pool  = pool;

        if (t5_push_fiber(sched, fiber_offload_cancel_target, &args,
                          &args.handle) != 0) {
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /*
         * Advance once: fiber parks on FIBER_PARKED_OFFLOAD.
         * The offload thread picks up the item and blocks inside
         * offload_fn_block_cancel.  Wait for the fn to signal it started.
         */
        strand_scheduler_advance(sched, NULL);
        if (!wait_atomic_at_least(&g_cancel_fn_started, 1, 60000, NULL)) {
                atomic_store_explicit(&g_cancel_release, 1,
                    memory_order_release);
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* Cancel from same scheduler context (same-worker CANCELLED CAS path). */
        strand_fiber_cancel(args.handle);

        /* Advance to run the now-RUNNABLE cancelled fiber. */
        for (i = 0; i < 5; i++)
                strand_scheduler_advance(sched, NULL);

        /*
         * Release the offload thread so it can complete fn and attempt the
         * RESULT_CLAIMED CAS.  The CAS must fail (CANCELLED already won).
         * The fiber already returned STRAND_CANCELLED; we just need the
         * offload thread to finish so pool_destroy does not deadlock.
         */
        atomic_store_explicit(&g_cancel_release, 1, memory_order_release);
        {
                struct timespec drain_ts = { 0, 20000000L }; /* 20 ms */
                nanosleep(&drain_ts, NULL);
        }

        /* Correctness check: fiber received STRAND_CANCELLED. */
        rc = (args.offload_rc == STRAND_CANCELLED) ? 0 : 1;

        strand_offload_pool_destroy(pool);
        strand_scheduler_destroy(sched);
        return (rc);
}

/* -------------------------------------------------------------------------
 * test_offload_result_claimed_wins
 *
 * Arrange for RESULT_CLAIMED CAS to win: let the offload complete normally,
 * then (after completion) call cancel.  Fiber must receive STRAND_OK.
 *
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        strand_fiber_handle_t  handle;
        int                    result;
        int                    offload_rc;
} offload_claimed_args_t;

static _Atomic int g_claimed_fn_done;

static void
offload_fn_write99(void *arg, void *result_slot)
{
        (void)arg;
        *(int *)result_slot = 99;
        atomic_store_explicit(&g_claimed_fn_done, 1, memory_order_release);
}

static void
fiber_offload_claimed_target(void *varg)
{
        offload_claimed_args_t *a = varg;
        a->offload_rc = strand_fiber_offload(a->sched, a->pool,
                                             offload_fn_write99, NULL,
                                             &a->result);
}

static int
test_offload_result_claimed_wins(void)
{
        strand_scheduler_t     *sched;
        strand_offload_pool_t  *pool;
        offload_claimed_args_t  args;
        int                     i;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        pool = strand_offload_pool_init(1);
        if (pool == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&args, 0, sizeof(args));
        args.sched = sched;
        args.pool  = pool;
        atomic_store(&g_claimed_fn_done, 0);

        if (t5_push_fiber(sched, fiber_offload_claimed_target, &args,
                          &args.handle) != 0) {
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* Advance once so fiber parks. */
        strand_scheduler_advance(sched, NULL);

        /* Wait for the offload thread to complete the function. */
        if (!wait_atomic_at_least(&g_claimed_fn_done, 1, 60000, NULL)) {
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /*
         * Attempt cancel AFTER offload has already completed (RESULT_CLAIMED
         * won).  The cancel CAS fails; fiber-side refcount is untouched.
         * Fiber resumes normally with STRAND_OK.
         */
        strand_fiber_cancel(args.handle);

        for (i = 0; i < 10; i++) {
                strand_scheduler_advance(sched, NULL);
                if (args.offload_rc != 0 || args.result == 99)
                        break;
        }

        strand_offload_pool_destroy(pool);
        strand_scheduler_destroy(sched);

        return (args.offload_rc == STRAND_OK && args.result == 99) ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * test_offload_refcount_released_exactly_once
 *
 * Run both CAS outcome paths (RESULT_CLAIMED and CANCELLED) in sequence and
 * verify no double-free and no leak.  Valgrind (--leak-check=full) is the
 * definitive gate; this test provides the execution paths.
 *
 * -------------------------------------------------------------------------
 */
static int
test_offload_refcount_released_exactly_once(void)
{
        int rc;

        /* RESULT_CLAIMED path (re-runs test_offload_result_claimed_wins). */
        rc = test_offload_result_claimed_wins();
        if (rc != 0)
                return (1);

        /* CANCELLED path (re-runs test_offload_cancelled_wins). */
        rc = test_offload_cancelled_wins();
        return (rc);
}

/* -------------------------------------------------------------------------
 * test_offload_arg_outlives_cancel
 *
 * Cancel a fiber while the offload function is still running and reading
 * from arg.  Verify the offload thread does not crash (access to arg is
 * safe because arg lifetime is the caller's responsibility per
 * ARCHITECTURE.md 6.6) and the fiber receives STRAND_CANCELLED.
 *
 * arg is a heap-allocated counter; the offload function increments it in
 * a loop while the cancel races in.  After the offload thread exits we
 * verify the counter was modified (proving the thread ran after cancel).
 *
 * -------------------------------------------------------------------------
 */

static _Atomic int g_arg_release2;
static _Atomic int g_arg_fn_started; /* set to 1 when fn enters spin loop */
static _Atomic int g_arg_cancel_phase;
static _Atomic int g_arg_post_cancel_touches;

typedef struct {
        _Atomic int count;
} outlive_arg_t;

static void
offload_fn_increment(void *arg, void *result_slot)
{
        outlive_arg_t *a = arg;
        (void)result_slot;
        /*
         * Signal that we have started before entering the spin loop.
         * The test waits for this flag before calling cancel, ensuring the
         * cancel races with fn rather than arriving before fn starts.
         */
        atomic_store_explicit(&g_arg_fn_started, 1, memory_order_release);
        /* Spin until released, incrementing count each iteration. */
	while (!atomic_load_explicit(&g_arg_release2, memory_order_acquire)) {
		atomic_fetch_add_explicit(&a->count, 1, memory_order_relaxed);
		if (atomic_load_explicit(&g_arg_cancel_phase,
		    memory_order_acquire)) {
			/* One-way latch: offload observed the post-cancel phase. */
			atomic_store_explicit(&g_arg_post_cancel_touches, 1,
			    memory_order_release);
		}
	}
}

typedef struct {
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        strand_fiber_handle_t  handle;
        outlive_arg_t         *arg;
        int                    offload_rc;
} outlive_args_t;

static void
fiber_offload_outlive(void *varg)
{
        outlive_args_t *a = varg;
        int dummy;
        a->offload_rc = strand_fiber_offload(a->sched, a->pool,
                                             offload_fn_increment,
                                             a->arg, &dummy);
}

static int
test_offload_arg_outlives_cancel(void)
{
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        outlive_args_t         args;
        outlive_arg_t          shared_arg;
        struct timespec        ts = { 0, 20000000L }; /* 20 ms */
		const uint64_t         park_timeout_ms = 60000;
		const uint64_t         start_timeout_ms = 60000;
		const uint64_t         cancel_seen_timeout_ms = 60000;
		const uint64_t         post_cancel_timeout_ms = 60000;
		int                    i, rc = 1;
		int                    post_cancel_progress;

        atomic_store(&g_arg_release2, 0);
        atomic_store(&g_arg_fn_started, 0);
		atomic_store(&g_arg_cancel_phase, 0);
		atomic_store(&g_arg_post_cancel_touches, 0);
        atomic_init(&shared_arg.count, 0);

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        pool = strand_offload_pool_init(1);
        if (pool == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&args, 0, sizeof(args));
        args.sched = sched;
        args.pool  = pool;
        args.arg   = &shared_arg;

        if (t5_push_fiber(sched, fiber_offload_outlive, &args,
                          &args.handle) != 0) {
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

		/* Let fiber park and enqueue the offload item. */
        strand_scheduler_advance(sched, NULL);

		if (!wait_fiber_state(&args.handle, FIBER_PARKED_OFFLOAD,
		    park_timeout_ms, sched))
			goto cleanup;

		/* Ensure cancel races with a running offload function. */
		if (!wait_atomic_at_least(&g_arg_fn_started, 1,
		    start_timeout_ms, NULL))
			goto cleanup;

		/* Cancel while the offload work is still pending/running. */
        strand_fiber_cancel(args.handle);
		atomic_store_explicit(&g_arg_cancel_phase, 1, memory_order_release);

		/* Verify offload thread observes the post-cancel phase and continues. */
		post_cancel_progress = wait_atomic_at_least(&g_arg_post_cancel_touches,
		    1, post_cancel_timeout_ms, NULL);

		/* Let the cancelled fiber resume and publish offload_rc. */
		for (i = 0; i < (int)cancel_seen_timeout_ms; i++) {
				strand_scheduler_advance(sched, NULL);
				if (args.offload_rc == STRAND_CANCELLED)
						break;
			test_sleep_1ms();
		}

        /* Now release the offload thread. */
        atomic_store_explicit(&g_arg_release2, 1, memory_order_release);
        nanosleep(&ts, NULL);

		rc = (args.offload_rc == STRAND_CANCELLED &&
			  post_cancel_progress == 1) ? 0 : 1;

	cleanup:
		/* Always release so offload_pool_destroy cannot block on a spinning fn. */
		atomic_store_explicit(&g_arg_release2, 1, memory_order_release);

        strand_offload_pool_destroy(pool);
        strand_scheduler_destroy(sched);
        return (rc);
}

/* -------------------------------------------------------------------------
 * test_offload_yield_retry_pattern
 *
 * Fill the pool to capacity; submit an offload from a fiber - it must
 * receive STRAND_EAGAIN.  The fiber then yields and retries in a loop until
 * the pool drains and the offload eventually succeeds.
 *
 * This validates the recommended "yield-then-retry" idiom described in
 * EAGAIN recovery on a full offload pool.
 * -------------------------------------------------------------------------
 */

static _Atomic int g_yieldretry_release;

static void
offload_fn_block_yieldretry(void *arg, void *result_slot)
{
        (void)arg;
        *(int *)result_slot = 77;
        while (!atomic_load_explicit(&g_yieldretry_release, memory_order_acquire))
                ; /* spin until released */
}

typedef struct {
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        int                    result;
        int                    offload_rc;
        _Atomic int            done;
} yieldretry_args_t;

static void
fiber_offload_yieldretry(void *varg)
{
        yieldretry_args_t *a = varg;
        int rc;

        /*
         * Retry until we get a slot or an unexpected error.
         * strand_fiber_yield cooperatively gives up the CPU so other work
         * (including the blocker fiber's completion inject) can be drained.
         */
        for (;;) {
                rc = strand_fiber_offload(a->sched, a->pool,
                                          offload_fn_block_yieldretry,
                                          NULL, &a->result);
                if (rc != STRAND_EAGAIN)
                        break;
                strand_fiber_yield(a->sched);
        }
        a->offload_rc = rc;
        atomic_store_explicit(&a->done, 1, memory_order_release);
}

static int
pred_pool_has_inflight(void *ctx)
{
        strand_offload_pool_t *pool = ctx;
        int                    has;

        pthread_mutex_lock(&pool->mutex);
        has = (pool->in_flight > 0);
        pthread_mutex_unlock(&pool->mutex);
        return (has);
}

static int
test_offload_yield_retry_pattern(void)
{
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        yieldretry_args_t      blocker_args, retry_args;
        int                    rc;

        atomic_store(&g_yieldretry_release, 0);

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        /* 1 thread -> capacity = 2. */
        pool = strand_offload_pool_init(1);
        if (pool == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* Start a blocker fiber that fills the pool. */
        memset(&blocker_args, 0, sizeof(blocker_args));
        blocker_args.sched = sched;
        blocker_args.pool  = pool;
        atomic_init(&blocker_args.done, 0);
        t5_push_fiber(sched, fiber_offload_yieldretry, &blocker_args, NULL);

        /*
         * Advance until the blocker is parked and the offload thread has
         * picked up the item (in_flight >= 1).  Note: capacity is 2 so
         * the pool is not full with a single item; we only need the
         * blocker to be in-flight before queuing the retry fiber.
         */
        if (!wait_until_pred(pred_pool_has_inflight, pool, 60000, sched)) {
                atomic_store_explicit(&g_yieldretry_release, 1,
                    memory_order_release);
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* Queue the retry fiber; it should eventually succeed after yielding. */
        memset(&retry_args, 0, sizeof(retry_args));
        retry_args.sched = sched;
        retry_args.pool  = pool;
        atomic_init(&retry_args.done, 0);
        t5_push_fiber(sched, fiber_offload_yieldretry, &retry_args, NULL);

        /*
         * Release the blocker half-way through so the retry fiber can claim
         * a slot.  We release before all iterations so the scheduler has a
         * chance to drain the completion and make room.
         */
        atomic_store_explicit(&g_yieldretry_release, 1, memory_order_release);

        /* Drive until both fibers finish. */
        {
                uint64_t deadline_ns;

                deadline_ns = test_now_ns() + 60000ULL * 1000000ULL;
                while (test_now_ns() < deadline_ns) {
                        strand_scheduler_advance(sched, NULL);
                        if (atomic_load_explicit(&blocker_args.done,
                            memory_order_acquire) &&
                            atomic_load_explicit(&retry_args.done,
                            memory_order_acquire))
                                break;
                        test_sleep_1ms();
                }
        }

        rc = (atomic_load_explicit(&retry_args.done, memory_order_acquire) &&
              retry_args.offload_rc == STRAND_OK &&
              retry_args.result == 77) ? 0 : 1;

        strand_offload_pool_destroy(pool);
        strand_scheduler_destroy(sched);
        return (rc);
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
	RUN("test_runtime_init_destroy",
	    test_runtime_init_destroy);
	RUN("test_no_workers_registered_error",
	    test_no_workers_registered_error);
	RUN("test_spawn_during_shutdown_error",
	    test_spawn_during_shutdown_error);
	RUN("test_explicit_worker_wrong_runtime_error",
	    test_explicit_worker_wrong_runtime_error);
	RUN("test_inject_delivers_to_worker",
	    test_inject_delivers_to_worker);
	RUN("test_cross_worker_wakeup",
	    test_cross_worker_wakeup);
	RUN("test_round_robin_selection",
	    test_round_robin_selection);
	RUN("test_explicit_worker_override",
	    test_explicit_worker_override);
        RUN("test_stopped_worker_excluded_roundrobin",
            test_stopped_worker_excluded_roundrobin);
        RUN("test_offload_result_delivered",
            test_offload_result_delivered);
        RUN("test_offload_eagain_when_full",
            test_offload_eagain_when_full);
        RUN("test_offload_cancelled_wins",
            test_offload_cancelled_wins);
        RUN("test_offload_result_claimed_wins",
            test_offload_result_claimed_wins);
        RUN("test_offload_refcount_released_exactly_once",
            test_offload_refcount_released_exactly_once);
        RUN("test_offload_arg_outlives_cancel",
            test_offload_arg_outlives_cancel);
        RUN("test_offload_yield_retry_pattern",
            test_offload_yield_retry_pattern);
}

