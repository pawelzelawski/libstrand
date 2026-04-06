/*
 * tests/test_layer2.c — Layer 2 (Phase 3) test suite.
 *
 * Tests are added task by task as Phase 3 progresses.
 * This file currently covers Task 3.4: strand_scheduler_run,
 * strand_scheduler_stop, strand_scheduler_next_deadline,
 * strand_scheduler_get_fd.
 *
 * Later tasks in Phase 3 will add tests for fiber spawning, yield,
 * timers, cancel, stack cache, and fiber-local storage.
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
#include "../src/strand_internal.h"
#include "../src/strand_sched.h"

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

void
run_layer2_tests(void)
{
	RUN("test_scheduler_run_blocks", test_scheduler_run_blocks);
	RUN("test_stop_writes_wakeup_fd", test_stop_writes_wakeup_fd);
	RUN("test_wakeup_fd_drained_as_control",
	    test_wakeup_fd_drained_as_control);
}
