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

/* Function declarations added in later phases. */

#endif /* STRAND_H */
