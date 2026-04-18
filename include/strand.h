#ifndef STRAND_H
#define STRAND_H

/*
 * strand.h - libstrand public API
 *
 * Initialise a runtime with strand_runtime_init().
 * Spawn fibers with strand_fiber_spawn().
 * Wait for scopes with strand_scope_wait().
 * Drive a scheduler from a host loop with strand_scheduler_advance().
 *
 * See ARCHITECTURE.md for the full five-layer design.
 * See DEVELOPMENT.md for the phased implementation plan.
 */

#include <stddef.h>
#include <stdint.h>

/*
 * Opaque types - embedders use pointers to these.
 * Full definitions live in src/strand_internal.h (internal only).
 */
typedef struct strand_scheduler strand_scheduler_t;
typedef struct strand_runtime strand_runtime_t;
typedef struct strand_worker strand_worker_t;
typedef struct strand_poller strand_poller_t;
typedef struct strand_scope strand_scope_t;
typedef struct strand_fiber strand_fiber_t;

/*
 * strand_fiber_handle_t - ABA-safe fiber reference.
 * Passed and returned by value. 16 bytes: pointer + generation counter.
 * See ARCHITECTURE.md §4.6.
 */
typedef struct strand_fiber_handle {
	strand_fiber_t *ptr;
	uint64_t generation;
} strand_fiber_handle_t;

/*
 * strand_fiber_fn_t - fiber entry function.
 * Called with the argument passed to strand_fiber_spawn().
 */
typedef void (*strand_fiber_fn_t)(void *arg);

/*
 * strand_destructor_t - fiber-local storage destructor.
 * Called with the stored pointer when the fiber finishes.
 */
typedef void (*strand_destructor_t)(void *ptr);

/*
 * Return codes.
 * STRAND_OK             - success; no error.
 * STRAND_HANDLE_INVALID - handle.ptr is NULL; not a valid handle.
 * STRAND_HANDLE_STALE   - handle ptr exists but generation differs;
 *                         fiber has been recycled since handle was issued.
 * See ARCHITECTURE.md §4.6 for ABA-protection semantics.
 */
#define STRAND_OK 0
#define STRAND_HANDLE_INVALID (-1)
#define STRAND_HANDLE_STALE (-2)
#define STRAND_ERR_SHUTDOWN (-3) /* scheduler stopped; spawn rejected */
#define STRAND_ERR_WRONGCTX (-4) /* called from wrong context / wrong worker */
#define STRAND_ERR_NOMEM (-5)    /* allocation failure */
#define STRAND_CANCELLED (-6)    /* operation cancelled by strand_fiber_cancel */
#define STRAND_ERR_IO (-7)       /* fd entered error or hangup state (EPOLLERR/EPOLLHUP/EV_EOF) */
#define STRAND_ERR_IO_CONFLICT (-8) /* waiter already registered on fd in the same direction */
#define STRAND_ERR_NO_WORKERS (-9)  /* host-thread spawn with no workers registered */
/*
 * Default scheduler and stack cache parameters.
 * These are the values used when zero is passed in strand_sched_config_t.
 * See ARCHITECTURE.md §4.4 and §13.
 */
#define STRAND_DEFAULT_STACK_SIZE ((size_t)(64 * 1024))
#define STRAND_DEFAULT_SCHED_BUDGET ((size_t)64)
#define STRAND_DEFAULT_CACHE_CAP ((size_t)64)
#define STRAND_DEFAULT_CACHE_FLOOR ((size_t)8)

/*
 * strand_sched_config_t - scheduler creation parameters.
 * Pass zero for any field to use the corresponding default.
 *
 * budget      - max fibers to run per strand_scheduler_advance call.
 * inject_cap  - inject queue capacity (Phase 5; ignored in Phase 3).
 * cache_cap   - stack cache hard cap (stacks per worker).
 * idle_floor  - stack cache idle reclamation floor.
 * See ARCHITECTURE.md §4.4, §13.
 */
typedef struct strand_sched_config {
	size_t budget;
	size_t inject_cap;
	size_t cache_cap;
	size_t idle_floor;
} strand_sched_config_t;

/*
 * strand_scheduler_create - allocate and initialise a scheduler.
 * cfg may be NULL to use all defaults.
 * Returns NULL on allocation failure.
 */
strand_scheduler_t *strand_scheduler_create(const strand_sched_config_t *cfg);

/*
 * strand_scheduler_destroy - tear down and free a scheduler.
 * All fibers must be finished before calling this.
 */
void strand_scheduler_destroy(strand_scheduler_t *sched);

/*
 * sched_result_t - return value of strand_scheduler_advance.
 *
 * SCHED_PROGRESS - at least one timer fired or fiber ran this call.
 * SCHED_IDLE     - nothing ran; next_deadline_ns holds the next timer
 *                  deadline as a nanosecond timestamp (UINT64_MAX = none).
 * See ARCHITECTURE.md §4.2.
 */
typedef enum { STRAND_SCHED_PROGRESS = 0, STRAND_SCHED_IDLE = 1 } sched_result_t;

/*
 * strand_scheduler_advance - perform one nonblocking scheduler pass.
 * Steps (per ARCHITECTURE.md §4.2):
 *   1. Drain inject queue.
 *   2. Expire timers whose deadline <= now_ns; move to run queue.
 *   3. Drain wakeup fd - bytes are control signals, not fiber events.
 *   4. Poll I/O with zero timeout.
 *   5. Run up to sched->budget fibers from the run queue.
 * Returns SCHED_PROGRESS if any fibers ran or any timer fired.
 * Returns SCHED_IDLE with *next_deadline_ns set otherwise.
 * next_deadline_ns may be NULL if the caller does not need it.
 */
sched_result_t strand_scheduler_advance(strand_scheduler_t *sched,
                                        uint64_t *next_deadline_ns);

/*
 * strand_scheduler_run - blocking worker-mode loop.
 * Calls strand_scheduler_advance in a loop.  Blocks in poll on the wakeup
 * fd when idle, using the next timer deadline as the timeout.  Returns
 * only after strand_scheduler_stop is called and the stop flag observed.
 * See ARCHITECTURE.md §4.2.
 */
void strand_scheduler_run(strand_scheduler_t *sched);

/*
 * strand_scheduler_stop - signal worker to stop.
 * Sets the atomic stop flag AND writes one byte to the wakeup fd so that
 * a blocked epoll_wait/kevent in strand_scheduler_run is interrupted.
 * Both steps are required.  Safe to call from any thread.
 * See ARCHITECTURE.md §4.2.
 */
void strand_scheduler_stop(strand_scheduler_t *sched);

/*
 * strand_scheduler_next_deadline - next pending timer deadline.
 * Returns a CLOCK_MONOTONIC nanosecond timestamp, or UINT64_MAX if none.
 * Used by the host loop to compute its own epoll_wait/kevent timeout.
 * See ARCHITECTURE.md §4.2.
 */
uint64_t strand_scheduler_next_deadline(const strand_scheduler_t *sched);

/*
 * strand_scheduler_get_fd - fd the host loop must monitor.
 *
 * Returns the scheduler's internal poller fd (epoll on Linux, kqueue on
 * OpenBSD). This fd becomes readable when scheduler-managed activity is
 * pending (I/O readiness, wakeup control signals, etc.).
 *
 * The host loop registers this fd with its own epoll/kqueue instance and
 * calls strand_scheduler_advance when it fires.
 * See ARCHITECTURE.md §4.2, §4.3, §5.
 */
int strand_scheduler_get_fd(const strand_scheduler_t *sched);

/*
 * strand_fiber_spawn - spawn a new fiber on sched.
 *
 * In Phase 3 (single-worker), must be called from inside a running fiber
 * (sched->current_fiber != NULL).  Host-thread spawning is added in
 * Phase 5 (Task 5.4).  Returns STRAND_ERR_WRONGCTX if called from the
 * host thread.
 *
 * fn       - fiber entry function; called as fn(arg)
 * arg      - argument passed to fn; ownership is the caller's
 * stack_sz - usable stack size in bytes; 0 uses STRAND_DEFAULT_STACK_SIZE
 * out      - if non-NULL, receives the ABA-safe handle on success
 *
 * Returns STRAND_OK on success.
 * Returns STRAND_ERR_SHUTDOWN if the scheduler has been stopped.
 * Returns STRAND_ERR_WRONGCTX if called from the host thread.
 * Returns STRAND_ERR_NOMEM on allocation failure.
 * See ARCHITECTURE.md §4.5 and §4.6.
 */
int strand_fiber_spawn(strand_scheduler_t *sched, strand_fiber_fn_t fn,
                       void *arg, size_t stack_sz, strand_fiber_handle_t *out);

/*
 * strand_fiber_yield - voluntarily yield the current fiber.
 *
 * Transitions the calling fiber from FIBER_RUNNING to FIBER_RUNNABLE,
 * appends it to the tail of the run queue, then switches back to the
 * scheduler.  The fiber will be resumed on the next advance pass when the
 * scheduler picks it from the run queue.
 *
 * Must be called from inside a running fiber (sched->current_fiber != NULL).
 * Calling from the host thread is a debug assertion failure.
 *
 * This is both a yield point and a cancellation point: after resuming, a
 * fiber should check for cancellation if needed.
 *
 * See ARCHITECTURE.md §4.5 (FIBER_RUNNING -> FIBER_RUNNABLE transition) and
 * §11.2 (explicit yield contracts).
 */
void strand_fiber_yield(strand_scheduler_t *sched);

/*
 * strand_fiber_sleep_until - park the current fiber until a deadline.
 *
 * Transitions the calling fiber from FIBER_RUNNING to FIBER_PARKED_TIMER,
 * inserts it into the per-scheduler timer min-heap keyed on deadline_ns,
 * then switches back to the scheduler.  The fiber is resumed when the
 * scheduler's Step 2 (timer expiry) pops it from the heap after the
 * deadline passes.
 *
 * deadline_ns - absolute CLOCK_MONOTONIC deadline in nanoseconds.  Use
 *               strand_scheduler_next_deadline to convert relative durations.
 *               A deadline already in the past causes the fiber to be queued
 *               for immediate resumption on the next advance call.
 *
 * Returns STRAND_OK       if the deadline expired normally.
 * Returns STRAND_CANCELLED if strand_fiber_cancel was called on this fiber
 *                           while it was parked (cancel_pending flag set).
 *
 * Must be called from inside a running fiber (sched->current_fiber != NULL).
 * Calling from the host thread is a debug assertion failure.
 *
 * See ARCHITECTURE.md §4.5 (FIBER_RUNNING -> FIBER_PARKED_TIMER transition)
 * and §8.1 (cancellation of FIBER_PARKED_TIMER).
 */
int strand_fiber_sleep_until(strand_scheduler_t *sched, uint64_t deadline_ns);

/*
 * strand_fiber_cancel - cancel a fiber identified by an ABA-safe handle.
 *
 * Validates the handle (null + generation check).  Then acts based on the
 * fiber's current state:
 *
 *   FIBER_PARKED_TIMER   - remove from the timer heap, set cancel_pending,
 *                          transition to FIBER_RUNNABLE, push to run queue.
 *                          The fiber resumes at the strand_fiber_sleep_until
 *                          call site and receives STRAND_CANCELLED.
 *   FIBER_RUNNABLE       - set cancel_pending (FIBER_CANCELLATION_PENDING).
 *   FIBER_RUNNING        - set cancel_pending (FIBER_CANCELLATION_PENDING).
 *   FIBER_PARKED_IO_READ/WRITE
 *                        - cancel waiter registration, transition to
 *                          FIBER_RUNNABLE, wake with STRAND_CANCELLED.
 *   FIBER_FINISHED       - no-op; returns STRAND_OK.
 *   Stale handle         - no-op; returns STRAND_HANDLE_STALE.
 *   Invalid handle       - returns STRAND_HANDLE_INVALID.
 *
 *   FIBER_PARKED_OFFLOAD, FIBER_PARKED_CHANNEL:
 *     placeholder - handled in later phases.
 *
 * Same-worker calls apply immediately. Cross-worker calls are enqueue-only:
 * the cancel request is queued to the target scheduler and processed in
 * Step 1 of strand_scheduler_advance on the owning worker.
 *
 * See ARCHITECTURE.md §8.1 for the full cancellation state table and
 * cross-worker semantics.
 */
int strand_fiber_cancel(strand_fiber_handle_t handle);

/*
 * strand_fiber_local_set - store a fiber-local pointer with an optional
 * destructor.
 *
 * Sets current_fiber->local_ptr = ptr and current_fiber->local_dtor = dtor.
 * Overwrites any previously stored values.
 *
 * The destructor (if non-NULL) is called with the stored pointer by the
 * scheduler when the fiber completes.  It is called exactly once, on the
 * scheduler's own stack after the fiber has switched back.  If local_ptr is
 * NULL when the fiber completes the destructor is still called with NULL.
 *
 * Must be called from inside a running fiber (sched->current_fiber != NULL).
 * Debug builds assert this; release builds silently do nothing if called from
 * the host thread.
 *
 * See ARCHITECTURE.md §4.7.
 */
void strand_fiber_local_set(strand_scheduler_t *sched, void *ptr,
                             strand_destructor_t dtor);

/*
 * strand_fiber_local_get - retrieve the fiber-local pointer.
 *
 * Returns current_fiber->local_ptr.
 *
 * Must be called from inside a running fiber.  Returns NULL if called from
 * the host thread (debug builds assert).
 *
 * See ARCHITECTURE.md §4.7.
 */
void *strand_fiber_local_get(strand_scheduler_t *sched);

/* Function declarations added in later phases. */

/*
 * strand_runtime_config_t - runtime creation parameters.
 * Pass NULL to strand_runtime_init to use all defaults.
 *
 * max_workers - maximum number of workers that may be registered with this
 *               runtime.  0 uses STRAND_DEFAULT_MAX_WORKERS.
 */
typedef struct strand_runtime_config {
	size_t max_workers;
} strand_runtime_config_t;

#define STRAND_DEFAULT_MAX_WORKERS ((size_t)64)

/*
 * strand_worker_config_t - per-worker creation parameters.
 * Pass NULL to strand_worker_start to use all defaults.
 *
 * sched_cfg     - scheduler parameters (budget, inject_cap, caches).
 *                 NULL means strand_scheduler_create defaults.
 * cpu_affinity  - Linux: pin worker to this CPU index via
 *                 pthread_setaffinity_np.  -1 = no affinity.
 *                 OpenBSD: silently ignored (no affinity API available).
 */
typedef struct strand_worker_config {
	const strand_sched_config_t *sched_cfg;
	int                          cpu_affinity;
} strand_worker_config_t;

/*
 * strand_runtime_init - allocate and initialise a multi-worker runtime.
 * cfg may be NULL for defaults.
 * Returns NULL on allocation failure.
 * See ARCHITECTURE.md §6.1, §6.2.
 */
strand_runtime_t *strand_runtime_init(const strand_runtime_config_t *cfg);

/*
 * strand_runtime_destroy - stop all workers, join all threads, free memory.
 *
 * Calls strand_scheduler_stop on every registered worker, joins each worker
 * thread, destroys each scheduler, then frees the runtime.
 * Safe to call even if some workers were never started or already stopped.
 * Must be called from the host thread only.
 * See ARCHITECTURE.md §6.2 shutdown sequence.
 */
void strand_runtime_destroy(strand_runtime_t *rt);

/*
 * strand_worker_start - create and register a new worker thread.
 *
 * Allocates a strand_worker_t, creates a scheduler, spawns a pthread that
 * calls strand_scheduler_run, and registers the worker with the runtime.
 * The worker is immediately eligible for round-robin fiber assignment.
 *
 * cfg may be NULL for defaults.
 * Returns NULL on allocation or thread creation failure.
 * Returns NULL if the runtime's worker_count has reached max_workers or if
 * shutdown has begun.
 * See ARCHITECTURE.md §6.1, §6.2.
 */
strand_worker_t *strand_worker_start(strand_runtime_t *rt,
    const strand_worker_config_t *cfg);

/*
 * strand_worker_stop - signal a worker to stop.
 * Calls strand_scheduler_stop on the worker's scheduler.
 * Safe to call from any thread.
 * See ARCHITECTURE.md §6.2.
 */
void strand_worker_stop(strand_worker_t *w);

/*
 * strand_worker_join - wait for a worker thread to exit.
 * Must be called after strand_worker_stop.
 * Blocks until the worker's pthread_t exits.
 * Must be called from the host thread only.
 */
void strand_worker_join(strand_worker_t *w);

/*
 * strand_runtime_spawn - spawn a fiber from the host thread.
 *
 * Selects a target worker via round-robin (worker == NULL) or uses the
 * explicitly supplied worker.  Allocates the fiber on the host thread and
 * injects it to the target worker's inject queue.  The fiber is made
 * runnable on the target worker's next strand_scheduler_advance call.
 *
 * worker   - explicit target worker; NULL for automatic round-robin.
 * fn, arg  - fiber entry function and argument.
 * stack_sz - usable stack bytes; 0 uses STRAND_DEFAULT_STACK_SIZE.
 * out      - if non-NULL, receives the ABA-safe handle on success.
 *
 * Returns STRAND_OK           on success.
 * Returns STRAND_ERR_SHUTDOWN if the runtime's shutdown_flag is set.
 * Returns STRAND_ERR_NO_WORKERS if no workers are registered.
 * Returns STRAND_ERR_NOMEM    on allocation failure.
 * See ARCHITECTURE.md §6.4.
 */
int strand_runtime_spawn(strand_runtime_t *rt, strand_fiber_fn_t fn,
    void *arg, size_t stack_sz, strand_worker_t *worker,
    strand_fiber_handle_t *out);

/*
 * strand_fiber_wait_readable - park the current fiber until fd is readable.
 *
 * Registers fd with the internal epoll/kqueue instance and transitions the
 * calling fiber from FIBER_RUNNING to FIBER_PARKED_IO_READ.  The fiber
 * resumes when the fd becomes readable, is cancelled, or the fd enters an
 * error/hangup state.
 *
 * fd must have O_NONBLOCK set (debug builds assert; release builds do not
 * check - undefined behaviour otherwise).  No other fiber must be parked
 * for reading on the same fd.
 *
 * Returns STRAND_OK          - fd is ready for reading.
 * Returns STRAND_CANCELLED   - strand_fiber_cancel was called on this fiber.
 * Returns STRAND_ERR_IO      - fd entered an error or hangup state.
 * Returns STRAND_ERR_IO_CONFLICT - a read waiter is already registered on fd.
 * Returns STRAND_ERR_NOMEM   - fd table allocation failed.
 *
 * Must be called from inside a running fiber.  Debug builds assert this.
 * See ARCHITECTURE.md §5.
 */
int strand_fiber_wait_readable(strand_scheduler_t *sched, int fd);

/*
 * strand_fiber_wait_writable - park the current fiber until fd is writable.
 *
 * Symmetric to strand_fiber_wait_readable for the write direction.
 * Transitions to FIBER_PARKED_IO_WRITE.  Returns same result codes.
 *
 * Returns STRAND_ERR_IO_CONFLICT if a write waiter is already registered on fd.
 *
 * See ARCHITECTURE.md §5.
 */
int strand_fiber_wait_writable(strand_scheduler_t *sched, int fd);

#endif /* STRAND_H */
