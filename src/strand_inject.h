#ifndef STRAND_INJECT_H
#define STRAND_INJECT_H

/*
 * strand_inject.h - bounded MPSC inject queue internal interface.
 *
 * One inject queue per worker.  Items are enqueued by any thread
 * (cross-worker cancel, offload completion, host-thread spawn) and
 * drained exclusively by the owning worker in Step 1 of
 * strand_scheduler_advance.
 *
 * See ARCHITECTURE.md §6.3, CODING_STANDARDS.md §4.2.
 */

#include "../include/strand.h"
#include "strand_internal.h"

/*
 * Default inject queue capacity when cfg->inject_cap is 0.
 * Must be a power of 2.  4 × STRAND_DEFAULT_SCHED_BUDGET is the
 * documented default.  See ARCHITECTURE.md §6.3.
 */
#define STRAND_DEFAULT_INJECT_CAP ((size_t)(4 * STRAND_DEFAULT_SCHED_BUDGET))

/*
 * inject_queue_init -- allocate and initialise the ring buffer slots.
 * capacity is rounded up to the next power of 2 if not already one.
 * Returns 0 on success, -1 on allocation failure.
 */
int inject_queue_init(strand_inject_queue_t *q, size_t capacity);

/*
 * inject_queue_destroy -- free ring buffer slots.
 * Must be called only after all producers have stopped.
 */
void inject_queue_destroy(strand_inject_queue_t *q);

/*
 * inject_queue_try_push_release -- enqueue one item with release ordering.
 *
 * ATOMIC: release semantics on the slot sequence store - publishes the
 * item write to the consumer.  Pairs with the acquire in
 * inject_queue_pop_acquire.  See ARCHITECTURE.md §6.3 and §6.7.
 *
 * Returns 0 on success, -1 if the queue is full or closed.  Producers must
 * release any resources associated with an item that was not enqueued.
 */
int inject_queue_try_push_release(strand_inject_queue_t *q,
    const inject_item_t *item);

/* Close the queue to new producers.  Existing published items remain valid. */
void inject_queue_close(strand_inject_queue_t *q);

/*
 * inject_queue_pop_acquire -- dequeue one item with acquire memory ordering.
 *
 * ATOMIC: acquire semantics on the slot sequence load - pairs with the
 * release in inject_queue_push_release, establishing happens-before with
 * the item write.  See ARCHITECTURE.md §6.3 and §6.7.
 *
 * Returns 1 and fills *item if an item was available.
 * Returns 0 if the queue is empty.
 * Called only from the owning worker thread.
 */
int inject_queue_pop_acquire(strand_inject_queue_t *q, inject_item_t *item);

/*
 * inject_queue_drain -- drain all queued items into the scheduler.
 * Called in Step 1 of strand_scheduler_advance.
 * For INJECT_CANCEL items: calls strand_fiber_cancel on the target handle.
 * Additional item types handled for each inject_type variant.
 */
void inject_queue_drain(strand_scheduler_t *sched);

/*
 * inject_queue_discard_all -- pop and discard every pending item.
 * Called during scheduler destroy when stale items are no longer relevant.
 */
void inject_queue_discard_all(strand_inject_queue_t *q);

#endif /* STRAND_INJECT_H */
