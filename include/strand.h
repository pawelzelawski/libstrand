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
/* Function declarations added in later phases. */

#endif /* STRAND_H */
