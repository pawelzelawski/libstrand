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

/* strand_fiber_t and run queue operations defined in Phase 3 (Task 3.1, 3.2).
 */

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

#endif /* STRAND_FIBER_H */
