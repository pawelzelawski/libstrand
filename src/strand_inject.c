/*
 * strand_inject.c - bounded MPSC inject ring buffer (Task 5.1).
 *
 * Algorithm: Dmitry Vyukov's MPMC queue adapted for single-consumer use.
 * Each slot carries an atomic sequence number that acts as the exclusive
 * ownership token passed between producers and the consumer.
 *
 * Slot sequence lifecycle (capacity C, slot index i, ring position pos):
 *   Initial:          slot[i].sequence = i
 *   Producer writes:  slot[pos & mask].sequence = pos + 1  (release)
 *   Consumer reads:   slot[pos & mask].sequence checked == pos + 1 (acquire)
 *   Consumer frees:   slot[pos & mask].sequence = pos + C  (release)
 *
 * Producer protocol (inject_queue_push_release):
 *   1. Atomically claim a ring position:
 *        pos = atomic_fetch_add(&q->head, 1, seq_cst)
 *   2. Compute slot = &q->slots[pos & mask].
 *   3. Spin until slot->sequence == pos (slot freed by consumer).
 *      Uses exponential backoff.  This is the "queue full" wait path.
 *   4. Write item into slot->item.
 *   5. ATOMIC: store slot->sequence = pos + 1, release - publishes item
 *      to the consumer.  Pairs with the acquire in pop.
 *
 * Consumer protocol (inject_queue_pop_acquire):
 *   1. Compute slot = &q->slots[q->tail & mask].
 *   2. ATOMIC: load slot->sequence, acquire.
 *   3. If sequence == q->tail + 1: item is ready.
 *      Read item.  ATOMIC: store slot->sequence = q->tail + capacity,
 *      release - frees the slot for a future producer.
 *      Advance q->tail.  Return 1.
 *   4. If sequence != q->tail + 1: queue is empty.  Return 0.
 *
 * Memory ordering:
 *   - Push: release on sequence store (step 5 above).
 *   - Pop:  acquire on sequence load  (step 2 above).
 *   This establishes the happens-before chain required for offload result
 *   visibility per ARCHITECTURE.md §6.7.
 *   See CODING_STANDARDS.md §4.2 for the documented exception rationale.
 *
 * Overflow behaviour:
 *   On full queue (step 3): spin with exponential backoff via sched_yield
 *   and nanosleep.  Silent drops are never permitted.  See
 *   ARCHITECTURE.md §6.3.
 *
 * Wakeup signal:
 *   The queue primitive only enqueues/dequeues items.  Wakeup writes are
 *   performed by caller paths after successful enqueue (runtime spawn,
 *   cross-worker cancel, offload completion) so a blocked epoll_wait/kevent
 *   is interrupted and Step 1 drains promptly.
 *   See ARCHITECTURE.md §4.2 and §6.3.
 */

#include "strand_inject.h"
#include "strand_sched.h"
#include "strand_fiber.h"
#include "strand_offload.h"

#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef STRAND_LINUX
#include <stdint.h>
#endif

/* -------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------
 */

/*
 * next_pow2 -- return the smallest power of 2 >= n.
 * Returns 1 for n == 0.
 */
static size_t
next_pow2(size_t n)
{
	size_t p;

	if (n == 0)
		return (1);
	p = 1;
	while (p < n)
		p <<= 1;
	return (p);
}

/*
 * backoff_spin -- yield CPU for an exponentially increasing delay.
 * iteration 0..7: sched_yield (cooperative)
 * iteration 8+:   nanosleep starting at 1ms, doubling, capped at 16ms.
 *
 * Called in the producer's "slot not yet freed" spin loop.
 */
static void
backoff_spin(unsigned int iteration)
{
	struct timespec ts;
	unsigned long   ns;

	if (iteration < 8) {
		sched_yield();
		return;
	}
	/* 1ms << (iteration - 8), capped at 16ms */
	ns = 1000000UL << (iteration - 8);
	if (ns > 16000000UL)
		ns = 16000000UL;
	ts.tv_sec  = 0;
	ts.tv_nsec = (long)ns;
	nanosleep(&ts, NULL);
}

/* -------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------
 */

int
inject_queue_init(strand_inject_queue_t *q, size_t capacity)
{
	size_t i;

	if (capacity == 0)
		capacity = STRAND_DEFAULT_INJECT_CAP;
	capacity = next_pow2(capacity);

	q->slots = malloc(capacity * sizeof(*q->slots));
	if (q->slots == NULL)
		return (-1);

	for (i = 0; i < capacity; i++)
		atomic_init(&q->slots[i].sequence, i);

	q->capacity = capacity;
	q->mask     = capacity - 1;
	atomic_init(&q->head, (size_t)0);
	q->tail     = 0;
	return (0);
}

void
inject_queue_destroy(strand_inject_queue_t *q)
{
	free(q->slots);
	q->slots    = NULL;
	q->capacity = 0;
	q->mask     = 0;
}

void
inject_queue_push_release(strand_inject_queue_t *q,
    const inject_item_t *item)
{
	size_t          pos;
	inject_slot_t  *slot;
	unsigned int    backoff;
	size_t          seq;

	/*
	 * Claim an exclusive ring position.  Multiple producers may call
	 * fetch_add concurrently; each gets a unique pos.
	 */
	pos  = atomic_fetch_add_explicit(&q->head, (size_t)1,
	    memory_order_seq_cst);
	slot = &q->slots[pos & q->mask];

	/*
	 * Spin until the consumer has freed this slot (sequence == pos).
	 * Initial state: slot[i].sequence == i, so the first producer at
	 * pos == i sees the slot immediately.  On subsequent laps the
	 * consumer sets sequence = pos (== old_tail + capacity).
	 *
	 * ATOMIC: acquire on load - ensures we do not observe a stale
	 * sequence from a previous producer–consumer cycle.
	 */
	backoff = 0;
	for (;;) {
		seq = atomic_load_explicit(&slot->sequence,
		    memory_order_acquire);
		if (seq == pos)
			break;
		backoff_spin(backoff++);
	}

	/* Write item data into the exclusively-owned slot. */
	slot->item = *item;

	/*
	 * ATOMIC: release on sequence store - publishes the item write to
	 * the consumer.  Pairs with the acquire in inject_queue_pop_acquire.
	 * See CODING_STANDARDS.md §4.2 and ARCHITECTURE.md §6.3.
	 */
	atomic_store_explicit(&slot->sequence, pos + 1,
	    memory_order_release);
}

int
inject_queue_pop_acquire(strand_inject_queue_t *q, inject_item_t *item)
{
	size_t          pos;
	inject_slot_t  *slot;
	size_t          seq;

	pos  = q->tail;
	slot = &q->slots[pos & q->mask];

	/*
	 * ATOMIC: acquire on sequence load - pairs with the release in
	 * inject_queue_push_release.  Establishes happens-before with the
	 * item write, making slot->item visible to us.
	 * See CODING_STANDARDS.md §4.2 and ARCHITECTURE.md §6.3, §6.7.
	 */
	seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);

	if (seq != pos + 1)
		return (0); /* queue empty */

	/* Read item while slot is exclusively owned by the consumer. */
	*item = slot->item;

	/*
	 * ATOMIC: release on sequence store - frees this slot for a future
	 * producer that has claimed pos + capacity.
	 */
	atomic_store_explicit(&slot->sequence, pos + q->capacity,
	    memory_order_release);
	q->tail++;
	return (1);
}

void
inject_queue_drain(strand_scheduler_t *sched)
{
	inject_item_t item;

	while (inject_queue_pop_acquire(&sched->inject_queue, &item)) {
		switch (item.type) {
		case INJECT_CANCEL:
			/*
			 * Deliver the cancel on the owning worker.  The fiber
			 * is pinned to this worker so strand_fiber_cancel is
			 * safe to call directly here (same-worker path).
			 */
			(void)strand_fiber_cancel(item.u.cancel_handle);
			break;
		case INJECT_SPAWN:
			/*
			 * Fiber was fully initialised by the host thread via
			 * strand_runtime_spawn.  home_sched is already set.
			 * Transition to RUNNABLE and push to run queue.
			 * See ARCHITECTURE.md §6.4 and DEVELOPMENT.md Task 5.4.
			 */
                        atomic_store(&item.u.fiber->state, FIBER_RUNNABLE);
                        run_queue_push(sched, item.u.fiber);
                        break;
                case INJECT_OFFLOAD_COMPLETE: {
                        /*
                         * Offload thread won RESULT_CLAIMED CAS and injected
                         * this completion.  result_slot was written before the
                         * inject enqueue (ARCHITECTURE.md 6.7).
                         *
                         * Validate the handle: a cancel that arrived after the
                         * inject was enqueued may have already moved the fiber
                         * to RUNNABLE.  If the handle matches, the fiber is
                         * still parked (cancel did not win) - make it runnable.
                         * If stale, release the fiber-side refcount that was
                         * held for the now-recycled fiber.
                         */
                        strand_offload_item_t *oi = item.u.offload;
                        if (fiber_handle_validate(oi->home_fiber) == STRAND_OK) {
                                strand_fiber_t *of = oi->home_fiber.ptr;
                                atomic_store(&of->state, FIBER_RUNNABLE);
                                run_queue_push(sched, of);
                        } else {
                                offload_item_release(oi);
                        }
                        break;
                }
		default:
			/* Unknown type - silently ignore (forward compat). */
			break;
		}
	}
}

void
inject_queue_discard_all(strand_inject_queue_t *q)
{
	inject_item_t item;

	while (inject_queue_pop_acquire(q, &item)) {
		switch (item.type) {
		case INJECT_SPAWN:
			/*
			 * The fiber was allocated by strand_runtime_spawn but
			 * never delivered to a run queue.  Free it here so that
			 * scheduler teardown does not leak it when stop_flag
			 * races ahead of the worker processing the item.
			 */
			strand_fiber_tsan_destroy(item.u.fiber);
			free(item.u.fiber);
			break;
		case INJECT_OFFLOAD_COMPLETE:
			/*
			 * The offload item holds one refcount on behalf of the
			 * inject path.  Drop it now; the fiber side has already
			 * released its own refcount (or will never run).
			 */
			offload_item_release(item.u.offload);
			break;
		case INJECT_CANCEL:
		default:
			/* No heap resources to free. */
			break;
		}
	}
}
