#ifndef STRAND_SCHED_H
#define STRAND_SCHED_H

/*
 * strand_sched.h — scheduler internal interface.
 * strand_scheduler_t forward declaration, timer heap types.
 * See ARCHITECTURE.md §4.2.
 */

#include "../include/strand.h"
#include "strand_internal.h"

/*
 * run_queue_push — append fiber to the tail of the run queue.
 * f->next is set to NULL before linking; caller must not use f->next
 * for other purposes while the fiber is queued.
 * Precondition: f != NULL, f is not already on any run queue.
 */
void run_queue_push(strand_scheduler_t *sched, strand_fiber_t *f);

/*
 * run_queue_pop — remove and return the head fiber from the run queue.
 * Returns NULL if the queue is empty.
 * Postcondition: f->next == NULL.
 */
strand_fiber_t *run_queue_pop(strand_scheduler_t *sched);

#endif /* STRAND_SCHED_H */
