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

/*
 * timer_heap_push — insert (deadline_ns, fiber) into the min-heap.
 * Grows the heap by doubling if at capacity.
 * Returns 0 on success, -1 on allocation failure.
 */
int timer_heap_push(strand_scheduler_t *sched, uint64_t deadline_ns,
                    strand_fiber_t *f);

/*
 * timer_heap_pop_min — remove and return the entry with the smallest
 * deadline_ns.  Caller must check timer_heap_len > 0 first.
 */
strand_timer_entry_t timer_heap_pop_min(strand_scheduler_t *sched);

/*
 * timer_heap_peek_deadline — return the smallest deadline_ns without
 * removing it, or UINT64_MAX if the heap is empty.
 */
uint64_t timer_heap_peek_deadline(const strand_scheduler_t *sched);

/*
 * timer_heap_remove — remove the entry for fiber f from the heap.
 *
 * Searches the heap by fiber pointer (linear scan — heap is keyed on
 * deadline, not on fiber address).  If found, replaces the entry with
 * the last element and restores heap order via sift-up then sift-down.
 * Returns 1 if the entry was found and removed, 0 if not found.
 * Caller must ensure f is actually parked in FIBER_PARKED_TIMER before
 * calling.
 */
int timer_heap_remove(strand_scheduler_t *sched, strand_fiber_t *f);

/*
 * strand_sched_current_tls — thread-local pointer to the scheduler owning
 * this worker thread.  Set by strand_scheduler_run at thread start.
 * NULL on non-worker (host) threads.
 *
 * Accessible to internal code and tests that include this header.
 * Not part of the public strand.h API.
 */
extern _Thread_local strand_scheduler_t *strand_sched_current_tls;

#endif /* STRAND_SCHED_H */
