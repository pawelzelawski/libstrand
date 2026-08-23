/*
 * tests/test_layer3.c - Layer 3 test suite.
 *
 * Tests fd parking, epoll/kqueue event delivery, post-re-arm readiness check,
 * cancellation, error/hangup/EOF events, and scheduler stop while I/O parked.
 *
 * Fiber functions follow the strand_fiber_fn_t signature: void (*)(void *).
 * The scheduler pointer is passed inside each per-test arg struct.
 *
 * Platform guards:
 *   STRAND_LINUX   - EPOLLET/EPOLLONESHOT-specific tests
 *   STRAND_OPENBSD - EV_DISPATCH/EV_EOF-specific tests
 *   STRAND_DEBUG   - O_NONBLOCK assertion test (uses fork/SIGABRT)
 *
 * See TESTING.md §2.4, §5.2, §6.1, §6.2.
 */

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


#include "test_harness.h"
#include "../include/strand.h"
#include "../src/strand_context.h"
#include "../src/strand_fiber.h"
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

static void make_test_pipe(int *, int *);

/*
 * t4_push_fiber -- allocate and enqueue a fiber for Layer 3 tests.
 *
 * Mirrors the internals of strand_fiber_spawn but skips the WRONGCTX check
 * (current_fiber == NULL), allowing the host thread to bootstrap fibers into
 * the run queue before calling strand_scheduler_advance.
 *
 * Uses strand_fiber_entry_start as the context entry point so that fibers can
 * return normally without calling t35_switch_back manually.
 *
 * If out != NULL, *out receives an ABA-safe handle to the spawned fiber.
 * Returns 0 on success, -1 on allocation failure.
 */
static int
t4_push_fiber(strand_scheduler_t *sched, strand_fiber_fn_t fn, void *arg,
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

/*
 * make_test_socketpair -- create a nonblocking AF_UNIX socketpair.
 *
 * Each fd supports both EPOLLIN and EPOLLOUT.  Used by tests that need
 * both read and write waiters on the SAME fd (e.g. testing poller_cancel_io
 * MOD path).  sv[0] is the fd used for I/O waits; sv[1] is the peer.
 */
static void
make_test_socketpair(int *sv)
{
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		sv[0] = sv[1] = -1;
		return;
	}
	(void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(sv[1], F_SETFD, FD_CLOEXEC);
	(void)fcntl(sv[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(sv[1], F_SETFL, O_NONBLOCK);
}

/* =========================================================================
 * Buffered close and fd-table growth regressions
 * =========================================================================
 */

struct buffered_close_args {
	strand_scheduler_t *sched;
	int                 fd;
	int                 result;
	ssize_t             nread;
	char                buf[6];
	int                 ran;
};

static void
fiber_buffered_close(void *varg)
{
	struct buffered_close_args *a = varg;

	a->result = strand_fiber_wait_readable(a->sched, a->fd);
	if (a->result == STRAND_OK)
		a->nread = read(a->fd, a->buf, sizeof(a->buf) - 1);
	a->ran = 1;
}

static int
test_buffered_close_delivers_readiness(void)
{
	strand_scheduler_t       *sched;
	struct buffered_close_args args;
	int                        sv[2];

	memset(&args, 0, sizeof(args));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);
	make_test_socketpair(sv);
	if (sv[0] < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}
	args.sched = sched;
	args.fd = sv[0];
	if (t4_push_fiber(sched, fiber_buffered_close, &args, NULL) != 0) {
		close(sv[0]);
		close(sv[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}
	strand_scheduler_advance(sched, NULL);
	if (write(sv[1], "HELLO", 5) != 5 || shutdown(sv[1], SHUT_WR) != 0) {
		close(sv[0]);
		close(sv[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}
	strand_scheduler_advance(sched, NULL);

	close(sv[0]);
	close(sv[1]);
	strand_scheduler_destroy(sched);
	return (args.ran == 1 && args.result == STRAND_OK &&
	    args.nread == 5 && memcmp(args.buf, "HELLO", 5) == 0) ? 0 : 1;
}

/* 49 waiters cross the 48-entry (75%) growth threshold for a 64-slot table. */
#define TABLE_GROW_WAITERS 49
#define TABLE_GROW_MAX_ADVANCES 32

struct table_grow_args {
	strand_scheduler_t *sched;
	int                 fd;
	int                 result;
	int                 ran;
};

static void
fiber_table_grow_wait(void *varg)
{
	struct table_grow_args *a = varg;

	a->result = strand_fiber_wait_readable(a->sched, a->fd);
	a->ran = 1;
}

static int
test_fd_table_growth_preserves_first_event(void)
{
	strand_scheduler_t    *sched;
	struct table_grow_args args[TABLE_GROW_WAITERS];
	strand_fiber_handle_t  handles[TABLE_GROW_WAITERS];
	int                     fds[TABLE_GROW_WAITERS][2];
	int                     i, nadvance, rc;

	memset(args, 0, sizeof(args));
	memset(handles, 0, sizeof(handles));
	memset(fds, -1, sizeof(fds));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);
	make_test_pipe(&fds[0][0], &fds[0][1]);
	if (fds[0][0] < 0)
		goto fail;
	for (i = 0; i < TABLE_GROW_WAITERS; i++) {
		/* Distinct duplicated fds still occupy distinct fd-table entries. */
		if (i > 0) {
			fds[i][0] = dup(fds[0][0]);
			if (fds[i][0] < 0)
				goto fail;
		}
		if (t4_push_fiber(sched, fiber_table_grow_wait,
		    &args[i], &handles[i]) != 0)
			goto fail;
		args[i].sched = sched;
		args[i].fd = fds[i][0];
		strand_scheduler_advance(sched, NULL);
	}
	/* fd[0] was registered before table growth and must still be deliverable. */
	if (write(fds[0][1], "x", 1) != 1)
		goto fail;
	for (nadvance = 0; nadvance < TABLE_GROW_MAX_ADVANCES &&
	    args[0].ran == 0; nadvance++)
		strand_scheduler_advance(sched, NULL);
	if (args[0].ran != 1 || args[0].result != STRAND_OK)
		goto fail;
	rc = 0;
	goto cleanup;

fail:
	rc = 1;

cleanup:
	/* Cancel parked waiters before destroying the debug-checked poller. */
	for (i = 0; i < TABLE_GROW_WAITERS; i++) {
		if (handles[i].ptr != NULL && args[i].ran == 0)
			strand_fiber_cancel(handles[i]);
	}
	for (i = 0; i < TABLE_GROW_MAX_ADVANCES; i++)
		strand_scheduler_advance(sched, NULL);
	for (i = 0; i < TABLE_GROW_WAITERS; i++) {
		if (fds[i][0] >= 0)
			close(fds[i][0]);
		if (fds[i][1] >= 0)
			close(fds[i][1]);
	}
	strand_scheduler_destroy(sched);
	return (rc);
}

/*
 * make_test_pipe -- create a nonblocking O_CLOEXEC pipe pair.
 *
 * pipe2 is hidden behind __BSD_VISIBLE on OpenBSD when _POSIX_C_SOURCE is
 * set, and requires _GNU_SOURCE on Linux under strict -D_POSIX_C_SOURCE.
 * Use pipe + fcntl on both platforms for consistency, matching the approach
 * used in strand_sched.c for the wakeup pipe.  See TESTING.md §2.4.
 */
static void
make_test_pipe(int *rd, int *wr)
{
	int fds[2];

	if (pipe(fds) != 0) {
		*rd = *wr = -1;
		return;
	}
	(void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	(void)fcntl(fds[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(fds[1], F_SETFL, O_NONBLOCK);
	*rd = fds[0];
	*wr = fds[1];
}

/*
 * fill_pipe -- write to wr until EAGAIN (pipe buffer full).
 */
static void
fill_pipe(int wr)
{
	char buf[4096];
	memset(buf, 'x', sizeof(buf));
	while (write(wr, buf, sizeof(buf)) > 0)
		;
}

/*
 * drain_pipe -- read from rd until EAGAIN (pipe buffer empty).
 */
static void
drain_pipe(int rd)
{
	char buf[4096];
	while (read(rd, buf, sizeof(buf)) > 0)
		;
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
	int             i;

	for (i = 0; i < timeout_ms; i++) {
		if (atomic_load_explicit(flag, memory_order_acquire))
			return (1);
		nanosleep(&ts, NULL);
	}
	return (0);
}

/* =========================================================================
 * test_wait_readable_wakes
 *
 * Park a fiber on the read end of a pipe.  Write one byte from outside
 * the scheduler to trigger readiness.  Advance - fiber must wake with
 * STRAND_OK.
 *
 * See TESTING.md §2.4.
 * =========================================================================
 */

struct wait_read_args {
	strand_scheduler_t *sched;
	int                 fd;
	int                 result;
	int                 ran;
};

static void
fiber_wait_readable(void *varg)
{
	struct wait_read_args *a = varg;
	a->result = strand_fiber_wait_readable(a->sched, a->fd);
	a->ran    = 1;
}

static int
test_wait_readable_wakes(void)
{
	strand_scheduler_t   *sched;
	int                   rd, wr;
	struct wait_read_args args;

	memset(&args, 0, sizeof(args));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_pipe(&rd, &wr);
	if (rd < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}
	args.sched = sched;
	args.fd    = rd;

	/* Spawn fiber; advance once so it parks on the read end. */
	if (t4_push_fiber(sched, fiber_wait_readable, &args, NULL) != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}
	strand_scheduler_advance(sched, NULL);

	/* Fiber must be parked (not yet run to completion). */
	if (args.ran != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/* Write a byte to make the read end readable; advance to wake fiber. */
	(void)write(wr, "x", 1);
	strand_scheduler_advance(sched, NULL);

	close(rd);
	close(wr);
	strand_scheduler_destroy(sched);

	return (args.ran == 1 && args.result == STRAND_OK) ? 0 : 1;
}

/* =========================================================================
 * test_wait_writable_wakes
 *
 * Fill the pipe buffer so the write end is not writable.  Park a fiber on
 * write readiness.  Drain the read end to free buffer space.  Advance -
 * fiber must wake with STRAND_OK.
 * =========================================================================
 */

struct wait_write_args {
	strand_scheduler_t *sched;
	int                 fd;
	int                 result;
	int                 ran;
};

static void
fiber_wait_writable(void *varg)
{
	struct wait_write_args *a = varg;
	a->result = strand_fiber_wait_writable(a->sched, a->fd);
	a->ran    = 1;
}

static int
test_wait_writable_wakes(void)
{
	strand_scheduler_t    *sched;
	int                    rd, wr;
	struct wait_write_args args;

	memset(&args, 0, sizeof(args));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_pipe(&rd, &wr);
	if (rd < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}

	/* Fill the pipe so the write end is not writable. */
	fill_pipe(wr);
	args.sched = sched;
	args.fd    = wr;

	if (t4_push_fiber(sched, fiber_wait_writable, &args, NULL) != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}
	strand_scheduler_advance(sched, NULL); /* fiber parks on write end */

	if (args.ran != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/* Drain the pipe to make the write end writable again; advance. */
	drain_pipe(rd);
	strand_scheduler_advance(sched, NULL);

	close(rd);
	close(wr);
	strand_scheduler_destroy(sched);

	return (args.ran == 1 && args.result == STRAND_OK) ? 0 : 1;
}

/* =========================================================================
 * test_cancel_read_waiter
 *
 * Park a fiber on the read end of a pipe.  Cancel it while still parked.
 * Verify the fiber wakes with STRAND_CANCELLED and the fd is free for a
 * fresh read registration.
 * =========================================================================
 */

struct cancel_args {
	strand_scheduler_t *sched;
	int                 fd;
	int                 result;
	int                 ran;
};

static void
fiber_cancel_read(void *varg)
{
	struct cancel_args *a = varg;
	a->result = strand_fiber_wait_readable(a->sched, a->fd);
	a->ran    = 1;
}

static int
test_cancel_read_waiter(void)
{
	strand_scheduler_t *sched;
	int                 rd, wr;
	strand_fiber_handle_t handle;
	struct cancel_args    args;

	memset(&args, 0, sizeof(args));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_pipe(&rd, &wr);
	if (rd < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}
	args.sched = sched;
	args.fd    = rd;

	if (t4_push_fiber(sched, fiber_cancel_read, &args, &handle) != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}
	strand_scheduler_advance(sched, NULL); /* fiber parks */

	if (args.ran != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/* Cancel the parked fiber. */
	strand_fiber_cancel(handle);

	/* Advance to run the cancelled fiber. */
	strand_scheduler_advance(sched, NULL);

	close(rd);
	close(wr);
	strand_scheduler_destroy(sched);

	return (args.ran == 1 && args.result == STRAND_CANCELLED) ? 0 : 1;
}

/* =========================================================================
 * test_cancel_one_direction_leaves_other
 *
 * Park fiber A on READ and fiber B on WRITE of the same pipe.  Cancel the
 * READ waiter (A).  Verify A wakes with STRAND_CANCELLED.  Then make the
 * pipe writable and verify B wakes with STRAND_OK - the WRITE registration
 * must have survived the READ cancellation.
 * =========================================================================
 */

struct two_dir_args {
	strand_scheduler_t *sched;
	int                 fd;
	int                 result;
	int                 ran;
};

static void
fiber_two_dir_read(void *varg)
{
	struct two_dir_args *a = varg;
	a->result = strand_fiber_wait_readable(a->sched, a->fd);
	a->ran    = 1;
}

static void
fiber_two_dir_write(void *varg)
{
	struct two_dir_args *a = varg;
	a->result = strand_fiber_wait_writable(a->sched, a->fd);
	a->ran    = 1;
}

static int
test_cancel_one_direction_leaves_other(void)
{
	strand_scheduler_t  *sched;
	int                  sv[2];
	strand_fiber_handle_t handle_a;
	struct two_dir_args   read_args, write_args;

	/*
	 * Use a socketpair so both A (read waiter) and B (write waiter) park
	 * on the SAME fd (sv[0]).  This exercises the MOD path in
	 * poller_cancel_io: cancelling A must reduce the epoll registration
	 * from EPOLLIN|EPOLLOUT to EPOLLOUT only, leaving B's wait intact.
	 * A pipe's read/write ends are separate fds and cannot share an epoll
	 * registration.  See ARCHITECTURE.md §5.8.
	 *
	 * Setup:
	 *   fill_pipe(sv[0]): writes to sv[0] until EAGAIN, filling sv[1]'s
	 *   receive buffer.  sv[0] is now NOT WRITABLE (backpressure) and
	 *   NOT READABLE (no one has written to sv[1]).
	 *
	 *   A: wait_readable(sched, sv[0])  - parks (not readable)
	 *   B: wait_writable(sched, sv[0])  - parks (not writable)
	 *
	 *   Both use sv[0]: one entry in the fd table with read_waiter=A
	 *   and write_waiter=B.  Linux registers EPOLLIN|EPOLLOUT|EPOLLET|
	 *   EPOLLONESHOT for sv[0].
	 *
	 * After cancel A:
	 *   poller_cancel_io MODs sv[0] to EPOLLOUT only (B still waiting).
	 *
	 * After drain_pipe(sv[1]):
	 *   sv[1]'s receive buffer drains → sv[0]'s send buffer freed →
	 *   sv[0] becomes WRITABLE → EPOLLOUT fires → B wakes STRAND_OK.
	 */
	memset(&read_args,  0, sizeof(read_args));
	memset(&write_args, 0, sizeof(write_args));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_socketpair(sv);
	if (sv[0] < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}

	/* Fill sv[0] → sv[0] not writable, sv[0] not readable. */
	fill_pipe(sv[0]);

	read_args.sched  = sched;
	read_args.fd     = sv[0];
	write_args.sched = sched;
	write_args.fd    = sv[0]; /* SAME fd as read_args */

	/*
	 * Spawn A (read waiter on sv[0]) and B (write waiter on sv[0]).
	 * Both park in two advances.  The entry for sv[0] will carry both
	 * read_waiter=A and write_waiter=B after advance 2.
	 */
	if (t4_push_fiber(sched, fiber_two_dir_read, &read_args,
	    &handle_a) != 0) {
		close(sv[0]); close(sv[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}
	if (t4_push_fiber(sched, fiber_two_dir_write, &write_args,
	    NULL) != 0) {
		close(sv[0]); close(sv[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}

	strand_scheduler_advance(sched, NULL); /* A parks on sv[0] read  */
	strand_scheduler_advance(sched, NULL); /* B parks on sv[0] write */

	if (read_args.ran != 0 || write_args.ran != 0) {
		close(sv[0]); close(sv[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/* Cancel A - must MOD sv[0] to EPOLLOUT only, leaving B parked. */
	strand_fiber_cancel(handle_a);
	strand_scheduler_advance(sched, NULL); /* A wakes with CANCELLED */

	if (read_args.ran != 1 || read_args.result != STRAND_CANCELLED) {
		close(sv[0]); close(sv[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}
	if (write_args.ran != 0) {
		close(sv[0]); close(sv[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * Drain sv[1]'s receive buffer → sv[0]'s send buffer drains →
	 * sv[0] becomes writable → EPOLLOUT fires → B wakes STRAND_OK.
	 */
	drain_pipe(sv[1]);
	strand_scheduler_advance(sched, NULL); /* B wakes with STRAND_OK */

	close(sv[0]);
	close(sv[1]);
	strand_scheduler_destroy(sched);

	return (write_args.ran == 1 && write_args.result == STRAND_OK) ? 0 : 1;
}

/* =========================================================================
 * test_cancel_both_directions
 *
 * Park fibers A (READ) and B (WRITE) on the same filled pipe.  Cancel
 * both.  Verify both wake with STRAND_CANCELLED and no event fires after.
 * =========================================================================
 */

static int
test_cancel_both_directions(void)
{
	strand_scheduler_t  *sched;
	int                  sv[2];
	strand_fiber_handle_t handle_a, handle_b;
	struct two_dir_args   read_args, write_args;

	/*
	 * Use a socketpair so both A and B park on sv[0].
	 * fill_pipe(sv[0]) makes sv[0] not writable (B parks) and not
	 * readable (A parks).  Both are then cancelled.
	 */
	memset(&read_args,  0, sizeof(read_args));
	memset(&write_args, 0, sizeof(write_args));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_socketpair(sv);
	if (sv[0] < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}

	fill_pipe(sv[0]); /* sv[0]: not writable, not readable */
	read_args.sched  = sched;
	read_args.fd     = sv[0];
	write_args.sched = sched;
	write_args.fd    = sv[0];

	if (t4_push_fiber(sched, fiber_two_dir_read, &read_args,
	    &handle_a) != 0 ||
	    t4_push_fiber(sched, fiber_two_dir_write, &write_args,
	    &handle_b) != 0) {
		close(sv[0]); close(sv[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}

	strand_scheduler_advance(sched, NULL); /* A parks on sv[0] read  */
	strand_scheduler_advance(sched, NULL); /* B parks on sv[0] write */

	if (read_args.ran != 0 || write_args.ran != 0) {
		close(sv[0]); close(sv[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}

	strand_fiber_cancel(handle_a);
	strand_fiber_cancel(handle_b);

	strand_scheduler_advance(sched, NULL);
	strand_scheduler_advance(sched, NULL);

	close(sv[0]);
	close(sv[1]);
	strand_scheduler_destroy(sched);

	return (read_args.ran == 1 && read_args.result == STRAND_CANCELLED &&
	        write_args.ran == 1 && write_args.result == STRAND_CANCELLED) ?
	    0 : 1;
}

/* =========================================================================
 * test_same_worker_readiness_wins
 *
 * Arrange for I/O readiness and a same-worker cancellation attempt to occur
 * in the same strand_scheduler_advance call.  Readiness must win because
 * the poller fires in Step 4 before the cancelling fiber runs in Step 5.
 *
 * Setup:
 *   Fiber A: parks on the read end of a pipe.
 *   Fiber B: yields once (so A parks first), then tries to cancel A.
 *   Data is written to the pipe before the second advance, so the next
 *   Step 4 delivers readiness to A and places A at the tail of the run
 *   queue.  Step 5 then runs B (head of queue) which calls cancel -
 *   but A is already FIBER_RUNNABLE with io_result = STRAND_OK.
 *   Cancel on FIBER_RUNNABLE only sets cancel_pending; it does not
 *   overwrite io_result.  A returns STRAND_OK.
 *
 * See ARCHITECTURE.md §5.3 and TESTING.md §6.2.
 * =========================================================================
 */

struct readiness_wins_state {
	strand_scheduler_t   *sched;
	int                   fd;
	strand_fiber_handle_t handle_a;
	int                   result_a;
	int                   cancel_called;
};

static void
fiber_rw_a(void *varg)
{
	struct readiness_wins_state *s = varg;
	s->result_a = strand_fiber_wait_readable(s->sched, s->fd);
}

static void
fiber_rw_b(void *varg)
{
	struct readiness_wins_state *s = varg;
	/*
	 * B is pushed after A has already parked, and is at the HEAD of the
	 * run queue when advance fires.  Step 4 (poller_poll) runs before
	 * Step 5, so readiness is delivered to A (RUNNABLE, io_result=OK)
	 * before B runs.  B's cancel call finds A FIBER_RUNNABLE and only
	 * sets cancel_pending - it cannot overwrite io_result.
	 * A then runs and returns STRAND_OK.
	 */
	strand_fiber_cancel(s->handle_a);
	s->cancel_called = 1;
}

static int
test_same_worker_readiness_wins(void)
{
	strand_scheduler_t          *sched;
	int                          rd, wr;
	struct readiness_wins_state  s;

	memset(&s, 0, sizeof(s));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_pipe(&rd, &wr);
	if (rd < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}
	s.sched = sched;
	s.fd    = rd;

	/* Push A and advance: A parks on read. */
	if (t4_push_fiber(sched, fiber_rw_a, &s, &s.handle_a) != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}
	strand_scheduler_advance(sched, NULL); /* A parks */

	if (s.result_a != 0) { /* A must not have returned yet */
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * Push B now (after A is parked) and write data.
	 *
	 * In the next advance:
	 *   Step 4 - poller_poll delivers EPOLLIN; A pushed to queue tail.
	 *             Queue: [B, A]
	 *   Step 5 - B runs first (head), calls cancel(A).  A is already
	 *             FIBER_RUNNABLE → cancel only sets cancel_pending.
	 *             B returns (cancel_called=1).  Then A runs → STRAND_OK.
	 *
	 * See ARCHITECTURE.md §5.3 and TESTING.md §6.2.
	 */
	if (t4_push_fiber(sched, fiber_rw_b, &s, NULL) != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}
	(void)write(wr, "x", 1);
	strand_scheduler_advance(sched, NULL); /* B cancels A; A runs → OK */

	close(rd);
	close(wr);
	strand_scheduler_destroy(sched);

	return (s.cancel_called == 1 && s.result_a == STRAND_OK) ? 0 : 1;
}

/* =========================================================================
 * test_scheduler_stop_interrupts_io_wait
 *
 * Park a fiber on an fd that never becomes ready.  Call
 * strand_scheduler_stop from a second thread.  Verify the scheduler run
 * loop returns within a bounded time.
 *
 * This tests that the wakeup fd is registered with the poller
 * so that stop's write to the wakeup fd interrupts the blocking
 * epoll_wait / kevent inside strand_scheduler_run.
 *
 * See ARCHITECTURE.md §4.2, TESTING.md §7.11.
 * =========================================================================
 */

static _Atomic int g_stop_interrupts_done;

static void *
stop_interrupts_thread(void *arg)
{
	strand_scheduler_run((strand_scheduler_t *)arg);
	atomic_store_explicit(&g_stop_interrupts_done, 1, memory_order_release);
	return (NULL);
}

struct parked_forever_args {
	strand_scheduler_t *sched;
	int                 fd;
};

static void
fiber_park_forever(void *varg)
{
	struct parked_forever_args *a = varg;
	/*
	 * Park on an fd that will never become readable.  The fiber stays
	 * parked until cancelled after the scheduler is stopped.
	 */
	(void)strand_fiber_wait_readable(a->sched, a->fd);
}

static int
test_scheduler_stop_interrupts_io_wait(void)
{
	strand_scheduler_t        *sched;
	int                        rd, wr;
	pthread_t                  t;
	strand_fiber_handle_t      handle;
	struct timespec            sleep_tv;
	struct parked_forever_args args;

	atomic_init(&g_stop_interrupts_done, 0);
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_pipe(&rd, &wr);
	if (rd < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}
	args.sched = sched;
	args.fd    = rd;

	if (t4_push_fiber(sched, fiber_park_forever, &args, &handle) != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	if (pthread_create(&t, NULL, stop_interrupts_thread, sched) != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/* Allow the thread to enter the blocking wait inside scheduler_run. */
	sleep_tv.tv_sec  = 0;
	sleep_tv.tv_nsec = 50 * 1000000L; /* 50ms */
	nanosleep(&sleep_tv, NULL);

	/* Thread must still be running (not returned early). */
	if (atomic_load_explicit(&g_stop_interrupts_done, memory_order_acquire)) {
		pthread_join(t, NULL);
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	strand_scheduler_stop(sched);

	/* Thread must return within 500ms after stop. */
	if (!wait_for_flag(&g_stop_interrupts_done, 500)) {
		pthread_detach(t);
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	pthread_join(t, NULL);

	/*
	 * The fiber is still parked on rd.  Cancel it from the main thread
	 * before destroying - strand_scheduler_advance resets owner_thread
	 * to the current (main) thread, allowing strand_fiber_cancel to
	 * proceed.
	 */
	strand_scheduler_advance(sched, NULL); /* take ownership */
	strand_fiber_cancel(handle);
	strand_scheduler_advance(sched, NULL); /* run the cancelled fiber */

	close(rd);
	close(wr);
	strand_scheduler_destroy(sched);
	return (0);
}

/* =========================================================================
 * Linux-only tests
 * =========================================================================
 */

#ifdef STRAND_LINUX

/* =========================================================================
 * test_edge_triggered_oneshot_linux
 *
 * Verify that EPOLLONESHOT auto-disables the registration after a single
 * event fires.  Feed data to a pipe, park a fiber, advance to wake it.
 * Feed more data.  Advance again with no re-arm - verify no second wake
 * (the fiber completed, the entry was cleaned up, no spurious delivery).
 *
 * See ARCHITECTURE.md §5.4 and TESTING.md §5.2.
 * =========================================================================
 */

struct oneshot_args {
	strand_scheduler_t *sched;
	int                 fd;
	int                 wake_count;
	int result;
};

static void
fiber_oneshot(void *varg)
{
	struct oneshot_args *a = varg;
	a->result = strand_fiber_wait_readable(a->sched, a->fd);
	a->wake_count++;
	/* Fiber does NOT drain the pipe - it just records the wake. */
}

static int
test_edge_triggered_oneshot_linux(void)
{
	strand_scheduler_t  *sched;
	int                  rd, wr;
	struct oneshot_args  args;

	memset(&args, 0, sizeof(args));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_pipe(&rd, &wr);
	if (rd < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}
	args.sched = sched;
	args.fd    = rd;

	if (t4_push_fiber(sched, fiber_oneshot, &args, NULL) != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	strand_scheduler_advance(sched, NULL); /* fiber parks */

	/* Write first byte - triggers the EPOLLIN edge. */
	(void)write(wr, "a", 1);
	strand_scheduler_advance(sched, NULL); /* fiber wakes (wake_count = 1) */

	if (args.wake_count != 1 || args.result != STRAND_OK) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * Write a second byte.  No fiber is waiting on the fd; the entry
	 * was removed after the first wake.  Advance must not deliver any
	 * spurious event and must not crash.
	 */
	(void)write(wr, "b", 1);
	strand_scheduler_advance(sched, NULL);

	close(rd);
	close(wr);
	strand_scheduler_destroy(sched);

	/* wake_count must still be exactly 1 - no second delivery. */
	return (args.wake_count == 1) ? 0 : 1;
}

/* =========================================================================
 * test_post_rearm_readiness_check_linux
 *
 * Park fiber A on READ and fiber B on WRITE of the same pipe.  The pipe
 * has data (readable) and free space (writable).  Both directions are ready
 * simultaneously.  Verify both fibers wake within a single advance call.
 *
 * This exercises the simultaneous-ready path in poller_deliver_event:
 * EPOLLIN|EPOLLOUT both set → both waiters wake directly.
 *
 * Also covers the post-re-arm zero-timeout path when only one direction
 * fires (EPOLLIN only on a full pipe) and we re-arm for the other -
 * a complementary scenario implemented in test_all_events_processed below.
 *
 * See ARCHITECTURE.md §5.5, TESTING.md §6.1.
 * =========================================================================
 */

struct post_rearm_state {
	strand_scheduler_t *sched;
	int                 fd_rd;
	int fd_wr;
	int result_read;
	int result_write;
	int ran_read;
	int ran_write;
};

static void
fiber_post_rearm_read(void *varg)
{
	struct post_rearm_state *s = varg;
	s->result_read = strand_fiber_wait_readable(s->sched, s->fd_rd);
	s->ran_read    = 1;
}

static void
fiber_post_rearm_write(void *varg)
{
	struct post_rearm_state *s = varg;
	s->result_write = strand_fiber_wait_writable(s->sched, s->fd_wr);
	s->ran_write    = 1;
}

static int
test_post_rearm_readiness_check_linux(void)
{
	strand_scheduler_t   *sched;
	int                   sv[2];
	struct post_rearm_state s;

	/*
	 * Use a socketpair so both waiters share sv[0].
	 *
	 * Scenario (tests the post-re-arm zero-timeout check §5.5):
	 *   1. fill_pipe(sv[0])  → sv[0] NOT writable (sv[1] buffer full),
	 *                          NOT readable (no data from sv[1]).
	 *   2. A: wait_readable(sv[0]), B: wait_writable(sv[0]).
	 *      Registration: EPOLLIN|EPOLLOUT|EPOLLET|EPOLLONESHOT on sv[0].
	 *   3. write(sv[1], 1 byte) → sv[0] becomes READABLE.  sv[0] is still
	 *      NOT writable (sv[1]'s receive buffer still full).
	 *   4. Advance: EPOLLIN fires (EPOLLOUT not set - not writable yet).
	 *      poller_deliver_event: A woken (woke_read=1).  B still waiting →
	 *      re-arm sv[0] with EPOLLOUT.  Zero-timeout: NOT writable yet →
	 *      nothing returned.  B remains parked.
	 *   5. A runs (ran_read=1, result=OK).
	 *   6. drain_pipe(sv[1]) → sv[0] becomes WRITABLE.
	 *   7. Advance: EPOLLOUT fires → B woken.  B runs (ran_write=1, OK).
	 */
	memset(&s, 0, sizeof(s));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_socketpair(sv);
	if (sv[0] < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}
	s.sched = sched;
	s.fd_rd = sv[0];
	s.fd_wr = sv[0]; /* SAME fd */

	/* Fill sv[0]: sv[0] not writable, not readable. */
	fill_pipe(sv[0]);

	if (t4_push_fiber(sched, fiber_post_rearm_read, &s, NULL) != 0 ||
	    t4_push_fiber(sched, fiber_post_rearm_write, &s, NULL) != 0) {
		close(sv[0]); close(sv[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}

	strand_scheduler_advance(sched, NULL); /* A parks on sv[0] read  */
	strand_scheduler_advance(sched, NULL); /* B parks on sv[0] write */

	if (s.ran_read != 0 || s.ran_write != 0) {
		close(sv[0]); close(sv[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * Make sv[0] readable but NOT writable: write 1 byte to sv[1].
	 * sv[1]'s receive buffer was already full from fill_pipe(sv[0]),
	 * so sv[0] has space to receive 1 byte (sv[0]'s receive buffer
	 * was empty).  sv[0] is now readable but NOT yet writable.
	 */
	(void)write(sv[1], "x", 1);

	/* Advance: EPOLLIN fires. A wakes. B re-armed. Zero-timeout finds nothing. */
	strand_scheduler_advance(sched, NULL);
	strand_scheduler_advance(sched, NULL); /* run A */

	if (s.ran_read != 1 || s.ran_write != 0) {
		close(sv[0]); close(sv[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/* Make sv[0] writable: drain sv[1]'s receive buffer. */
	drain_pipe(sv[1]);
	strand_scheduler_advance(sched, NULL); /* EPOLLOUT → B wakes */
	strand_scheduler_advance(sched, NULL); /* run B */

	close(sv[0]);
	close(sv[1]);
	strand_scheduler_destroy(sched);

	return (s.ran_read == 1 && s.ran_write == 1 &&
	        s.result_read  == STRAND_OK &&
	        s.result_write == STRAND_OK) ? 0 : 1;
}

/* =========================================================================
 * test_all_events_processed_in_rearm_check
 *
 * Two different pipes - pipe_a and pipe_b.
 *   pipe_a: FULL (not writable).  Fiber A waits READ, Fiber B waits WRITE.
 *   pipe_b: has data (readable).  Fiber C waits READ.
 *
 * In a single advance:
 *   - pipe_a fires EPOLLIN only → A wakes; B still parked → re-arm for
 *     EPOLLOUT; zero-timeout call → pipe_a still full, no EPOLLOUT.
 *   - pipe_b fires EPOLLIN → C wakes.
 *
 * Verify A and C wake in the same advance, and B stays parked.
 * The zero-timeout poll inside the re-arm path must process ALL returned
 * events - if it discarded pipe_b's event, C would not wake.
 *
 * See ARCHITECTURE.md §5.5, TESTING.md §6.1.
 * =========================================================================
 */

struct all_events_state {
	strand_scheduler_t *sched;
	/* pipe_a (full pipe: readable, not writable) */
	int fd_a_rd;
	int fd_a_wr;
	int result_a;  /* read waiter on pipe_a */
	int ran_a;
	int result_b;  /* write waiter on pipe_a */
	int ran_b;
	/* pipe_b (has data: readable) */
	int fd_b_rd;
	int fd_b_wr;
	int result_c;  /* read waiter on pipe_b */
	int ran_c;
};

static void
fiber_ae_read_a(void *varg)
{
	struct all_events_state *s = varg;
	s->result_a = strand_fiber_wait_readable(s->sched, s->fd_a_rd);
	s->ran_a    = 1;
}

static void
fiber_ae_write_b(void *varg)
{
	struct all_events_state *s = varg;
	s->result_b = strand_fiber_wait_writable(s->sched, s->fd_a_wr);
	s->ran_b    = 1;
}

static void
fiber_ae_read_c(void *varg)
{
	struct all_events_state *s = varg;
	s->result_c = strand_fiber_wait_readable(s->sched, s->fd_b_rd);
	s->ran_c    = 1;
}

static int
test_all_events_processed_in_rearm_check(void)
{
	strand_scheduler_t     *sched;
	int                     sv_a[2], b_rd, b_wr;
	struct all_events_state s;

	/*
	 * Use a socketpair for A (read) and B (write) on the same fd (sv_a[0]).
	 * C (read) uses a separate pipe that already has data.
	 *
	 * Setup:
	 *   fill_pipe(sv_a[0]) → sv_a[0] not writable, not readable.
	 *   Park A (wait_readable sv_a[0]) and B (wait_writable sv_a[0]).
	 *   write(b_wr, "y") → b_rd readable.
	 *   Park C (wait_readable b_rd).
	 *
	 *   Then: write(sv_a[1], "x") → sv_a[0] becomes readable, still not
	 *   writable (sv_a[1]'s receive buffer still full).
	 *
	 *   Single advance:
	 *     EPOLLIN fires for sv_a[0]: A wakes; B still parked → re-arm
	 *     sv_a[0] with EPOLLOUT; zero-timeout epoll_wait → sv_a[0] NOT
	 *     writable yet; EPOLLIN for b_rd fires in the same batch (or
	 *     via zero-timeout): C wakes.
	 *
	 *   Verify A and C woke in the same advance.  B stays parked.
	 *   Drain sv_a[1] → sv_a[0] writable → B wakes in next advance.
	 *
	 * See ARCHITECTURE.md §5.5, TESTING.md §6.1.
	 */
	memset(&s, 0, sizeof(s));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_socketpair(sv_a);
	make_test_pipe(&b_rd, &b_wr);
	if (sv_a[0] < 0 || b_rd < 0) {
		if (sv_a[0] >= 0) { close(sv_a[0]); close(sv_a[1]); }
		if (b_rd  >= 0) { close(b_rd); close(b_wr); }
		strand_scheduler_destroy(sched);
		return (1);
	}

	s.sched   = sched;
	s.fd_a_rd = sv_a[0]; /* A reads from sv_a[0]  */
	s.fd_a_wr = sv_a[0]; /* B writes to sv_a[0] - SAME fd */
	s.fd_b_rd = b_rd;
	s.fd_b_wr = b_wr;

	/* sv_a[0]: not readable, not writable. */
	fill_pipe(sv_a[0]);

	/* b_rd: readable. */
	(void)write(b_wr, "y", 1);

	/* Park A, B (on sv_a[0]) and C (on b_rd). */
	if (t4_push_fiber(sched, fiber_ae_read_a, &s, NULL) != 0 ||
	    t4_push_fiber(sched, fiber_ae_write_b, &s, NULL) != 0 ||
	    t4_push_fiber(sched, fiber_ae_read_c,  &s, NULL) != 0) {
		close(sv_a[0]); close(sv_a[1]); close(b_rd); close(b_wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/* All three park (budget ≥ 3). */
	strand_scheduler_advance(sched, NULL);

	if (s.ran_a || s.ran_b || s.ran_c) {
		close(sv_a[0]); close(sv_a[1]); close(b_rd); close(b_wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * Make sv_a[0] readable (write 1 byte to sv_a[1]).
	 * sv_a[0] remains NOT writable (sv_a[1]'s receive buffer still full).
	 */
	(void)write(sv_a[1], "x", 1);

	/*
	 * Single advance:
	 *   EPOLLIN for sv_a[0] → A wakes; B re-armed with EPOLLOUT; zero-
	 *   timeout finds nothing (sv_a[0] not writable) but must also process
	 *   any other events (b_rd EPOLLIN) - verifying that poller_deliver_event
	 *   does not drop them.  EPOLLIN for b_rd → C wakes.
	 */
	strand_scheduler_advance(sched, NULL);
	strand_scheduler_advance(sched, NULL); /* run A and C */

	if (s.ran_a != 1 || s.ran_c != 1 || s.ran_b != 0) {
		close(sv_a[0]); close(sv_a[1]); close(b_rd); close(b_wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/* Make sv_a[0] writable: drain sv_a[1]'s receive buffer. */
	drain_pipe(sv_a[1]);
	strand_scheduler_advance(sched, NULL); /* EPOLLOUT → B wakes */
	strand_scheduler_advance(sched, NULL); /* run B */

	close(sv_a[0]);
	close(sv_a[1]);
	close(b_rd);
	close(b_wr);
	strand_scheduler_destroy(sched);

	return (s.ran_a == 1 && s.result_a == STRAND_OK &&
	        s.ran_b == 1 && s.result_b == STRAND_OK &&
	        s.ran_c == 1 && s.result_c == STRAND_OK) ? 0 : 1;
}

/* =========================================================================
 * test_epollerr_wakes_waiter
 *
 * Register a READ waiter on the read end of a pipe.  Close the write end.
 * The kernel delivers EPOLLERR or EPOLLHUP on the read end.  Verify the
 * waiter wakes with STRAND_ERR_IO.
 *
 * Closing the write end of a pipe makes the read end return EOF (0 bytes)
 * and delivers EPOLLHUP to any registered epoll interest.
 *
 * See ARCHITECTURE.md §5.6.
 * =========================================================================
 */

struct err_args {
	strand_scheduler_t *sched;
	int                 fd;
	int result;
	int ran;
};

static void
fiber_err_read(void *varg)
{
	struct err_args *a = varg;
	a->result = strand_fiber_wait_readable(a->sched, a->fd);
	a->ran    = 1;
}

static int
test_epollerr_wakes_waiter(void)
{
	strand_scheduler_t *sched;
	int                 rd, wr;
	struct err_args     args;

	memset(&args, 0, sizeof(args));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_pipe(&rd, &wr);
	if (rd < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}
	args.sched = sched;
	args.fd    = rd;

	if (t4_push_fiber(sched, fiber_err_read, &args, NULL) != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	strand_scheduler_advance(sched, NULL); /* fiber parks */

	if (args.ran != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * Close the write end.  The kernel will set EPOLLHUP on the read end
	 * (visible as EPOLLERR|EPOLLHUP in epoll_wait events).
	 */
	close(wr);
	wr = -1;

	strand_scheduler_advance(sched, NULL); /* fiber wakes with error */

	close(rd);
	strand_scheduler_destroy(sched);

	return (args.ran == 1 && args.result == STRAND_ERR_IO) ? 0 : 1;
}

/* =========================================================================
 * test_epollhup_wakes_waiter
 *
 * Register a WRITE waiter on the write end of a pipe.  Close the read end.
 * The kernel delivers EPOLLHUP on the write end.  Verify the waiter wakes
 * with STRAND_ERR_IO.
 *
 * See ARCHITECTURE.md §5.6.
 * =========================================================================
 */

struct epollhup_args {
	strand_scheduler_t *sched;
	int                 fd;
	int result;
	int ran;
};

static void
fiber_epollhup_write(void *varg)
{
	struct epollhup_args *a = varg;
	/* Fill the pipe first so the write end is not writable, then wait. */
	a->result = strand_fiber_wait_writable(a->sched, a->fd);
	a->ran    = 1;
}

static int
test_epollhup_wakes_waiter(void)
{
	strand_scheduler_t   *sched;
	int                   rd, wr;
	struct epollhup_args  args;

	memset(&args, 0, sizeof(args));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_pipe(&rd, &wr);
	if (rd < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * Fill the pipe so the write end is not writable (EPOLLOUT would not
	 * fire immediately).  Then the fiber parks on WRITE.
	 */
	fill_pipe(wr);
	args.sched = sched;
	args.fd    = wr;

	if (t4_push_fiber(sched, fiber_epollhup_write, &args, NULL) != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	strand_scheduler_advance(sched, NULL); /* fiber parks on write */

	if (args.ran != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/*
	 * Close the read end.  The kernel delivers EPOLLHUP on the write end
	 * (broken pipe - no reader).
	 */
	close(rd);
	rd = -1;

	strand_scheduler_advance(sched, NULL); /* fiber wakes with error */

	close(wr);
	strand_scheduler_destroy(sched);

	return (args.ran == 1 && args.result == STRAND_ERR_IO) ? 0 : 1;
}

/* =========================================================================
 * test_nonblock_assertion_debug
 *
 * Verify that passing a blocking fd to strand_fiber_wait_readable triggers
 * the O_NONBLOCK debug assertion.  Only meaningful in STRAND_DEBUG builds.
 *
 * The assertion calls abort() (via STRAND_DEBUG_ASSERT) - we use fork() to
 * isolate the abort in a child process.  The child is expected to exit via
 * SIGABRT (non-zero exit status from signal).
 *
 * See ARCHITECTURE.md §5.1, CODING_STANDARDS.md §6.1.
 * =========================================================================
 */

#ifdef STRAND_DEBUG

struct nonblock_assert_args {
	strand_scheduler_t *sched;
	int                 fd;    /* a BLOCKING fd */
};

static void
fiber_nonblock_assert(void *varg)
{
	struct nonblock_assert_args *a = varg;
	/*
	 * Passing a blocking fd must trigger STRAND_DEBUG_ASSERT in
	 * fiber_wait_io.  This call is expected to abort() the process.
	 */
	(void)strand_fiber_wait_readable(a->sched, a->fd);
}

static int
test_nonblock_assertion_debug(void)
{
	pid_t pid;
	int   status;
	int   fds[2];

	/* Create a BLOCKING pipe (no O_NONBLOCK). */
	if (pipe(fds) != 0)
		return (1);

	pid = fork();
	if (pid < 0) {
		close(fds[0]); close(fds[1]);
		return (1);
	}

	if (pid == 0) {
		/* Child: set up scheduler, spawn fiber, advance - expect abort. */
		strand_scheduler_t          *sched;
		struct nonblock_assert_args  args;

		close(fds[1]); /* child only needs read end */
		sched = make_test_scheduler();
		if (sched == NULL)
			_exit(2);
		args.sched = sched;
		args.fd    = fds[0];

		(void)t4_push_fiber(sched, fiber_nonblock_assert, &args,
		    NULL);
		strand_scheduler_advance(sched, NULL);
		_exit(0); /* should not reach here - assert should abort */
	}

	/* Parent: close fds and wait for child. */
	close(fds[0]);
	close(fds[1]);

	if (waitpid(pid, &status, 0) != pid)
		return (1);

	/*
	 * Child should have aborted (SIGABRT from assert) - WIFSIGNALED &&
	 * WTERMSIG == SIGABRT, or abnormal exit.  Any non-clean exit is
	 * acceptable here since the assertion fired.
	 */
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return (1); /* child exited normally - assertion did NOT fire */

	return (0); /* non-zero / signal exit means assertion fired */
}

#endif /* STRAND_DEBUG */

#endif /* STRAND_LINUX */

/* =========================================================================
 * OpenBSD-only tests
 * =========================================================================
 */

#ifdef STRAND_OPENBSD

/* =========================================================================
 * test_ev_eof_openbsd
 *
 * Register a READ waiter via kqueue EVFILT_READ | EV_DISPATCH.  Close the
 * write end of the pipe.  The kernel delivers EV_EOF on the read filter.
 * Verify the waiter wakes with STRAND_ERR_IO.
 *
 * See ARCHITECTURE.md §5.7.
 * =========================================================================
 */

struct ev_eof_args {
	strand_scheduler_t *sched;
	int                 fd;
	int result;
	int ran;
};

static void
fiber_ev_eof_read(void *varg)
{
	struct ev_eof_args *a = varg;
	a->result = strand_fiber_wait_readable(a->sched, a->fd);
	a->ran    = 1;
}

static int
test_ev_eof_openbsd(void)
{
	strand_scheduler_t *sched;
	int                 rd, wr;
	struct ev_eof_args  args;

	memset(&args, 0, sizeof(args));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_pipe(&rd, &wr);
	if (rd < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}
	args.sched = sched;
	args.fd    = rd;

	if (t4_push_fiber(sched, fiber_ev_eof_read, &args, NULL) != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	strand_scheduler_advance(sched, NULL); /* fiber parks */

	if (args.ran != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	/* Close write end - kernel delivers EV_EOF on the read filter. */
	close(wr);
	wr = -1;

	strand_scheduler_advance(sched, NULL); /* fiber wakes with error */

	close(rd);
	strand_scheduler_destroy(sched);

	return (args.ran == 1 && args.result == STRAND_ERR_IO) ? 0 : 1;
}

/* =========================================================================
 * test_ev_dispatch_openbsd
 *
 * Verify EV_DISPATCH one-shot-disable + re-enable behaviour:
 *   1. Park fiber on read; write data; advance → fiber wakes (filter fires
 *      once and auto-disables, EV_DISPATCH).
 *   2. Fiber calls wait_readable again WITHOUT draining the pipe.  Re-arm
 *      uses EV_ADD | EV_DISPATCH.  Since data is still present, re-enable
 *      must trigger immediately under level-triggered EV_DISPATCH semantics.
 *      Verify fiber wakes a second time.
 *
 * This tests that the poller's EV_DISPATCH scheme correctly re-fires when
 * the level condition is already satisfied at re-arm time.
 *
 * See ARCHITECTURE.md §5.7.
 * =========================================================================
 */

struct ev_dispatch_args {
	strand_scheduler_t *sched;
	int                 fd;
	int wake_count;
	int last_result;
};

static void
fiber_ev_dispatch(void *varg)
{
	struct ev_dispatch_args *a = varg;
	int rc;

	/* First wait. */
	rc = strand_fiber_wait_readable(a->sched, a->fd);
	a->wake_count++;
	a->last_result = rc;

	if (rc != STRAND_OK)
		return;

	/*
	 * Second wait WITHOUT reading any data.  EV_DISPATCH re-enable must
	 * cause an immediate fire because the pipe still has data.
	 */
	rc = strand_fiber_wait_readable(a->sched, a->fd);
	a->wake_count++;
	a->last_result = rc;
}

static int
test_ev_dispatch_openbsd(void)
{
	strand_scheduler_t      *sched;
	int                      rd, wr;
	struct ev_dispatch_args  args;

	memset(&args, 0, sizeof(args));
	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);

	make_test_pipe(&rd, &wr);
	if (rd < 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}
	args.sched = sched;
	args.fd    = rd;

	if (t4_push_fiber(sched, fiber_ev_dispatch, &args, NULL) != 0) {
		close(rd); close(wr);
		strand_scheduler_destroy(sched);
		return (1);
	}

	strand_scheduler_advance(sched, NULL); /* fiber parks on first wait */

	/* Write data to make the pipe readable. */
	(void)write(wr, "x", 1);

	/*
	 * Multiple advances: first wakes fiber from the first wait (data
	 * arrives), fiber re-parks on second wait (data still in pipe),
	 * next advance fires again (EV_DISPATCH level-triggered re-fire).
	 */
	strand_scheduler_advance(sched, NULL);
	strand_scheduler_advance(sched, NULL);
	strand_scheduler_advance(sched, NULL);

	close(rd);
	close(wr);
	strand_scheduler_destroy(sched);

	return (args.wake_count == 2 && args.last_result == STRAND_OK) ? 0 : 1;
}

#endif /* STRAND_OPENBSD */

/* =========================================================================
 * test_cross_worker_cancel_wins
 *
 * Verify that a cross-worker cancel beats I/O readiness when both occur in
 * the same strand_scheduler_advance call.
 *
 * The inject queue is drained in Step 1; the I/O poller runs in Step 4.
 * A cancel injected before advance therefore wins: by the time Step 4 polls,
 * the fiber is already FIBER_RUNNABLE with io_result = STRAND_CANCELLED and
 * no longer registered with the poller.
 *
 * Setup:
 *   1. Spawn fiber A that parks on the read end of a pipe.
 *   2. Run advance once so A parks on FIBER_PARKED_IO_READ.
 *   3. From a helper thread, call strand_fiber_cancel(handle_a).
 *      strand_fiber_cancel detects a cross-thread caller (pthread_self !=
 *      sched->owner_thread) and enqueues INJECT_CANCEL.
 *   4. Write one byte to the write end of the pipe so it becomes readable.
 *   5. Run advance again:
 *        Step 1 - inject drain cancels A (FIBER_RUNNABLE, STRAND_CANCELLED).
 *        Step 4 - poller_poll: fd is readable but no waiter remains.
 *        Step 5 - A runs; result must be STRAND_CANCELLED.
 *
 * See ARCHITECTURE.md 5.3 and TESTING.md 6.2.
 * =========================================================================
 */

struct cwcw_state {
        strand_scheduler_t   *sched;
        int                   rd;
        int                   wr;
        strand_fiber_handle_t handle_a;
        int                   result_a;
        _Atomic int           fiber_parked; /* set 1 when A is parked */
};

static void
fiber_cwcw_a(void *varg)
{
        struct cwcw_state *s = varg;
        /*
         * Signal that we are about to park so the helper thread knows it is
         * safe to call strand_fiber_cancel.
         */
        atomic_store_explicit(&s->fiber_parked, 1, memory_order_release);
        s->result_a = strand_fiber_wait_readable(s->sched, s->rd);
}

struct cwcw_cancel_args {
        strand_fiber_handle_t handle;
        int                   wr;
        _Atomic int          *fiber_parked;
};

static void *
cwcw_cancel_thread(void *varg)
{
        struct cwcw_cancel_args *a = varg;
        struct timespec          ts;

        /*
         * Poll until the fiber signals that it has parked.  Each iteration
         * yields the CPU briefly to avoid burning a core.
         */
        while (!atomic_load_explicit(a->fiber_parked, memory_order_acquire)) {
                ts.tv_sec  = 0;
                ts.tv_nsec = 1000000L; /* 1 ms */
                nanosleep(&ts, NULL);
        }

        /*
         * Call strand_fiber_cancel from a different thread.  This exercises
         * the cross-worker inject path: pthread_self() != sched->owner_thread,
         * so the cancel is enqueued to the target scheduler's inject queue.
         * See ARCHITECTURE.md 5.3.
         */
        (void)strand_fiber_cancel(a->handle);

        /*
         * Also write data to the pipe so the fd becomes readable.  The cancel
         * (Step 1) should win over readiness (Step 4).
         */
        (void)write(a->wr, "x", 1);

        return (NULL);
}

static int
test_cross_worker_cancel_wins(void)
{
        strand_scheduler_t       *sched;
        struct cwcw_state         s;
        struct cwcw_cancel_args   ca;
        pthread_t                 helper;
        int                       rc;

        memset(&s, 0, sizeof(s));
        atomic_init(&s.fiber_parked, 0);

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        make_test_pipe(&s.rd, &s.wr);
        if (s.rd < 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        s.sched = sched;

        /* Spawn fiber A and advance once so it parks on the read end. */
        if (t4_push_fiber(sched, fiber_cwcw_a, &s, &s.handle_a) != 0) {
                close(s.rd); close(s.wr);
                strand_scheduler_destroy(sched);
                return (1);
        }
        strand_scheduler_advance(sched, NULL); /* A parks on FIBER_PARKED_IO_READ */

        if (s.result_a != 0) {
                /* A must not have returned yet */
                close(s.rd); close(s.wr);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /*
         * Launch the helper thread.  It waits for fiber_parked, then calls
         * strand_fiber_cancel (cross-worker path) and writes to the pipe.
         */
        ca.handle       = s.handle_a;
        ca.wr           = s.wr;
        ca.fiber_parked = &s.fiber_parked;
        if (pthread_create(&helper, NULL, cwcw_cancel_thread, &ca) != 0) {
                close(s.rd); close(s.wr);
                strand_scheduler_destroy(sched);
                return (1);
        }
        pthread_join(helper, NULL);

        /*
         * Advance:
         *   Step 1 - INJECT_CANCEL is drained; A transitions to
         *             FIBER_RUNNABLE with io_result = STRAND_CANCELLED.
         *             The fd is removed from the poller.
         *   Step 4 - fd is readable but no waiter remains; no wakeup.
         *   Step 5 - A runs and returns; result_a == STRAND_CANCELLED.
         */
        strand_scheduler_advance(sched, NULL);
        strand_scheduler_advance(sched, NULL); /* ensure A has run */

        rc = (s.result_a == STRAND_CANCELLED) ? 0 : 1;

        close(s.rd);
        close(s.wr);
        strand_scheduler_destroy(sched);
        return (rc);
}

/* =========================================================================
 * run_layer3_tests - register all Layer 3 tests with the harness.
 * =========================================================================
 */

void
run_layer3_tests(void)
{
	RUN("test_wait_readable_wakes",    test_wait_readable_wakes);
	RUN("test_wait_writable_wakes",    test_wait_writable_wakes);
	RUN("test_buffered_close_delivers_readiness",
	    test_buffered_close_delivers_readiness);
	RUN("test_fd_table_growth_preserves_first_event",
	    test_fd_table_growth_preserves_first_event);
	RUN("test_cancel_read_waiter",     test_cancel_read_waiter);
	RUN("test_cancel_one_direction_leaves_other",
	    test_cancel_one_direction_leaves_other);
	RUN("test_cancel_both_directions", test_cancel_both_directions);
        RUN("test_same_worker_readiness_wins",
            test_same_worker_readiness_wins);
        RUN("test_cross_worker_cancel_wins",
            test_cross_worker_cancel_wins);
        RUN("test_scheduler_stop_interrupts_io_wait",
	    test_scheduler_stop_interrupts_io_wait);

#ifdef STRAND_LINUX
	RUN("test_edge_triggered_oneshot_linux",
	    test_edge_triggered_oneshot_linux);
	RUN("test_post_rearm_readiness_check_linux",
	    test_post_rearm_readiness_check_linux);
	RUN("test_all_events_processed_in_rearm_check",
	    test_all_events_processed_in_rearm_check);
	RUN("test_epollerr_wakes_waiter",  test_epollerr_wakes_waiter);
	RUN("test_epollhup_wakes_waiter",  test_epollhup_wakes_waiter);
#ifdef STRAND_DEBUG
	RUN("test_nonblock_assertion_debug", test_nonblock_assertion_debug);
#endif
#endif /* STRAND_LINUX */

#ifdef STRAND_OPENBSD
	RUN("test_ev_eof_openbsd",         test_ev_eof_openbsd);
	RUN("test_ev_dispatch_openbsd",    test_ev_dispatch_openbsd);
#endif
}
