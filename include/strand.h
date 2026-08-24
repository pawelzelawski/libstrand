/*
 * strand.h - libstrand public API
 *
 * libstrand is a stackful fiber concurrency library for C, designed as an
 * embeddable runtime for connection-oriented network daemons on Linux and
 * OpenBSD.
 *
 * THE FIVE-LAYER MODEL:
 *
 *   Layer 1 - Execution Contexts: x86_64/AArch64 context save/restore.
 *   Layer 2 - Fiber Scheduler: cooperative run queue, timer heap, budget.
 *   Layer 3 - I/O Integration: epoll/kqueue fd parking (edge-triggered).
 *   Layer 4 - Multi-Worker Runtime: pinned workers, inject queues, offload.
 *   Layer 5 - Coordination: scopes, cancellation, fiber-local storage.
 *
 * TWO USAGE MODES:
 *
 *   Guest mode - the embedder drives the scheduler from their own event
 *   loop via strand_scheduler_advance().  Key functions:
 *     strand_scheduler_create, strand_scheduler_advance,
 *     strand_scheduler_get_fd, strand_scheduler_next_deadline.
 *
 *   Worker mode - the library owns a thread that blocks in
 *   strand_scheduler_run() until strand_scheduler_stop() is called.
 *   Key functions:
 *     strand_runtime_init, strand_worker_start, strand_runtime_spawn,
 *     strand_runtime_destroy.
 *
 * HOST LOOP INTEGRATION (guest mode):
 *
 *   while (running) {
 *       uint64_t deadline = strand_scheduler_next_deadline(sched);
 *       int timeout_ms    = deadline_to_ms(deadline);
 *
 *       int nfds = epoll_wait(host_epoll, events, max, timeout_ms);
 *
 *       for (int i = 0; i < nfds; i++) {
 *           if (events[i].data.fd == strand_scheduler_get_fd(sched))
 *               strand_scheduler_advance(sched, NULL);
 *           else
 *               handle_app_event(&events[i]);
 *       }
 *       // Always call advance after epoll_wait for timer expiry.
 *       strand_scheduler_advance(sched, NULL);
 *   }
 *
 * THREADING RULES (summary):
 *
 *   Fiber-only: strand_fiber_yield, strand_fiber_self_scheduler,
 *     strand_fiber_sleep_until,
 *     strand_fiber_wait_readable, strand_fiber_wait_writable,
 *     strand_fiber_offload, strand_fiber_local_get/set,
 *     strand_scope_open, strand_scope_spawn, strand_scope_wait,
 *     strand_scope_wait_timeout.
 *
 *   Host-thread-only: strand_scheduler_advance, strand_scheduler_run,
 *     strand_scheduler_next_deadline, strand_scheduler_get_fd,
 *     strand_runtime_init, strand_runtime_destroy,
 *     strand_worker_join, strand_worker_get_scheduler.
 *
 *   Any thread: strand_scheduler_stop, strand_fiber_cancel,
 *     strand_scope_cancel, strand_scope_abandon.
 *
 *   Fiber or host thread: strand_fiber_spawn, strand_fiber_spawn_detached,
 *     strand_runtime_spawn.
 *
 * FLOATING-POINT LIMITATION:
 *
 *   The x87 FP environment is NOT saved across context switches.  Code that
 *   modifies x87 state (including long double computations) across yield
 *   points may see corrupted floating-point results.  SSE/NEON state
 *   (MXCSR on x86_64, FPCR/FPSR on AArch64) IS saved and restored.
 *
 * See ARCHITECTURE.md for the full design document.
 */

#ifndef STRAND_H
#define STRAND_H

#include <stddef.h>
#include <stdint.h>

/* ===========================================================================
 * Version
 * ===========================================================================
 */

#define STRAND_VERSION_MAJOR 1
#define STRAND_VERSION_MINOR 0
#define STRAND_VERSION_PATCH 0

/* ===========================================================================
 * Opaque types
 *
 * Internal layouts are private and subject to change between versions.
 * Embedders use pointers to these; full definitions live in
 * src/strand_internal.h.
 * ===========================================================================
 */

typedef struct strand_scheduler strand_scheduler_t;
typedef struct strand_runtime strand_runtime_t;
typedef struct strand_worker strand_worker_t;
typedef struct strand_poller strand_poller_t;
typedef struct strand_scope strand_scope_t;
typedef struct strand_fiber strand_fiber_t;

/* ===========================================================================
 * Value types
 * ===========================================================================
 */

/*
 * strand_fiber_handle_t - ABA-safe fiber reference.
 *
 * Passed and returned by value.  16 bytes: pointer + generation counter.
 * The generation counter prevents use-after-recycle bugs when a fiber
 * descriptor is reused from the dead pool.
 *
 * Handles are created only by strand_fiber_spawn, strand_fiber_spawn_detached,
 * strand_scope_spawn, and strand_runtime_spawn.  Manual construction is not
 * part of the API.
 *
 * See ARCHITECTURE.md §4.6.
 */
typedef struct strand_fiber_handle {
	strand_fiber_t *ptr;
	uint64_t generation;
} strand_fiber_handle_t;

/*
 * strand_fiber_fn_t - fiber entry function.
 *
 * Called with the argument passed to strand_fiber_spawn.  Returns void;
 * errors are not propagated to a scope.  Used for fibers spawned via
 * strand_fiber_spawn and strand_fiber_spawn_detached.
 */
typedef void (*strand_fiber_fn_t)(void *arg);

/*
 * strand_scope_fiber_fn_t - scope-tracked fiber entry function.
 *
 * Returns 0 on success, non-zero on failure.  The return value is captured
 * by the runtime trampoline and passed to the scope's first-error slot via
 * CAS.  Used exclusively with strand_scope_spawn.
 *
 * See ARCHITECTURE.md §7.1.
 */
typedef int (*strand_scope_fiber_fn_t)(void *arg);

/*
 * strand_destructor_t - fiber-local storage destructor.
 *
 * Called with the stored pointer when the fiber finishes.  Called exactly
 * once on the scheduler's stack after the fiber has switched back.  If
 * local_ptr is NULL the destructor is still called with NULL.
 */
typedef void (*strand_destructor_t)(void *ptr);

/*
 * blocking_fn_t - function prototype for offloaded blocking calls.
 *
 * Receives arg and must write its result into result_slot before returning.
 * Runs on a plain OS thread; must not call any fiber or scheduler API.
 */
typedef void (*blocking_fn_t)(void *arg, void *result_slot);

/* ===========================================================================
 * Error codes
 *
 * All public functions that can fail return int.  STRAND_OK (0) indicates
 * success; all error codes are negative.
 * ===========================================================================
 */

#define STRAND_OK 0                /* Success; no error. */
#define STRAND_HANDLE_INVALID (-1) /* handle.ptr is NULL. */
#define STRAND_HANDLE_STALE (-2)   /* Generation mismatch; fiber recycled. */
#define STRAND_ERR_SHUTDOWN (-3)   /* Scheduler stopped; operation rejected. */
#define STRAND_ERR_WRONGCTX (-4)   /* Called from invalid context. */
#define STRAND_ERR_NOMEM (-5)      /* Memory allocation failure. */
#define STRAND_CANCELLED (-6)      /* Operation cancelled via strand_fiber_cancel. */
#define STRAND_ERR_IO (-7)         /* fd entered error/hangup state. */
#define STRAND_ERR_IO_CONFLICT (-8)  /* Waiter already registered on fd. */
#define STRAND_ERR_NO_WORKERS (-9)   /* No workers registered in runtime. */
#define STRAND_ERR_NO_OFFLOAD_POOL (-10) /* Offload called with no pool. */
#define STRAND_EAGAIN (-11)        /* Offload pool full; yield and retry. */
#define STRAND_TIMEOUT (-12)       /* Deadline expired before completion. */

/* ===========================================================================
 * Default parameters
 *
 * These values are used when zero is passed in the corresponding config
 * field.  See ARCHITECTURE.md §4.4 and §13.
 * ===========================================================================
 */

#define STRAND_DEFAULT_STACK_SIZE    ((size_t)(64 * 1024))
#define STRAND_DEFAULT_SCHED_BUDGET  ((size_t)64)
#define STRAND_DEFAULT_CACHE_CAP     ((size_t)64)
#define STRAND_DEFAULT_CACHE_FLOOR   ((size_t)8)
#define STRAND_DEFAULT_WATCHDOG_NS   ((uint64_t)(100 * 1000000ULL))
#define STRAND_DEFAULT_MAX_WORKERS   ((size_t)64)

/* ===========================================================================
 * Configuration structs
 * ===========================================================================
 */

/*
 * strand_sched_config_t - scheduler creation parameters.
 *
 * Caller-allocated.  Pass zero for any field to use the default.
 * Pass NULL to strand_scheduler_create for all defaults.
 *
 * budget                - max fibers to run per advance call.
 * inject_cap            - inject queue capacity; must be power of 2.
 * cache_cap             - stack cache hard cap (stacks per worker).
 * idle_floor            - stack cache idle reclamation floor.
 * watchdog_threshold_ns - debug watchdog threshold in nanoseconds.
 *                         Only active in STRAND_DEBUG builds.
 *
 * See ARCHITECTURE.md §4.4, §13.
 */
typedef struct strand_sched_config {
	size_t budget;
	size_t inject_cap;
	size_t cache_cap;
	size_t idle_floor;
	uint64_t watchdog_threshold_ns;
} strand_sched_config_t;

/*
 * strand_runtime_config_t - runtime creation parameters.
 *
 * Caller-allocated.  Pass NULL to strand_runtime_init for defaults.
 *
 * max_workers - maximum workers that may be registered.
 *               0 uses STRAND_DEFAULT_MAX_WORKERS.
 */
typedef struct strand_runtime_config {
	size_t max_workers;
} strand_runtime_config_t;

/*
 * strand_worker_config_t - per-worker creation parameters.
 *
 * Caller-allocated.  Pass NULL to strand_worker_start for defaults.
 *
 * sched_cfg    - scheduler parameters; NULL for defaults.
 * cpu_affinity - Linux: pin worker to this CPU index.
 *                -1 = no affinity.  OpenBSD: silently ignored.
 */
typedef struct strand_worker_config {
	const strand_sched_config_t *sched_cfg;
	int cpu_affinity;
} strand_worker_config_t;

/* ===========================================================================
 * Runtime and worker lifecycle
 * See ARCHITECTURE.md §6.1, §6.2.
 * ===========================================================================
 */

/*
 * strand_runtime_init - allocate and initialise a multi-worker runtime.
 *
 * cfg: runtime configuration; NULL for all defaults.
 *
 * Returns a runtime-allocated handle on success.
 * Returns NULL on allocation failure.
 *
 * Callable from: host thread only.
 */
strand_runtime_t *strand_runtime_init(const strand_runtime_config_t *cfg);

/*
 * strand_runtime_destroy - stop all workers, join threads, free runtime.
 *
 * Calls strand_scheduler_stop on every registered worker, joins each
 * worker thread, destroys each scheduler, then frees the runtime.
 * Safe to call even if some workers were never started.
 *
 * rt: runtime handle returned by strand_runtime_init.
 *
 * Callable from: host thread only.
 */
void strand_runtime_destroy(strand_runtime_t *rt);

/*
 * strand_worker_start - create and register a new worker thread.
 *
 * Allocates a worker, creates its scheduler, spawns a pthread that calls
 * strand_scheduler_run, and registers the worker with the runtime.
 *
 * rt:  runtime handle.
 * cfg: worker configuration; NULL for defaults.
 *
 * Returns a worker handle on success.
 * Returns NULL on allocation/thread creation failure, if max_workers
 * reached, or if shutdown has begun.
 *
 * Callable from: host thread only.
 */
strand_worker_t *strand_worker_start(strand_runtime_t *rt,
    const strand_worker_config_t *cfg);

/*
 * strand_worker_stop - signal a worker to stop.
 *
 * Calls strand_scheduler_stop on the worker's scheduler.
 *
 * w: worker handle.
 *
 * Callable from: any thread.
 */
void strand_worker_stop(strand_worker_t *w);

/*
 * strand_worker_join - wait for a worker thread to exit.
 *
 * Blocks until the worker's pthread exits.  Must be called after
 * strand_worker_stop.
 *
 * w: worker handle.
 *
 * Callable from: host thread only.
 */
void strand_worker_join(strand_worker_t *w);

/*
 * strand_worker_get_scheduler - get the scheduler owned by a worker.
 *
 * Returns a pointer to the worker's scheduler.  Useful when the host
 * thread needs the scheduler pointer (e.g. for strand_scheduler_get_fd
 * in a guest-mode host loop, or to pass via an arg struct to fibers).
 *
 * w: worker handle returned by strand_worker_start.
 *
 * Returns a valid strand_scheduler_t pointer.  The pointer remains valid
 * until the worker is destroyed.
 *
 * Callable from: host thread only.
 */
strand_scheduler_t *strand_worker_get_scheduler(strand_worker_t *w);

/*
 * strand_runtime_spawn - spawn a fiber from the host thread.
 *
 * Selects a target worker via round-robin (worker == NULL) or uses the
 * explicitly supplied worker.  Injects the fiber to the target worker's
 * inject queue.
 *
 * rt:       runtime handle.
 * fn:       fiber entry function.
 * arg:      argument passed to fn; ownership is the caller's.
 * stack_sz: usable stack bytes; 0 uses STRAND_DEFAULT_STACK_SIZE. Any
 *           positive size is accepted; libstrand aligns initial execution
 *           internally without increasing the requested usable mapping.
 * worker:   explicit target; NULL for automatic round-robin.
 * out:      if non-NULL, receives ABA-safe handle on success.
 *
 * Returns STRAND_OK           on success.
 * Returns STRAND_ERR_SHUTDOWN if runtime shutdown has begun.
 * Returns STRAND_ERR_NO_WORKERS if no workers are registered.
 * Returns STRAND_ERR_WRONGCTX if worker does not belong to rt.
 * Returns STRAND_ERR_NOMEM    on allocation failure.
 *
 * Callable from: host thread or running fiber.
 * See ARCHITECTURE.md §6.4.
 */
int strand_runtime_spawn(strand_runtime_t *rt, strand_fiber_fn_t fn,
    void *arg, size_t stack_sz, strand_worker_t *worker,
    strand_fiber_handle_t *out);

/* ===========================================================================
 * Scheduler (guest mode)
 * See ARCHITECTURE.md §4.2, §4.3.
 * ===========================================================================
 */

/*
 * sched_result_t - return value of strand_scheduler_advance.
 *
 * STRAND_SCHED_PROGRESS - at least one fiber ran or timer fired.
 * STRAND_SCHED_IDLE     - nothing ran; next_deadline_ns holds the next
 *                         timer deadline (UINT64_MAX if none).
 */
typedef enum {
	STRAND_SCHED_PROGRESS = 0,
	STRAND_SCHED_IDLE = 1
} sched_result_t;

/*
 * strand_scheduler_create - allocate and initialise a scheduler.
 *
 * cfg: scheduler configuration; NULL for all defaults.
 *
 * Returns a runtime-allocated scheduler on success.
 * Returns NULL on allocation failure.
 *
 * Callable from: host thread only.
 */
strand_scheduler_t *strand_scheduler_create(const strand_sched_config_t *cfg);

/*
 * strand_scheduler_destroy - tear down and free a scheduler.
 *
 * All fibers must have finished before calling this.
 *
 * sched: scheduler handle.
 *
 * Callable from: host thread only.
 */
void strand_scheduler_destroy(strand_scheduler_t *sched);

/*
 * strand_scheduler_advance - perform one nonblocking scheduler pass.
 *
 * Steps: (1) drain inject queue, (2) expire timers, (3) drain wakeup fd,
 * (4) poll I/O with zero timeout, (5) run up to budget fibers.
 *
 * sched:           scheduler handle.
 * next_deadline_ns: if non-NULL and result is SCHED_IDLE, receives the
 *                   next timer deadline (UINT64_MAX if none).
 *
 * Returns STRAND_SCHED_PROGRESS if any work was done.
 * Returns STRAND_SCHED_IDLE otherwise.
 *
 * Callable from: host thread only. The caller exclusively owns sched for the
 * duration of this call; concurrent advance or run calls on the same
 * scheduler are not supported.
 * See ARCHITECTURE.md §4.2.
 */
sched_result_t strand_scheduler_advance(strand_scheduler_t *sched,
    uint64_t *next_deadline_ns);

/*
 * strand_scheduler_run - blocking worker-mode loop.
 *
 * Calls strand_scheduler_advance in a loop; blocks in poll when idle.
 * Returns only after strand_scheduler_stop is called.
 *
 * sched: scheduler handle.
 *
 * Callable from: host thread only (one call per worker thread).
 * See ARCHITECTURE.md §4.2.
 */
void strand_scheduler_run(strand_scheduler_t *sched);

/*
 * strand_scheduler_stop - signal a scheduler to stop.
 *
 * Sets the atomic stop flag and writes to the wakeup fd to interrupt a
 * blocked poll.
 *
 * sched: scheduler handle.
 *
 * Callable from: any thread.
 * See ARCHITECTURE.md §4.2.
 */
void strand_scheduler_stop(strand_scheduler_t *sched);

/*
 * strand_scheduler_next_deadline - next pending timer deadline.
 *
 * sched: scheduler handle.
 *
 * Returns a CLOCK_MONOTONIC nanosecond timestamp, or UINT64_MAX if no
 * timers are pending.
 *
 * Callable from: host thread only.
 */
uint64_t strand_scheduler_next_deadline(const strand_scheduler_t *sched);

/*
 * strand_scheduler_get_fd - fd the host loop must monitor.
 *
 * Returns the scheduler's internal poller fd (epoll on Linux, kqueue on
 * OpenBSD).  Becomes readable when scheduler-managed activity is pending.
 * The host loop registers this fd and calls strand_scheduler_advance when
 * it fires.
 *
 * sched: scheduler handle.
 *
 * Returns a valid file descriptor.
 *
 * Callable from: host thread only.
 * See ARCHITECTURE.md §4.2, §4.3.
 */
int strand_scheduler_get_fd(const strand_scheduler_t *sched);

/* ===========================================================================
 * Fiber operations
 * See ARCHITECTURE.md §4.5, §4.6.
 * ===========================================================================
 */

/*
 * strand_fiber_spawn - spawn a new fiber on the current worker.
 *
 * The new fiber is assigned to the same worker as the calling fiber.
 * Host-thread spawning uses strand_runtime_spawn instead.
 *
 * sched:    scheduler handle.
 * fn:       fiber entry function; called as fn(arg).
 * arg:      argument passed to fn; ownership is the caller's.
 * stack_sz: usable stack size in bytes; 0 uses STRAND_DEFAULT_STACK_SIZE.
 *           Any positive size is accepted; libstrand aligns initial
 *           execution internally without increasing the requested mapping.
 * out:      if non-NULL, receives ABA-safe handle on success.
 *
 * Returns STRAND_OK           on success.
 * Returns STRAND_ERR_SHUTDOWN if the scheduler has been stopped.
 * Returns STRAND_ERR_WRONGCTX if called from the host thread.
 * Returns STRAND_ERR_NOMEM    on allocation failure.
 *
 * Callable from: running fiber only.
 * See ARCHITECTURE.md §4.5, §4.6.
 */
int strand_fiber_spawn(strand_scheduler_t *sched, strand_fiber_fn_t fn,
    void *arg, size_t stack_sz, strand_fiber_handle_t *out);

/*
 * strand_fiber_spawn_detached - spawn a fiber with no scope tracking.
 *
 * Identical to strand_fiber_spawn except the fiber's scope pointer is
 * explicitly NULL.  Not tracked by any scope; errors are not propagated.
 * STRONGLY DISCOURAGED.  See ARCHITECTURE.md §7.7.
 *
 * Parameters and return values identical to strand_fiber_spawn.
 *
 * Callable from: running fiber only.
 */
int strand_fiber_spawn_detached(strand_scheduler_t *sched,
    strand_fiber_fn_t fn, void *arg, size_t stack_sz,
    strand_fiber_handle_t *out);

/*
 * strand_fiber_yield - voluntarily yield the current fiber.
 *
 * Moves the fiber to the tail of the run queue and switches to the
 * scheduler.  This is both a yield point and a cancellation point.
 *
 * sched: scheduler handle.
 *
 * Precondition: must be called from inside a running fiber.
 *
 * Callable from: running fiber only.
 * See ARCHITECTURE.md §4.5.
 */
void strand_fiber_yield(strand_scheduler_t *sched);

/*
 * strand_fiber_self_scheduler - get the current fiber's home scheduler.
 *
 * Returns the scheduler that owns the calling fiber.  Allows fiber code
 * to obtain its scheduler without requiring it to be passed through the
 * arg struct.
 *
 * Returns the scheduler pointer if called from a running fiber.
 * Returns NULL if called from outside a fiber context (host thread,
 * signal handler, offload thread).
 *
 * Callable from: running fiber (returns NULL from any other context).
 */
strand_scheduler_t *strand_fiber_self_scheduler(void);

/*
 * strand_fiber_cancel - cancel a fiber identified by an ABA-safe handle.
 *
 * Validates the handle, then acts based on the fiber's current state:
 *   FIBER_PARKED_TIMER      - remove from heap; wake with STRAND_CANCELLED.
 *   FIBER_PARKED_IO_*       - cancel registration; wake with CANCELLED.
 *   FIBER_RUNNABLE/RUNNING  - set cancel_pending flag.
 *   FIBER_PARKED_OFFLOAD    - CAS on work item; see ARCHITECTURE.md §6.6.
 *   FIBER_FINISHED          - no-op; returns STRAND_OK.
 *
 * handle: ABA-safe fiber handle.
 *
 * Returns STRAND_OK             on success or no-op.
 * Returns STRAND_HANDLE_INVALID if handle.ptr is NULL.
 * Returns STRAND_HANDLE_STALE   if generation does not match.
 *
 * Same-worker calls apply immediately.  Cross-worker calls are
 * enqueue-only: the cancel is processed on the next advance pass.
 *
 * Callable from: any thread.
 * See ARCHITECTURE.md §8.1.
 */
int strand_fiber_cancel(strand_fiber_handle_t handle);

/* ===========================================================================
 * I/O parking
 * See ARCHITECTURE.md §5.
 * ===========================================================================
 */

/*
 * strand_fiber_wait_readable - park until fd is readable.
 *
 * Registers fd with the internal poller and parks the fiber.  fd must
 * have O_NONBLOCK set.  No other fiber may be waiting for read on the
 * same fd.
 *
 * sched: scheduler handle.
 * fd:    file descriptor with O_NONBLOCK set.
 *
 * Returns STRAND_OK              - fd is ready for reading.
 * Returns STRAND_CANCELLED       - fiber was cancelled while parked.
 * Returns STRAND_ERR_IO          - fd entered error/hangup state.
 * Returns STRAND_ERR_IO_CONFLICT - read waiter already registered on fd.
 * Returns STRAND_ERR_NOMEM       - fd table allocation failed.
 *
 * Callable from: running fiber only.
 * See ARCHITECTURE.md §5.
 */
int strand_fiber_wait_readable(strand_scheduler_t *sched, int fd);

/*
 * strand_fiber_wait_writable - park until fd is writable.
 *
 * Symmetric to strand_fiber_wait_readable for the write direction.
 *
 * sched: scheduler handle.
 * fd:    file descriptor with O_NONBLOCK set.
 *
 * Returns STRAND_OK              - fd is ready for writing.
 * Returns STRAND_CANCELLED       - fiber was cancelled while parked.
 * Returns STRAND_ERR_IO          - fd entered error/hangup state.
 * Returns STRAND_ERR_IO_CONFLICT - write waiter already registered on fd.
 * Returns STRAND_ERR_NOMEM       - fd table allocation failed.
 *
 * Callable from: running fiber only.
 * See ARCHITECTURE.md §5.
 */
int strand_fiber_wait_writable(strand_scheduler_t *sched, int fd);

/* ===========================================================================
 * Timers
 * See ARCHITECTURE.md §4.5.
 * ===========================================================================
 */

/*
 * strand_fiber_sleep_until - park the current fiber until a deadline.
 *
 * Inserts the fiber into the timer min-heap.  Resumes when the deadline
 * passes or the fiber is cancelled.  A deadline in the past causes
 * immediate resumption on the next advance call.
 *
 * sched:       scheduler handle.
 * deadline_ns: absolute CLOCK_MONOTONIC deadline in nanoseconds.
 *
 * Returns STRAND_OK       if the deadline expired normally.
 * Returns STRAND_CANCELLED if the fiber was cancelled while parked.
 *
 * Callable from: running fiber only.
 * See ARCHITECTURE.md §4.5.
 */
int strand_fiber_sleep_until(strand_scheduler_t *sched, uint64_t deadline_ns);

/* ===========================================================================
 * Blocking syscall offload pool
 * See ARCHITECTURE.md §6.5, §6.6, §6.7.
 * ===========================================================================
 */

/*
 * strand_offload_pool_t - opaque handle to the global offload thread pool.
 *
 * One pool per process.  Runtime-allocated by strand_offload_pool_init;
 * do not free directly.  Internal layout is private.
 */
typedef struct strand_offload_pool strand_offload_pool_t;

/*
 * strand_offload_pool_init - create the global offload thread pool.
 *
 * Spawns thread_count OS threads that service blocking work items.
 * Must be called at most once before any strand_fiber_offload calls.
 *
 * thread_count: number of offload threads to spawn.
 *
 * Returns non-NULL on success.
 * Returns NULL on allocation or thread-creation failure.
 *
 * Callable from: host thread only.
 * See ARCHITECTURE.md §6.5.
 */
strand_offload_pool_t *strand_offload_pool_init(size_t thread_count);

/*
 * strand_offload_pool_destroy - shut down and join all offload threads.
 *
 * Signals threads to exit, waits for in-flight work, joins and frees.
 * Must not be called while any fiber may still call strand_fiber_offload.
 *
 * pool: offload pool handle.
 *
 * Callable from: host thread only.
 * See ARCHITECTURE.md §6.5.
 */
void strand_offload_pool_destroy(strand_offload_pool_t *pool);

/*
 * strand_fiber_offload - run a blocking function on an offload thread.
 *
 * Parks the calling fiber and submits fn(arg, result_slot) to the pool.
 * The fiber resumes when fn completes or is cancelled.
 *
 * sched:       scheduler handle.
 * pool:        offload pool handle.
 * fn:          blocking function; must not call any fiber/scheduler API.
 * arg:         passed to fn; must remain valid until fn completes, even
 *              if the fiber is cancelled.
 * result_slot: caller-owned storage that fn writes its result into;
 *              must remain valid until fn completes.
 *
 * Pool full: returns STRAND_EAGAIN immediately.  Caller MUST yield before
 * retrying:
 *
 *   while ((rc = strand_fiber_offload(sched, pool, fn, arg, &res))
 *          == STRAND_EAGAIN) {
 *       strand_fiber_yield(sched);
 *   }
 *
 * Returns STRAND_OK                 on success (fn ran, result written).
 * Returns STRAND_CANCELLED          if cancelled before fn claimed result.
 * Returns STRAND_EAGAIN             if pool is full; yield and retry.
 * Returns STRAND_ERR_NO_OFFLOAD_POOL if pool is NULL.
 * Returns STRAND_ERR_WRONGCTX       if called from outside a fiber.
 * Returns STRAND_ERR_NOMEM          on work item allocation failure.
 *
 * Callable from: running fiber only.
 * See ARCHITECTURE.md §6.5, §6.6, §6.7.
 */
int strand_fiber_offload(strand_scheduler_t *sched,
    strand_offload_pool_t *pool, blocking_fn_t fn, void *arg,
    void *result_slot);

/* ===========================================================================
 * Structured concurrency scopes
 * See ARCHITECTURE.md §7.
 * ===========================================================================
 */

/*
 * strand_scope_open - initialise a scope control block.
 *
 * Sets the scope to SCOPE_ACTIVE with OWNER_CALLER and records the
 * calling fiber as the parent.
 *
 * sched: scheduler handle.
 * scope: caller-allocated scope control block (stack or heap).
 *
 * Returns STRAND_OK on success.
 * Returns STRAND_ERR_WRONGCTX if called from a host thread.
 *
 * Callable from: running fiber only.
 * See ARCHITECTURE.md §7.1, §7.2.
 */
int strand_scope_open(strand_scheduler_t *sched, strand_scope_t *scope);

/*
 * strand_scope_spawn - spawn a scope-tracked child fiber.
 *
 * Spawns fn(arg) on the same worker, tracks it under scope, and
 * increments live_child_count.
 *
 * sched: scheduler handle.
 * scope: scope control block (must be SCOPE_ACTIVE or SCOPE_CANCELLING).
 * fn:    scope fiber entry; returns 0 on success, non-zero on failure.
 * arg:   argument passed to fn; ownership is the caller's.
 * out:   if non-NULL, receives ABA-safe handle on success.
 *
 * Returns STRAND_OK           on success.
 * Returns STRAND_ERR_WRONGCTX if called from a host thread.
 * Returns STRAND_ERR_SHUTDOWN if the scheduler has been stopped.
 * Returns STRAND_ERR_NOMEM    on allocation failure.
 *
 * Callable from: running fiber only.
 * See ARCHITECTURE.md §7.1.
 */
int strand_scope_spawn(strand_scheduler_t *sched, strand_scope_t *scope,
    strand_scope_fiber_fn_t fn, void *arg, strand_fiber_handle_t *out);

/*
 * strand_scope_wait - block until all scope children have finished.
 *
 * Parks the fiber until scope reaches SCOPE_COMPLETED.  Terminal: no
 * follow-up call is required or permitted.
 *
 * If the scope has no children, transitions to SCOPE_COMPLETED and
 * returns 0 immediately without parking.
 *
 * sched: scheduler handle.
 * scope: scope control block.
 *
 * Returns scope->first_error (>= 0) on normal completion.
 * Returns STRAND_CANCELLED if the calling fiber is cancelled while parked.
 * Returns STRAND_ERR_WRONGCTX if called from a host thread.
 *
 * Callable from: running fiber only.
 * See ARCHITECTURE.md §7.4.
 */
int strand_scope_wait(strand_scheduler_t *sched, strand_scope_t *scope);

/*
 * strand_scope_wait_timeout - block until scope completes or deadline
 * expires.
 *
 * Terminal if the scope completes before the deadline.  Non-terminal if
 * the deadline fires first; caller must follow with strand_scope_wait or
 * strand_scope_abandon.
 *
 * sched:       scheduler handle.
 * scope:       scope control block.
 * deadline_ns: absolute CLOCK_MONOTONIC deadline in nanoseconds.
 *
 * Returns scope->first_error (>= 0) if scope completed.
 * Returns STRAND_TIMEOUT    if deadline expired (non-terminal).
 * Returns STRAND_CANCELLED  if fiber cancelled while parked (non-terminal).
 * Returns STRAND_ERR_WRONGCTX if called from a host thread.
 * Returns STRAND_ERR_NOMEM  if timer heap allocation fails.
 *
 * Callable from: running fiber only.
 */
int strand_scope_wait_timeout(strand_scheduler_t *sched,
    strand_scope_t *scope, uint64_t deadline_ns);

/*
 * strand_scope_cancel - initiate cancellation of a scope (non-terminal).
 *
 * Transitions from SCOPE_ACTIVE to SCOPE_CANCELLING and cancels all
 * children in reverse spawn order.  Returns immediately.  Caller must
 * follow with strand_scope_wait or strand_scope_abandon.
 *
 * No-op if scope is already SCOPE_CANCELLING, SCOPE_DRAINING, or
 * SCOPE_COMPLETED.
 *
 * sched: scheduler handle.
 * scope: scope control block.
 *
 * Returns STRAND_OK on success or no-op.
 *
 * Callable from: any thread.
 * See ARCHITECTURE.md §7.3.
 */
int strand_scope_cancel(strand_scheduler_t *sched, strand_scope_t *scope);

/*
 * strand_scope_abandon - hand scope ownership to the runtime (terminal).
 *
 * Sets owner_flag to OWNER_RUNTIME.  The scope pointer is invalid for the
 * caller after this call.  Children continue running; the runtime frees
 * the control block when all children complete.
 *
 * IMPORTANT: The control block MUST be heap-allocated.  Passing a
 * stack-allocated scope is undefined behaviour.  Debug builds assert.
 *
 * sched: scheduler handle.
 * scope: heap-allocated scope control block.
 *
 * Callable from: running fiber or host thread.
 * See ARCHITECTURE.md §7.4.
 */
void strand_scope_abandon(strand_scheduler_t *sched, strand_scope_t *scope);

/* ===========================================================================
 * Fiber-local storage
 * See ARCHITECTURE.md §4.7, §7.8.
 * ===========================================================================
 */

/*
 * strand_fiber_local_set - store a fiber-local pointer with optional
 * destructor.
 *
 * Overwrites any previously stored value.  The destructor is called with
 * the stored pointer when the fiber completes.
 *
 * sched: scheduler handle.
 * ptr:   pointer to store; NULL is permitted.
 * dtor:  destructor called on fiber completion; NULL for none.
 *
 * Callable from: running fiber only.
 * See ARCHITECTURE.md §4.7.
 */
void strand_fiber_local_set(strand_scheduler_t *sched, void *ptr,
    strand_destructor_t dtor);

/*
 * strand_fiber_local_get - retrieve the fiber-local pointer.
 *
 * sched: scheduler handle.
 *
 * Returns the stored pointer, or NULL if none set or called from the
 * host thread (debug builds assert).
 *
 * Callable from: running fiber only.
 * See ARCHITECTURE.md §4.7.
 */
void *strand_fiber_local_get(strand_scheduler_t *sched);

/* ===========================================================================
 * Thread safety summary
 *
 * Running fiber only:
 *   strand_fiber_spawn, strand_fiber_spawn_detached, strand_fiber_yield,
 *   strand_fiber_self_scheduler, strand_fiber_sleep_until,
 *   strand_fiber_wait_readable, strand_fiber_wait_writable,
 *   strand_fiber_offload, strand_fiber_local_set, strand_fiber_local_get,
 *   strand_scope_open, strand_scope_spawn, strand_scope_wait,
 *   strand_scope_wait_timeout.
 *
 * Host thread only:
 *   strand_runtime_init, strand_runtime_destroy, strand_worker_start,
 *   strand_worker_join, strand_worker_get_scheduler,
 *   strand_scheduler_create, strand_scheduler_destroy,
 *   strand_scheduler_advance, strand_scheduler_run,
 *   strand_scheduler_next_deadline, strand_scheduler_get_fd,
 *   strand_offload_pool_init, strand_offload_pool_destroy.
 *
 * Any thread:
 *   strand_scheduler_stop, strand_worker_stop, strand_fiber_cancel,
 *   strand_scope_cancel.
 *
 * Running fiber or host thread:
 *   strand_runtime_spawn, strand_scope_abandon.
 *
 * No fiber API may be called from a signal handler or an offload thread.
 *
 * See ARCHITECTURE.md §12 for the full thread safety matrix.
 * ===========================================================================
 */

#endif /* STRAND_H */
