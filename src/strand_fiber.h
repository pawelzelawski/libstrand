#ifndef STRAND_FIBER_H
#define STRAND_FIBER_H

/*
 * strand_fiber.h — fiber descriptor, state machine, and run queue (internal).
 * See ARCHITECTURE.md §4.5 for the fiber state machine.
 * See ARCHITECTURE.md §4.6 for fiber handle ABA protection.
 */

#include <stddef.h>

#include "../include/strand.h"
#include "strand_internal.h"

/*
 * stack_alloc — allocate a fiber stack with a guard page.
 * Returns mmap base on success, NULL on failure.
 * *vg_id_out receives the Valgrind stack registration ID.
 * Usable stack top = (char *)base + page_size() + stack_size.
 */
void *stack_alloc(size_t stack_size, unsigned long *vg_id_out);

/*
 * stack_free — deregister from Valgrind and unmap.
 * base must be the mmap base returned by stack_alloc.
 */
void stack_free(void *base, size_t stack_size, unsigned long vg_id);

/* page_size — cached system page size. */
size_t page_size(void);

/*
 * TSan fiber lifecycle hooks.
 * In non-TSan builds these are cheap no-ops.
 */
void strand_fiber_tsan_init(strand_fiber_t *f);
void strand_fiber_tsan_destroy(strand_fiber_t *f);
void strand_fiber_tsan_bind_current(strand_fiber_t *f);

/*
 * fiber_alloc — allocate a fiber descriptor.
 *
 * If dead_pool is non-NULL and *dead_pool is non-NULL, pops one descriptor
 * from the dead pool, increments its generation counter (ABA protection),
 * zeros the descriptor, and returns it.
 * Otherwise allocates a fresh descriptor via malloc() with generation = 1.
 *
 * Caller must initialise all fields (context, stack, state, etc.) before
 * the fiber is made runnable.
 *
 * Returns NULL on allocation failure.
 * See ARCHITECTURE.md §4.6 for generation counter semantics.
 */
strand_fiber_t *fiber_alloc(strand_fiber_t **dead_pool);

/*
 * fiber_free — return a finished fiber descriptor to the dead pool.
 *
 * The fiber stack must already have been freed via stack_free() before
 * calling fiber_free(). Pushes the descriptor onto *dead_pool if
 * dead_pool is non-NULL; otherwise calls free() immediately.
 *
 * Generation is NOT incremented here — it is incremented by fiber_alloc
 * when the descriptor is next taken from the pool.
 * See ARCHITECTURE.md §4.6.
 */
void fiber_free(strand_fiber_t **dead_pool, strand_fiber_t *f);

/*
 * fiber_handle_validate — validate an ABA-safe fiber handle.
 *
 * Returns STRAND_OK           if the handle is live and matches current
 * generation. Returns STRAND_HANDLE_INVALID if handle.ptr is NULL. Returns
 * STRAND_HANDLE_STALE  if the descriptor has been recycled (generation
 * mismatch).
 *
 * Safe to call at any time: dead-pool descriptors are never freed, so
 * handle.ptr is always safe to dereference for the generation check.
 * See ARCHITECTURE.md §4.6.
 */
static inline int
fiber_handle_validate(strand_fiber_handle_t handle)
{
	if (handle.ptr == NULL)
		return (STRAND_HANDLE_INVALID);
	if (handle.ptr->generation != handle.generation)
		return (STRAND_HANDLE_STALE);
	return (STRAND_OK);
}

#endif /* STRAND_FIBER_H */
