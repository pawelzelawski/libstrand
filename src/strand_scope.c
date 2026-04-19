/*
 * strand_scope.c - structured concurrency scopes.
 * See ARCHITECTURE.md §7.
 *
 * Tasks 6.2, 6.3, 6.5: strand_scope_open, strand_scope_spawn,
 * scope_child_finish (internal), strand_scope_wait.
 *
 * Task 6.4 (cancellation walk) is stubbed - see scope_child_finish.
 */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "strand_scope.h"
#include "strand_fiber.h"
#include "strand_sched.h"

static void scope_walk_cancel(strand_scheduler_t *sched, strand_scope_t *scope);

/* ---------------------------------------------------------------------------
 * scope_fiber_trampoline - internal entry wrapper for scope-tracked fibers.
 *
 * Installed as entry_fn by strand_scope_spawn.  entry_arg points to a
 * heap-allocated scope_trampoline_t.  The trampoline:
 *   1. Copies all needed fields from the trampoline struct to locals.
 *   2. Frees the trampoline struct (no longer needed).
 *   3. Calls the user's int-returning function.
 *   4. Calls scope_child_finish with the return value.
 *
 * After this function returns, strand_fiber_entry_start transitions the
 * fiber to FIBER_FINISHED and switches back to the scheduler.
 * ---------------------------------------------------------------------------
 */
static void
scope_fiber_trampoline(void *varg)
{
        scope_trampoline_t     *ta = (scope_trampoline_t *)varg;
        strand_scope_t         *scope = ta->scope;
        strand_scope_fiber_fn_t fn    = ta->fn;
        void                   *arg   = ta->arg;
        strand_scheduler_t     *sched = ta->sched;
        int                     rc;

        free(ta);

        rc = fn(arg);
        scope_child_finish(sched, scope, rc);
}

/* ---------------------------------------------------------------------------
 * strand_scope_open - initialise a scope control block.
 *
 * See include/strand.h for the full contract.
 * ---------------------------------------------------------------------------
 */
int
strand_scope_open(strand_scheduler_t *sched, strand_scope_t *scope)
{
        strand_fiber_t *current;

        current = sched->current_fiber;
        if (current == NULL)
                return (STRAND_ERR_WRONGCTX);

        memset(scope, 0, sizeof(*scope));

        atomic_store(&scope->lifecycle,       SCOPE_ACTIVE);
        atomic_store(&scope->owner_flag,      OWNER_CALLER);
        atomic_store(&scope->live_child_count, 0);
        atomic_store(&scope->walk_ref_count,  0);
        atomic_store(&scope->first_error,     0);

        scope->spawn_list_head     = NULL;
        scope->cancellation_flag   = 0;

        /*
         * Record the opening fiber as the parent that strand_scope_wait
         * will use to identify which fiber to wake when SCOPE_COMPLETED.
         * The parent is always the fiber that calls scope_open; it must
         * later call scope_wait (or scope_abandon) on this scope.
         */
        scope->parent_fiber.ptr        = current;
        scope->parent_fiber.generation = current->generation;

        return (STRAND_OK);
}

/* ---------------------------------------------------------------------------
 * strand_scope_spawn - spawn a scope-tracked child fiber.
 *
 * See include/strand.h for the full contract.
 * ---------------------------------------------------------------------------
 */
int
strand_scope_spawn(strand_scheduler_t *sched, strand_scope_t *scope,
                   strand_scope_fiber_fn_t fn, void *arg,
                   strand_fiber_handle_t *out)
{
        scope_trampoline_t  *ta;
        strand_fiber_handle_t handle;
        strand_fiber_t      *f;
        int                  rc;

        ta = malloc(sizeof(*ta));
        if (ta == NULL)
                return (STRAND_ERR_NOMEM);

        ta->scope = scope;
        ta->fn    = fn;
        ta->arg   = arg;
        ta->sched = sched;

        /*
         * Spawn the fiber using the existing within-worker path.  entry_fn
         * is scope_fiber_trampoline (void-returning); entry_arg is ta.
         * STRAND_DEFAULT_STACK_SIZE (0 → default) is used for scope fibers.
         */
        rc = strand_fiber_spawn(sched, scope_fiber_trampoline, ta, 0, &handle);
        if (rc != STRAND_OK) {
                free(ta);
                return (rc);
        }

        f = handle.ptr;

        /*
         * Set the scope back-pointer on the fiber descriptor.  Safe here
         * because the fiber has been queued (FIBER_RUNNABLE) but has not
         * started executing - cooperative scheduling guarantees it will not
         * run until the current fiber yields.
         */
        f->scope = scope;

        /*
         * Prepend to the spawn-order list via scope_next.  Forward traversal
         * of the list = reverse spawn order, which is what the cancellation
         * walk (Task 6.4) requires.  See ARCHITECTURE.md 7.6.
         */
        f->scope_next          = scope->spawn_list_head;
        scope->spawn_list_head = f;

        atomic_fetch_add(&scope->live_child_count, 1);

        if (out != NULL)
                *out = handle;

        return (STRAND_OK);
}

/* ---------------------------------------------------------------------------
 * scope_child_finish - called by scope_fiber_trampoline on child exit.
 *
 * See strand_scope.h for the full contract.
 * ---------------------------------------------------------------------------
 */
void
scope_child_finish(strand_scheduler_t *sched, strand_scope_t *scope,
                   int retval)
{
        int      prev_count;
        int      expected_err;
        scope_lifecycle_t lifecycle;
        strand_fiber_t   *parent;

        /*
         * First-error CAS: if this child failed and no prior child has
         * claimed first_error, claim it now.  If the CAS fails, a sibling
         * already won - discard this error (first error wins, ARCHITECTURE.md
         * 7.5).  If the CAS succeeds, initiate the cancellation walk.
         *
         * Task 6.4 stub: the walk is not yet implemented; scope transitions
         * directly ACTIVE -> CANCELLING without sending cancel signals.
         * Tests for error propagation and sibling cancellation require 6.4.
         */
        if (retval != 0) {
                expected_err = 0;
                if (atomic_compare_exchange_strong_explicit(
                        &scope->first_error,
                        &expected_err,
                        retval,
                        memory_order_seq_cst,
                        memory_order_seq_cst)) {
                        /*
                         * SAFETY: We won the first-error CAS.  Transition
                         * lifecycle ACTIVE -> CANCELLING before initiating
                         * the walk, so that sibling completions that race
                         * see CANCELLING and handle it correctly.
                         */
                        scope_lifecycle_t expected_lc = SCOPE_ACTIVE;
                        atomic_compare_exchange_strong_explicit(
                            &scope->lifecycle,
                            &expected_lc,
                            SCOPE_CANCELLING,
                            memory_order_seq_cst,
                            memory_order_seq_cst);
                        scope->cancellation_flag = 1;
                        scope_walk_cancel(sched, scope);
                }
        }

        /*
         * Decrement live_child_count.  If this was the last child, attempt
         * to transition lifecycle to SCOPE_COMPLETED.
         *
         * ATOMIC: seq_cst fetch_sub matches the seq_cst CAS on lifecycle
         * so that any reader that sees live_child_count == 0 also sees
         * the final lifecycle state.
         */
        prev_count = atomic_fetch_sub_explicit(&scope->live_child_count, 1,
                                               memory_order_seq_cst);
        if (prev_count != 1)
                return; /* more children still running */

        /*
         * We decremented to zero.  Try to transition to SCOPE_COMPLETED.
         * The valid source states are ACTIVE, CANCELLING, and DRAINING.
         * Use a seq_cst exchange (not CAS) since we are the only thread
         * that can perform this transition (live_child_count just hit zero
         * on this thread).
         */
        lifecycle = atomic_exchange_explicit(&scope->lifecycle,
                                             SCOPE_COMPLETED,
                                             memory_order_seq_cst);

        /*
         * OWNER_RUNTIME free path (Task 6.8): if scope was abandoned, the
         * walk_ref_count check and free would happen here.
         * TODO Task 6.8: if OWNER_RUNTIME and walk_ref_count == 0: free.
         */
        if (atomic_load(&scope->owner_flag) == OWNER_RUNTIME) {
                /* Abandoned scope: no parent to wake; runtime owns cleanup. */
                return;
        }

        (void)lifecycle;

        /*
         * Wake the parent fiber if it is currently parked in scope_wait.
         *
         * The parent_fiber handle was recorded at scope_open time.  If the
         * parent has already finished (e.g. it was cancelled or the scope
         * control block lifetime contract was violated), handle validation
         * returns STRAND_HANDLE_STALE and we skip the wakeup safely.
         *
         * Single-worker invariant: the parent and all scope children run on
         * the same worker.  scope_child_finish runs on that worker too, so
         * run_queue_push is safe without cross-worker inject.
         */
        if (fiber_handle_validate(scope->parent_fiber) != STRAND_OK)
                return;

        parent = scope->parent_fiber.ptr;
        if (atomic_load(&parent->state) != FIBER_PARKED_SCOPE)
                return;

        atomic_store(&parent->state, FIBER_RUNNABLE);
        run_queue_push(sched, parent);
}

/* ---------------------------------------------------------------------------
 * strand_scope_wait - park until all scope children have finished.
 *
 * See include/strand.h for the full contract.
 * ---------------------------------------------------------------------------
 */
int
strand_scope_wait(strand_scheduler_t *sched, strand_scope_t *scope)
{
        strand_fiber_t *f;

        f = sched->current_fiber;
        if (f == NULL)
                return (STRAND_ERR_WRONGCTX);

        /*
         * Fast path A: scope already completed.  This can happen when all
         * children finish before the parent calls scope_wait (within the
         * same advance call, or when scope_wait is called after multiple
         * advance iterations).
         */
        if (atomic_load_explicit(&scope->lifecycle, memory_order_seq_cst) ==
            SCOPE_COMPLETED)
                return ((int)atomic_load(&scope->first_error));

        /*
         * Fast path B: no children were ever spawned.  Transition to
         * SCOPE_COMPLETED immediately and return 0.
         */
        if (atomic_load_explicit(&scope->live_child_count,
                                 memory_order_seq_cst) == 0) {
                scope_lifecycle_t expected = SCOPE_ACTIVE;
                atomic_compare_exchange_strong_explicit(
                    &scope->lifecycle, &expected, SCOPE_COMPLETED,
                    memory_order_seq_cst, memory_order_seq_cst);
                return (0);
        }

        /*
         * Slow path: park until scope_child_finish wakes us.
         *
         * Transition to FIBER_PARKED_SCOPE before the context switch.
         * scope_child_finish checks for exactly this state before doing
         * run_queue_push, so there is no race: we are on the same worker
         * and the children cannot run until we yield here.
         *
         * Cancellation: if strand_fiber_cancel is called on this fiber
         * while parked, cancel transitions us to FIBER_RUNNABLE and sets
         * cancel_pending.  We check cancel_pending on return.
         */
        atomic_store(&f->state, FIBER_PARKED_SCOPE);
        strand_context_switch(f, &sched->scheduler_ctx);

        /*
         * Resumed.  Two possible reasons:
         * 1. scope_child_finish woke us: lifecycle == SCOPE_COMPLETED.
         * 2. strand_fiber_cancel woke us: cancel_pending is set.
         */
        if (atomic_load(&f->cancel_pending)) {
                atomic_store(&f->cancel_pending, 0);
                return (STRAND_CANCELLED);
        }

        return ((int)atomic_load(&scope->first_error));
}

/* ---------------------------------------------------------------------------
 * strand_scope_cancel - externally initiate cancellation of a scope.
 *
 * See include/strand.h for the full contract.
 *
 * Non-terminal: returns immediately.  The caller must follow with
 * strand_scope_wait or strand_scope_abandon.
 *
 * May be called from a running fiber, the host thread, or another worker
 * thread (see ARCHITECTURE.md §12, thread safety matrix).  No WRONGCTX
 * check is required.
 * ---------------------------------------------------------------------------
 */
int
strand_scope_cancel(strand_scheduler_t *sched, strand_scope_t *scope)
{
        scope_lifecycle_t expected;

        /*
         * CAS lifecycle SCOPE_ACTIVE -> SCOPE_CANCELLING.
         *
         * If the scope is already SCOPE_CANCELLING, SCOPE_DRAINING, or
         * SCOPE_COMPLETED a concurrent cancellation or natural completion
         * has already begun.  The no-op path is correct for all three:
         *   CANCELLING  - walk already in progress or will start.
         *   DRAINING    - walk already completed.
         *   COMPLETED   - all children already done; nothing to cancel.
         * ATOMIC: seq_cst CAS matches all other seq_cst accesses on lifecycle.
         */
        expected = SCOPE_ACTIVE;
        if (!atomic_compare_exchange_strong_explicit(
                &scope->lifecycle, &expected, SCOPE_CANCELLING,
                memory_order_seq_cst, memory_order_seq_cst))
                return (STRAND_OK); /* already cancelling/draining/completed */

        scope->cancellation_flag = 1;
        scope_walk_cancel(sched, scope);
        return (STRAND_OK);
}

/* ---------------------------------------------------------------------------
 * scope_walk_cancel - walk the spawn-order list and cancel all children.
 *
 * Called when the first child error is recorded (from scope_child_finish)
 * or when the caller explicitly calls strand_scope_cancel.
 *
 * The walk:
 *   1. Increments walk_ref_count to hold a reference for the duration of
 *      the walk, preventing OWNER_RUNTIME free while iterating the list.
 *   2. Walks spawn_list_head via scope_next (forward = reverse spawn order).
 *   3. Calls strand_fiber_cancel on each child handle.  Stale handles
 *      (child already finished) return STRAND_HANDLE_STALE - idempotent,
 *      continue walking.
 *   4. Transitions lifecycle SCOPE_CANCELLING -> SCOPE_DRAINING.
 *   5. Decrements walk_ref_count.  If both conditions for OWNER_RUNTIME
 *      free are met (SCOPE_COMPLETED and walk_ref_count == 0), frees the
 *      control block.
 *
 * See ARCHITECTURE.md §7.3 (walk reference rule) and §7.6.
 * ---------------------------------------------------------------------------
 */
static void
scope_walk_cancel(strand_scheduler_t *sched, strand_scope_t *scope)
{
        strand_fiber_t       *f;
        strand_fiber_handle_t handle;
        scope_lifecycle_t     expected_lc;
        int                   prev_walk;

        /*
         * Hold walk reference so OWNER_RUNTIME free cannot race with our
         * traversal of the spawn list.
         * ATOMIC: seq_cst increment matches seq_cst decrement below and
         * the seq_cst store in scope_child_finish (ARCHITECTURE.md 7.3).
         */
        atomic_fetch_add_explicit(&scope->walk_ref_count, 1,
                                  memory_order_seq_cst);

        /*
         * Walk forward through spawn_list_head (forward order = reverse
         * spawn order, because spawns are prepended).  See strand_scope_spawn.
         */
        for (f = scope->spawn_list_head; f != NULL; f = f->scope_next) {
                handle.ptr        = f;
                handle.generation = f->generation;
                /*
                 * strand_fiber_cancel is idempotent for stale handles and
                 * for fibers already in FIBER_FINISHED.  Ignore the return
                 * value; both STRAND_OK and STRAND_HANDLE_STALE are benign.
                 */
                (void)strand_fiber_cancel(handle);
        }

        /*
         * Transition SCOPE_CANCELLING -> SCOPE_DRAINING.
         * If the last child finished before we completed the walk the
         * lifecycle will already be SCOPE_COMPLETED; that is valid per
         * the state machine (ARCHITECTURE.md 7.3) - leave it as is.
         */
        expected_lc = SCOPE_CANCELLING;
        atomic_compare_exchange_strong_explicit(
            &scope->lifecycle, &expected_lc, SCOPE_DRAINING,
            memory_order_seq_cst, memory_order_seq_cst);

        /*
         * Release walk reference.
         * ATOMIC: seq_cst decrement synchronises with the seq_cst load of
         * walk_ref_count that checks the free condition below.
         */
        prev_walk = atomic_fetch_sub_explicit(&scope->walk_ref_count, 1,
                                              memory_order_seq_cst);

        /*
         * OWNER_RUNTIME free check: free the control block if all of the
         * following hold:
         *   - We were the last walk (prev_walk == 1, so new count is 0).
         *   - live_child_count == 0 (all children have finished).
         *   - lifecycle == SCOPE_COMPLETED.
         *   - OWNER_RUNTIME (no caller is waiting).
         *
         * This is the only free site for OWNER_RUNTIME scopes where the
         * last walk completes after the last child (Task 6.8 covers the
         * complementary case where the last child completes after the walk).
         */
        if (prev_walk == 1 &&
            atomic_load_explicit(&scope->live_child_count,
                                 memory_order_seq_cst) == 0 &&
            atomic_load_explicit(&scope->lifecycle,
                                 memory_order_seq_cst) == SCOPE_COMPLETED &&
            atomic_load_explicit(&scope->owner_flag,
                                 memory_order_seq_cst) == OWNER_RUNTIME) {
                free(scope);
        }

        (void)sched; /* sched reserved for future cross-worker enqueue path */
}
