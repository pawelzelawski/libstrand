#ifndef STRAND_H
#define STRAND_H

/*
 * strand.h — libstrand public API
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
 * Opaque types — embedders use pointers to these.
 * Full definitions live in src/strand_internal.h (internal only).
 */
typedef struct strand_scheduler strand_scheduler_t;
typedef struct strand_runtime strand_runtime_t;
typedef struct strand_worker strand_worker_t;
typedef struct strand_poller strand_poller_t;
typedef struct strand_scope strand_scope_t;
typedef struct strand_fiber strand_fiber_t;

/*
 * strand_fiber_handle_t — ABA-safe fiber reference.
 * Passed and returned by value. 16 bytes: pointer + generation counter.
 * See ARCHITECTURE.md §4.6.
 */
typedef struct strand_fiber_handle {
	strand_fiber_t *ptr;
	uint64_t generation;
} strand_fiber_handle_t;

/*
 * strand_fiber_fn_t — fiber entry function.
 * Called with the argument passed to strand_fiber_spawn().
 */
typedef void (*strand_fiber_fn_t)(void *arg);

/*
 * strand_destructor_t — fiber-local storage destructor.
 * Called with the stored pointer when the fiber finishes.
 */
typedef void (*strand_destructor_t)(void *ptr);

/*
 * Return codes.
 * STRAND_OK             — success; no error.
 * STRAND_HANDLE_INVALID — handle.ptr is NULL; not a valid handle.
 * STRAND_HANDLE_STALE   — handle ptr exists but generation differs;
 *                         fiber has been recycled since handle was issued.
 * See ARCHITECTURE.md §4.6 for ABA-protection semantics.
 */
#define STRAND_OK 0
#define STRAND_HANDLE_INVALID (-1)
#define STRAND_HANDLE_STALE (-2)
#define STRAND_ERR_SHUTDOWN (-3) /* scheduler stopped; spawn rejected */
#define STRAND_ERR_WRONGCTX (-4) /* called from wrong context (host vs fiber)  \
	                          */
#define STRAND_ERR_NOMEM (-5)    /* allocation failure */
#define STRAND_CANCELLED (-6)    /* operation cancelled by strand_fiber_cancel */
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
 * strand_sched_config_t — scheduler creation parameters.
 * Pass zero for any field to use the corresponding default.
 *
 * budget      — max fibers to run per strand_scheduler_advance call.
 * inject_cap  — inject queue capacity (Phase 5; ignored in Phase 3).
 * cache_cap   — stack cache hard cap (stacks per worker).
 * idle_floor  — stack cache idle reclamation floor.
 * See ARCHITECTURE.md §4.4, §13.
 */
typedef struct strand_sched_config {
	size_t budget;
	size_t inject_cap;
	size_t cache_cap;
	size_t idle_floor;
} strand_sched_config_t;

/*
 * strand_scheduler_create — allocate and initialise a scheduler.
 * cfg may be NULL to use all defaults.
 * Returns NULL on allocation failure.
 */
strand_scheduler_t *strand_scheduler_create(const strand_sched_config_t *cfg);

/*
 * strand_scheduler_destroy — tear down and free a scheduler.
 * All fibers must be finished before calling this.
 */
void strand_scheduler_destroy(strand_scheduler_t *sched);

/*
 * sched_result_t — return value of strand_scheduler_advance.
 *
 * SCHED_PROGRESS — at least one timer fired or fiber ran this call.
 * SCHED_IDLE     — nothing ran; next_deadline_ns holds the next timer
 *                  deadline as a nanosecond timestamp (UINT64_MAX = none).
 * See ARCHITECTURE.md §4.2.
 */
typedef enum { SCHED_PROGRESS = 0, SCHED_IDLE = 1 } sched_result_t;

/*
 * strand_scheduler_advance — perform one nonblocking scheduler pass.
 * Steps (per ARCHITECTURE.md §4.2):
 *   1. Drain inject queue (stub in Phase 3).
 *   2. Expire timers whose deadline <= now_ns; move to run queue.
 *   3. Drain wakeup fd — bytes are control signals, not fiber events.
 *   4. Poll I/O with zero timeout (stub in Phase 3).
 *   5. Run up to sched->budget fibers from the run queue.
 * Returns SCHED_PROGRESS if any fibers ran or any timer fired.
 * Returns SCHED_IDLE with *next_deadline_ns set otherwise.
 * next_deadline_ns may be NULL if the caller does not need it.
 */
sched_result_t strand_scheduler_advance(strand_scheduler_t *sched,
                                        uint64_t *next_deadline_ns);

/*
 * strand_scheduler_run — blocking worker-mode loop.
 * Calls strand_scheduler_advance in a loop.  Blocks in poll on the wakeup
 * fd when idle, using the next timer deadline as the timeout.  Returns
 * only after strand_scheduler_stop is called and the stop flag observed.
 * See ARCHITECTURE.md §4.2.
 */
void strand_scheduler_run(strand_scheduler_t *sched);

/*
 * strand_scheduler_stop — signal worker to stop.
 * Sets the atomic stop flag AND writes one byte to the wakeup fd so that
 * a blocked epoll_wait/kevent in strand_scheduler_run is interrupted.
 * Both steps are required.  Safe to call from any thread.
 * See ARCHITECTURE.md §4.2.
 */
void strand_scheduler_stop(strand_scheduler_t *sched);

/*
 * strand_scheduler_next_deadline — next pending timer deadline.
 * Returns a CLOCK_MONOTONIC nanosecond timestamp, or UINT64_MAX if none.
 * Used by the host loop to compute its own epoll_wait/kevent timeout.
 * See ARCHITECTURE.md §4.2.
 */
uint64_t strand_scheduler_next_deadline(const strand_scheduler_t *sched);

/*
 * strand_scheduler_get_fd — fd the host loop must monitor.
 * Returns the eventfd (Linux) or the read end of the wakeup pipe (OpenBSD).
 * The host loop registers this fd with its own epoll/kqueue instance and
 * calls strand_scheduler_advance when it fires.
 * See ARCHITECTURE.md §4.2 and §4.3.
 */
int strand_scheduler_get_fd(const strand_scheduler_t *sched);

/*
 * strand_fiber_spawn — spawn a new fiber on sched.
 *
 * In Phase 3 (single-worker), must be called from inside a running fiber
 * (sched->current_fiber != NULL).  Host-thread spawning is added in
 * Phase 5 (Task 5.4).  Returns STRAND_ERR_WRONGCTX if called from the
 * host thread.
 *
 * fn       — fiber entry function; called as fn(arg)
 * arg      — argument passed to fn; ownership is the caller's
 * stack_sz — usable stack size in bytes; 0 uses STRAND_DEFAULT_STACK_SIZE
 * out      — if non-NULL, receives the ABA-safe handle on success
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
 * strand_fiber_yield — voluntarily yield the current fiber.
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
 * strand_fiber_sleep_until — park the current fiber until a deadline.
 *
 * Transitions the calling fiber from FIBER_RUNNING to FIBER_PARKED_TIMER,
 * inserts it into the per-scheduler timer min-heap keyed on deadline_ns,
 * then switches back to the scheduler.  The fiber is resumed when the
 * scheduler's Step 2 (timer expiry) pops it from the heap after the
 * deadline passes.
 *
 * deadline_ns — absolute CLOCK_MONOTONIC deadline in nanoseconds.  Use
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

/* Function declarations added in later phases. */

#endif /* STRAND_H */
