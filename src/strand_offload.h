#ifndef STRAND_OFFLOAD_H
#define STRAND_OFFLOAD_H

/*
 * strand_offload.h - blocking syscall offload pool internal interface.
 * See ARCHITECTURE.md 6.5, 6.6, 6.7.
 *
 * Public API (strand_offload_pool_init, strand_offload_pool_destroy,
 * strand_fiber_offload) is declared in include/strand.h.  This header
 * exposes the internal struct layouts needed by strand_fiber.c (for
 * strand_fiber_cancel on FIBER_PARKED_OFFLOAD) and strand_inject.c
 * (for INJECT_OFFLOAD_COMPLETE drain).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

#include "../include/strand.h"
#include "strand_internal.h"

/*
 * offload_state_t - atomic state of one work item.
 *
 * OFFLOAD_PENDING        - fn not yet started or still running.
 * OFFLOAD_RESULT_CLAIMED - offload thread won the CAS; result_slot written.
 * OFFLOAD_CANCELLED      - strand_fiber_cancel won the CAS; result_slot not
 *                          written; fiber resumes with STRAND_CANCELLED.
 *
 * CONCURRENT: state is the synchronisation pivot between the offload thread
 * (writer) and the cancelling fiber (writer).  All transitions are CAS with
 * seq_cst to guarantee mutual exclusion.  See ARCHITECTURE.md 6.6.
 */
typedef enum {
        OFFLOAD_PENDING        = 0,
        OFFLOAD_RESULT_CLAIMED = 1,
        OFFLOAD_CANCELLED      = 2,
} offload_state_t;

/*
 * strand_offload_item_t - one unit of work submitted to the offload pool.
 *
 * Fields:
 *   fn          - blocking function to execute on the offload thread.
 *   arg         - opaque argument forwarded to fn; lifetime is caller's
 *                 responsibility (must outlive fn completion).
 *   result_slot - caller-owned storage for fn's result.  Written only if
 *                 the RESULT_CLAIMED CAS succeeds (ARCHITECTURE.md 6.6).
 *   home_fiber  - ABA-safe handle to the parked fiber; used by the offload
 *                 thread to inject completion back to the fiber's worker.
 *   home_sched  - scheduler that owns home_fiber; the inject target.
 *   state       - atomic: PENDING | RESULT_CLAIMED | CANCELLED.
 *                 CONCURRENT: see offload_state_t above.
 *   refcount    - atomic reference count; starts at 2 (fiber side + offload
 *                 thread side).  Work item freed when it reaches 0.
 *                 CONCURRENT: decremented independently by both sides.
 *   next        - intrusive link for the pool's pending work queue.
 */
typedef struct strand_offload_item {
        blocking_fn_t              fn;
        void                      *arg;
        void                      *result_slot;
        strand_fiber_handle_t      home_fiber;
        struct strand_scheduler   *home_sched;
        /*
         * CONCURRENT: state is written by both the offload thread and
         * strand_fiber_cancel; use seq_cst CAS exclusively.
         * See ARCHITECTURE.md 6.6.
         */
        _Atomic offload_state_t    state;
        /*
         * CONCURRENT: refcount is decremented by both the fiber side and the
         * offload thread side; use atomic_fetch_sub with acq_rel.
         */
        _Atomic int                refcount;
        struct strand_offload_item *next;
} strand_offload_item_t;

/*
 * strand_offload_pool_t - thread pool for blocking syscall offload.
 *
 * threads      - array of pthread_t worker threads.
 * thread_count - number of threads.
 * mutex        - protects head/tail/count and the shutdown flag.
 * cond         - signalled when a new item is enqueued or shutdown is set.
 * head/tail    - singly-linked FIFO work queue of strand_offload_item_t.
 * count        - number of items currently queued (not yet claimed by a thread).
 * in_flight    - number of items currently running on an offload thread.
 * capacity     - maximum number of outstanding items (queued + in-flight)
 *                (= thread_count * 2; each thread gets a full backlog slot).
 * shutdown     - set to 1 under mutex to ask threads to exit.
 *
 * Thread safety: all queue and shutdown fields are protected by mutex.
 * See ARCHITECTURE.md 6.5.
 */
struct strand_offload_pool {
        pthread_t             *threads;
        size_t                 thread_count;
        pthread_mutex_t        mutex;
        pthread_cond_t         cond;
        strand_offload_item_t *head;
        strand_offload_item_t *tail;
        size_t                 count;
        size_t                 in_flight;
        size_t                 capacity;
        int                    shutdown;
};

/*
 * offload_item_release - decrement refcount and free item when it hits zero.
 *
 * Called by both the fiber side (in strand_fiber_offload on return or on
 * successful CANCELLED CAS) and the offload thread side (after fn completes
 * and the injection or skip path finishes).
 * Uses acq_rel ordering to ensure result_slot writes are visible before free.
 */
void offload_item_release(strand_offload_item_t *item);

#endif /* STRAND_OFFLOAD_H */
