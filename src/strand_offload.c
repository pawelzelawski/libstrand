/*
 * strand_offload.c - blocking syscall offload pool.
 * See ARCHITECTURE.md 6.5, 6.6, 6.7.
 *
 * Tasks 5.6, 5.7, 5.8 - offload pool init/destroy, strand_fiber_offload,
 * and the RESULT_CLAIMED / CANCELLED CAS completion protocol.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "strand_offload.h"
#include "strand_inject.h"
#include "strand_sched.h"
#include "strand_context.h"

/* ---------------------------------------------------------------------------
 * offload_item_release - drop one reference; free when zero.
 *
 * ATOMIC: fetch_sub with acq_rel: the acquire side ensures that any writes
 * to result_slot performed before the sub are visible here before free().
 * See ARCHITECTURE.md 6.6 refcount ownership rule.
 * ---------------------------------------------------------------------------
 */
void
offload_item_release(strand_offload_item_t *item)
{
        int prev;

        prev = atomic_fetch_sub_explicit(&item->refcount, 1,
                                         memory_order_acq_rel);
        if (prev == 1)
                free(item);
}

/* ---------------------------------------------------------------------------
 * offload_thread_worker - body of each offload pool thread.
 *
 * Blocks on pool->cond until an item is enqueued or shutdown is requested.
 * For each item:
 *   1. Run fn(arg, result_slot).
 *   2. CAS(state, PENDING -> RESULT_CLAIMED) with seq_cst.
 *      ATOMIC: seq_cst CAS establishes happens-before with the fiber-cancel
 *      CAS (PENDING -> CANCELLED).  Exactly one side wins.
 *      See ARCHITECTURE.md 6.6, 6.7.
 *   3. If CAS succeeds: result already in result_slot; inject
 *      INJECT_OFFLOAD_COMPLETE to the fiber's home worker.
 *   4. If CAS fails (CANCELLED already won): skip result_slot; the fiber
 *      is already RUNNABLE on the home worker.
 *   5. Release the offload-side refcount.
 * ---------------------------------------------------------------------------
 */
static void *
offload_thread_worker(void *varg)
{
        strand_offload_pool_t  *pool = varg;
        strand_offload_item_t  *item;
        offload_state_t         expected;
        inject_item_t           inj;

        for (;;) {
                pthread_mutex_lock(&pool->mutex);
                while (pool->head == NULL && !pool->shutdown)
                        pthread_cond_wait(&pool->cond, &pool->mutex);

                if (pool->head == NULL && pool->shutdown) {
                        pthread_mutex_unlock(&pool->mutex);
                        break;
                }

                /* Dequeue one item and mark it in-flight. */
                item       = pool->head;
                pool->head = item->next;
                if (pool->head == NULL)
                        pool->tail = NULL;
                pool->count--;
                pool->in_flight++;
                pthread_mutex_unlock(&pool->mutex);

                item->next = NULL;

                /* Run the blocking function. */
                item->fn(item->arg, item->result_slot);

                /* Mark no longer in-flight under mutex. */
                pthread_mutex_lock(&pool->mutex);
                pool->in_flight--;
                pthread_mutex_unlock(&pool->mutex);

                /*
                 * ATOMIC: CAS(PENDING -> RESULT_CLAIMED) with seq_cst.
                 * This is the exclusive claim that guards result_slot writes.
                 * Release semantics publish the result_slot write to the fiber.
                 * If CANCELLED already won, the fiber has been woken with
                 * STRAND_CANCELLED - skip the inject entirely.
                 * See ARCHITECTURE.md 6.6 and 6.7.
                 */
                expected = OFFLOAD_PENDING;
                if (atomic_compare_exchange_strong_explicit(
                        &item->state,
                        &expected,
                        OFFLOAD_RESULT_CLAIMED,
                        memory_order_seq_cst,
                        memory_order_seq_cst)) {
                        /*
                         * RESULT_CLAIMED won.  Inject completion to the fiber's
                         * home worker.  The inject enqueue uses release semantics
                         * (inside inject_queue_push_release) which, combined with
                         * the acquire drain in inject_queue_drain, establishes the
                         * full ordering chain required by ARCHITECTURE.md 6.7:
                         *   fn writes result_slot -> CAS release -> inject release
                         *   -> drain acquire -> fiber reads result_slot.
                         */
                        inj.type     = INJECT_OFFLOAD_COMPLETE;
                        inj.u.offload = item;
                        inject_queue_push_release(&item->home_sched->inject_queue,
                                                  &inj);
                        /*
                         * Wake the home worker so it processes the inject
                         * promptly rather than waiting for the next poll timeout.
                         */
#ifdef STRAND_LINUX
                        {
                                uint64_t v = 1;
                                (void)write(item->home_sched->wakeup_fd,
                                            &v, sizeof(v));
                        }
#endif
#ifdef STRAND_OPENBSD
                        {
                                char v = 1;
                                (void)write(item->home_sched->wakeup_pipe[1],
                                            &v, sizeof(v));
                        }
#endif
                }
                /* else: CANCELLED won; result_slot not written; no inject. */

                /* Release offload-thread-side refcount. */
                offload_item_release(item);
        }

        return (NULL);
}

/* ---------------------------------------------------------------------------
 * strand_offload_pool_init - create the global offload thread pool.
 * See ARCHITECTURE.md 6.5 and DEVELOPMENT.md Task 5.6.
 * ---------------------------------------------------------------------------
 */
strand_offload_pool_t *
strand_offload_pool_init(size_t thread_count)
{
        strand_offload_pool_t *pool;
        size_t                 i;

        if (thread_count == 0)
                return (NULL);

        pool = calloc(1, sizeof(*pool));
        if (pool == NULL)
                return (NULL);

        pool->threads = calloc(thread_count, sizeof(pthread_t));
        if (pool->threads == NULL) {
                free(pool);
                return (NULL);
        }

        if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
                free(pool->threads);
                free(pool);
                return (NULL);
        }
        if (pthread_cond_init(&pool->cond, NULL) != 0) {
                pthread_mutex_destroy(&pool->mutex);
                free(pool->threads);
                free(pool);
                return (NULL);
        }

        pool->thread_count = thread_count;
        pool->capacity     = thread_count * 2; /* per ARCHITECTURE.md 6.5 */
        pool->shutdown     = 0;

        for (i = 0; i < thread_count; i++) {
                if (pthread_create(&pool->threads[i], NULL,
                                   offload_thread_worker, pool) != 0) {
                        /*
                         * Partial creation: signal threads already running to
                         * exit, join them, then tear down.
                         */
                        pthread_mutex_lock(&pool->mutex);
                        pool->shutdown = 1;
                        pthread_cond_broadcast(&pool->cond);
                        pthread_mutex_unlock(&pool->mutex);
                        while (i-- > 0)
                                pthread_join(pool->threads[i], NULL);
                        pthread_cond_destroy(&pool->cond);
                        pthread_mutex_destroy(&pool->mutex);
                        free(pool->threads);
                        free(pool);
                        return (NULL);
                }
        }

        return (pool);
}

/* ---------------------------------------------------------------------------
 * strand_offload_pool_destroy - shut down pool and join all threads.
 * See ARCHITECTURE.md 6.5 and DEVELOPMENT.md Task 5.6.
 * ---------------------------------------------------------------------------
 */
void
strand_offload_pool_destroy(strand_offload_pool_t *pool)
{
        size_t i;

        if (pool == NULL)
                return;

        pthread_mutex_lock(&pool->mutex);
        pool->shutdown = 1;
        pthread_cond_broadcast(&pool->cond);
        pthread_mutex_unlock(&pool->mutex);

        for (i = 0; i < pool->thread_count; i++)
                pthread_join(pool->threads[i], NULL);

        pthread_cond_destroy(&pool->cond);
        pthread_mutex_destroy(&pool->mutex);
        free(pool->threads);
        free(pool);
}

/* ---------------------------------------------------------------------------
 * strand_fiber_offload - park calling fiber and run fn on an offload thread.
 * See ARCHITECTURE.md 6.5, 6.6, 6.7 and DEVELOPMENT.md Tasks 5.7, 5.8.
 *
 * Returns:
 *   STRAND_OK              - fn completed; result written to result_slot.
 *   STRAND_CANCELLED       - cancel won the CAS; result_slot not written.
 *   STRAND_EAGAIN          - pool queue full; do NOT park; caller must yield.
 *   STRAND_ERR_NO_OFFLOAD_POOL - pool is NULL.
 *   STRAND_ERR_WRONGCTX    - not called from a running fiber.
 *   STRAND_ERR_NOMEM       - work item allocation failed.
 *
 * Mandatory retry pattern (ARCHITECTURE.md 6.5):
 *   while ((rc = strand_fiber_offload(sched, pool, fn, arg, &result)) ==
 *          STRAND_EAGAIN) {
 *       strand_fiber_yield(sched);   // MUST yield; spinning is a liveness violation
 *   }
 * ---------------------------------------------------------------------------
 */
int
strand_fiber_offload(strand_scheduler_t *sched,
                     strand_offload_pool_t *pool,
                     blocking_fn_t fn, void *arg, void *result_slot)
{
        strand_fiber_t        *f;
        strand_offload_item_t *item;
        int                    cancelled;

        if (pool == NULL)
                return (STRAND_ERR_NO_OFFLOAD_POOL);

        STRAND_DEBUG_ASSERT(sched != NULL);
        STRAND_DEBUG_ASSERT(sched->current_fiber != NULL);

        if (sched->current_fiber == NULL)
                return (STRAND_ERR_WRONGCTX);

        f = sched->current_fiber;

        /* Check pool capacity (queued + in-flight) before allocating. */
        pthread_mutex_lock(&pool->mutex);
        if (pool->count + pool->in_flight >= pool->capacity) {
                pthread_mutex_unlock(&pool->mutex);
                /*
                 * Pool full.  Return STRAND_EAGAIN without parking.
                 * Caller MUST call strand_fiber_yield before retrying to avoid
                 * spinning the worker thread (ARCHITECTURE.md 6.5).
                 */
                return (STRAND_EAGAIN);
        }
        pthread_mutex_unlock(&pool->mutex);

        /* Allocate work item with refcount = 2. */
        item = calloc(1, sizeof(*item));
        if (item == NULL)
                return (STRAND_ERR_NOMEM);

        item->fn          = fn;
        item->arg         = arg;
        item->result_slot = result_slot;
        item->home_fiber.ptr        = f;
        item->home_fiber.generation = f->generation;
        item->home_sched  = sched;
        atomic_init(&item->state,    OFFLOAD_PENDING);
        atomic_init(&item->refcount, 2);
        item->next        = NULL;

        /* Transition fiber RUNNING -> PARKED_OFFLOAD before enqueue.
         * Store the item pointer on the fiber so strand_fiber_cancel can
         * find it for the CANCELLED CAS without any pool data structure access.
         * Both writes happen on the worker thread before the item is visible
         * to any offload thread. */
        f->offload_item = item;
        atomic_store_explicit(&f->state, FIBER_PARKED_OFFLOAD,
                              memory_order_relaxed);

        /* Enqueue to pool work queue under mutex; signal one thread. */
        pthread_mutex_lock(&pool->mutex);
        if (pool->head == NULL) {
                pool->head = item;
                pool->tail = item;
        } else {
                pool->tail->next = item;
                pool->tail       = item;
        }
        pool->count++;
        pthread_cond_signal(&pool->cond);
        pthread_mutex_unlock(&pool->mutex);

        /* Park: switch back to the scheduler.  Resumes here when the
         * offload completion inject is drained (RESULT_CLAIMED path) or
         * when strand_fiber_cancel's CANCELLED CAS succeeds. */
        strand_context_switch(f, &sched->scheduler_ctx);

        /*
         * Resumed.  Check cancel_pending (set by the CANCELLED path in
         * strand_fiber_cancel when its CAS won) vs normal offload completion.
         * Clear the flag so subsequent park calls start clean.
         *
         * Note: if RESULT_CLAIMED won, cancel_pending is 0 and we return OK.
         * If CANCELLED won, cancel_pending is 1 and we return STRAND_CANCELLED.
         * See ARCHITECTURE.md 6.6.
         */
        cancelled = atomic_exchange_explicit(&f->cancel_pending, 0,
                                             memory_order_acq_rel);

        /* Clear the offload item pointer now that we have resumed. */
        f->offload_item = NULL;

        /*
         * Release fiber-side refcount ONLY if RESULT_CLAIMED won (normal path).
         * In the CANCELLED path, strand_fiber_cancel already called
         * offload_item_release for the fiber-side reference.  Releasing it
         * again here would be a double-free.
         * See ARCHITECTURE.md 6.6 refcount ownership rule.
         */
        if (!cancelled)
                offload_item_release(item);

        return (cancelled ? STRAND_CANCELLED : STRAND_OK);
}
