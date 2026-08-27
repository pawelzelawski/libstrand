/*
 * strand_scope.c - structured concurrency scopes.
 * See ARCHITECTURE.md §7.
 *
 * Tasks 6.2, 6.3, 6.4, 6.5, 6.6, 6.7, 6.8: strand_scope_open, strand_scope_spawn,
 * scope_child_finish (internal), strand_scope_wait, scope_walk_cancel,
 * strand_scope_cancel, strand_scope_wait_timeout, strand_scope_abandon.
 */

#include <stdatomic.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "strand_scope.h"
#include "strand_fiber.h"
#include "strand_inject.h"
#include "strand_sched.h"

static void scope_walk_cancel(strand_scheduler_t *sched, strand_scope_t *scope);

strand_scope_t *
strand_scope_create(void)
{
	return (calloc(1, sizeof(strand_scope_t)));
}

void
strand_scope_destroy(strand_scope_t *scope)
{
	free(scope);
}

static __attribute__((noinline)) strand_fiber_t *
scope_current_fiber(strand_scheduler_t *sched)
{
	if (strand_sched_current_tls != sched)
		return (NULL);
	return (sched->current_fiber);
}

static void
scope_free_members(strand_scope_t *scope)
{
	strand_scope_child_t *child, *next;

	for (child = scope->spawn_list_head; child != NULL; child = next) {
		next = child->next;
		free(child);
	}
	scope->spawn_list_head = NULL;
}

static void
scope_put(strand_scope_t *scope)
{
	if (atomic_fetch_sub_explicit(&scope->ref_count, 1,
	    memory_order_seq_cst) == 1) {
		scope_free_members(scope);
		free(scope);
	}
}

#ifdef STRAND_TEST_CLOCK
_Atomic int strand_test_scope_walk_hook_enable;
_Atomic int strand_test_scope_walk_hook_entered;
_Atomic int strand_test_scope_walk_hook_release;
#endif

static void
scope_release_finish_hold(strand_scope_t *scope)
{
        int prev_walk;

        prev_walk = atomic_fetch_sub_explicit(&scope->walk_ref_count, 1,
                                              memory_order_seq_cst);
	(void)prev_walk;
}

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
	atomic_store(&scope->ref_count,       1);
        atomic_store(&scope->first_error,     0);

        scope->spawn_list_head     = NULL;
        scope->cancellation_flag   = 0;

        /*
         * Record the opening fiber as the parent that strand_scope_wait
         * will use to identify which fiber to wake when SCOPE_COMPLETED.
         * The parent is always the fiber that calls scope_open; it must
         * later call scope_wait (or scope_abandon) on this scope.
         */
        atomic_store_explicit(&scope->parent_fiber_ptr, current,
                              memory_order_seq_cst);
        atomic_store_explicit(&scope->parent_fiber_generation,
                              current->generation,
                              memory_order_seq_cst);

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
	strand_scope_child_t *child;
        strand_fiber_handle_t handle;
        strand_fiber_t      *f;
        int                  rc;

	ta = malloc(sizeof(*ta));
	if (ta == NULL)
		return (STRAND_ERR_NOMEM);
	child = malloc(sizeof(*child));
	if (child == NULL) {
		free(ta);
		return (STRAND_ERR_NOMEM);
	}

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
		free(child);
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
         * Prepend a stable handle node.  Forward traversal is reverse spawn
         * order, and descriptor recycling cannot affect this membership.
         */
	child->handle = handle;
	child->next = scope->spawn_list_head;
	scope->spawn_list_head = child;

	atomic_fetch_add(&scope->live_child_count, 1);
	atomic_fetch_add(&scope->ref_count, 1);

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
        int      do_walk;
        int      walk_hold;
        scope_lifecycle_t lifecycle;
        strand_fiber_t   *parent;
        strand_fiber_handle_t parent_handle;

        do_walk = 0;
        walk_hold = 0;

        /*
         * First-error CAS: if this child failed and no prior child has
         * claimed first_error, claim it now.  If the CAS fails, a sibling
         * already won - discard this error (first error wins, ARCHITECTURE.md
         * §7.5).  If the CAS succeeds, initiate the cancellation walk.
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
                        do_walk = 1;
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
        if (prev_count == 1) {
                /*
                 * We decremented to zero.  Try to transition to
                 * SCOPE_COMPLETED.  The valid source states are ACTIVE,
                 * CANCELLING, and DRAINING.
                 */
                lifecycle = atomic_exchange_explicit(&scope->lifecycle,
                                                     SCOPE_COMPLETED,
                                                     memory_order_seq_cst);
        } else {
                lifecycle = atomic_load_explicit(&scope->lifecycle,
                                                 memory_order_seq_cst);
        }

        /*
         * Run the cancellation walk after decrementing live_child_count.
         * This preserves the CANCELLING -> COMPLETED shortcut semantics
         * when the failing child is also the last live child.
         */
        if (do_walk) {
                /*
                 * Hold one extra walk reference so scope_walk_cancel cannot
                 * free the control block before this function completes its
                 * post-walk completion path.
                 */
                atomic_fetch_add_explicit(&scope->walk_ref_count, 1,
                                          memory_order_seq_cst);
                walk_hold = 1;
                scope_walk_cancel(sched, scope);
        }

	if (prev_count != 1) {
		if (walk_hold)
			scope_release_finish_hold(scope);
		scope_put(scope);
		return; /* more children still running */
	}

	/* An abandoned scope has no parent to wake. */
	if (atomic_load_explicit(&scope->owner_flag,
	                         memory_order_seq_cst) == OWNER_RUNTIME) {
		if (walk_hold) {
			scope_release_finish_hold(scope);
		}
		scope_put(scope);
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
        parent_handle.ptr = atomic_load_explicit(&scope->parent_fiber_ptr,
                                                 memory_order_seq_cst);
        parent_handle.generation = atomic_load_explicit(
            &scope->parent_fiber_generation,
            memory_order_seq_cst);

	if (fiber_handle_validate(parent_handle) != STRAND_OK) {
		if (walk_hold)
			scope_release_finish_hold(scope);
		scope_free_members(scope);
		scope_put(scope);
		return;
	}

        parent = parent_handle.ptr;

        /*
         * The parent may be parked in one of two states:
         *
         * FIBER_PARKED_SCOPE  - plain strand_scope_wait; wake directly.
         * FIBER_PARKED_TIMER  - strand_scope_wait_timeout; remove from
         *   timer heap first so the timer does not fire spuriously, then
         *   wake the fiber.  On resume the fiber checks lifecycle to
         *   distinguish scope-complete wakeup from timer expiry.
         */
        {
                fiber_state_t st = atomic_load(&parent->state);
                if (st == FIBER_PARKED_SCOPE) {
                        atomic_store(&parent->state, FIBER_RUNNABLE);
                        run_queue_push(sched, parent);
                } else if (st == FIBER_PARKED_TIMER) {
                        /*
                         * SAFETY: timer_heap_remove is safe here because
                         * parent and all children are pinned to the same
                         * single worker; no concurrent heap mutation is
                         * possible.  If the timer already fired and moved
                         * the fiber to FIBER_RUNNABLE before this point,
                         * the state load above would have been FIBER_RUNNABLE
                         * and we would skip this branch correctly.
                         */
                        timer_heap_remove(sched, parent);
                        atomic_store(&parent->state, FIBER_RUNNABLE);
                        run_queue_push(sched, parent);
                }
        }

	if (walk_hold)
		scope_release_finish_hold(scope);
	scope_free_members(scope);
	scope_put(scope);
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

	/* Do not read scheduler-owned current_fiber from a host thread. */
	f = scope_current_fiber(sched);
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
        inject_item_t      item;

	if (!pthread_equal(pthread_self(), atomic_load_explicit(
	    &sched->owner_thread, memory_order_acquire))) {
                /*
                 * SAFETY: cross-thread scope cancel is enqueue-only.  The owner
                 * worker applies the cancellation walk in Step 1 while draining
                 * inject items, preserving single-owner access to scope lists.
                 */
                item.type = INJECT_SCOPE_CANCEL;
                item.u.scope = scope;
		if (!scheduler_inject_acquire(sched))
			return (STRAND_ERR_SHUTDOWN);
		if (inject_queue_try_push_release(&sched->inject_queue, &item) != 0) {
			scheduler_inject_release(sched);
			return (STRAND_EAGAIN);
		}
		scheduler_inject_release(sched);

#ifdef STRAND_LINUX
                {
                        uint64_t v = 1;
                        (void)write(sched->wakeup_fd, &v, sizeof(v));
                }
#endif
#ifdef STRAND_OPENBSD
                {
                        char v = 1;
                        (void)write(sched->wakeup_pipe[1], &v, sizeof(v));
                }
#endif
                return (STRAND_OK);
        }

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
 * strand_scope_wait_timeout - park until scope completes or deadline passes.
 *
 * See include/strand.h for the full contract.
 *
 * Implementation strategy: park the fiber in FIBER_PARKED_TIMER so the
 * scheduler's timer heap can wake it on deadline expiry (same mechanism as
 * strand_fiber_sleep_until).  scope_child_finish detects FIBER_PARKED_TIMER
 * on the parent, removes it from the heap, and moves it to FIBER_RUNNABLE
 * when all children complete - whichever event fires first wins.
 *
 * On resume, the fiber inspects lifecycle to determine which event fired:
 *   SCOPE_COMPLETED -> terminal path (return first_error).
 *   other           -> timeout fired (return STRAND_TIMEOUT).
 * cancel_pending is also checked first.
 * ---------------------------------------------------------------------------
 */
int
strand_scope_wait_timeout(strand_scheduler_t *sched, strand_scope_t *scope,
                          uint64_t deadline_ns)
{
        strand_fiber_t *f;

	f = scope_current_fiber(sched);
        if (f == NULL)
                return (STRAND_ERR_WRONGCTX);

        /*
         * Fast path A: scope already completed.
         */
        if (atomic_load_explicit(&scope->lifecycle, memory_order_seq_cst) ==
            SCOPE_COMPLETED)
                return ((int)atomic_load(&scope->first_error));

        /*
         * Fast path B: no children spawned.
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
         * Slow path: arm the timer and park.
         *
         * Transition to FIBER_PARKED_TIMER before pushing the timer entry.
         * scope_child_finish checks for FIBER_PARKED_TIMER in addition to
         * FIBER_PARKED_SCOPE, and will remove this entry from the heap
         * and enqueue the fiber if scope completion races the timer.
         * ATOMIC: relaxed store is safe - visibility to other fibers is
         * provided by the context switch that follows.
         */
        atomic_store_explicit(&f->state, FIBER_PARKED_TIMER,
                              memory_order_relaxed);

        if (timer_heap_push(sched, deadline_ns, f) != 0) {
                /* Restore state on alloc failure - cannot park. */
                atomic_store_explicit(&f->state, FIBER_RUNNING,
                                      memory_order_relaxed);
                return (STRAND_ERR_NOMEM);
        }

        strand_context_switch(f, &sched->scheduler_ctx);

        /*
         * Resumed.  Three possible reasons:
         * 1. Timer fired (scheduler popped expired entry): lifecycle may
         *    or may not be SCOPE_COMPLETED depending on race.
         * 2. scope_child_finish woke us (removed from heap): lifecycle IS
         *    SCOPE_COMPLETED.
         * 3. strand_fiber_cancel: cancel_pending set; timer removed by
         *    strand_fiber_cancel already.
         */
        if (atomic_load(&f->cancel_pending)) {
                atomic_store(&f->cancel_pending, 0);
                return (STRAND_CANCELLED);
        }

        if (atomic_load_explicit(&scope->lifecycle, memory_order_seq_cst) ==
            SCOPE_COMPLETED)
                return ((int)atomic_load(&scope->first_error));

        /* Deadline expired before scope completed. */
        return (STRAND_TIMEOUT);
}

/* ---------------------------------------------------------------------------
 * strand_scope_abandon - hand scope ownership to the runtime (terminal for
 * the caller).
 *
 * See include/strand.h for the full contract.
 *
 * After this call the scope pointer is invalid for the caller.  Children
 * continue running.  When live_child_count reaches zero and lifecycle
 * reaches SCOPE_COMPLETED, scope_child_finish (or scope_walk_cancel if a
 * walk is in progress) frees the control block.
 *
 * Callable from a running fiber or the host thread.  No WRONGCTX check.
 *
 * SAFETY: In debug builds, if called from a fiber context, assert that the
 * scope pointer does not fall within the current fiber's stack.  A scope
 * on the calling fiber's stack will be invalidated when the stack unwinds;
 * passing it to strand_scope_abandon is undefined behaviour.
 * See ARCHITECTURE.md §7.4 (allocation requirement) and CODING_STANDARDS.md §6.2.
 * ---------------------------------------------------------------------------
 */
void
strand_scope_abandon(strand_scheduler_t *sched, strand_scope_t *scope)
{
	strand_fiber_t *f;

	f = scope_current_fiber(sched);

#ifdef STRAND_DEBUG
        /*
         * SAFETY: assert the scope control block is NOT on the current
         * fiber's stack.  Stack-allocated scopes are only valid when
         * strand_scope_wait is used exclusively and the scope is destroyed
         * before the stack frame exits.  Passing a stack-allocated scope
         * to strand_scope_abandon is undefined behaviour because the
         * runtime will later try to free() a stack pointer.
         *
         * Only check when called from a fiber context (f != NULL).
         * Host-thread callers have no fiber stack to check against.
         */
        if (f != NULL && f->stack_base != NULL) {
                const char *lo = (const char *)f->stack_base;
                const char *hi = lo + f->stack_size;
                const char *sp = (const char *)scope;
                STRAND_DEBUG_ASSERT(
                    (sp < lo || sp >= hi) &&
                    "strand_scope_abandon: scope must not be stack-allocated "
                    "on the calling fiber's stack");
        }
#endif /* STRAND_DEBUG */

        /*
         * Record the parent fiber handle as null so scope_child_finish
         * will not attempt to wake a fiber that is gone.
         * ATOMIC: seq_cst stores are required because scope_child_finish may
         * read these fields concurrently on the owner worker while abandon is
         * called from a host thread.
         */
        atomic_store_explicit(&scope->parent_fiber_ptr, NULL,
                              memory_order_seq_cst);
        atomic_store_explicit(&scope->parent_fiber_generation, 0,
                              memory_order_seq_cst);

        /*
         * Transfer ownership to the runtime.
         * ATOMIC: seq_cst store ensures that scope_child_finish, which
         * loads owner_flag with seq_cst, sees OWNER_RUNTIME immediately
         * after this point.
         */
	{
		int expected_owner = OWNER_CALLER;
		if (!atomic_compare_exchange_strong_explicit(&scope->owner_flag,
		    &expected_owner, OWNER_RUNTIME, memory_order_seq_cst,
		    memory_order_seq_cst))
			return;
	}

	/* Release the caller's owner reference exactly once. */
	scope_put(scope);

        (void)f; /* used only in STRAND_DEBUG block above */
}

/* ---------------------------------------------------------------------------
 * scope_walk_cancel - walk the spawn-order list and cancel all children.
 *
 * Called when the first child error is recorded (from scope_child_finish)
 * or when the caller explicitly calls strand_scope_cancel.
 *
 * The walk:
 *   1. Increments walk_ref_count while it iterates the stable membership list.
 *   2. Walks spawn_list_head (forward = reverse spawn order).
 *   3. Calls strand_fiber_cancel on each child handle.  Stale handles
 *      (child already finished) return STRAND_HANDLE_STALE - idempotent,
 *      continue walking.
 *   4. Transitions lifecycle SCOPE_CANCELLING -> SCOPE_DRAINING.
 *   5. Decrements walk_ref_count.
 *
 * See ARCHITECTURE.md §7.3 (lifetime rule) and §7.6.
 * ---------------------------------------------------------------------------
 */
static void
scope_walk_cancel(strand_scheduler_t *sched, strand_scope_t *scope)
{
	strand_fiber_handle_t handle;
        scope_lifecycle_t     expected_lc;
        int                   prev_walk;

        /* Retain the existing cancellation-walk synchronization. */
        atomic_fetch_add_explicit(&scope->walk_ref_count, 1,
                                  memory_order_seq_cst);

#ifdef STRAND_TEST_CLOCK
        if (atomic_load_explicit(&strand_test_scope_walk_hook_enable,
                                 memory_order_seq_cst)) {
                atomic_store_explicit(&strand_test_scope_walk_hook_entered, 1,
                                      memory_order_seq_cst);
                while (!atomic_load_explicit(&strand_test_scope_walk_hook_release,
                                             memory_order_seq_cst))
                        sched_yield();
        }
#endif

        /*
         * Walk forward through spawn_list_head (forward order = reverse
         * spawn order, because spawns are prepended).  See strand_scope_spawn.
         */
	for (const strand_scope_child_t *child = scope->spawn_list_head;
	    child != NULL; child = child->next) {
		handle = child->handle;
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

        /* Release the cancellation-walk synchronization hold. */
        prev_walk = atomic_fetch_sub_explicit(&scope->walk_ref_count, 1,
                                              memory_order_seq_cst);

	(void)prev_walk;

        (void)sched; /* sched reserved for future cross-worker enqueue path */
}
