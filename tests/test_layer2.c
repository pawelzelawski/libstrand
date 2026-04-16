/*
 * tests/test_layer2.c — Layer 2 (Phase 3) test suite.
 *
 * Tests are added task by task as Phase 3 progresses.
 * This file currently covers:
 *   Task 3.4: strand_scheduler_run, strand_scheduler_stop,
 *             strand_scheduler_next_deadline, strand_scheduler_get_fd.
 *   Task 3.5: strand_fiber_spawn (within-worker path), advance nonblocking,
 *             FIFO run-queue ordering, budget limiting, shutdown error.
 *   Task 3.6: strand_fiber_yield — verify yield re-queues the fiber.
 *   Task 3.7: strand_fiber_sleep_until — timer fires, timer order.
 *
 * See DEVELOPMENT.md §"Tests for Phase 3".
 */

#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "test_harness.h"
#include "../include/strand.h"
#include "../src/strand_context.h"
#include "../src/strand_fiber.h"
#include "../src/strand_internal.h"
#include "../src/strand_sched.h"

/*
 * strand_test_clock_ns — mock clock used by the scheduler when built with
 * -DSTRAND_TEST_CLOCK.  Tests advance time by writing to this variable
 * instead of sleeping.  See TESTING.md §2.3.
 */
extern uint64_t strand_test_clock_ns;

/* -------------------------------------------------------------------------
 * Shared helpers
 * -------------------------------------------------------------------------
 */

/*
 * make_test_scheduler -- create a scheduler with small defaults suitable
 * for unit tests.  Returns NULL on failure.
 */
static strand_scheduler_t *
make_test_scheduler(void)
{
	strand_sched_config_t cfg = {
	    .budget = 64,
	    .inject_cap = 256,
	    .cache_cap = 8,
	    .idle_floor = 2,
	};
	return (strand_scheduler_create(&cfg));
}

/*
 * wait_for_flag -- poll an atomic flag for up to timeout_ms milliseconds.
 * Returns 1 if the flag was set within the timeout, 0 otherwise.
 * Uses 1ms sleep intervals.  See TESTING.md §2.5.
 */
static int
wait_for_flag(const _Atomic int *flag, int timeout_ms)
{
	struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L}; /* 1ms */
	int i;

	for (i = 0; i < timeout_ms; i++) {
		if (atomic_load_explicit(flag, memory_order_acquire))
			return (1);
		nanosleep(&ts, NULL);
	}
	return (0);
}

/* =========================================================================
 * test_scheduler_run_blocks
 *
 * Start strand_scheduler_run on a separate thread; verify the thread is
 * blocked (does not exit on its own); call strand_scheduler_stop; verify
 * the thread returns within a bounded time (~500ms).
 *
 * This verifies that:
 *   - strand_scheduler_run does not busy-spin and return immediately
 *     when there is no work.
 *   - strand_scheduler_stop causes the blocked run to return.
 * =========================================================================
 */

static _Atomic int g_run_blocks_done;

static void *
run_blocks_thread(void *arg)
{
	strand_scheduler_run((strand_scheduler_t *)arg);
	atomic_store_explicit(&g_run_blocks_done, 1, memory_order_release);
	return (NULL);
}

static int
test_scheduler_run_blocks(void)
{
	strand_scheduler_t *sched;
	pthread_t t;
	struct timespec sleep_tv;

	atomic_init(&g_run_blocks_done, 0);

	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	if (pthread_create(&t, NULL, run_blocks_thread, sched) != 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * Give the thread time to enter the blocking poll inside
	 * strand_scheduler_run.  50ms is sufficient on any real machine.
	 */
	sleep_tv.tv_sec = 0;
	sleep_tv.tv_nsec = 50 * 1000000L;
	nanosleep(&sleep_tv, NULL);

	/*
	 * Thread must still be running (not returned early).  If the flag is
	 * already set, strand_scheduler_run returned without being stopped —
	 * that is a failure.
	 */
	if (atomic_load_explicit(&g_run_blocks_done, memory_order_acquire)) {
		pthread_join(t, NULL);
		strand_scheduler_destroy(sched);
		return (1);
	}

	strand_scheduler_stop(sched);

	/* Thread must complete within 500ms after stop. */
	if (!wait_for_flag(&g_run_blocks_done, 500)) {
		/*
		 * Timed out — strand_scheduler_stop failed to interrupt the
		 * worker.  Detach to avoid blocking the test process.
		 */
		pthread_detach(t);
		strand_scheduler_destroy(sched);
		return (1);
	}

	pthread_join(t, NULL);
	strand_scheduler_destroy(sched);
	return (0);
}

/* =========================================================================
 * test_stop_writes_wakeup_fd
 *
 * Verify that strand_scheduler_stop writes to the wakeup fd.
 * After stop, reading the wakeup fd (or polling it) must show data.
 *
 * Uses strand_scheduler_get_fd to obtain the fd and poll(2) to check
 * for readability without consuming the data.
 * =========================================================================
 */

static int
test_stop_writes_wakeup_fd(void)
{
	strand_scheduler_t *sched;
	struct pollfd pfd;
	int fd;
	int rc;

	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	fd = strand_scheduler_get_fd(sched);
	if (fd < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}

	strand_scheduler_stop(sched);

	/*
	 * Poll with zero timeout: if stop wrote to the fd, it is immediately
	 * readable.
	 */
	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	rc = poll(&pfd, 1, 0);

	strand_scheduler_destroy(sched);
	return ((rc == 1 && (pfd.revents & POLLIN)) ? 0 : 1);
}

/* =========================================================================
 * test_wakeup_fd_drained_as_control
 *
 * Write a byte to the wakeup fd manually (simulating a stop signal),
 * then call strand_scheduler_advance.  Verify that:
 *   1. Advance returns SCHED_IDLE (no fibers ran — the byte is not a fiber
 *      event, it is a control signal).
 *   2. After advance, the wakeup fd is empty (Step 3 drained it).
 *
 * This confirms the Step 3 ("drain wakeup fd as control") semantics from
 * ARCHITECTURE.md §4.2.
 * =========================================================================
 */

static int
test_wakeup_fd_drained_as_control(void)
{
	strand_scheduler_t *sched;
	struct pollfd pfd;
	sched_result_t rc;
	int fd_read;
	int r;

	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	/*
	 * Write to the write side of the wakeup channel.
	 * On Linux wakeup_fd is an eventfd — we can write 8 bytes (uint64_t).
	 * On OpenBSD wakeup_pipe[1] is the write end of the pipe.
	 * Access internal fields directly since tests include
	 * strand_internal.h.
	 */
#ifdef STRAND_LINUX
	{
		uint64_t val = 1;
		(void)write(sched->wakeup_fd, &val, sizeof(val));
	}
#endif
#ifdef STRAND_OPENBSD
	{
		char val = 1;
		(void)write(sched->wakeup_pipe[1], &val, sizeof(val));
	}
#endif

	fd_read = strand_scheduler_get_fd(sched);

	/* Advance must return SCHED_IDLE — no fibers, no timer, no I/O. */
	rc = strand_scheduler_advance(sched, NULL);
	if (rc != SCHED_IDLE) {
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * After advance the wakeup fd must be empty: Step 3 drained all
	 * bytes unconditionally.  Poll with zero timeout must return 0.
	 */
	pfd.fd = fd_read;
	pfd.events = POLLIN;
	pfd.revents = 0;
	r = poll(&pfd, 1, 0);

	strand_scheduler_destroy(sched);
	return ((r == 0) ? 0 : 1);
}

/* =========================================================================
 * Suite entry point
 * =========================================================================
 */

/* =========================================================================
 * Task 3.5 — strand_fiber_spawn, advance nonblocking, FIFO ordering,
 *             budget limiting, shutdown error.
 *
 * Design note on fiber lifecycle in Phase 3 tests:
 * The scheduler has no automatic fiber-completion handling in Phase 3.
 * Fiber entry functions MUST NOT return (the trampoline has a ud2 guard).
 * Each test fiber calls t35_switch_back(), which pushes the descriptor
 * to the dead pool BEFORE the context switch, so that
 * strand_scheduler_destroy() frees the malloc'd descriptor cleanly.
 * The mmap'd stacks are not explicitly unmapped here; Valgrind's heap
 * leak checker does not track mmap allocations, only malloc.
 * =========================================================================
 */

/*
 * t35_switch_back -- from within a fiber, push the fiber descriptor to the
 * dead pool and switch back to the scheduler.
 *
 * fiber_free() modifies only the 'next' intrusive-link field, leaving
 * the context registers intact so the context switch succeeds.  The
 * strand_scheduler_destroy() dead-pool drain then frees the descriptor.
 * No code in this fiber may run after t35_switch_back() returns, but
 * in practice it does not return: the scheduler never re-queues a fiber
 * that has called t35_switch_back().
 */
static void
t35_switch_back(strand_scheduler_t *sched)
{
	strand_fiber_t *me = sched->current_fiber;

	fiber_free(&sched->dead_pool, me);
	strand_context_switch(me, &sched->scheduler_ctx);
}

/*
 * t35_push_fiber -- allocate a fiber descriptor and stack using the
 * internal API and push it to the scheduler's run queue.
 *
 * Used by tests that require fibers in the run queue before the first
 * advance call (bypassing strand_fiber_spawn's WRONGCTX check), and for
 * the root "spawner" fiber pattern where a host-thread fiber is needed
 * to satisfy the within-worker precondition of strand_fiber_spawn.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
static int
t35_push_fiber(strand_scheduler_t *sched, strand_fiber_fn_t fn, void *arg)
{
	strand_fiber_t *f;
	void *base;
	unsigned long vg_id;
	size_t sz;

	sz = STRAND_DEFAULT_STACK_SIZE;
	f = fiber_alloc(&sched->dead_pool);
	if (f == NULL)
		return (-1);

	base = stack_alloc(sz, &vg_id);
	if (base == NULL) {
		fiber_free(&sched->dead_pool, f);
		return (-1);
	}

	f->stack_base = (char *)base + page_size();
	f->stack_size = sz;
	f->valgrind_stack_id = vg_id;
	strand_context_init(&f->context, (char *)f->stack_base + sz, fn, arg);
	strand_fiber_tsan_init(f);
	atomic_store_explicit(&f->state, FIBER_RUNNABLE, memory_order_relaxed);
	run_queue_push(sched, f);
	return (0);
}

/* =========================================================================
 * test_advance_nonblocking
 *
 * Call strand_scheduler_advance with an empty run queue, no timers, and
 * no pending I/O.  Must return SCHED_IDLE without blocking.
 * =========================================================================
 */

static int
test_advance_nonblocking(void)
{
	strand_scheduler_t *sched;
	sched_result_t r;

	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	r = strand_scheduler_advance(sched, NULL);
	strand_scheduler_destroy(sched);
	return (r == SCHED_IDLE ? 0 : 1);
}

/* =========================================================================
 * test_fiber_runs
 *
 * Spawn one fiber via strand_fiber_spawn from a root fiber (so that
 * sched->current_fiber != NULL, satisfying the Phase 3 within-worker
 * precondition).  Call advance; verify the spawned fiber executed.
 *
 * With budget = 64, both the root and the spawned child run in the first
 * advance call.  The second advance call is a no-op (empty queue).
 * =========================================================================
 */

struct t35_runs_child_arg {
	strand_scheduler_t *sched;
	_Atomic int ran;
};

static void
t35_runs_child_fn(void *varg)
{
	struct t35_runs_child_arg *a = varg;

	atomic_store_explicit(&a->ran, 1, memory_order_release);
	t35_switch_back(a->sched);
}

struct t35_runs_root_arg {
	strand_scheduler_t *sched;
	struct t35_runs_child_arg *child;
};

static void
t35_runs_root_fn(void *varg)
{
	struct t35_runs_root_arg *a = varg;

	strand_fiber_spawn(a->sched, t35_runs_child_fn, a->child, 0, NULL);
	t35_switch_back(a->sched);
}

static int
test_fiber_runs(void)
{
	strand_scheduler_t *sched;
	struct t35_runs_child_arg child_arg;
	struct t35_runs_root_arg root_arg;
	int result;

	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	atomic_init(&child_arg.ran, 0);
	child_arg.sched = sched;
	root_arg.sched = sched;
	root_arg.child = &child_arg;

	if (t35_push_fiber(sched, t35_runs_root_fn, &root_arg) != 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * The root fiber runs, calls strand_fiber_spawn, then switches back.
	 * With budget = 64 the spawned child also runs in the same advance.
	 */
	strand_scheduler_advance(sched, NULL);

	result =
	    atomic_load_explicit(&child_arg.ran, memory_order_acquire) ? 0 : 1;
	strand_scheduler_destroy(sched);
	return (result);
}

/* =========================================================================
 * test_fifo_order
 *
 * Spawn three fibers A (id=0), B (id=1), C (id=2) in that order from a
 * root fiber.  Each records its id at the next slot of a shared array.
 * Verify the run order is 0 → 1 → 2 (FIFO, not LIFO).
 * =========================================================================
 */

struct t35_order_arg {
	strand_scheduler_t *sched;
	_Atomic int *next_slot;
	int *order;
	int id;
};

static void
t35_order_fiber_fn(void *varg)
{
	struct t35_order_arg *a = varg;
	int slot;

	slot = atomic_fetch_add_explicit(a->next_slot, 1, memory_order_relaxed);
	a->order[slot] = a->id;
	t35_switch_back(a->sched);
}

struct t35_fifo_root_arg {
	strand_scheduler_t *sched;
	struct t35_order_arg *children; /* array[3] */
};

static void
t35_fifo_root_fn(void *varg)
{
	struct t35_fifo_root_arg *a = varg;

	strand_fiber_spawn(a->sched, t35_order_fiber_fn, &a->children[0], 0,
	                   NULL);
	strand_fiber_spawn(a->sched, t35_order_fiber_fn, &a->children[1], 0,
	                   NULL);
	strand_fiber_spawn(a->sched, t35_order_fiber_fn, &a->children[2], 0,
	                   NULL);
	t35_switch_back(a->sched);
}

static int
test_fifo_order(void)
{
	strand_scheduler_t *sched;
	_Atomic int next_slot;
	int order[3];
	struct t35_order_arg children[3];
	struct t35_fifo_root_arg root_arg;
	int i;

	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	atomic_init(&next_slot, 0);
	for (i = 0; i < 3; i++) {
		order[i] = -1;
		children[i].sched = sched;
		children[i].next_slot = &next_slot;
		children[i].order = order;
		children[i].id = i;
	}
	root_arg.sched = sched;
	root_arg.children = children;

	if (t35_push_fiber(sched, t35_fifo_root_fn, &root_arg) != 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * Root spawns children 0, 1, 2 then switches back.  With budget = 64
	 * the three children also run in the same advance call in FIFO order.
	 */
	strand_scheduler_advance(sched, NULL);

	strand_scheduler_destroy(sched);
	return ((order[0] == 0 && order[1] == 1 && order[2] == 2) ? 0 : 1);
}

/* =========================================================================
 * test_budget_limiting
 *
 * Push 200 fibers directly to the run queue (budget = 64 from
 * make_test_scheduler).  Call advance once.  Verify exactly 64 ran.
 * Drain the remainder with further advance calls so that all descriptors
 * reach the dead pool before strand_scheduler_destroy is called.
 * =========================================================================
 */

struct t35_budget_arg {
	strand_scheduler_t *sched;
	_Atomic int *count;
};

static void
t35_budget_fiber_fn(void *varg)
{
	struct t35_budget_arg *a = varg;

	atomic_fetch_add_explicit(a->count, 1, memory_order_relaxed);
	t35_switch_back(a->sched);
}

static int
test_budget_limiting(void)
{
	strand_scheduler_t *sched;
	_Atomic int count;
	struct t35_budget_arg arg;
	int count_after_one;
	int i;

	sched = make_test_scheduler(); /* budget = 64 */
	if (sched == NULL)
		return (1);

	atomic_init(&count, 0);
	arg.sched = sched;
	arg.count = &count;

	for (i = 0; i < 200; i++) {
		if (t35_push_fiber(sched, t35_budget_fiber_fn, &arg) != 0) {
			/* Drain whatever was pushed before bailing. */
			while (strand_scheduler_advance(sched, NULL) ==
			       SCHED_PROGRESS)
				;
			strand_scheduler_destroy(sched);
			return (1);
		}
	}

	/* First advance: must run exactly budget (64) fibers, not all 200. */
	strand_scheduler_advance(sched, NULL);
	count_after_one = atomic_load_explicit(&count, memory_order_acquire);

	/* Drain remaining fibers so the scheduler is clean before destroy. */
	while (strand_scheduler_advance(sched, NULL) == SCHED_PROGRESS)
		;

	strand_scheduler_destroy(sched);
	return (count_after_one == 64 ? 0 : 1);
}

/* =========================================================================
 * test_spawn_returns_error_after_stop
 *
 * Call strand_scheduler_stop to set the stop flag, then attempt
 * strand_fiber_spawn.  The stop flag is checked before the
 * current_fiber (WRONGCTX) check, so STRAND_ERR_SHUTDOWN must be
 * returned regardless of calling context.
 * =========================================================================
 */

static int
test_spawn_returns_error_after_stop(void)
{
	strand_scheduler_t *sched;
	int rc;

	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	strand_scheduler_stop(sched);

	/*
	 * fn = NULL is safe: stop_flag is checked first and the function
	 * returns before dereferencing fn.
	 */
	rc = strand_fiber_spawn(sched, NULL, NULL, 0, NULL);

	strand_scheduler_destroy(sched);
	return (rc == STRAND_ERR_SHUTDOWN ? 0 : 1);
}

/* =========================================================================
 * Task 3.6 — test_yield_requeues
 *
 * A fiber calls strand_fiber_yield() once, which re-queues it rather than
 * finishing it.  After the first advance call the fiber must NOT have
 * completed.  After the second advance call it must have completed.
 *
 * Design:
 *   - The fiber has a phase counter (0 → 1 → done).
 *   - Phase 0: increment to 1, call strand_fiber_yield, return to phase 1.
 *   - Phase 1: mark done, call t35_switch_back.
 *   - After first advance:  phase == 1, done == 0  → yield worked.
 *   - After second advance: done == 1              → fiber resumed.
 * =========================================================================
 */

struct t36_yield_arg {
	strand_scheduler_t *sched;
	_Atomic int phase;
	_Atomic int done;
};

static void
t36_yield_fiber_fn(void *varg)
{
	struct t36_yield_arg *a = varg;

	/* Phase 0: record that we ran once, then yield back to scheduler. */
	atomic_store_explicit(&a->phase, 1, memory_order_release);
	strand_fiber_yield(a->sched);

	/* Phase 1: we were re-queued and resumed; mark completion. */
	atomic_store_explicit(&a->done, 1, memory_order_release);
	t35_switch_back(a->sched);
}

static int
test_yield_requeues(void)
{
	strand_scheduler_t *sched;
	strand_sched_config_t cfg;
	struct t36_yield_arg arg;
	int phase_after_first, done_after_first;
	int done_after_second;
	int result;

	/*
	 * Budget = 1: each strand_scheduler_advance runs at most one fiber.
	 * This is required to observe that the yielded fiber is deferred to
	 * the next advance pass rather than immediately re-run within the
	 * same pass.  With budget > 1 the scheduler would pick the re-queued
	 * fiber in the same loop iteration.
	 */
	cfg.budget = 1;
	cfg.inject_cap = 256;
	cfg.cache_cap = 8;
	cfg.idle_floor = 2;
	sched = strand_scheduler_create(&cfg);
	if (sched == NULL)
		return (1);

	atomic_init(&arg.phase, 0);
	atomic_init(&arg.done, 0);
	arg.sched = sched;

	if (t35_push_fiber(sched, t36_yield_fiber_fn, &arg) != 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * First advance (budget=1): fiber runs to the yield point, sets
	 * phase = 1, calls strand_fiber_yield, and returns control to the
	 * scheduler.  Budget is exhausted after one fiber; the re-queued
	 * fiber is NOT run again.  done must still be 0.
	 */
	strand_scheduler_advance(sched, NULL);
	phase_after_first =
	    atomic_load_explicit(&arg.phase, memory_order_acquire);
	done_after_first =
	    atomic_load_explicit(&arg.done, memory_order_acquire);

	/*
	 * Second advance: the scheduler picks the re-queued fiber, resumes
	 * it past the yield, and it sets done = 1 then switches back.
	 */
	strand_scheduler_advance(sched, NULL);
	done_after_second =
	    atomic_load_explicit(&arg.done, memory_order_acquire);

	strand_scheduler_destroy(sched);

	/*
	 * Pass conditions:
	 *   - phase was set to 1 in the first pass (fiber ran before yield)
	 *   - done was NOT set after the first pass (yield deferred resumption)
	 *   - done IS set after the second pass (fiber resumed and completed)
	 */
	result = (phase_after_first == 1 && done_after_first == 0 &&
	          done_after_second == 1)
	             ? 0
	             : 1;
	return (result);
}

/* =========================================================================
 * Task 3.7 — test_timer_fires
 *
 * A fiber calls strand_fiber_sleep_until with a future deadline.  The mock
 * clock is advanced past the deadline and then strand_scheduler_advance is
 * called again.  The fiber must resume and record that it woke.
 *
 * Uses strand_test_clock_ns (compiled in with -DSTRAND_TEST_CLOCK) so the
 * test does not need to sleep.  See TESTING.md §2.3.
 * =========================================================================
 */

struct t37_sleep_arg {
	strand_scheduler_t *sched;
	uint64_t deadline_ns;
	_Atomic int woke;
	int rc; /* return value of strand_fiber_sleep_until */
};

static void
t37_sleep_fiber_fn(void *varg)
{
	struct t37_sleep_arg *a = varg;

	a->rc = strand_fiber_sleep_until(a->sched, a->deadline_ns);
	atomic_store_explicit(&a->woke, 1, memory_order_release);
	t35_switch_back(a->sched);
}

static int
test_timer_fires(void)
{
	strand_scheduler_t *sched;
	struct t37_sleep_arg arg;
	int woke_before, woke_after;
	int result;

	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	/* Start the mock clock well before the deadline. */
	strand_test_clock_ns = 1000;
	arg.deadline_ns = 2000;
	arg.sched = sched;
	arg.rc = -999;
	atomic_init(&arg.woke, 0);

	if (t35_push_fiber(sched, t37_sleep_fiber_fn, &arg) != 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * First advance: fiber runs, calls strand_fiber_sleep_until(2000),
	 * parks on the timer heap, and returns control to the scheduler.
	 * Clock is still at 1000 — deadline has not passed.
	 */
	strand_scheduler_advance(sched, NULL);
	woke_before = atomic_load_explicit(&arg.woke, memory_order_acquire);

	/*
	 * Advance the mock clock past the deadline, then run another advance.
	 * Step 2 must pop the timer entry and move the fiber to the run queue.
	 * Step 5 runs the fiber, which resumes past sleep_until, sets woke = 1,
	 * and calls t35_switch_back.
	 */
	strand_test_clock_ns = 2001;
	strand_scheduler_advance(sched, NULL);
	woke_after = atomic_load_explicit(&arg.woke, memory_order_acquire);

	strand_scheduler_destroy(sched);

	/*
	 * Pass conditions:
	 *   - fiber did NOT wake before deadline (woke_before == 0)
	 *   - fiber DID wake after deadline (woke_after == 1)
	 *   - sleep_until returned STRAND_OK (not cancelled)
	 */
	result = (woke_before == 0 && woke_after == 1 &&
	          arg.rc == STRAND_OK)
	             ? 0
	             : 1;
	return (result);
}

/* =========================================================================
 * Task 3.7 — test_timer_order
 *
 * Two fibers sleep until different deadlines.  Fiber A sleeps until T=3000,
 * fiber B until T=2000.  The mock clock is advanced past both deadlines in
 * one step.  The scheduler's timer heap must pop B first (lower deadline),
 * so B's run-order slot must be filled before A's.
 * =========================================================================
 */

struct t37_order_arg {
	strand_scheduler_t *sched;
	uint64_t deadline_ns;
	_Atomic int *next_slot;
	int *order;
	int id;
};

static void
t37_order_fiber_fn(void *varg)
{
	struct t37_order_arg *a = varg;
	int slot;

	strand_fiber_sleep_until(a->sched, a->deadline_ns);

	/* Record the order in which fibers wake after the sleep. */
	slot = atomic_fetch_add_explicit(a->next_slot, 1, memory_order_relaxed);
	a->order[slot] = a->id;
	t35_switch_back(a->sched);
}

static int
test_timer_order(void)
{
	strand_scheduler_t *sched;
	_Atomic int next_slot;
	int order[2];
	struct t37_order_arg args[2];
	int i;

	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	strand_test_clock_ns = 1000;

	atomic_init(&next_slot, 0);
	for (i = 0; i < 2; i++) {
		order[i] = -1;
		args[i].sched = sched;
		args[i].next_slot = &next_slot;
		args[i].order = order;
	}
	/* A: id=0, later deadline */
	args[0].id = 0;
	args[0].deadline_ns = 3000;
	/* B: id=1, earlier deadline — must wake first */
	args[1].id = 1;
	args[1].deadline_ns = 2000;

	/* Push A then B to the run queue. */
	for (i = 0; i < 2; i++) {
		if (t35_push_fiber(sched, t37_order_fiber_fn, &args[i]) != 0) {
			while (strand_scheduler_advance(sched, NULL) ==
			       SCHED_PROGRESS)
				;
			strand_scheduler_destroy(sched);
			return (1);
		}
	}

	/*
	 * First advance: both fibers run (budget=64), both call sleep_until,
	 * both park on the timer heap.  A is at deadline 3000, B at 2000.
	 */
	strand_scheduler_advance(sched, NULL);

	/*
	 * Advance mock clock past both deadlines in one step.
	 * Step 2 of the next advance pops min-heap entries in order:
	 *   B (deadline 2000) is popped first → added to run queue head
	 *   A (deadline 3000) is popped second → added to run queue tail
	 * Step 5 runs them in FIFO order: B records slot 0, A records slot 1.
	 */
	strand_test_clock_ns = 4000;
	strand_scheduler_advance(sched, NULL);

	strand_scheduler_destroy(sched);

	/*
	 * Pass condition: B (id=1) woke first (order[0]==1),
	 *                 A (id=0) woke second (order[1]==0).
	 */
	return ((order[0] == 1 && order[1] == 0) ? 0 : 1);
}

void
run_layer2_tests(void)
{
	RUN("test_scheduler_run_blocks", test_scheduler_run_blocks);
	RUN("test_stop_writes_wakeup_fd", test_stop_writes_wakeup_fd);
	RUN("test_wakeup_fd_drained_as_control",
	    test_wakeup_fd_drained_as_control);
	RUN("test_advance_nonblocking", test_advance_nonblocking);
	RUN("test_fiber_runs", test_fiber_runs);
	RUN("test_fifo_order", test_fifo_order);
	RUN("test_budget_limiting", test_budget_limiting);
	RUN("test_spawn_returns_error_after_stop",
	    test_spawn_returns_error_after_stop);
	RUN("test_yield_requeues", test_yield_requeues);
	RUN("test_timer_fires", test_timer_fires);
	RUN("test_timer_order", test_timer_order);
}
