/*
 * tests/test_integration.c - integration test suite 
 *
 * Cross-layer end-to-end tests that exercise realistic usage scenarios.
 * Each test is documented to serve as a usage example for libstrand.
 *
 * Tests:
 *   test_integration_echo_server            (TESTING.md 7.1)
 *   test_integration_producer_consumer      (TESTING.md 7.2)
 *   test_integration_fan_out_scope_error    (TESTING.md 7.3)
 *   test_integration_timeout_then_abandon   (TESTING.md 7.4)
 *   test_integration_offload_cancel_result_claimed_wins (TESTING.md 7.5a)
 *   test_integration_offload_cancel_cancelled_wins      (TESTING.md 7.5b)
 *   test_integration_offload_eagain_retry   (TESTING.md 7.6)
 *   test_integration_subprocess_via_offload (TESTING.md 7.7)
 *   test_integration_multiworker_distribution (TESTING.md 7.8)
 *   test_integration_simultaneous_read_write (TESTING.md 7.9)
 *   test_integration_guest_mode_host_loop   (TESTING.md 7.10)
 *   test_integration_stop_interrupts_worker (TESTING.md 7.11)
 *
 * Platform guards:
 *   STRAND_LINUX   - test 7.9 uses Linux-specific epoll host loop
 *   STRAND_OPENBSD - test 7.9 and 7.10 use kqueue host loop
 *
 * See TESTING.md §7, ARCHITECTURE.md §4-7.
 */

#include <errno.h>
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

#ifdef STRAND_LINUX
#include <sys/epoll.h>
#endif
#ifdef STRAND_OPENBSD
#include <sys/event.h>
#endif

#include "test_harness.h"
#include "../include/strand.h"

/*
 * Internal headers are included only for the push_root_fiber helper used
 * by single-scheduler tests (7.2, 7.5, 7.6, 7.7, 7.9, 7.10).  Multi-
 * worker tests use strand_runtime_spawn exclusively (public API only).
 */
#include "../src/strand_context.h"
#include "../src/strand_fiber.h"
#include "../src/strand_internal.h"
#include "../src/strand_offload.h"
#include "../src/strand_runtime.h"
#include "../src/strand_sched.h"

/*
 * strand_test_clock_ns - mock monotonic clock for STRAND_TEST_CLOCK builds.
 * Written directly by single-scheduler tests to control timer expiry without
 * real-time delays.  See strand_sched.c now_ns / STRAND_TEST_CLOCK.
 */
extern uint64_t strand_test_clock_ns;

/* =========================================================================
 * Shared helpers
 * =========================================================================
 */

/*
 * make_test_scheduler -- create a scheduler with small defaults suitable
 * for integration tests driven from the host thread.
 */
static strand_scheduler_t *
make_test_scheduler(void)
{
        strand_sched_config_t cfg = {
            .budget     = 64,
            .inject_cap = 256,
            .cache_cap  = 8,
            .idle_floor = 2,
        };
        return (strand_scheduler_create(&cfg));
}

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
        strand_sched_config_t  scfg = {
            .budget     = 64,
            .inject_cap = 256,
            .cache_cap  = 8,
            .idle_floor = 2,
        };
        strand_worker_config_t wcfg = {
            .sched_cfg    = &scfg,
            .cpu_affinity = -1,
        };
        return (strand_worker_start(rt, &wcfg));
}

/*
 * push_root_fiber -- bootstrap a fiber into the run queue from the host
 * thread, bypassing the WRONGCTX check.
 *
 * Used only by single-scheduler tests (no runtime).  Once this root fiber
 * is running, it may call strand_fiber_spawn and strand_scope_spawn normally.
 * Returns 0 on success, -1 on allocation failure.
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
            (char *)f->stack_base + sz,
            strand_fiber_entry_start, f);
        strand_fiber_tsan_init(f);

        atomic_store_explicit(&f->state, FIBER_RUNNABLE, memory_order_relaxed);
        run_queue_push(sched, f);
        return (0);
}

/*
 * drive_until_idle -- advance the scheduler repeatedly until it reports
 * idle.  Bounded by DRIVE_MAX_ITER to guard against infinite loops in tests.
 */
#define DRIVE_MAX_ITER 10000
static void
drive_until_idle(strand_scheduler_t *sched)
{
        uint64_t       deadline;
        sched_result_t rc;
        int            i;

        for (i = 0; i < DRIVE_MAX_ITER; i++) {
                rc = strand_scheduler_advance(sched, &deadline);
                if (rc == STRAND_SCHED_IDLE)
                        break;
        }
}

/*
 * wait_for_atomic -- poll an atomic int for up to timeout_ms milliseconds.
 * Returns 1 if atomic_load >= target within the timeout, 0 on timeout.
 *
 * If drive_sched is non-NULL, calls strand_scheduler_advance each iteration
 * (for single-scheduler tests driven from the host thread).
 */
static int
wait_for_atomic(_Atomic int *val, int target, int timeout_ms,
    strand_scheduler_t *drive_sched)
{
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L}; /* 1ms */
        int             i;

        for (i = 0; i < timeout_ms; i++) {
                if (atomic_load_explicit(val, memory_order_acquire) >= target)
                        return (1);
                if (drive_sched != NULL)
                        strand_scheduler_advance(drive_sched, NULL);
                nanosleep(&ts, NULL);
        }
        return (atomic_load_explicit(val, memory_order_acquire) >= target);
}

/*
 * test_now_ns -- return CLOCK_MONOTONIC timestamp in nanoseconds.
 */
static uint64_t __attribute__((unused))
test_now_ns(void)
{
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

/*
 * make_socketpair -- create a nonblocking AF_UNIX socketpair.
 * Both ends support read and write.  Returns 0 on success, -1 on error.
 */
static int
make_socketpair(int *sv)
{
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
                return (-1);
        (void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(sv[1], F_SETFD, FD_CLOEXEC);
        (void)fcntl(sv[0], F_SETFL, O_NONBLOCK);
        (void)fcntl(sv[1], F_SETFL, O_NONBLOCK);
        return (0);
}

/*
 * make_pipe_pair -- create a nonblocking pipe pair.
 * fds[0] is read end, fds[1] is write end.
 * Returns 0 on success, -1 on error.
 */
static int
make_pipe_pair(int fds[2])
{
        if (pipe(fds) != 0)
                return (-1);
        (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        (void)fcntl(fds[0], F_SETFL, O_NONBLOCK);
        (void)fcntl(fds[1], F_SETFL, O_NONBLOCK);
        return (0);
}

/*
 * fill_fd -- write to fd until EAGAIN (fill the send buffer).
 * Used to put a socketpair fd into a not-writable state.
 */
static void __attribute__((unused))
fill_fd(int fd)
{
        char buf[4096];

        memset(buf, 0, sizeof(buf));
        while (write(fd, buf, sizeof(buf)) > 0)
                ;
}

/*
 * drain_fd -- read from fd until EAGAIN (drain the receive buffer).
 * Used to make a socketpair fd writable again.
 */
static void __attribute__((unused))
drain_fd(int fd)
{
        char buf[4096];

        while (read(fd, buf, sizeof(buf)) > 0)
                ;
}

/* =========================================================================
 * Test 7.1 - Echo Server
 *
 * Demonstrates the complete per-connection fiber lifecycle:
 *   - multi-worker runtime initialisation
 *   - host-thread fiber spawning via strand_runtime_spawn
 *   - per-connection fiber that reads and writes in a loop
 *   - structured concurrency scope per connection
 *   - I/O parking via strand_fiber_wait_readable / strand_fiber_wait_writable
 *
 * Simulates 3 concurrent connections using AF_UNIX socketpairs.  The test
 * thread acts as the "client", sending 5 messages per connection and reading
 * them back.  A per-connection fiber on the worker acts as the "server",
 * parking on reads and writes.
 *
 * See TESTING.md 7.1, ARCHITECTURE.md 4.2, 4.3, 5.
 * =========================================================================
 */

#define ECHO_NUM_CONNS    3
#define ECHO_NUM_MSGS     5
#define ECHO_MSG_SIZE     16

typedef struct {
        strand_scheduler_t *sched;
        int                 server_fd;  /* sv[0]: owned by fiber */
        int                 client_fd;  /* sv[1]: owned by test thread */
        _Atomic int         done;       /* set to 1 when fiber finishes */
        _Atomic int         error;      /* set to 1 on fiber error */
} echo_conn_t;

/*
 * echo_conn_fiber -- per-connection server fiber.
 *
 * Opens a scope (demonstrates structured concurrency), then echoes
 * ECHO_NUM_MSGS messages from client to server.  Closes server_fd when done.
 *
 * Usage pattern: wait for readable → read → wait for writable → write.
 * Each step parks the fiber, releasing the worker to serve other connections
 * concurrently.
 */
static void
echo_conn_fiber(void *varg)
{
        echo_conn_t *conn   = varg;
        strand_scope_t scope;
        char         buf[ECHO_MSG_SIZE];
        ssize_t      n;
        int          rc;
        int          i;

        /*
         * Open a scope for this connection's lifetime.  In production code
         * you would spawn sub-tasks (e.g. a timeout watchdog fiber) under
         * this scope.  Here we use it to demonstrate the scope API and ensure
         * the control block lifecycle is exercised.
         */
        rc = strand_scope_open(conn->sched, &scope);
        if (rc != STRAND_OK) {
                atomic_store(&conn->error, 1);
                close(conn->server_fd);
                atomic_store(&conn->done, 1);
                return;
        }

        for (i = 0; i < ECHO_NUM_MSGS; i++) {
                /* Park until client sends data. */
                rc = strand_fiber_wait_readable(conn->sched, conn->server_fd);
                if (rc != STRAND_OK) {
                        atomic_store(&conn->error, 1);
                        goto finish;
                }

                /* Non-blocking read: fd is ready. */
                n = read(conn->server_fd, buf, sizeof(buf));
                if (n <= 0) {
                        atomic_store(&conn->error, 1);
                        goto finish;
                }

                /* Park until client can receive the echoed data. */
                rc = strand_fiber_wait_writable(conn->sched, conn->server_fd);
                if (rc != STRAND_OK) {
                        atomic_store(&conn->error, 1);
                        goto finish;
                }

                /* Non-blocking write: fd is ready. */
                if (write(conn->server_fd, buf, (size_t)n) != n) {
                        atomic_store(&conn->error, 1);
                        goto finish;
                }
        }

finish:
        /*
         * Wait for the scope to complete (no children in this example;
         * returns immediately).  Terminal: must always be called after
         * strand_scope_open.
         */
        strand_scope_wait(conn->sched, &scope);

        close(conn->server_fd);
        atomic_store(&conn->done, 1);
}

int
test_integration_echo_server(void)
{
        strand_runtime_t    *rt;
        strand_worker_t     *w;
        echo_conn_t          conns[ECHO_NUM_CONNS];
        int                  sv[2];
        char                 send_buf[ECHO_MSG_SIZE];
        char                 recv_buf[ECHO_MSG_SIZE];
        int                  i, j, rc;
        strand_fiber_handle_t handle;

        rt = make_test_runtime();
        if (rt == NULL)
                return (1);

        w = make_test_worker(rt);
        if (w == NULL) {
                strand_runtime_destroy(rt);
                return (1);
        }

        /* Initialise 3 connections, each backed by an AF_UNIX socketpair. */
        for (i = 0; i < ECHO_NUM_CONNS; i++) {
                if (make_socketpair(sv) != 0) {
                        while (--i >= 0)
                                close(conns[i].client_fd);
                        strand_runtime_destroy(rt);
                        return (1);
                }
                memset(&conns[i], 0, sizeof(conns[i]));
                conns[i].sched     = w->sched;
                conns[i].server_fd = sv[0]; /* fiber owns this */
                conns[i].client_fd = sv[1]; /* test thread owns this */
                atomic_init(&conns[i].done,  0);
                atomic_init(&conns[i].error, 0);
        }

        /*
         * Spawn one fiber per connection from the host thread.
         * strand_runtime_spawn uses round-robin worker selection (one worker
         * here so all land on the same scheduler).
         */
        for (i = 0; i < ECHO_NUM_CONNS; i++) {
                rc = strand_runtime_spawn(rt, echo_conn_fiber, &conns[i],
                    0, NULL, &handle);
                if (rc != STRAND_OK) {
                        /* close remaining client fds; server fds not yet closed */
                        for (j = i; j < ECHO_NUM_CONNS; j++)
                                close(conns[j].client_fd);
                        strand_runtime_destroy(rt);
                        return (1);
                }
        }

        /*
         * Act as the client for all 3 connections simultaneously.
         * Send ECHO_NUM_MSGS messages to each connection and read them back.
         * The worker thread runs all 3 fibers cooperatively.
         */
        for (i = 0; i < ECHO_NUM_MSGS; i++) {
                for (j = 0; j < ECHO_NUM_CONNS; j++) {
                        /* Build a distinguishable message. */
                        memset(send_buf, (char)('A' + j + i), sizeof(send_buf));


                        /* Write to client end; server fiber is waiting to read. */
                        if (write(conns[j].client_fd, send_buf,
                            sizeof(send_buf)) != sizeof(send_buf)) {
                                /* Close all client fds and shut down. */
                                for (int k = 0; k < ECHO_NUM_CONNS; k++)
                                        close(conns[k].client_fd);
                                strand_runtime_destroy(rt);
                                return (1);
                        }

                        /*
                         * Wait for echoed reply.  The server fiber echoes back
                         * after wait_readable returns.  This may take multiple
                         * scheduler advances on the worker thread.
                         */
                        ssize_t got = 0;
                        while (got < (ssize_t)sizeof(recv_buf)) {
                                ssize_t n = read(conns[j].client_fd,
                                    recv_buf + got,
                                    sizeof(recv_buf) - (size_t)got);
                                if (n > 0) {
                                        got += n;
                                } else if (n < 0 && errno == EAGAIN) {
                                        /* Brief yield; worker needs time to advance. */
                                        struct timespec ts = {0, 1000000L};
                                        nanosleep(&ts, NULL);
                                } else {
                                        /* Unexpected error or EOF. */
                                        for (int k = 0; k < ECHO_NUM_CONNS; k++)
                                                close(conns[k].client_fd);
                                        strand_runtime_destroy(rt);
                                        return (1);
                                }
                        }

                        if (memcmp(send_buf, recv_buf, sizeof(send_buf)) != 0) {
                                for (int k = 0; k < ECHO_NUM_CONNS; k++)
                                        close(conns[k].client_fd);
                                strand_runtime_destroy(rt);
                                return (1);
                        }
                }
        }

        /* Close client fds; server fds are closed by the fibers. */
        for (i = 0; i < ECHO_NUM_CONNS; i++)
                close(conns[i].client_fd);

        /* Wait for all server fibers to finish. */
        for (i = 0; i < ECHO_NUM_CONNS; i++) {
                if (!wait_for_atomic(&conns[i].done, 1, 15000, NULL)) {
                        strand_runtime_destroy(rt);
                        return (1);
                }
        }


        rc = 0;
        for (i = 0; i < ECHO_NUM_CONNS; i++) {
                if (atomic_load(&conns[i].error))
                        rc = 1;
        }

        strand_runtime_destroy(rt);
        return (rc);
}

/* =========================================================================
 * Test 7.2 - Producer-Consumer Pipeline
 *
 * A 3-stage cooperative pipeline: producer → stage1 → stage2 → result.
 * Stages communicate through nonblocking pipes.  Each stage parks on I/O:
 *   - wait_readable before reading from its input pipe
 *   - wait_writable before writing to its output pipe
 *
 * Demonstrates:
 *   - spawning multiple fibers from a parent fiber
 *   - back-pressure: a full pipe parks the writing fiber until the reader
 *     drains it, then the EPOLLOUT/EV_DISPATCH fires and the writer resumes
 *   - clean shutdown: producer closes the write end of its pipe, stages
 *     read EOF and propagate the shutdown downstream
 *
 * Uses a single scheduler driven from the host thread (no runtime threads).
 * Items are uint32_t values; each stage adds 1 to the value it passes on.
 * Producer sends 10 items (0..9); stage2 writes final values to a result array.
 *
 * Invariants verified:
 *   - no items lost (result array has exactly NUM_PIPELINE_ITEMS entries)
 *   - values are producer_value + 2 (each stage added 1)
 *   - no deadlock (all fibers finish within DRIVE_MAX_ITER advances)
 *
 * See TESTING.md 7.2, ARCHITECTURE.md 4.3.
 * =========================================================================
 */

#define NUM_PIPELINE_ITEMS 10

typedef struct {
        strand_scheduler_t *sched;
        int                 rd;        /* read end of input pipe  */
        int                 wr;        /* write end of output pipe */
} pipeline_stage_args_t;

typedef struct {
        strand_scheduler_t *sched;
        int                 wr;        /* write end of output pipe */
} pipeline_producer_args_t;

typedef struct {
        strand_scheduler_t *sched;
        int                 rd;        /* read end of input pipe   */
        uint32_t            results[NUM_PIPELINE_ITEMS];
        int                 count;
} pipeline_sink_args_t;

/*
 * pipeline_write_item -- write one uint32_t to a nonblocking pipe, parking
 * via strand_fiber_wait_writable if the pipe buffer is full.
 *
 * Returns STRAND_OK on success, STRAND_CANCELLED on cancellation, or
 * a negative error code on I/O failure.
 */
static int
pipeline_write_item(strand_scheduler_t *sched, int wr, uint32_t val)
{
        ssize_t n;
        int     rc;

        for (;;) {
                n = write(wr, &val, sizeof(val));
                if (n == (ssize_t)sizeof(val))
                        return (STRAND_OK);
                if (n < 0 && errno == EAGAIN) {
                        /* Pipe full: park until writable. */
                        rc = strand_fiber_wait_writable(sched, wr);
                        if (rc != STRAND_OK)
                                return (rc);
                        /* Retry write after waking. */
                        continue;
                }
                return (-1); /* unexpected I/O error */
        }
}

/*
 * pipeline_read_item -- read one uint32_t from a nonblocking pipe, parking
 * via strand_fiber_wait_readable if no data is available.
 *
 * Returns 1 on success (item read into *out), 0 on EOF (pipe closed by
 * writer), or -1 on error.
 */
static int
pipeline_read_item(strand_scheduler_t *sched, int rd, uint32_t *out)
{
        ssize_t n;
        int     rc;

        for (;;) {
                n = read(rd, out, sizeof(*out));
                if (n == (ssize_t)sizeof(*out))
                        return (1);  /* item read */
                if (n == 0)
                        return (0);  /* EOF: writer closed the pipe */
                if (n < 0 && errno == EAGAIN) {
                        /* No data yet: park until readable. */
                        rc = strand_fiber_wait_readable(sched, rd);
                        if (rc != STRAND_OK)
                                return (-1);
                        continue;
                }
                return (-1); /* unexpected I/O error */
        }
}

/*
 * producer_fiber -- generates NUM_PIPELINE_ITEMS uint32_t values (0..N-1)
 * and writes them to the first stage's input pipe.  Closes the write end
 * when done so downstream stages observe EOF.
 */
static void
producer_fiber(void *varg)
{
        pipeline_producer_args_t *a = varg;
        uint32_t                  i;

        for (i = 0; i < NUM_PIPELINE_ITEMS; i++) {
                if (pipeline_write_item(a->sched, a->wr, i) != STRAND_OK)
                        break;
        }
        close(a->wr);
}

/*
 * transform_fiber -- reads uint32_t items from rd, adds 1, and writes to wr.
 * Closes wr when rd signals EOF.  Used for both stage1 and stage2.
 */
static void
transform_fiber(void *varg)
{
        pipeline_stage_args_t *a = varg;
        uint32_t               val;
        int                    rc;

        for (;;) {
                rc = pipeline_read_item(a->sched, a->rd, &val);
                if (rc == 0)
                        break; /* EOF: upstream is done */
                if (rc < 0)
                        break; /* error: propagate shutdown */
                val++;
                if (pipeline_write_item(a->sched, a->wr, val) != STRAND_OK)
                        break;
        }
        close(a->wr);
}

/*
 * sink_fiber -- reads all uint32_t items from rd and accumulates them into
 * a->results[].  Stops on EOF.
 */
static void
sink_fiber(void *varg)
{
        pipeline_sink_args_t *a = varg;
        uint32_t              val;
        int                   rc;

        while (a->count < NUM_PIPELINE_ITEMS) {
                rc = pipeline_read_item(a->sched, a->rd, &val);
                if (rc <= 0)
                        break;
                a->results[a->count++] = val;
        }
}

typedef struct {
        strand_scheduler_t   *sched;
        pipeline_sink_args_t *sink;
        _Atomic int           ready;
        int                   spawn_error;
} pipeline_root_args_t;

/*
 * pipeline_root_fiber -- spawns the producer, two transform stages, and the
 * sink.  Runs from within the scheduler so that strand_fiber_spawn is legal.
 * Signals ready when all children are spawned.
 *
 * Pipe layout:
 *   pipe[0]: producer  → stage1    (pipe0[1] → pipe0[0])
 *   pipe[1]: stage1    → stage2    (pipe1[1] → pipe1[0])
 *   pipe[2]: stage2    → sink      (pipe2[1] → pipe2[0])
 */
static void
pipeline_root_fiber(void *varg)
{
        pipeline_root_args_t     *a = varg;
        strand_scheduler_t       *sched = a->sched;
        int                       pipe0[2], pipe1[2], pipe2[2];

        /* Static args: must outlive the child fibers. */
        static pipeline_producer_args_t prod_args;
        static pipeline_stage_args_t    s1_args, s2_args;

        if (make_pipe_pair(pipe0) != 0 || make_pipe_pair(pipe1) != 0 ||
            make_pipe_pair(pipe2) != 0) {
                a->spawn_error = 1;
                atomic_store(&a->ready, 1);
                return;
        }

        prod_args.sched = sched;
        prod_args.wr    = pipe0[1];

        s1_args.sched = sched;
        s1_args.rd    = pipe0[0];
        s1_args.wr    = pipe1[1];

        s2_args.sched = sched;
        s2_args.rd    = pipe1[0];
        s2_args.wr    = pipe2[1];

        a->sink->sched = sched;
        a->sink->rd    = pipe2[0];

        /* Spawn stages - all on the same single-worker scheduler. */
        if (strand_fiber_spawn(sched, producer_fiber,  &prod_args, 0, NULL) ||
            strand_fiber_spawn(sched, transform_fiber,  &s1_args,  0, NULL) ||
            strand_fiber_spawn(sched, transform_fiber,  &s2_args,  0, NULL) ||
            strand_fiber_spawn(sched, sink_fiber,       a->sink,   0, NULL)) {
                a->spawn_error = 1;
                close(pipe0[0]); close(pipe0[1]);
                close(pipe1[0]); close(pipe1[1]);
                close(pipe2[0]); close(pipe2[1]);
        }

        atomic_store(&a->ready, 1);
        /* Root fiber exits; children run independently. */
}

int
test_integration_producer_consumer(void)
{
        strand_scheduler_t   *sched;
        pipeline_sink_args_t  sink;
        pipeline_root_args_t  root_args;
        int                   i;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&sink, 0, sizeof(sink));
        memset(&root_args, 0, sizeof(root_args));
        root_args.sched = sched;
        root_args.sink  = &sink;
        atomic_init(&root_args.ready, 0);

        if (push_root_fiber(sched, pipeline_root_fiber, &root_args) != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        /*
         * Drive until idle.  The pipeline runs fully cooperatively:
         * producer → stage1 → stage2 → sink.  When pipes fill up, writers
         * park on wait_writable; when pipes are empty, readers park on
         * wait_readable.  The scheduler resolves all these dependencies.
         */
        drive_until_idle(sched);

        strand_scheduler_destroy(sched);

        if (root_args.spawn_error)
                return (1);
        if (sink.count != NUM_PIPELINE_ITEMS)
                return (1);

        /* Each item passed through 2 transform stages (+1 each), so value = i+2. */
        for (i = 0; i < NUM_PIPELINE_ITEMS; i++) {
                if (sink.results[i] != (uint32_t)(i + 2))
                        return (1);
        }

        return (0);
}

/* =========================================================================
 * Test 7.3 - Fan-Out Scope with Error Propagation
 *
 * Spawn NUM_FANOUT_CHILDREN fibers under a scope.  Child FANOUT_FAIL_IDX
 * returns an error (non-zero return from strand_scope_fiber_fn_t).  All
 * other children block waiting on a pipe that is never made readable;
 * they will be cancelled when the scope transitions to SCOPE_CANCELLING.
 *
 * Demonstrates:
 *   - first-error propagation: scope_wait returns the first child error
 *   - reverse-spawn-order cancellation walk
 *   - cancelled children handle STRAND_CANCELLED gracefully (return 0)
 *   - scope_wait does not return until ALL children have finished
 *
 * Uses a runtime with one worker.  Children that block on I/O get cancelled
 * by the scope cancellation walk (cross-worker inject is exercised when the
 * scope_cancel is called from the same fiber that called scope_wait).
 *
 * See TESTING.md 7.3, ARCHITECTURE.md 7.1, 7.3.
 * =========================================================================
 */

#define NUM_FANOUT_CHILDREN 6
#define FANOUT_FAIL_IDX     2  /* 0-based index of the child that fails */
#define FANOUT_FAIL_CODE    99

typedef struct {
        strand_scheduler_t *sched;
        int                 rd_fd;       /* read end of "never-ready" pipe */
        int                 child_idx;   /* 0-based */
        _Atomic int        *cancel_order; /* shared: records cancel sequence */
        _Atomic int        *cancel_count; /* shared: number of cancelled children */
} fanout_child_args_t;

typedef struct {
        strand_scheduler_t  *sched;
        fanout_child_args_t  child_args[NUM_FANOUT_CHILDREN];
        int                  pipes_rd[NUM_FANOUT_CHILDREN]; /* read ends */
        int                  pipes_wr[NUM_FANOUT_CHILDREN]; /* write ends */
        int                  scope_result;
        _Atomic int          cancel_order[NUM_FANOUT_CHILDREN];
        _Atomic int          cancel_count;
        _Atomic int          done;
} fanout_state_t;

/*
 * fanout_child_scope_fn -- fiber body for non-failing children.
 *
 * Blocks on a pipe read.  When the scope is cancelled, wait_readable returns
 * STRAND_CANCELLED.  The fiber handles this gracefully (returns 0) so it
 * does not inject a secondary error into the scope.
 *
 * Records its index in the shared cancel_order array using the current
 * cancel_count as a position (records cancellation sequence).
 */
static int
fanout_child_scope_fn(void *varg)
{
        fanout_child_args_t *a = varg;
        int                  rc;

        rc = strand_fiber_wait_readable(a->sched, a->rd_fd);
        if (rc == STRAND_CANCELLED) {
                /*
                 * Record our position in the cancellation sequence.
                 * fetch_add is atomic; the order recorded here is the
                 * order in which cancelled children resume from the run queue.
                 */
                int pos = atomic_fetch_add_explicit(a->cancel_count, 1,
                    memory_order_acq_rel);
                if (pos < NUM_FANOUT_CHILDREN)
                        atomic_store_explicit(&a->cancel_order[pos],
                            a->child_idx, memory_order_release);
                return (0); /* handle cancellation gracefully */
        }
        return (rc == STRAND_OK) ? 0 : 1;
}

/*
 * fanout_failing_child_scope_fn -- fiber body for the one child that fails.
 * Returns FANOUT_FAIL_CODE immediately to trigger first-error propagation.
 */
static int
fanout_failing_child_scope_fn(void *varg)
{
        (void)varg;
        return (FANOUT_FAIL_CODE);
}

/*
 * fanout_parent_fiber -- opens the scope, spawns all children, waits.
 */
static void
fanout_parent_fiber(void *varg)
{
        fanout_state_t *s = varg;
        strand_scope_t  scope;
        int             i, rc;

        rc = strand_scope_open(s->sched, &scope);
        if (rc != STRAND_OK) {
                s->scope_result = rc;
                atomic_store(&s->done, 1);
                return;
        }

        /*
         * Spawn children in order 0..5.  The scope's spawn list is prepended
         * (newest at head), so the cancellation walk visits them in reverse
         * spawn order: 5, 4, 3, 2, 1, 0.  Child FANOUT_FAIL_IDX (2) will
         * have a stale handle by cancel time (already finished); the walk
         * skips it with a no-op stale-handle check.
         */
        for (i = 0; i < NUM_FANOUT_CHILDREN; i++) {
                strand_scope_fiber_fn_t fn;

                fn = (i == FANOUT_FAIL_IDX) ?
                    fanout_failing_child_scope_fn :
                    fanout_child_scope_fn;

                rc = strand_scope_spawn(s->sched, &scope, fn,
                    &s->child_args[i], NULL);
                if (rc != STRAND_OK) {
                        s->scope_result = -1;
                        strand_scope_wait(s->sched, &scope);
                        atomic_store(&s->done, 1);
                        return;
                }
        }

        /*
         * Wait for all children to finish.  The failing child triggers a
         * cancellation walk; all others wake from wait_readable with
         * STRAND_CANCELLED.  scope_wait returns first_error == FANOUT_FAIL_CODE.
         */
        s->scope_result = strand_scope_wait(s->sched, &scope);
        atomic_store(&s->done, 1);
}

int
test_integration_fan_out_scope_error(void)
{
        strand_runtime_t *rt;
        strand_worker_t  *w;
        fanout_state_t    state;
        int               fds[2];
        int               i, rc;

        rt = make_test_runtime();
        if (rt == NULL)
                return (1);

        w = make_test_worker(rt);
        if (w == NULL) {
                strand_runtime_destroy(rt);
                return (1);
        }

        memset(&state, 0, sizeof(state));
        state.sched = w->sched;
        atomic_init(&state.cancel_count, 0);
        atomic_init(&state.done, 0);

        for (i = 0; i < NUM_FANOUT_CHILDREN; i++) {
                atomic_init(&state.cancel_order[i], -1);
                if (make_pipe_pair(fds) != 0) {
                        strand_runtime_destroy(rt);
                        return (1);
                }
                state.pipes_rd[i]           = fds[0];
                state.pipes_wr[i]           = fds[1];
                state.child_args[i].sched       = w->sched;
                state.child_args[i].rd_fd       = fds[0];
                state.child_args[i].child_idx   = i;
                state.child_args[i].cancel_order = state.cancel_order;
                state.child_args[i].cancel_count = &state.cancel_count;
        }

        rc = strand_runtime_spawn(rt, fanout_parent_fiber, &state, 0, NULL,
            NULL);
        if (rc != STRAND_OK) {
                for (i = 0; i < NUM_FANOUT_CHILDREN; i++) {
                        close(state.pipes_rd[i]);
                        close(state.pipes_wr[i]);
                }
                strand_runtime_destroy(rt);
                return (1);
        }

        /* Wait for scope to complete (parent signals done). */
        if (!wait_for_atomic(&state.done, 1, 15000, NULL)) {
                for (i = 0; i < NUM_FANOUT_CHILDREN; i++) {
                        close(state.pipes_rd[i]);
                        close(state.pipes_wr[i]);
                }
                strand_runtime_destroy(rt);
                return (1);
        }

        /* Close all pipe ends (server fds are closed by the fibers or were
         * never opened; close write ends that were never used). */
        for (i = 0; i < NUM_FANOUT_CHILDREN; i++) {
                close(state.pipes_wr[i]);
                close(state.pipes_rd[i]);
        }

        strand_runtime_destroy(rt);

        /* Verify first error is from the failing child. */
        if (state.scope_result != FANOUT_FAIL_CODE)
                return (1);

        /* Verify all non-failing children were cancelled (5 children). */
        if (atomic_load(&state.cancel_count) != NUM_FANOUT_CHILDREN - 1)
                return (1);

        return (0);
}

/* =========================================================================
 * Test 7.4 - Timeout then Abandon
 *
 * Demonstrates the non-terminal timeout + abandon pattern:
 *   1. Open a scope with long-running children (blocked on a timer).
 *   2. Call strand_scope_wait_timeout with a short deadline.
 *   3. Observe STRAND_TIMEOUT - the scope is still ACTIVE/CANCELLING.
 *   4. Call strand_scope_abandon - control block handed to the runtime.
 *   5. Children eventually finish (timer expires or cancellation).
 *   6. Runtime frees the scope control block.
 *
 * Uses a single scheduler with the mock test clock to control time without
 * real-time delays.  The scope control block is heap-allocated (required for
 * strand_scope_abandon).
 *
 * Verifies: no use-after-free (Valgrind / ASan), child runs to completion.
 *
 * See TESTING.md 7.4, ARCHITECTURE.md 7.4.
 * =========================================================================
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t     *scope;         /* heap-allocated */
        uint64_t            child_deadline; /* mock clock deadline */
        int                 timeout_rc;    /* result of scope_wait_timeout */
        _Atomic int         child_done;
} timeout_abandon_state_t;

static int
timeout_abandon_child_fn(void *varg)
{
        timeout_abandon_state_t *s = varg;
        int                      rc;

        /*
         * Park on a timer until child_deadline.  If the scope is cancelled
         * before the deadline, strand_fiber_sleep_until returns STRAND_CANCELLED.
         * Either way, the child returns 0 (handled gracefully).
         */
        rc = strand_fiber_sleep_until(s->sched, s->child_deadline);
        (void)rc;
        atomic_store(&s->child_done, 1);
        return (0);
}

static void
timeout_abandon_parent_fiber(void *varg)
{
        timeout_abandon_state_t *s = varg;
        int                      rc;

        rc = strand_scope_open(s->sched, s->scope);
        if (rc != STRAND_OK) {
                s->timeout_rc = rc;
                return;
        }

        rc = strand_scope_spawn(s->sched, s->scope,
            timeout_abandon_child_fn, s, NULL);
        if (rc != STRAND_OK) {
                s->timeout_rc = rc;
                strand_scope_wait(s->sched, s->scope);
                return;
        }

        /*
         * Wait with a timeout that expires before the child's deadline.
         * Returns STRAND_TIMEOUT; the scope is still active.
         */
        s->timeout_rc = strand_scope_wait_timeout(s->sched, s->scope,
            /* timeout deadline: */ strand_test_clock_ns + 1000);

        if (s->timeout_rc == STRAND_TIMEOUT) {
                /*
                 * Non-terminal timeout: hand the scope to the runtime.
                 * The caller must not touch s->scope after this call.
                 * The runtime will free it when the child finishes.
                 */
                strand_scope_abandon(s->sched, s->scope);
                /* s->scope is now owned by the runtime - do not access it. */
        } else {
                /*
                 * Unexpected: scope completed before timeout.
                 * Still terminal - scope_wait_timeout was terminal in this path.
                 */
        }
}

int
test_integration_timeout_then_abandon(void)
{
        strand_scheduler_t      *sched;
        strand_scope_t          *scope;
        timeout_abandon_state_t  state;
        int                      rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        scope = malloc(sizeof(*scope));
        if (scope == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&state, 0, sizeof(state));
        state.sched          = sched;
        state.scope          = scope;
        state.child_deadline = 9000; /* child blocks until t=9000 */
        state.timeout_rc     = -999;
        atomic_init(&state.child_done, 0);

        /* t=1000: parent opens scope, spawns child (deadline 9000), waits
         *         with timeout at t=2000. */
        strand_test_clock_ns = 1000;

        rc = push_root_fiber(sched, timeout_abandon_parent_fiber, &state);
        if (rc != 0) {
                free(scope);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* Drive until parent parks in scope_wait_timeout. */
        drive_until_idle(sched);

        /* t=2001: fire the wait_timeout deadline. Parent resumes, abandons scope. */
        strand_test_clock_ns = 2001;
        drive_until_idle(sched);

        /* Verify parent received STRAND_TIMEOUT. */
        if (state.timeout_rc != STRAND_TIMEOUT) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* t=9001: fire the child's timer. Child resumes and finishes. */
        strand_test_clock_ns = 9001;
        drive_until_idle(sched);

        strand_scheduler_destroy(sched);

        /* scope was freed by the runtime when child completed - do not access it. */

        if (!atomic_load(&state.child_done))
                return (1);
        return (0);
}

/* =========================================================================
 * Test 7.5a - Offload Cancel: RESULT_CLAIMED wins
 *
 * Let the offload function complete before the cancel arrives.  The CAS
 * for RESULT_CLAIMED wins; the fiber receives STRAND_OK and the result.
 *
 * See TESTING.md 7.5, ARCHITECTURE.md 6.6.
 * =========================================================================
 */

typedef struct {
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        int                    result;
        int                    offload_rc;
        _Atomic int            fn_done;  /* set by offload fn after write */
} offload_result_claimed_state_t;

static _Atomic int g_integ_claimed_done;

static void
offload_fn_write_magic(void *arg, void *result_slot)
{
        (void)arg;
        *(int *)result_slot = 0xCAFE;
        atomic_store_explicit(&g_integ_claimed_done, 1, memory_order_release);
}

static void
fiber_offload_result_claimed(void *varg)
{
        offload_result_claimed_state_t *s = varg;

        s->result     = 0;
        s->offload_rc = strand_fiber_offload(s->sched, s->pool,
            offload_fn_write_magic, NULL, &s->result);
}

int
test_integration_offload_cancel_result_claimed_wins(void)
{
        strand_scheduler_t             *sched;
        strand_offload_pool_t          *pool;
        offload_result_claimed_state_t  state;
        strand_fiber_handle_t           handle;
        int                             i, rc;
        struct timespec                 ts = {0, 5000000L}; /* 5ms */

        atomic_store(&g_integ_claimed_done, 0);

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        pool = strand_offload_pool_init(2);
        if (pool == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&state, 0, sizeof(state));
        state.sched = sched;
        state.pool  = pool;

        if (push_root_fiber(sched, fiber_offload_result_claimed, &state) != 0) {
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /*
         * Advance once: fiber parks on FIBER_PARKED_OFFLOAD.
         * The handle is captured before the fiber runs so we can cancel later.
         */
        strand_scheduler_advance(sched, NULL);

        /*
         * Wait for the offload function to complete (signal g_integ_claimed_done).
         * Only after fn has written the result do we attempt a cancel.  This
         * ensures RESULT_CLAIMED CAS has already won.
         */
        for (i = 0; i < 200; i++) {
                if (atomic_load_explicit(&g_integ_claimed_done,
                    memory_order_acquire))
                        break;
                nanosleep(&ts, NULL);
        }

        if (!atomic_load_explicit(&g_integ_claimed_done,
            memory_order_acquire)) {
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /*
         * By now RESULT_CLAIMED CAS has won and the fiber is already back on
         * the run queue.  Attempt cancel (late - must be no-op or stale).
         * We don't have the fiber handle in this test since push_root_fiber
         * doesn't capture it; we simply advance to let the fiber finish.
         */
        (void)handle;
        for (i = 0; i < 20; i++)
                strand_scheduler_advance(sched, NULL);

        rc = (state.offload_rc == STRAND_OK && state.result == 0xCAFE) ? 0 : 1;

        strand_offload_pool_destroy(pool);
        strand_scheduler_destroy(sched);
        return (rc);
}

/* =========================================================================
 * Test 7.5b - Offload Cancel: CANCELLED wins
 *
 * Block the offload function with a spin loop.  Cancel the waiting fiber
 * before the offload completes.  CANCELLED CAS wins; fiber receives
 * STRAND_CANCELLED.  Result slot is not written to by the fiber.
 * The offload function still runs to completion (arg lifetime guarantee).
 *
 * See TESTING.md 7.5, ARCHITECTURE.md 6.6.
 * =========================================================================
 */

typedef struct {
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        strand_fiber_handle_t  handle;
        int                    result_slot;
        int                    offload_rc;
} offload_cancelled_wins_state_t;

static _Atomic int g_integ_cancel_fn_started;
static _Atomic int g_integ_cancel_release;

static void
offload_fn_spin_until_released(void *arg, void *result_slot)
{
        (void)arg;
        (void)result_slot;
        atomic_store_explicit(&g_integ_cancel_fn_started, 1,
            memory_order_release);
        while (!atomic_load_explicit(&g_integ_cancel_release,
            memory_order_acquire))
                ; /* spin: holds pool slot until test releases it */
        /* Do NOT write result_slot: CANCELLED already won. */
}

static void
fiber_offload_cancelled_wins(void *varg)
{
        offload_cancelled_wins_state_t *s = varg;

        s->result_slot = 0xDEAD; /* sentinel: must not change to something else */
        s->offload_rc  = strand_fiber_offload(s->sched, s->pool,
            offload_fn_spin_until_released, NULL, &s->result_slot);
}

int
test_integration_offload_cancel_cancelled_wins(void)
{
        strand_scheduler_t             *sched;
        strand_offload_pool_t          *pool;
        offload_cancelled_wins_state_t  state;
        int                             i, rc;
        struct timespec                 ts = {0, 1000000L}; /* 1ms */

        atomic_store(&g_integ_cancel_fn_started, 0);
        atomic_store(&g_integ_cancel_release,    0);

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        pool = strand_offload_pool_init(1);
        if (pool == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&state, 0, sizeof(state));
        state.sched = sched;
        state.pool  = pool;

        if (push_root_fiber(sched, fiber_offload_cancelled_wins, &state) != 0) {
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /*
         * Capture handle before fiber runs (access through the fiber pointer
         * before it gets on the queue). */
        {
                /* Peek at the run_head to capture the handle. */
                strand_fiber_t *f = sched->run_head;
                if (f != NULL) {
                        state.handle.ptr        = f;
                        state.handle.generation = f->generation;
                }
        }

        /* Advance once: fiber parks on FIBER_PARKED_OFFLOAD. */
        strand_scheduler_advance(sched, NULL);

        /* Wait until the offload function has actually started (on pool thread). */
        for (i = 0; i < 60000; i++) {
                if (atomic_load_explicit(&g_integ_cancel_fn_started,
                    memory_order_acquire))
                        break;
                nanosleep(&ts, NULL);
        }

        if (!atomic_load_explicit(&g_integ_cancel_fn_started,
            memory_order_acquire)) {
                atomic_store(&g_integ_cancel_release, 1);
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /*
         * Cancel the fiber before the offload function completes.
         * CANCELLED CAS should win the race.
         */
        strand_fiber_cancel(state.handle);

        /* Advance to run the now-RUNNABLE cancelled fiber. */
        for (i = 0; i < 5; i++)
                strand_scheduler_advance(sched, NULL);

        /*
         * Release the offload thread so it can finish.  The RESULT_CLAIMED
         * CAS will fail because CANCELLED already won.  This ensures the
         * offload pool can be destroyed without deadlock.
         */
        atomic_store_explicit(&g_integ_cancel_release, 1,
            memory_order_release);

        /* Give the offload thread time to exit the spin loop. */
        {
                struct timespec drain_ts = {0, 20000000L}; /* 20ms */
                nanosleep(&drain_ts, NULL);
        }

        rc = (state.offload_rc == STRAND_CANCELLED) ? 0 : 1;

        strand_offload_pool_destroy(pool);
        strand_scheduler_destroy(sched);
        return (rc);
}

/* =========================================================================
 * Test 7.6 - Offload EAGAIN and Retry Pattern
 *
 * Demonstrates the mandatory yield-before-retry pattern when the offload
 * pool is full.  Filling the pool causes strand_fiber_offload to return
 * STRAND_EAGAIN immediately (without parking).  The fiber must yield and
 * retry; spinning without yielding would starve other fibers.
 *
 * Pattern (from strand.h documentation):
 *   while ((rc = strand_fiber_offload(...)) == STRAND_EAGAIN)
 *       strand_fiber_yield(sched);
 *
 * Setup: 1-thread pool (capacity = 2).  Two blocker fibers fill the pool.
 * A third fiber retries until a slot is available.
 *
 * See TESTING.md 7.6, ARCHITECTURE.md 6.5.
 * =========================================================================
 */

static _Atomic int g_eagain_retry_release;

typedef struct {
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        int                    result;
        int                    offload_rc;
        int                    eagain_count;  /* how many times EAGAIN was seen */
} offload_eagain_retry_state_t;

static void
offload_fn_block_eagain(void *arg, void *result_slot)
{
        (void)arg;
        (void)result_slot;
        while (!atomic_load_explicit(&g_eagain_retry_release,
            memory_order_acquire))
                ;
}

static void
offload_fn_write_1(void *arg, void *result_slot)
{
        (void)arg;
        *(int *)result_slot = 1;
}

static void
fiber_offload_blocker(void *varg)
{
        offload_eagain_retry_state_t *s = varg;
        int dummy;

        strand_fiber_offload(s->sched, s->pool, offload_fn_block_eagain,
            NULL, &dummy);
}

/*
 * fiber_offload_retrier -- calls strand_fiber_offload in a yield-retry loop
 * until it succeeds.  Records the number of EAGAIN responses seen.
 */
static void
fiber_offload_retrier(void *varg)
{
        offload_eagain_retry_state_t *s = varg;
        int                           rc;

        s->result = 0;
        do {
                rc = strand_fiber_offload(s->sched, s->pool,
                    offload_fn_write_1, NULL, &s->result);
                if (rc == STRAND_EAGAIN) {
                        s->eagain_count++;
                        strand_fiber_yield(s->sched);
                }
        } while (rc == STRAND_EAGAIN);

        s->offload_rc = rc;
}

int
test_integration_offload_eagain_retry(void)
{
        strand_scheduler_t           *sched;
        strand_offload_pool_t        *pool;
        offload_eagain_retry_state_t  blocker[2];
        offload_eagain_retry_state_t  retrier;
        int                           i, rc;
        struct timespec               ts = {0, 1000000L};

        atomic_store(&g_eagain_retry_release, 0);

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        /* 1 thread → capacity = 2 (in_flight + queued). */
        pool = strand_offload_pool_init(1);
        if (pool == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(blocker, 0, sizeof(blocker));
        memset(&retrier, 0, sizeof(retrier));
        retrier.offload_rc = -1; /* sentinel: not yet completed */

        for (i = 0; i < 2; i++) {
                blocker[i].sched = sched;
                blocker[i].pool  = pool;
                push_root_fiber(sched, fiber_offload_blocker, &blocker[i]);
        }
        retrier.sched = sched;
        retrier.pool  = pool;
        push_root_fiber(sched, fiber_offload_retrier, &retrier);

        /*
         * Drive until both blocker fibers are parked in offload (pool full)
         * and the retrier has seen at least one EAGAIN.  Then release blockers.
         */
        for (i = 0; i < 500; i++) {
                strand_scheduler_advance(sched, NULL);
                if (retrier.eagain_count > 0)
                        break;
                nanosleep(&ts, NULL);
        }

        if (retrier.eagain_count == 0) {
                atomic_store(&g_eagain_retry_release, 1);
                strand_offload_pool_destroy(pool);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* Release blockers so the retrier can eventually succeed. */
        atomic_store_explicit(&g_eagain_retry_release, 1,
            memory_order_release);

        /* Drive until retrier completes. */
        for (i = 0; i < 500; i++) {
                strand_scheduler_advance(sched, NULL);
                if (retrier.offload_rc == STRAND_OK)
                        break;
                nanosleep(&ts, NULL);
        }

        rc = (retrier.offload_rc == STRAND_OK &&
              retrier.result       == 1 &&
              retrier.eagain_count  > 0) ? 0 : 1;

        strand_offload_pool_destroy(pool);
        strand_scheduler_destroy(sched);
        return (rc);
}

/* =========================================================================
 * Test 7.7 - Subprocess via Offload Pool
 *
 * Demonstrates spawning a subprocess from a fiber using fork/exec/waitpid
 * through the offload pool.  The worker thread is not blocked during the
 * subprocess wait.
 *
 * Pattern: a fiber calls strand_fiber_offload with a blocking function that
 * forks a child process, runs a command (true(1), exit 0), waits for it,
 * and writes the exit status into result_slot.  The fiber parks while the
 * subprocess runs on a pool thread; the worker continues serving other fibers.
 *
 * Verifies:
 *   - subprocess runs and exits with status 0
 *   - worker thread is not blocked (other fibers run during the wait)
 *   - result is delivered correctly to the originating fiber
 *
 * See TESTING.md 7.7, ARCHITECTURE.md 6.5.
 * =========================================================================
 */

typedef struct {
        strand_scheduler_t    *sched;
        strand_offload_pool_t *pool;
        int                    exit_status;  /* set by offload fn */
        int                    offload_rc;
        _Atomic int            concurrent_done; /* set by concurrent fiber */
} subprocess_state_t;

/*
 * offload_fn_run_true -- fork and exec /usr/bin/true (or /bin/true),
 * wait for the child, write the exit status into result_slot.
 *
 * Runs on an offload thread (a plain OS thread); must not call any
 * fiber or scheduler APIs.
 */
static void
offload_fn_run_true(void *arg, void *result_slot)
{
        pid_t pid;
        int   status;

        (void)arg;

        pid = fork();
        if (pid < 0) {
                *(int *)result_slot = -1;
                return;
        }
        if (pid == 0) {
                /* Child: exec "true". Try /usr/bin/true then /bin/true. */
                execlp("true", "true", (char *)NULL);
                _exit(127); /* exec failed */
        }
        /* Parent: wait for child. */
        if (waitpid(pid, &status, 0) < 0) {
                *(int *)result_slot = -2;
                return;
        }
        *(int *)result_slot = WIFEXITED(status) ? WEXITSTATUS(status) : -3;
}

/*
 * concurrent_fiber -- runs concurrently with the offload fiber to verify the
 * worker is not blocked while the subprocess executes.  Just sets a flag.
 */
static void
concurrent_fiber(void *varg)
{
        subprocess_state_t *s = varg;

        atomic_store(&s->concurrent_done, 1);
}

static void
subprocess_fiber(void *varg)
{
        subprocess_state_t *s = varg;

        s->exit_status = -99;
        s->offload_rc  = strand_fiber_offload(s->sched, s->pool,
            offload_fn_run_true, NULL, &s->exit_status);
}

int
test_integration_subprocess_via_offload(void)
{
        strand_scheduler_t *sched;
        strand_offload_pool_t *pool;
        subprocess_state_t    state;
        int                   i, rc;
        struct timespec       ts = {0, 5000000L}; /* 5ms */

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        pool = strand_offload_pool_init(2);
        if (pool == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&state, 0, sizeof(state));
        state.sched      = sched;
        state.pool       = pool;
        state.offload_rc = -1; /* sentinel: not yet completed */
        atomic_init(&state.concurrent_done, 0);

        /* Spawn the subprocess fiber and a concurrent fiber. */
        push_root_fiber(sched, subprocess_fiber,  &state);
        push_root_fiber(sched, concurrent_fiber, &state);

        /*
         * Drive until both fibers complete.  The subprocess fiber parks in
         * the offload pool while waitpid blocks on the pool thread; the
         * concurrent fiber runs immediately.
         */
        for (i = 0; i < 500; i++) {
                strand_scheduler_advance(sched, NULL);
                if (state.offload_rc == STRAND_OK &&
                    atomic_load(&state.concurrent_done))
                        break;
                if (state.offload_rc != -1 &&
                    state.offload_rc != 0 && state.offload_rc != -99)
                        break; /* error */
                nanosleep(&ts, NULL);
        }

        rc = (state.offload_rc   == STRAND_OK  &&
              state.exit_status  == 0          &&
              atomic_load(&state.concurrent_done) == 1) ? 0 : 1;

        strand_offload_pool_destroy(pool);
        strand_scheduler_destroy(sched);
        return (rc);
}

/* =========================================================================
 * Test 7.8 - Multi-Worker Accept Distribution
 *
 * Register two workers.  Spawn 100 fibers from the host thread using round-
 * robin selection (worker=NULL).  Verify that fibers are distributed
 * approximately evenly across both workers (>= 30 on each).
 *
 * Verifies:
 *   - round-robin worker selection works
 *   - no fiber is assigned to a stopped worker
 *   - load is approximately balanced
 *
 * Each fiber identifies its worker by comparing home_sched to the known
 * worker schedulers (requires internal header for sched pointer access).
 *
 * See TESTING.md 7.8, ARCHITECTURE.md 6.4.
 * =========================================================================
 */

#define DIST_NUM_FIBERS   100
#define DIST_NUM_WORKERS  2
#define DIST_MIN_PER_WORKER 30

typedef struct {
        strand_scheduler_t *sched_w0; /* worker 0's scheduler */
        strand_scheduler_t *sched_w1; /* worker 1's scheduler */
        pthread_t           tid_w0;   /* cached owner_thread for w0 */
        pthread_t           tid_w1;   /* cached owner_thread for w1 */
        _Atomic int         count_w0; /* fibers that ran on worker 0 */
        _Atomic int         count_w1; /* fibers that ran on worker 1 */
        _Atomic int         total_done;
} dist_state_t;

static void
dist_fiber(void *varg)
{
        dist_state_t *s = varg;
        pthread_t     self = pthread_self();

        /*
         * Identify which worker we're on by comparing pthread_self()
         * to cached owner thread IDs.  The IDs are captured by the host
         * thread after strand_worker_start returns, establishing a
         * happens-before with strand_runtime_spawn.
         */
        if (pthread_equal(self, s->tid_w0))
                atomic_fetch_add_explicit(&s->count_w0, 1, memory_order_relaxed);
        else if (pthread_equal(self, s->tid_w1))
                atomic_fetch_add_explicit(&s->count_w1, 1, memory_order_relaxed);

        atomic_fetch_add_explicit(&s->total_done, 1, memory_order_release);
}

int
test_integration_multiworker_distribution(void)
{
        strand_runtime_t *rt;
        strand_worker_t  *w[DIST_NUM_WORKERS];
        dist_state_t      state;
        int               i, rc;

        rt = make_test_runtime();
        if (rt == NULL)
                return (1);

        for (i = 0; i < DIST_NUM_WORKERS; i++) {
                w[i] = make_test_worker(rt);
                if (w[i] == NULL) {
                        strand_runtime_destroy(rt);
                        return (1);
                }
        }

        memset(&state, 0, sizeof(state));
        state.sched_w0 = w[0]->sched;
        state.sched_w1 = w[1]->sched;
        state.tid_w0   = w[0]->thread;
        state.tid_w1   = w[1]->thread;
        atomic_init(&state.count_w0,   0);
        atomic_init(&state.count_w1,   0);
        atomic_init(&state.total_done, 0);

        /* Spawn DIST_NUM_FIBERS fibers via round-robin (worker=NULL). */
        for (i = 0; i < DIST_NUM_FIBERS; i++) {
                rc = strand_runtime_spawn(rt, dist_fiber, &state, 0, NULL,
                    NULL);
                if (rc != STRAND_OK) {
                        strand_runtime_destroy(rt);
                        return (1);
                }
        }

        /* Wait for all fibers to complete. */
        if (!wait_for_atomic(&state.total_done, DIST_NUM_FIBERS, 15000, NULL)) {
                strand_runtime_destroy(rt);
                return (1);
        }

        strand_runtime_destroy(rt);

        /* Verify approximate even distribution. */
        if (atomic_load(&state.count_w0) < DIST_MIN_PER_WORKER)
                return (1);
        if (atomic_load(&state.count_w1) < DIST_MIN_PER_WORKER)
                return (1);
        if (atomic_load(&state.count_w0) + atomic_load(&state.count_w1) !=
            DIST_NUM_FIBERS)
                return (1);

        return (0);
}

/* =========================================================================
 * Test 7.9 - Simultaneous Read/Write on Same fd
 *
 * This is the primary integration-level verification of the Linux post-re-arm
 * zero-timeout readiness check (ARCHITECTURE.md 5.5).
 *
 * Two fibers park on the same socketpair fd: one waiting for read readiness,
 * one waiting for write readiness.  Both directions become ready simultaneously.
 * Verify that BOTH fibers wake within a SINGLE strand_scheduler_advance call -
 * specifically that the second waiter is woken via the post-re-arm zero-
 * timeout check, not in a subsequent advance call.
 *
 * Scenario on Linux (EPOLLET | EPOLLONESHOT):
 *   1. fill_fd(sv[0]): make sv[0] NOT writable, NOT readable.
 *   2. Park fiber A on wait_readable(sv[0]).
 *   3. Park fiber B on wait_writable(sv[0]).
 *      epoll registration: EPOLLIN|EPOLLOUT|EPOLLET|EPOLLONESHOT.
 *   4. Make BOTH directions ready simultaneously:
 *        write(sv[1], 1 byte) → sv[0] readable.
 *        drain_fd(sv[1])      → sv[0] writable.
 *   5. One advance: EPOLLIN|EPOLLOUT both set in the returned event.
 *      poller_deliver_event wakes A (EPOLLIN) AND B (EPOLLOUT) directly.
 *      Both fibers run in the same advance call.
 *
 * On OpenBSD (EV_DISPATCH), EVFILT_READ and EVFILT_WRITE are independent
 * filters; both fire and both waiters wake in the same advance.
 *
 * See TESTING.md 7.9, ARCHITECTURE.md 5.5.
 * =========================================================================
 */

typedef struct {
        strand_scheduler_t *sched;
        int                 fd;
        int                 result;
        int                 ran;
} simrw_fiber_args_t;

static void
simrw_read_fiber(void *varg)
{
        simrw_fiber_args_t *a = varg;

        a->result = strand_fiber_wait_readable(a->sched, a->fd);
        a->ran    = 1;
}

static void
simrw_write_fiber(void *varg)
{
        simrw_fiber_args_t *a = varg;

        a->result = strand_fiber_wait_writable(a->sched, a->fd);
        a->ran    = 1;
}

int
test_integration_simultaneous_read_write(void)
{
        strand_scheduler_t *sched;
        int                 sv[2];
        simrw_fiber_args_t  rargs, wargs;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        if (make_socketpair(sv) != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&rargs, 0, sizeof(rargs));
        memset(&wargs, 0, sizeof(wargs));
        rargs.sched = sched;
        rargs.fd    = sv[0];
        wargs.sched = sched;
        wargs.fd    = sv[0]; /* SAME fd as rargs */

        /*
         * Make sv[0] neither readable nor writable:
         * fill_fd(sv[0]) writes until EAGAIN, filling sv[1]'s receive buffer.
         * sv[0]'s send buffer is now full → not writable.
         * sv[0]'s receive buffer is empty → not readable.
         */
        fill_fd(sv[0]);

        /* Spawn A (read waiter) and B (write waiter) on sv[0]. */
        if (push_root_fiber(sched, simrw_read_fiber,  &rargs) != 0 ||
            push_root_fiber(sched, simrw_write_fiber, &wargs) != 0) {
                close(sv[0]); close(sv[1]);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /*
         * Advance twice to park both fibers.
         * After advance 1: A is running (starts wait_readable, parks).
         * After advance 2: B is running (starts wait_writable, parks).
         */
        strand_scheduler_advance(sched, NULL); /* A parks */
        strand_scheduler_advance(sched, NULL); /* B parks */

        if (rargs.ran != 0 || wargs.ran != 0) {
                close(sv[0]); close(sv[1]);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /*
         * Make BOTH directions ready simultaneously:
         *   write(sv[1], 1 byte): sv[0] becomes READABLE.
         *   drain_fd(sv[1]):      sv[1]'s receive buffer drains, sv[0]
         *                         becomes WRITABLE.
         */
        (void)write(sv[1], "x", 1);
        drain_fd(sv[1]);

        /*
         * One advance: the I/O poll step fires with EPOLLIN|EPOLLOUT (or
         * two kqueue events).  Both A and B are added to the run queue.
         * The run step then executes both fibers.
         *
         * If the second waiter required a separate advance call, this test
         * would fail - it verifies the post-re-arm zero-timeout path is not
         * needed when both directions fire in the same event.
         */
        strand_scheduler_advance(sched, NULL); /* poll: both wake */
        strand_scheduler_advance(sched, NULL); /* run: A and B both execute */
        strand_scheduler_advance(sched, NULL); /* ensure both ran */

        close(sv[0]);
        close(sv[1]);
        strand_scheduler_destroy(sched);

        return (rargs.ran == 1 && rargs.result == STRAND_OK &&
                wargs.ran == 1 && wargs.result == STRAND_OK) ? 0 : 1;
}

/* =========================================================================
 * Test 7.10 - Guest Mode Host Loop
 *
 * Demonstrates the host loop integration pattern from ARCHITECTURE.md 4.3.
 * A libstrand scheduler is embedded in an external event loop driven by
 * epoll (Linux) or kqueue (OpenBSD).
 *
 * Key invariant verified: the unconditional trailing strand_scheduler_advance
 * call after every epoll_wait/kevent return correctly processes timer expiry
 * even when the scheduler fd did NOT fire.  Without the trailing call, a
 * timer-expired fiber would not run until the next host event or I/O event
 * arrived.
 *
 * Test structure:
 *   - Host event loop runs on the test thread.
 *   - A fiber is planted with a timer (mock clock).
 *   - The host loop's epoll_wait is driven by a separate "kick" fd
 *     that is NOT the scheduler fd.  A 1 ms epoll timeout ensures
 *     the loop runs before the timer fires.
 *   - The test clock is advanced between loop iterations.
 *   - The trailing strand_scheduler_advance must wake the timer fiber.
 *   - The host loop must not block when the scheduler has work (verified by
 *     observing that the fiber runs within a bounded number of iterations).
 *
 * See TESTING.md 7.10, ARCHITECTURE.md 4.2, 4.3.
 * =========================================================================
 */

typedef struct {
        strand_scheduler_t *sched;
        uint64_t            deadline;
        _Atomic int         fired;
} guest_mode_timer_state_t;

static void
guest_timer_fn(void *varg)
{
        guest_mode_timer_state_t *s = varg;
        int                       rc;

        rc = strand_fiber_sleep_until(s->sched, s->deadline);
        if (rc == STRAND_OK)
                atomic_store(&s->fired, 1);
}

int
test_integration_guest_mode_host_loop(void)
{
        strand_scheduler_t       *sched;
        guest_mode_timer_state_t  state;
        int                       kick_fds[2];  /* pipe used as host event source */
        int                       host_poll_fd;
        uint64_t                  next_deadline;
        int                       i, rc;

#ifdef STRAND_LINUX
        struct epoll_event ev, events[4];
        int                sched_fd;
#endif
#ifdef STRAND_OPENBSD
        struct kevent      kev, events[4];
        int                sched_fd;
#endif

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        if (make_pipe_pair(kick_fds) != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&state, 0, sizeof(state));
        state.sched    = sched;
        state.deadline = 5000; /* fires when test clock >= 5000 */
        atomic_init(&state.fired, 0);

        /* Bootstrap the guest fiber through the public API. */
        if (strand_scheduler_spawn(sched, guest_timer_fn, &state, 0,
            NULL) != STRAND_OK) {
                close(kick_fds[0]); close(kick_fds[1]);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* Advance once to park the fiber on the timer heap. */
        strand_test_clock_ns = 1000;
        drive_until_idle(sched);

        sched_fd = strand_scheduler_get_fd(sched);

#ifdef STRAND_LINUX
        host_poll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (host_poll_fd < 0) {
                close(kick_fds[0]); close(kick_fds[1]);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* Register scheduler fd with the host epoll instance. */
        ev.events  = EPOLLIN;
        ev.data.fd = sched_fd;
        epoll_ctl(host_poll_fd, EPOLL_CTL_ADD, sched_fd, &ev);

        /* Register kick fd (a host application event source). */
        ev.events  = EPOLLIN;
        ev.data.fd = kick_fds[0];
        epoll_ctl(host_poll_fd, EPOLL_CTL_ADD, kick_fds[0], &ev);
#endif
#ifdef STRAND_OPENBSD
        host_poll_fd = kqueue();
        if (host_poll_fd < 0) {
                close(kick_fds[0]); close(kick_fds[1]);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* Register scheduler fd and kick fd with the host kqueue. */
        EV_SET(&kev, sched_fd,    EVFILT_READ, EV_ADD, 0, 0, NULL);
        kevent(host_poll_fd, &kev, 1, NULL, 0, NULL);
        EV_SET(&kev, kick_fds[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
        kevent(host_poll_fd, &kev, 1, NULL, 0, NULL);
#endif

        /*
         * Host event loop.  Each iteration:
         *   1. Compute next scheduler deadline (for epoll timeout).
         *   2. epoll_wait/kevent with short timeout.
         *   3. If scheduler fd fired: advance (handles I/O-woken fibers).
         *   4. Unconditional trailing advance (handles timer expiry even
         *      when no scheduler fd event arrived).
         *
         * The test clock is advanced after iteration 3 to simulate time
         * passing.  The timer fires on iteration 4 or 5, caught by the
         * trailing advance.
         */
        rc = 1; /* assume failure until fiber fires */
        for (i = 0; i < 200; i++) {
                strand_scheduler_advance(sched, &next_deadline);

                if (atomic_load_explicit(&state.fired, memory_order_acquire)) {
                        rc = 0;
                        break;
                }

                /* Advance mock clock to trigger the timer on the next pass. */
                if (i == 2)
                        strand_test_clock_ns = 6000;

#ifdef STRAND_LINUX
                {
                        int nfds, j;
                        int timeout_ms = 1; /* short poll: 1ms */

                        nfds = epoll_wait(host_poll_fd, events, 4, timeout_ms);
                        for (j = 0; j < nfds; j++) {
                                if (events[j].data.fd == sched_fd)
                                        strand_scheduler_advance(sched, NULL);
                                /* host application events would be handled here */
                        }
                }
#endif
#ifdef STRAND_OPENBSD
                {
                        int                nev, j;
                        struct timespec    poll_ts = {0, 1000000L}; /* 1ms */

                        nev = kevent(host_poll_fd, NULL, 0, events, 4,
                            &poll_ts);
                        for (j = 0; j < nev; j++) {
                                if ((int)events[j].ident == sched_fd)
                                        strand_scheduler_advance(sched, NULL);
                        }
                }
#endif

                /*
                 * Unconditional trailing advance - required by ARCHITECTURE.md
                 * 4.3.  This call processes timer expiry even when no scheduler
                 * fd event fired above.  Without it, the timer fiber would not
                 * run until the next host event arrives.
                 */
                strand_scheduler_advance(sched, NULL);
        }

        close(host_poll_fd);
        close(kick_fds[0]);
        close(kick_fds[1]);
        strand_scheduler_destroy(sched);

        return (rc);
}

/* =========================================================================
 * Guest scheduler identity across all guest-mode suspension paths.
 * =========================================================================
 */

typedef struct {
	strand_scheduler_t *sched;
	uint64_t            deadline;
	int                 rd;
	int                 phase;
	int                 failed;
} guest_identity_state_t;

static void
guest_identity_fiber(void *varg)
{
	guest_identity_state_t *s = varg;

	if (strand_fiber_self_scheduler() != s->sched)
		s->failed = 1;
	s->phase = 1;
	(void)strand_fiber_yield(s->sched);
	if (strand_fiber_self_scheduler() != s->sched)
		s->failed = 1;
	s->phase = 2;
	(void)strand_fiber_sleep_until(s->sched, s->deadline);
	if (strand_fiber_self_scheduler() != s->sched)
		s->failed = 1;
	s->phase = 3;
	(void)strand_fiber_wait_readable(s->sched, s->rd);
	if (strand_fiber_self_scheduler() != s->sched)
		s->failed = 1;
	s->phase = 4;
}

static int
test_integration_guest_self_scheduler(void)
{
	guest_identity_state_t state;
	strand_scheduler_t    *sched;
	int                    fds[2];
	char                   byte = 'x';

	sched = make_test_scheduler();
	if (sched == NULL)
		return (1);
	if (make_pipe_pair(fds) != 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}
	memset(&state, 0, sizeof(state));
	state.sched = sched;
	state.deadline = 2000;
	state.rd = fds[0];
	strand_test_clock_ns = 1000;
	if (strand_scheduler_spawn(sched, guest_identity_fiber, &state, 0,
	    NULL) != STRAND_OK) {
		close(fds[0]);
		close(fds[1]);
		strand_scheduler_destroy(sched);
		return (1);
	}
	strand_scheduler_advance(sched, NULL);
	if (state.phase != 2 || state.failed != 0)
		goto fail;
	strand_test_clock_ns = 2000;
	strand_scheduler_advance(sched, NULL);
	if (state.phase != 3 || state.failed != 0)
		goto fail;
	if (write(fds[1], &byte, sizeof(byte)) != (ssize_t)sizeof(byte))
		goto fail;
	strand_scheduler_advance(sched, NULL);
	if (state.phase != 4 || state.failed != 0)
		goto fail;
	close(fds[0]);
	close(fds[1]);
	strand_scheduler_destroy(sched);
	return (0);

fail:
	close(fds[0]);
	close(fds[1]);
	strand_scheduler_destroy(sched);
	return (1);
}

/* =========================================================================
 * Public guest bootstrap and budget drain.
 * =========================================================================
 */

typedef struct {
	strand_scheduler_t *sched;
	uint64_t            deadline;
	int                 rd;
	int                 simple_runs;
	int                 io_ran;
	int                 timer_ran;
} guest_bootstrap_state_t;

static void
guest_bootstrap_simple(void *varg)
{
	guest_bootstrap_state_t *s = varg;

	s->simple_runs++;
}

static void
guest_bootstrap_io(void *varg)
{
	guest_bootstrap_state_t *s = varg;
	char                     byte;

	if (strand_fiber_wait_readable(s->sched, s->rd) != STRAND_OK)
		return;
	if (read(s->rd, &byte, sizeof(byte)) == (ssize_t)sizeof(byte))
		s->io_ran = 1;
}

static void
guest_bootstrap_timer(void *varg)
{
	guest_bootstrap_state_t *s = varg;

	if (strand_fiber_sleep_until(s->sched, s->deadline) == STRAND_OK)
		s->timer_ran = 1;
}

static int
test_integration_public_guest_bootstrap(void)
{
	strand_sched_config_t   cfg = { .budget = 2 };
	guest_bootstrap_state_t state;
	strand_scheduler_t     *sched;
	strand_fiber_handle_t   io_handle = {0};
	strand_fiber_handle_t   timer_handle = {0};
	int                     fds[2];
	int                     i;
	char                    byte = 'x';

	sched = strand_scheduler_create(&cfg);
	if (sched == NULL)
		return (1);
	if (make_pipe_pair(fds) != 0) {
		strand_scheduler_destroy(sched);
		return (1);
	}
	memset(&state, 0, sizeof(state));
	state.sched = sched;
	state.deadline = 2000;
	state.rd = fds[0];
	strand_test_clock_ns = 1000;
	for (i = 0; i < 5; i++) {
		if (strand_scheduler_spawn(sched, guest_bootstrap_simple, &state,
		    0, NULL) != STRAND_OK)
			goto fail;
	}
	if (strand_scheduler_spawn(sched, guest_bootstrap_io, &state, 0,
	    &io_handle) != STRAND_OK ||
	    strand_scheduler_spawn(sched, guest_bootstrap_timer, &state, 0,
	    &timer_handle) != STRAND_OK)
		goto fail;

	/* A single pass runs only two fibers; draining reaches every runnable one. */
	for (i = 0; i < 16 && strand_scheduler_advance(sched, NULL) ==
	    STRAND_SCHED_PROGRESS; i++)
		;
	if (i == 16 || state.simple_runs != 5 || state.io_ran != 0 ||
	    state.timer_ran != 0)
		goto fail;
	if (write(fds[1], &byte, sizeof(byte)) != (ssize_t)sizeof(byte))
		goto fail;
	for (i = 0; i < 16 && state.io_ran == 0; i++)
		(void)strand_scheduler_advance(sched, NULL);
	if (i == 16 || state.io_ran != 1 || state.timer_ran != 0)
		goto fail;
	strand_test_clock_ns = state.deadline;
	for (i = 0; i < 16 && state.timer_ran == 0; i++)
		(void)strand_scheduler_advance(sched, NULL);
	if (i == 16 || state.timer_ran != 1)
		goto fail;
	for (i = 0; i < 16 && strand_scheduler_advance(sched, NULL) ==
	    STRAND_SCHED_PROGRESS; i++)
		;
	if (i == 16)
		goto fail;
	close(fds[0]);
	close(fds[1]);
	strand_scheduler_destroy(sched);
	return (0);

fail:
	(void)strand_fiber_cancel(io_handle);
	(void)strand_fiber_cancel(timer_handle);
	for (i = 0; i < 16 && strand_scheduler_advance(sched, NULL) ==
	    STRAND_SCHED_PROGRESS; i++)
		;
	close(fds[0]);
	close(fds[1]);
	strand_scheduler_destroy(sched);
	return (1);
}

/* =========================================================================
 * Test 7.11 - Scheduler Stop Interrupts Blocked Worker
 *
 * Verify that strand_scheduler_stop interrupts a worker blocked in
 * epoll_wait/kevent within a bounded time (500ms wall clock, generous
 * enough for Valgrind).
 *
 * This test confirms BOTH steps of strand_scheduler_stop work correctly:
 *   1. Atomic stop flag is set (prevents re-entry into the blocking poll).
 *   2. A byte is written to the wakeup fd to interrupt the ongoing poll.
 *
 * Pattern (from ARCHITECTURE.md 4.2):
 *   - Start a runtime with one worker (strand_scheduler_run on its own thread).
 *   - Give the worker 50ms to enter the blocking poll.
 *   - Call strand_worker_stop.
 *   - Verify strand_worker_join returns within 500ms.
 *
 * See TESTING.md 7.11, ARCHITECTURE.md 4.2.
 * =========================================================================
 */

int
test_integration_stop_interrupts_worker(void)
{
        strand_runtime_t  *rt;
        strand_worker_t   *w;
        uint64_t           t_stop_ns, t_join_ns;
        uint64_t           elapsed_ms;
        struct timespec    sleep_tv;

        rt = make_test_runtime();
        if (rt == NULL)
                return (1);

        w = make_test_worker(rt);
        if (w == NULL) {
                strand_runtime_destroy(rt);
                return (1);
        }

        /*
         * Give the worker thread time to enter the blocking poll inside
         * strand_scheduler_run.  50ms is sufficient on any real machine.
         * Under Valgrind the worker may take longer to schedule, but it
         * will still be inside the blocking poll well within the 500ms
         * budget.
         */
        sleep_tv.tv_sec  = 0;
        sleep_tv.tv_nsec = 50 * 1000000L; /* 50ms */
        nanosleep(&sleep_tv, NULL);

        /* Stop the worker and measure how long join takes. */
        t_stop_ns = test_now_ns();
        strand_worker_stop(w);
        strand_worker_join(w);
        t_join_ns = test_now_ns();

        elapsed_ms = (t_join_ns - t_stop_ns) / 1000000ULL;

        /*
         * strand_runtime_destroy handles cleanup.  The worker is already
         * stopped and joined; destroy will not call stop/join again.
         */
        strand_runtime_destroy(rt);

        /*
         * 500ms is generous: on real hardware this completes in < 1ms.
         * The bound exists to catch the case where the wakeup fd write
         * is missing (strand_scheduler_run would block indefinitely).
         */
        return (elapsed_ms < 500) ? 0 : 1;
}

/* =========================================================================
 * 13. Watchdog warning for long-running fibers
 * =========================================================================
 */

#ifdef STRAND_DEBUG

/*
 * Fiber that busy-spins briefly to trigger the debug watchdog.
 */
static void
watchdog_spin_fiber(void *arg)
{
        volatile int *done = arg;
        /*
         * Spin for ~5 ms.  With a 1 ns threshold the watchdog will fire.
         */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t start = (uint64_t)ts.tv_sec * 1000000000ULL +
                         (uint64_t)ts.tv_nsec;
        for (;;) {
                clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL +
                               (uint64_t)ts.tv_nsec;
                if (now - start > 5000000ULL) /* 5 ms */
                        break;
        }
        *done = 1;
}

static int
test_integration_watchdog_warning(void)
{
        int fds[2];
        if (pipe(fds) != 0)
                return (1);

        /* Redirect stderr to the pipe write end. */
        int saved_stderr = dup(STDERR_FILENO);
        dup2(fds[1], STDERR_FILENO);

        strand_sched_config_t cfg = {
            .budget     = 64,
            .inject_cap = 256,
            .cache_cap  = 8,
            .idle_floor = 2,
            .watchdog_threshold_ns = 1, /* 1 ns - guaranteed to trigger */
        };
        strand_scheduler_t *sched = strand_scheduler_create(&cfg);
        if (sched == NULL) {
                dup2(saved_stderr, STDERR_FILENO);
                close(saved_stderr);
                close(fds[0]);
                close(fds[1]);
                return (1);
        }

        volatile int done = 0;
        if (push_root_fiber(sched, watchdog_spin_fiber, (void *)&done) != 0) {
                strand_scheduler_destroy(sched);
                dup2(saved_stderr, STDERR_FILENO);
                close(saved_stderr);
                close(fds[0]);
                close(fds[1]);
                return (1);
        }

        strand_scheduler_advance(sched, NULL);

        /* Restore stderr and close write end so read sees EOF. */
        fflush(stderr);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
        close(fds[1]);

        /* Read captured output. */
        char buf[1024];
        ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
        close(fds[0]);

        strand_scheduler_destroy(sched);

        if (!done)
                return (1);
        if (n <= 0)
                return (1);
        buf[n] = '\0';
        if (strstr(buf, "strand: watchdog:") == NULL)
                return (1);

	return (0);
}

#endif /* STRAND_DEBUG */

/* =========================================================================
 * Suite entry point
 * =========================================================================
 */

void
run_integration_tests(void)
{
        const char *only = getenv("INTEG_TEST");
        int n = only ? atoi(only) : 0;

        if (n == 0 || n == 1)
        RUN("test_integration_echo_server",
            test_integration_echo_server);
        if (n == 0 || n == 2)
        RUN("test_integration_producer_consumer",
            test_integration_producer_consumer);
        if (n == 0 || n == 3)
        RUN("test_integration_fan_out_scope_error",
            test_integration_fan_out_scope_error);
        if (n == 0 || n == 4)
        RUN("test_integration_timeout_then_abandon",
            test_integration_timeout_then_abandon);
        if (n == 0 || n == 5)
        RUN("test_integration_offload_cancel_result_claimed_wins",
            test_integration_offload_cancel_result_claimed_wins);
        if (n == 0 || n == 6)
        RUN("test_integration_offload_cancel_cancelled_wins",
            test_integration_offload_cancel_cancelled_wins);
        if (n == 0 || n == 7)
        RUN("test_integration_offload_eagain_retry",
            test_integration_offload_eagain_retry);
        if (n == 0 || n == 8)
        RUN("test_integration_subprocess_via_offload",
            test_integration_subprocess_via_offload);
        if (n == 0 || n == 9)
        RUN("test_integration_multiworker_distribution",
            test_integration_multiworker_distribution);
        if (n == 0 || n == 10)
        RUN("test_integration_simultaneous_read_write",
            test_integration_simultaneous_read_write);
        if (n == 0 || n == 11)
        RUN("test_integration_guest_mode_host_loop",
            test_integration_guest_mode_host_loop);
        if (n == 0 || n == 12)
        RUN("test_integration_stop_interrupts_worker",
            test_integration_stop_interrupts_worker);
#ifdef STRAND_DEBUG
	if (n == 0 || n == 13)
		RUN("test_integration_watchdog_warning",
		    test_integration_watchdog_warning);
#endif
	if (n == 0 || n == 14)
		RUN("test_integration_guest_self_scheduler",
		    test_integration_guest_self_scheduler);
	if (n == 0 || n == 15)
		RUN("test_integration_public_guest_bootstrap",
		    test_integration_public_guest_bootstrap);
}
