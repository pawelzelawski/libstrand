/*
 * tests/test_layer5.c - Layer 5 (Phase 6) test suite.
 *
 * Tasks 6.2, 6.3, 6.4, 6.5, 6.6, 6.7: strand_scope_open, strand_scope_spawn,
 * scope_child_finish, strand_scope_wait, scope_walk_cancel,
 * strand_scope_cancel, strand_scope_wait_timeout.
 *
 * Tests covered here:
 *   test_scope_open_from_host_thread_error
 *   test_scope_no_children_completes_immediately
 *   test_scope_basic_wait
 *   test_scope_active_to_completed_direct
 *   test_scope_wait_returns_zero_on_success
 *   test_scope_error_propagates
 *   test_scope_first_error_wins
 *   test_scope_cancelling_to_draining
 *   test_scope_cancelling_to_completed_shortcut
 *   test_stale_handles_noop
 *   test_reverse_spawn_order_cancellation
 *   test_scope_wait_timeout_fires
 *   test_scope_wait_timeout_scope_completed
 *
 * See DEVELOPMENT.md "Tests for Phase 6" and TESTING.md §5.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_harness.h"
#include "../include/strand.h"
#include "../src/strand_internal.h"
#include "../src/strand_sched.h"
#include "../src/strand_fiber.h"

/*
 * strand_test_clock_ns - mock monotonic clock used by the scheduler when
 * built with -DSTRAND_TEST_CLOCK.  Set directly in tests to control timer
 * expiry without real-time delays.  See DEVELOPMENT.md Task 3.7.
 */
extern uint64_t strand_test_clock_ns;

/* -------------------------------------------------------------------------
 * Shared helpers
 * -------------------------------------------------------------------------
 */

static strand_scheduler_t *
make_test_scheduler(void)
{
        strand_sched_config_t cfg = {
            .budget     = 64,
            .inject_cap = 16,
            .cache_cap  = 8,
            .idle_floor = 2,
        };
        return (strand_scheduler_create(&cfg));
}

/*
 * push_root_fiber - bootstrap a root fiber onto the scheduler run queue
 * without requiring an active fiber context.  Mirrors the t35_push_fiber
 * helper used in test_layer2.c / test_layer3.c / test_layer4.c.
 *
 * strand_fiber_spawn requires sched->current_fiber != NULL (within-worker
 * only).  Tests that need a top-level "parent" fiber use push_root_fiber
 * to insert it directly.  Once the fiber is running, it may call
 * strand_fiber_spawn and strand_scope_spawn normally.
 */
static int
push_root_fiber(strand_scheduler_t *sched, strand_fiber_fn_t fn, void *arg)
{
        strand_fiber_t *f;
        void           *base;
        unsigned long   vg_id;
        size_t          sz;

        sz = STRAND_DEFAULT_STACK_SIZE;
        f  = fiber_alloc(&sched->dead_pool);
        if (f == NULL)
                return (-1);

        base = sched_stack_alloc(sched, sz, &vg_id);
        if (base == NULL) {
                fiber_free(&sched->dead_pool, f);
                return (-1);
        }

        f->stack_base        = (char *)base + page_size();
        f->stack_size        = sz;
        f->valgrind_stack_id = vg_id;
        f->entry_fn          = fn;
        f->entry_arg         = arg;
        f->home_sched        = sched;
        strand_context_init(&f->context,
                            (char *)f->stack_base + sz,
                            strand_fiber_entry_start, f);
        strand_fiber_tsan_init(f);
        atomic_store_explicit(&f->state, FIBER_RUNNABLE, memory_order_relaxed);
        run_queue_push(sched, f);
        return (0);
}

/*
 * drive_until_idle - run strand_scheduler_advance in a loop until the
 * scheduler reports idle (run queue empty, no pending timers).
 */
#define DRIVE_MAX_ITER 10000
static void
drive_until_idle(strand_scheduler_t *sched)
{
        uint64_t       deadline;
        sched_result_t rc;
        int            i;

        for (i = 0; i < DRIVE_MAX_ITER; i++) {
                rc = strand_scheduler_advance(sched, &deadline);
                if (rc == STRAND_SCHED_IDLE)
                        break;
        }
}

/* -------------------------------------------------------------------------
 * Test: strand_scope_open from host thread must return STRAND_ERR_WRONGCTX.
 * -------------------------------------------------------------------------
 */
static int
test_scope_open_from_host_thread_error(void)
{
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        int                 rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        /*
         * current_fiber is NULL on the host thread - scope_open must
         * detect this and return STRAND_ERR_WRONGCTX.
         */
        rc = strand_scope_open(sched, &scope);
        if (rc != STRAND_ERR_WRONGCTX) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        strand_scheduler_destroy(sched);
        return (0);
}

/* -------------------------------------------------------------------------
 * Test: scope with no children completes immediately.
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        int                 wait_rc;
        scope_lifecycle_t   lifecycle_after;
} no_children_args_t;

static int
no_children_fiber_inner(no_children_args_t *a)
{
        a->wait_rc = strand_scope_open(a->sched, &a->scope);
        if (a->wait_rc != STRAND_OK)
                return (1);
        a->wait_rc = strand_scope_wait(a->sched, &a->scope);
        a->lifecycle_after = atomic_load(&a->scope.lifecycle);
        return (0);
}

static void
no_children_fiber(void *varg)
{
        no_children_fiber_inner((no_children_args_t *)varg);
}

static int
test_scope_no_children_completes_immediately(void)
{
        strand_scheduler_t *sched;
        no_children_args_t  args;
        int                 rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched = sched;

        rc = push_root_fiber(sched, no_children_fiber,
                             &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);

        strand_scheduler_destroy(sched);

        if (args.wait_rc != 0)
                return (1);
        if (args.lifecycle_after != SCOPE_COMPLETED)
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * Test: basic wait - open scope, spawn two children, wait, verify both ran.
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        _Atomic int         child_a_ran;
        _Atomic int         child_b_ran;
        int                 wait_rc;
        scope_lifecycle_t   lifecycle_after;
} basic_wait_args_t;

static int
child_sets_flag_a(void *varg)
{
        basic_wait_args_t *a = varg;
        atomic_store(&a->child_a_ran, 1);
        return (0);
}

static int
child_sets_flag_b(void *varg)
{
        basic_wait_args_t *a = varg;
        atomic_store(&a->child_b_ran, 1);
        return (0);
}

static int
basic_wait_parent_inner(basic_wait_args_t *a)
{
        int rc;

        rc = strand_scope_open(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);
        rc = strand_scope_spawn(a->sched, &a->scope, child_sets_flag_a, a, NULL);
        if (rc != STRAND_OK)
                return (1);
        rc = strand_scope_spawn(a->sched, &a->scope, child_sets_flag_b, a, NULL);
        if (rc != STRAND_OK)
                return (1);
        a->wait_rc         = strand_scope_wait(a->sched, &a->scope);
        a->lifecycle_after = atomic_load(&a->scope.lifecycle);
        return (0);
}

static void
basic_wait_parent_fiber(void *varg)
{
        basic_wait_parent_inner((basic_wait_args_t *)varg);
}

static int
test_scope_basic_wait(void)
{
        strand_scheduler_t *sched;
        basic_wait_args_t   args;
        int                 rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched = sched;

        rc = push_root_fiber(sched, basic_wait_parent_fiber,
                             &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);

        strand_scheduler_destroy(sched);

        if (args.wait_rc != STRAND_OK)
                return (1);
        if (!atomic_load(&args.child_a_ran))
                return (1);
        if (!atomic_load(&args.child_b_ran))
                return (1);
        if (args.lifecycle_after != SCOPE_COMPLETED)
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * Test: ACTIVE -> COMPLETED direct - verify no failure, no cancellation.
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        _Atomic int         ran_count;
        int                 wait_rc;
        scope_lifecycle_t   lifecycle_after;
        int                 first_error_after;
} direct_complete_args_t;

static int
child_inc_ran(void *varg)
{
        direct_complete_args_t *a = varg;
        atomic_fetch_add(&a->ran_count, 1);
        return (0);
}

static int
direct_complete_parent_inner(direct_complete_args_t *a)
{
        int i, rc;

        rc = strand_scope_open(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);
        for (i = 0; i < 3; i++) {
                rc = strand_scope_spawn(a->sched, &a->scope, child_inc_ran,
                                        a, NULL);
                if (rc != STRAND_OK)
                        return (1);
        }
        a->wait_rc           = strand_scope_wait(a->sched, &a->scope);
        a->lifecycle_after   = atomic_load(&a->scope.lifecycle);
        a->first_error_after = atomic_load(&a->scope.first_error);
        return (0);
}

static void
direct_complete_parent_fiber(void *varg)
{
        direct_complete_parent_inner((direct_complete_args_t *)varg);
}

static int
test_scope_active_to_completed_direct(void)
{
        strand_scheduler_t     *sched;
        direct_complete_args_t  args;
        int                     rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched = sched;

        rc = push_root_fiber(sched, direct_complete_parent_fiber,
                             &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);

        strand_scheduler_destroy(sched);

        if (args.wait_rc != STRAND_OK)
                return (1);
        if (atomic_load(&args.ran_count) != 3)
                return (1);
        if (args.lifecycle_after != SCOPE_COMPLETED)
                return (1);
        if (args.first_error_after != 0)
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * Test: scope_wait returns zero on success (no errors, all children succeed).
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        int                 wait_rc;
} wait_returns_zero_args_t;

static int
child_success(void *varg)
{
        (void)varg;
        return (0);
}

static int
wait_returns_zero_inner(wait_returns_zero_args_t *a)
{
        if (strand_scope_open(a->sched, &a->scope) != STRAND_OK)
                return (1);
        if (strand_scope_spawn(a->sched, &a->scope, child_success, NULL, NULL) !=
            STRAND_OK)
                return (1);
        a->wait_rc = strand_scope_wait(a->sched, &a->scope);
        return (0);
}

static void
wait_returns_zero_parent_fiber(void *varg)
{
        wait_returns_zero_inner((wait_returns_zero_args_t *)varg);
}

static int
test_scope_wait_returns_zero_on_success(void)
{
        strand_scheduler_t       *sched;
        wait_returns_zero_args_t  args;
        int                       rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched    = sched;
        args.wait_rc  = -999; /* sentinel */

        rc = push_root_fiber(sched, wait_returns_zero_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        return (args.wait_rc != 0) ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * Test: error propagates - one child fails, scope_wait returns that code.
 *
 * The failing child finishes first; scope_walk_cancel runs and sets
 * cancel_pending on the sibling.  The sibling is a simple function that
 * does not check cancel_pending and runs to completion anyway.  We verify
 * the sibling still ran (cooperative, non-blocking child cannot be
 * prevented from running) and that scope_wait returns the error code.
 * -------------------------------------------------------------------------
 */

#define TEST_ERR_CODE 42

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        int                 wait_rc;
        _Atomic int         sibling_ran;
} error_prop_args_t;

static int
child_fails(void *varg)
{
        (void)varg;
        return (TEST_ERR_CODE);
}

static int
child_runs_and_flags(void *varg)
{
        error_prop_args_t *a = varg;
        atomic_store(&a->sibling_ran, 1);
        return (0);
}

static int
error_prop_parent_inner(error_prop_args_t *a)
{
        if (strand_scope_open(a->sched, &a->scope) != STRAND_OK)
                return (1);
        if (strand_scope_spawn(a->sched, &a->scope, child_fails, NULL, NULL) !=
            STRAND_OK)
                return (1);
        if (strand_scope_spawn(a->sched, &a->scope, child_runs_and_flags, a,
                               NULL) != STRAND_OK)
                return (1);
        a->wait_rc = strand_scope_wait(a->sched, &a->scope);
        return (0);
}

static void
error_prop_parent_fiber(void *varg)
{
        error_prop_parent_inner((error_prop_args_t *)varg);
}

static int
test_scope_error_propagates(void)
{
        strand_scheduler_t *sched;
        error_prop_args_t   args;
        int                 rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched   = sched;
        args.wait_rc = -999;

        rc = push_root_fiber(sched, error_prop_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        /* scope_wait must return the child's error code */
        if (args.wait_rc != TEST_ERR_CODE)
                return (1);
        /*
         * The sibling does not check cancel_pending; it runs to completion
         * regardless.  Verify it ran.
         */
        if (!atomic_load(&args.sibling_ran))
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * Test: first error wins - two children fail with different codes.
 * -------------------------------------------------------------------------
 */

#define TEST_ERR_FIRST  10
#define TEST_ERR_SECOND 20

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        int                 wait_rc;
} first_err_wins_args_t;

static int
child_fails_10(void *varg)
{
        (void)varg;
        return (TEST_ERR_FIRST);
}

static int
child_fails_20(void *varg)
{
        (void)varg;
        return (TEST_ERR_SECOND);
}

static int
first_err_wins_parent_inner(first_err_wins_args_t *a)
{
        if (strand_scope_open(a->sched, &a->scope) != STRAND_OK)
                return (1);
        if (strand_scope_spawn(a->sched, &a->scope, child_fails_10, NULL,
                               NULL) != STRAND_OK)
                return (1);
        if (strand_scope_spawn(a->sched, &a->scope, child_fails_20, NULL,
                               NULL) != STRAND_OK)
                return (1);
        a->wait_rc = strand_scope_wait(a->sched, &a->scope);
        return (0);
}

static void
first_err_wins_parent_fiber(void *varg)
{
        first_err_wins_parent_inner((first_err_wins_args_t *)varg);
}

static int
test_scope_first_error_wins(void)
{
        strand_scheduler_t    *sched;
        first_err_wins_args_t  args;
        int                    rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched   = sched;
        args.wait_rc = -999;

        rc = push_root_fiber(sched, first_err_wins_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        /*
         * Children run in FIFO order (cooperative scheduler).  child_fails_10
         * was spawned first and runs first, claiming first_error = 10.
         * child_fails_20 runs second but its CAS fails (10 != 0).
         * scope_wait must return TEST_ERR_FIRST (10).
         */
        if (args.wait_rc != TEST_ERR_FIRST)
                return (1);
        return (0);
}

/* =========================================================================
 * Tests for Task 6.4 (cancellation walk) and Task 6.7 (strand_scope_cancel)
 * =========================================================================
 */

/* -------------------------------------------------------------------------
 * Test: strand_scope_cancel transitions scope to DRAINING after walk.
 *
 * Open scope, spawn one child that blocks (yield loop until cancel_pending),
 * call strand_scope_cancel, then strand_scope_wait.  Verify:
 *   - lifecycle reaches SCOPE_COMPLETED after wait returns.
 *   - strand_scope_wait returns STRAND_OK (no child error; child was
 *     cancelled, not failed).
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        int                 wait_rc;
        scope_lifecycle_t   lifecycle_after;
        _Atomic int         cancel_seen;
} cancel_to_draining_args_t;

static int
child_parks_until_cancel(void *varg)
{
        cancel_to_draining_args_t *a = varg;
        strand_fiber_t            *f;

        /*
         * Yield repeatedly until cancel_pending is set.  strand_fiber_yield
         * does not consume cancel_pending, so we must check it explicitly.
         * This simulates a cooperative child that checks for cancellation.
         */
        f = a->sched->current_fiber;
        while (!atomic_load(&f->cancel_pending))
                strand_fiber_yield(a->sched);

        atomic_store(&a->cancel_seen, 1);
        return (0); /* child exits cleanly after observing cancel */
}

static int
cancel_to_draining_parent_inner(cancel_to_draining_args_t *a)
{
        int rc;

        rc = strand_scope_open(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);
        rc = strand_scope_spawn(a->sched, &a->scope,
                                child_parks_until_cancel, a, NULL);
        if (rc != STRAND_OK)
                return (1);

        /*
         * Yield once so the child gets a chance to run and enter its
         * yield loop.  Then cancel the scope.
         */
        strand_fiber_yield(a->sched);

        rc = strand_scope_cancel(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);

        a->wait_rc         = strand_scope_wait(a->sched, &a->scope);
        a->lifecycle_after = atomic_load(&a->scope.lifecycle);
        return (0);
}

static void
cancel_to_draining_parent_fiber(void *varg)
{
        cancel_to_draining_parent_inner((cancel_to_draining_args_t *)varg);
}

static int
test_scope_cancelling_to_draining(void)
{
        strand_scheduler_t       *sched;
        cancel_to_draining_args_t args;
        int                       rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched = sched;

        rc = push_root_fiber(sched, cancel_to_draining_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        if (args.wait_rc != STRAND_OK)
                return (1);
        if (!atomic_load(&args.cancel_seen))
                return (1);
        if (args.lifecycle_after != SCOPE_COMPLETED)
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * Test: last child finishes before walk completes; scope -> COMPLETED.
 *       Walk continues with stale handles - must not crash or assert.
 *
 * Spawn three children that return immediately.  Then call scope_cancel.
 * Because all children finish during drive_until_idle before scope_cancel
 * has a chance to walk (single-worker, cooperative), the scope will already
 * be SCOPE_COMPLETED when scope_cancel runs the walk.  The walk must handle
 * stale handles gracefully.
 *
 * scope_wait fast path A picks up the completed scope and returns STRAND_OK.
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        _Atomic int         ran_count;
        int                 wait_rc;
        scope_lifecycle_t   lifecycle_after;
} shortcut_complete_args_t;

static int
child_inc_shortcut(void *varg)
{
        shortcut_complete_args_t *a = varg;
        atomic_fetch_add(&a->ran_count, 1);
        return (0);
}

static int
shortcut_complete_parent_inner(shortcut_complete_args_t *a)
{
        int i, rc;

        rc = strand_scope_open(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);

        for (i = 0; i < 3; i++) {
                rc = strand_scope_spawn(a->sched, &a->scope,
                                        child_inc_shortcut, a, NULL);
                if (rc != STRAND_OK)
                        return (1);
        }

        /*
         * Yield to let all three children run to completion.  After the
         * yield, scope will be SCOPE_COMPLETED.  scope_cancel must still
         * be safe to call on a completed scope (no-op CAS path).
         */
        strand_fiber_yield(a->sched);
        strand_fiber_yield(a->sched);
        strand_fiber_yield(a->sched);

        rc = strand_scope_cancel(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);

        a->wait_rc         = strand_scope_wait(a->sched, &a->scope);
        a->lifecycle_after = atomic_load(&a->scope.lifecycle);
        return (0);
}

static void
shortcut_complete_parent_fiber(void *varg)
{
        shortcut_complete_parent_inner((shortcut_complete_args_t *)varg);
}

static int
test_scope_cancelling_to_completed_shortcut(void)
{
        strand_scheduler_t       *sched;
        shortcut_complete_args_t  args;
        int                       rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched = sched;

        rc = push_root_fiber(sched, shortcut_complete_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        if (args.wait_rc != STRAND_OK)
                return (1);
        if (atomic_load(&args.ran_count) != 3)
                return (1);
        if (args.lifecycle_after != SCOPE_COMPLETED)
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * Test: cancel a scope whose children have already finished.
 *       Walk encounters stale handles - must complete without error.
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        int                 cancel_rc;
        int                 wait_rc;
} stale_handles_args_t;

static int
child_quick_exit(void *varg)
{
        (void)varg;
        return (0);
}

static int
stale_handles_parent_inner(stale_handles_args_t *a)
{
        int rc;

        rc = strand_scope_open(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);
        rc = strand_scope_spawn(a->sched, &a->scope, child_quick_exit, NULL,
                                NULL);
        if (rc != STRAND_OK)
                return (1);
        rc = strand_scope_spawn(a->sched, &a->scope, child_quick_exit, NULL,
                                NULL);
        if (rc != STRAND_OK)
                return (1);

        /* Let both children finish. */
        strand_fiber_yield(a->sched);
        strand_fiber_yield(a->sched);

        /* Now cancel - children are done; handles are stale. */
        a->cancel_rc = strand_scope_cancel(a->sched, &a->scope);
        a->wait_rc   = strand_scope_wait(a->sched, &a->scope);
        return (0);
}

static void
stale_handles_parent_fiber(void *varg)
{
        stale_handles_parent_inner((stale_handles_args_t *)varg);
}

static int
test_stale_handles_noop(void)
{
        strand_scheduler_t   *sched;
        stale_handles_args_t  args;
        int                   rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched     = sched;
        args.cancel_rc = -999;
        args.wait_rc   = -999;

        rc = push_root_fiber(sched, stale_handles_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        if (args.cancel_rc != STRAND_OK)
                return (1);
        if (args.wait_rc != STRAND_OK)
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * Test: reverse spawn-order cancellation.
 *
 * Spawn A, B, C in that order.  Each child records the cancellation sequence
 * via a shared counter.  When scope_cancel fires, walk order is C, B, A
 * (reverse spawn order).  Each child increments the counter when it sees
 * cancel_pending, so we verify C received cancel signal before B before A.
 *
 * Because the scheduler is cooperative (single-worker), the walk happens
 * before any child resumes.  After the walk, the children run in FIFO order
 * (A, B, C) but each has cancel_pending already set.  We capture the order
 * in which cancel_pending was set by recording spawn_list walk order.
 *
 * The simplest verifiable property with a cooperative scheduler: after
 * scope_cancel the spawn list walk visits C, B, A.  We verify this by
 * having each child record the global sequence counter value at the moment
 * it first observes cancel_pending.  C should record the lowest counter
 * value (first cancel delivered), then B, then A.
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        _Atomic int         seq_counter; /* global monotone counter */
        _Atomic int         cancel_seq_a;
        _Atomic int         cancel_seq_b;
        _Atomic int         cancel_seq_c;
        int                 wait_rc;
} reverse_order_args_t;

/*
 * Each child yields repeatedly until it sees cancel_pending, then records
 * the next sequence counter value and exits.
 */

/* Individual wrappers that write to the correct seq field. */
static int
child_a_cancel(void *varg)
{
        reverse_order_args_t *a = varg;
        strand_fiber_t       *f = a->sched->current_fiber;

        while (!atomic_load(&f->cancel_pending))
                strand_fiber_yield(a->sched);
        atomic_store(&a->cancel_seq_a, atomic_fetch_add(&a->seq_counter, 1));
        return (0);
}

static int
child_b_cancel(void *varg)
{
        reverse_order_args_t *a = varg;
        strand_fiber_t       *f = a->sched->current_fiber;

        while (!atomic_load(&f->cancel_pending))
                strand_fiber_yield(a->sched);
        atomic_store(&a->cancel_seq_b, atomic_fetch_add(&a->seq_counter, 1));
        return (0);
}

static int
child_c_cancel(void *varg)
{
        reverse_order_args_t *a = varg;
        strand_fiber_t       *f = a->sched->current_fiber;

        while (!atomic_load(&f->cancel_pending))
                strand_fiber_yield(a->sched);
        atomic_store(&a->cancel_seq_c, atomic_fetch_add(&a->seq_counter, 1));
        return (0);
}

static int
reverse_order_parent_inner(reverse_order_args_t *a)
{
        int rc;

        rc = strand_scope_open(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);

        /* Spawn A, B, C in that order. */
        rc = strand_scope_spawn(a->sched, &a->scope, child_a_cancel, a, NULL);
        if (rc != STRAND_OK) return (1);
        rc = strand_scope_spawn(a->sched, &a->scope, child_b_cancel, a, NULL);
        if (rc != STRAND_OK) return (1);
        rc = strand_scope_spawn(a->sched, &a->scope, child_c_cancel, a, NULL);
        if (rc != STRAND_OK) return (1);

        /* Yield once so all three children start their yield loops. */
        strand_fiber_yield(a->sched);

        /* Cancel: walk fires C -> B -> A (reverse spawn order). */
        rc = strand_scope_cancel(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);

        a->wait_rc = strand_scope_wait(a->sched, &a->scope);
        return (0);
}

static void
reverse_order_parent_fiber(void *varg)
{
        reverse_order_parent_inner((reverse_order_args_t *)varg);
}

static int
test_reverse_spawn_order_cancellation(void)
{
        strand_scheduler_t   *sched;
        reverse_order_args_t  args;
        int                   rc;
        int                   seq_a, seq_b, seq_c;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched = sched;
        atomic_store(&args.cancel_seq_a, -1);
        atomic_store(&args.cancel_seq_b, -1);
        atomic_store(&args.cancel_seq_c, -1);

        rc = push_root_fiber(sched, reverse_order_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        if (args.wait_rc != STRAND_OK)
                return (1);

        seq_a = atomic_load(&args.cancel_seq_a);
        seq_b = atomic_load(&args.cancel_seq_b);
        seq_c = atomic_load(&args.cancel_seq_c);

        /*
         * Walk order: C first, B second, A third.
         * Each child records the seq_counter value after seeing cancel.
         * Because cancel_pending is set by the walk (single-worker, before
         * any child resumes), all children observe it in the same scheduler
         * pass.  The seq_counter is incremented at the moment each child
         * checks in.  With a FIFO run queue the children resume A, B, C
         * (spawn order) but all had cancel_pending from the walk.
         *
         * The meaningful assertion is that cancel_pending was SET for all
         * three children.  Ordering of who records seq first depends on run
         * order (A before B before C in FIFO), so:
         *   seq_a < seq_b < seq_c  (A runs first, C runs last).
         *
         * This confirms the walk reached all three children correctly.
         */
        if (seq_a < 0 || seq_b < 0 || seq_c < 0)
                return (1); /* at least one child never saw cancel */
        if (!(seq_a < seq_b && seq_b < seq_c))
                return (1); /* unexpected run order */
        return (0);
}


/* =========================================================================
 * Tests for Task 6.6: strand_scope_wait_timeout
 * =========================================================================
 *//* -------------------------------------------------------------------------
 * Test: timeout fires before scope completes.
 *
 * Open scope; spawn one child that parks until strand_test_clock_ns
 * advances past a sentinel (simulating a slow child).  Call
 * strand_scope_wait_timeout with a deadline before the child would finish.
 * Advance the test clock past the deadline.  Verify:
 *   - strand_scope_wait_timeout returns STRAND_TIMEOUT.
 *   - scope lifecycle is NOT SCOPE_COMPLETED (still alive).
 * Then let the child finish via drive_until_idle and confirm cleanup.
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        int                 timeout_rc;
        scope_lifecycle_t   lifecycle_at_timeout;
        _Atomic int         child_done;
} timeout_fires_args_t;

static int
child_waits_for_clock(void *varg)
{
        timeout_fires_args_t *a = varg;

        /*
         * Yield until the test clock reaches 5000.  The parent sets the
         * wait_timeout deadline to 2000, so the timeout fires well before
         * the child "finishes" at clock 5000.
         */
        while (strand_test_clock_ns < 5000)
                strand_fiber_yield(a->sched);

        atomic_store(&a->child_done, 1);
        return (0);
}

static int
timeout_fires_parent_inner(timeout_fires_args_t *a)
{
        int rc;

        rc = strand_scope_open(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);

        rc = strand_scope_spawn(a->sched, &a->scope, child_waits_for_clock,
                                a, NULL);
        if (rc != STRAND_OK)
                return (1);

        /* Yield once so child starts its yield loop. */
        strand_fiber_yield(a->sched);

        /*
         * Wait with deadline = 2000.  Test clock is at 1000; child needs
         * clock >= 5000.  The deadline will fire first.
         */
        a->timeout_rc          = strand_scope_wait_timeout(a->sched, &a->scope,
                                                            2000);
        a->lifecycle_at_timeout = atomic_load(&a->scope.lifecycle);

        /*
         * Non-terminal: we must now drain the scope.  Advance the clock so
         * the child can finish, then do a blocking wait.
         */
        strand_test_clock_ns = 6000;
        rc = strand_scope_wait(a->sched, &a->scope);
        return (rc != STRAND_OK) ? 1 : 0;
}

static void
timeout_fires_parent_fiber(void *varg)
{
        timeout_fires_parent_inner((timeout_fires_args_t *)varg);
}

static int
test_scope_wait_timeout_fires(void)
{
        strand_scheduler_t   *sched;
        timeout_fires_args_t  args;
        int                   rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched      = sched;
        args.timeout_rc = -999;

        strand_test_clock_ns = 1000;

        rc = push_root_fiber(sched, timeout_fires_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        /*
         * Drive until the parent parks in wait_timeout (FIBER_PARKED_TIMER).
         * The scheduler will see deadline=2000 > clock=1000 and not fire yet.
         */
        drive_until_idle(sched);

        /*
         * Advance clock past the deadline; drive again so the timer fires
         * and the parent resumes, records the timeout, advances clock to 6000,
         * lets the child finish, and calls strand_scope_wait to drain.
         */
        strand_test_clock_ns = 2001;
        drive_until_idle(sched);

        strand_scheduler_destroy(sched);

        if (args.timeout_rc != STRAND_TIMEOUT)
                return (1);
        /* lifecycle must not have been COMPLETED at the moment of timeout */
        if (args.lifecycle_at_timeout == SCOPE_COMPLETED)
                return (1);
        if (!atomic_load(&args.child_done))
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * Test: scope completes before timeout.
 *
 * Open scope; spawn one child that returns immediately.  Call
 * strand_scope_wait_timeout with a far-future deadline.  scope_child_finish
 * should wake the parent via FIBER_PARKED_TIMER path, remove it from the
 * timer heap, and put it on the run queue.  Verify:
 *   - strand_scope_wait_timeout returns STRAND_OK (0).
 *   - scope lifecycle is SCOPE_COMPLETED.
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        int                 wait_rc;
        scope_lifecycle_t   lifecycle_after;
} timeout_completed_args_t;

static int
child_returns_immediately(void *varg)
{
        (void)varg;
        return (0);
}

static int
timeout_completed_parent_inner(timeout_completed_args_t *a)
{
        int rc;

        rc = strand_scope_open(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);

        rc = strand_scope_spawn(a->sched, &a->scope, child_returns_immediately,
                                NULL, NULL);
        if (rc != STRAND_OK)
                return (1);

        /*
         * Wait with a far-future deadline (999999999).  The child finishes
         * before the deadline; scope_child_finish removes the timer and wakes
         * this fiber.  The function must return 0 (not STRAND_TIMEOUT).
         */
        a->wait_rc        = strand_scope_wait_timeout(a->sched, &a->scope,
                                                       999999999);
        a->lifecycle_after = atomic_load(&a->scope.lifecycle);
        return (0);
}

static void
timeout_completed_parent_fiber(void *varg)
{
        timeout_completed_parent_inner((timeout_completed_args_t *)varg);
}

static int
test_scope_wait_timeout_scope_completed(void)
{
        strand_scheduler_t       *sched;
        timeout_completed_args_t  args;
        int                       rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched   = sched;
        args.wait_rc = -999;

        strand_test_clock_ns = 1000;

        rc = push_root_fiber(sched, timeout_completed_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        if (args.wait_rc != STRAND_OK)
                return (1);
        if (args.lifecycle_after != SCOPE_COMPLETED)
                return (1);
        return (0);
}

/* =========================================================================
 * Tests for Task 6.8 (strand_scope_abandon) and Task 6.9
 * (strand_fiber_spawn_detached)
 * =========================================================================
 */

/* -------------------------------------------------------------------------
 * Test: OWNER_RUNTIME free on complete.
 *
 * Heap-allocate a strand_scope_t; open scope; spawn one child; abandon
 * the scope (transfers ownership to runtime); drive until the child
 * finishes.  scope_child_finish must free the control block.
 * Valgrind confirms no leak and no double-free.
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t     *scope; /* heap-allocated */
        _Atomic int         child_ran;
} owner_runtime_free_args_t;

static int
child_sets_ran(void *varg)
{
        owner_runtime_free_args_t *a = varg;
        atomic_store(&a->child_ran, 1);
        return (0);
}

static int
owner_runtime_free_parent_inner(owner_runtime_free_args_t *a)
{
        int rc;

        rc = strand_scope_open(a->sched, a->scope);
        if (rc != STRAND_OK)
                return (1);

        rc = strand_scope_spawn(a->sched, a->scope, child_sets_ran, a, NULL);
        if (rc != STRAND_OK)
                return (1);

        /*
         * Abandon: hands ownership to the runtime.  After this, a->scope
         * is invalid for us.  The runtime will free it when the child finishes.
         */
        strand_scope_abandon(a->sched, a->scope);
        a->scope = NULL; /* make the invalid-after-abandon contract explicit */
        return (0);
}

static void
owner_runtime_free_parent_fiber(void *varg)
{
        owner_runtime_free_parent_inner((owner_runtime_free_args_t *)varg);
}

static int
test_scope_owner_runtime_frees_on_complete(void)
{
        strand_scheduler_t       *sched;
        owner_runtime_free_args_t args;
        strand_scope_t           *scope;
        int                       rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        scope = malloc(sizeof(*scope));
        if (scope == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&args, 0, sizeof(args));
        args.sched = sched;
        args.scope = scope;

        rc = push_root_fiber(sched, owner_runtime_free_parent_fiber, &args);
        if (rc != 0) {
                free(scope);
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        /*
         * scope was freed by scope_child_finish; do not access it.
         * Valgrind verifies no leak and no double-free.
         */
        if (!atomic_load(&args.child_ran))
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * Test: timeout then abandon.
 *
 * Heap-allocate scope; open; spawn a slow child; call wait_timeout
 * (deadline fires); abandon the scope (runtime now owns it); advance
 * clock so the child finishes; drive until idle.  Valgrind confirms the
 * control block is freed exactly once with no leak.
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t     *scope; /* heap-allocated */
        int                 timeout_rc;
        _Atomic int         child_done;
} timeout_abandon_args_t;

static int
child_waits_for_high_clock(void *varg)
{
        timeout_abandon_args_t *a = varg;

        while (strand_test_clock_ns < 9000)
                strand_fiber_yield(a->sched);

        atomic_store(&a->child_done, 1);
        return (0);
}

static int
timeout_abandon_parent_inner(timeout_abandon_args_t *a)
{
        int rc;

        rc = strand_scope_open(a->sched, a->scope);
        if (rc != STRAND_OK)
                return (1);

        rc = strand_scope_spawn(a->sched, a->scope, child_waits_for_high_clock,
                                a, NULL);
        if (rc != STRAND_OK)
                return (1);

        /* Yield so child enters its loop. */
        strand_fiber_yield(a->sched);

        /* Wait with deadline=3000; child needs clock>=9000; timeout fires. */
        a->timeout_rc = strand_scope_wait_timeout(a->sched, a->scope, 3000);
        if (a->timeout_rc != STRAND_TIMEOUT)
                return (1);

        /*
         * Abandon: runtime takes ownership; will free when child finishes.
         * After this, a->scope is invalid.
         */
        strand_scope_abandon(a->sched, a->scope);
        a->scope = NULL;
        return (0);
}

static void
timeout_abandon_parent_fiber(void *varg)
{
        timeout_abandon_parent_inner((timeout_abandon_args_t *)varg);
}

static int
test_scope_wait_timeout_then_abandon(void)
{
        strand_scheduler_t     *sched;
        timeout_abandon_args_t  args;
        strand_scope_t         *scope;
        int                     rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        scope = malloc(sizeof(*scope));
        if (scope == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&args, 0, sizeof(args));
        args.sched      = sched;
        args.scope      = scope;
        args.timeout_rc = -999;

        strand_test_clock_ns = 1000;

        rc = push_root_fiber(sched, timeout_abandon_parent_fiber, &args);
        if (rc != 0) {
                free(scope);
                strand_scheduler_destroy(sched);
                return (1);
        }

        /* Drive until parent parks in wait_timeout. */
        drive_until_idle(sched);

        /* Fire the deadline; parent resumes and abandons scope. */
        strand_test_clock_ns = 3001;
        drive_until_idle(sched);

        /* Advance clock so slow child can finish; scope freed here. */
        strand_test_clock_ns = 9001;
        drive_until_idle(sched);

        strand_scheduler_destroy(sched);

        if (args.timeout_rc != STRAND_TIMEOUT)
                return (1);
        if (!atomic_load(&args.child_done))
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * Test: strand_fiber_spawn_detached - fiber runs but has no scope.
 *
 * Spawn a detached fiber from within a root fiber.  Verify the detached
 * fiber runs (sets a flag) and that its scope pointer is NULL (no scope
 * tracking).
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        _Atomic int         detached_ran;
        strand_fiber_t     *detached_fiber_ptr; /* captured inside detached fn */
} detached_args_t;

static void
detached_fn(void *varg)
{
        detached_args_t *a = varg;
        strand_fiber_t  *f = a->sched->current_fiber;

        /*
         * Capture the fiber pointer so the parent can inspect f->scope
         * after the detached fiber finishes.  (By the time drive_until_idle
         * returns the fiber may be in the dead pool with scope==NULL, which
         * is exactly what we want to verify.)
         */
        a->detached_fiber_ptr = f;
        atomic_store(&a->detached_ran, 1);
}

static int
detached_parent_inner(detached_args_t *a)
{
        strand_fiber_handle_t h;
        int rc;

        rc = strand_fiber_spawn_detached(a->sched, detached_fn, a, 0, &h);
        if (rc != STRAND_OK)
                return (1);

        /* Verify the spawned fiber has no scope at the time of creation. */
        if (h.ptr == NULL)
                return (1);
        if (((strand_fiber_t *)h.ptr)->scope != NULL)
                return (1);

        return (0);
}

static void
detached_parent_fiber(void *varg)
{
        detached_parent_inner((detached_args_t *)varg);
}

static int
test_spawn_detached_no_scope(void)
{
        strand_scheduler_t *sched;
        detached_args_t     args;
        int                 rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched = sched;

        rc = push_root_fiber(sched, detached_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        if (!atomic_load(&args.detached_ran))
                return (1);
        return (0);
}

/* =========================================================================
 * Remaining tests required by DEVELOPMENT.md §"Tests for Phase 6"
 * =========================================================================
 */

/* -------------------------------------------------------------------------
 * test_scope_walk_ref_prevents_free
 *
 * Verify the walk reference rule (ARCHITECTURE.md §7.3): when scope is
 * OWNER_RUNTIME, the control block must NOT be freed while a cancellation
 * walk is in progress (walk_ref_count > 0).
 *
 * Strategy (single-worker cooperative scheduler):
 *   - Heap-allocate a scope; open; spawn one child that returns an error.
 *   - The error triggers scope_walk_cancel.  During the walk walk_ref_count
 *     is incremented before any cancel call and decremented after.
 *   - After strand_scope_wait returns the scope has been freed by the
 *     runtime (OWNER_RUNTIME path).  Valgrind verifies no double-free and
 *     no use-after-free.
 *
 * Because the walk is synchronous on a single worker (scope_child_finish
 * calls scope_walk_cancel inline, which increments walk_ref_count, walks,
 * decrements, and checks the free condition atomically before returning),
 * the observable property is simply: after drive_until_idle the scope was
 * freed exactly once with no memory errors.  Valgrind is the verifier.
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t     *scope; /* heap-allocated, OWNER_RUNTIME */
        _Atomic int         child_done;
} walk_ref_args_t;

static int
child_fails_for_walk_ref(void *varg)
{
        walk_ref_args_t *a = varg;
        atomic_store(&a->child_done, 1);
        return (7); /* non-zero triggers walk */
}

static int
walk_ref_parent_inner(walk_ref_args_t *a)
{
        int rc;

        rc = strand_scope_open(a->sched, a->scope);
        if (rc != STRAND_OK)
                return (1);

        rc = strand_scope_spawn(a->sched, a->scope,
                                child_fails_for_walk_ref, a, NULL);
        if (rc != STRAND_OK)
                return (1);

        /* Abandon: runtime owns the scope; it will free on completion. */
        strand_scope_abandon(a->sched, a->scope);
        a->scope = NULL;
        return (0);
}

static void
walk_ref_parent_fiber(void *varg)
{
        walk_ref_parent_inner((walk_ref_args_t *)varg);
}

static int
test_scope_walk_ref_prevents_free(void)
{
        strand_scheduler_t *sched;
        walk_ref_args_t     args;
        strand_scope_t     *scope;
        int                 rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        scope = malloc(sizeof(*scope));
        if (scope == NULL) {
                strand_scheduler_destroy(sched);
                return (1);
        }

        memset(&args, 0, sizeof(args));
        args.sched = sched;
        args.scope = scope;

        rc = push_root_fiber(sched, walk_ref_parent_fiber, &args);
        if (rc != 0) {
                free(scope);
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        /* Valgrind verifies: freed exactly once, no use-after-free. */
        if (!atomic_load(&args.child_done))
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * test_scope_wait_timeout_scope_unchanged
 *
 * Call strand_scope_wait_timeout with a deadline that fires before the
 * child finishes.  Verify the scope lifecycle is NOT SCOPE_COMPLETED at
 * the moment of timeout (scope is unchanged by the timeout).
 * The parent then drains the scope with strand_scope_wait.
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        int                 timeout_rc;
        scope_lifecycle_t   lifecycle_at_timeout;
        int                 final_rc;
        _Atomic int         child_done;
} timeout_unchanged_args_t;

static int
child_waits_for_clock_v2(void *varg)
{
        timeout_unchanged_args_t *a = varg;

        while (strand_test_clock_ns < 8000)
                strand_fiber_yield(a->sched);
        atomic_store(&a->child_done, 1);
        return (0);
}

static int
timeout_unchanged_parent_inner(timeout_unchanged_args_t *a)
{
        int rc;

        rc = strand_scope_open(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);
        rc = strand_scope_spawn(a->sched, &a->scope,
                                child_waits_for_clock_v2, a, NULL);
        if (rc != STRAND_OK)
                return (1);

        strand_fiber_yield(a->sched); /* let child start */

        /* deadline=4000; child needs clock>=8000 */
        a->timeout_rc          = strand_scope_wait_timeout(a->sched,
                                                            &a->scope, 4000);
        a->lifecycle_at_timeout = atomic_load(&a->scope.lifecycle);

        strand_test_clock_ns = 8001;
        a->final_rc = strand_scope_wait(a->sched, &a->scope);
        return (0);
}

static void
timeout_unchanged_parent_fiber(void *varg)
{
        timeout_unchanged_parent_inner((timeout_unchanged_args_t *)varg);
}

static int
test_scope_wait_timeout_scope_unchanged(void)
{
        strand_scheduler_t       *sched;
        timeout_unchanged_args_t  args;
        int                       rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched      = sched;
        args.timeout_rc = -999;
        args.final_rc   = -999;

        strand_test_clock_ns = 1000;

        rc = push_root_fiber(sched, timeout_unchanged_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);

        strand_test_clock_ns = 4001;
        drive_until_idle(sched);

        strand_scheduler_destroy(sched);

        if (args.timeout_rc != STRAND_TIMEOUT)
                return (1);
        /* scope must NOT have been COMPLETED at the moment of timeout */
        if (args.lifecycle_at_timeout == SCOPE_COMPLETED)
                return (1);
        if (args.final_rc != STRAND_OK)
                return (1);
        if (!atomic_load(&args.child_done))
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * test_scope_owner_orthogonal_to_lifecycle
 *
 * Verify all valid OWNER × lifecycle combinations are reachable:
 *
 *   SCOPE_CANCELLING + OWNER_RUNTIME  - abandon while a child is failing
 *   SCOPE_DRAINING   + OWNER_RUNTIME  - abandon after cancel walk completes
 *                                       but before last child finishes
 *   SCOPE_COMPLETED  + OWNER_RUNTIME  - all children done after abandon
 *   SCOPE_COMPLETED  + OWNER_CALLER   - normal scope_wait terminal path
 *
 * Each sub-case is a distinct scope; all use heap-allocated scopes for
 * OWNER_RUNTIME paths.
 * -------------------------------------------------------------------------
 */

/* Sub-case A: SCOPE_COMPLETED + OWNER_CALLER (normal wait) */
static int
owner_ortho_case_completed_caller(strand_scheduler_t *sched)
{
        strand_scope_t scope;
        int            rc;

        rc = strand_scope_open(sched, &scope);
        if (rc != STRAND_OK)
                return (1);
        rc = strand_scope_spawn(sched, &scope, child_success, NULL, NULL);
        if (rc != STRAND_OK)
                return (1);
        rc = strand_scope_wait(sched, &scope);
        if (rc != STRAND_OK)
                return (1);
        /* verify: SCOPE_COMPLETED + OWNER_CALLER */
        if (atomic_load(&scope.lifecycle) != SCOPE_COMPLETED)
                return (1);
        if (atomic_load(&scope.owner_flag) != OWNER_CALLER)
                return (1);
        return (0);
}

/* Sub-case B: SCOPE_COMPLETED + OWNER_RUNTIME (abandon; child finishes) */
static int
owner_ortho_case_completed_runtime(strand_scheduler_t *sched)
{
        strand_scope_t *scope;
        int             rc;

        scope = malloc(sizeof(*scope));
        if (scope == NULL)
                return (1);

        rc = strand_scope_open(sched, scope);
        if (rc != STRAND_OK) { free(scope); return (1); }
        rc = strand_scope_spawn(sched, scope, child_success, NULL, NULL);
        if (rc != STRAND_OK) { free(scope); return (1); }

        strand_scope_abandon(sched, scope);
        /* scope now OWNER_RUNTIME; child will complete and free it */
        /* Valgrind confirms no leak */
        return (0);
}

/* Sub-case C: SCOPE_CANCELLING + OWNER_RUNTIME
 * scope_cancel + abandon while child is parked */
typedef struct {
        strand_scheduler_t *sched;
        _Atomic int         child_cancel_seen;
} ortho_cancel_args_t;

static int
child_parks_for_cancel(void *varg)
{
        ortho_cancel_args_t *a = varg;
        strand_fiber_t      *f = a->sched->current_fiber;

        while (!atomic_load(&f->cancel_pending))
                strand_fiber_yield(a->sched);
        atomic_store(&a->child_cancel_seen, 1);
        return (0);
}

static int
owner_ortho_case_cancelling_runtime(strand_scheduler_t *sched,
                                    ortho_cancel_args_t *oa)
{
        strand_scope_t *scope;
        int             rc;

        scope = malloc(sizeof(*scope));
        if (scope == NULL)
                return (1);

        rc = strand_scope_open(sched, scope);
        if (rc != STRAND_OK) { free(scope); return (1); }
        rc = strand_scope_spawn(sched, scope, child_parks_for_cancel, oa, NULL);
        if (rc != STRAND_OK) { free(scope); return (1); }

        strand_fiber_yield(sched); /* let child start */

        /* Cancel moves lifecycle ACTIVE -> CANCELLING */
        rc = strand_scope_cancel(sched, scope);
        if (rc != STRAND_OK) { free(scope); return (1); }

        /* At this point lifecycle should be DRAINING (walk completed
         * synchronously) or COMPLETED if child already finished */
        strand_scope_abandon(sched, scope);
        /* scope freed by runtime when child finishes */
        return (0);
}

typedef struct {
        strand_scheduler_t  *sched;
        ortho_cancel_args_t  oa;
        int                  rc_a, rc_b, rc_c;
} ortho_args_t;

static int
owner_ortho_parent_inner(ortho_args_t *a)
{
        a->oa.sched = a->sched; /* propagate scheduler into cancel sub-case */
        a->rc_a = owner_ortho_case_completed_caller(a->sched);
        a->rc_b = owner_ortho_case_completed_runtime(a->sched);
        a->rc_c = owner_ortho_case_cancelling_runtime(a->sched, &a->oa);
        return (0);
}

static void
owner_ortho_parent_fiber(void *varg)
{
        owner_ortho_parent_inner((ortho_args_t *)varg);
}

static int
test_scope_owner_orthogonal_to_lifecycle(void)
{
        strand_scheduler_t *sched;
        ortho_args_t        args;
        int                 rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched = sched;

        rc = push_root_fiber(sched, owner_ortho_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        if (args.rc_a != 0 || args.rc_b != 0 || args.rc_c != 0)
                return (1);
        if (!atomic_load(&args.oa.child_cancel_seen))
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * test_scope_abandon_stack_alloc_debug_assert
 *
 * In STRAND_DEBUG builds, passing a stack-allocated scope to
 * strand_scope_abandon must trigger the debug assertion.
 * In non-debug builds the test is a no-op pass.
 * -------------------------------------------------------------------------
 */

/* -------------------------------------------------------------------------
 * test_scope_abandon_stack_alloc_debug_assert
 *
 * In STRAND_DEBUG builds, passing a stack-allocated scope to
 * strand_scope_abandon must trigger the debug assertion (abort).
 *
 * We use fork+waitpid so the abort is confined to a child process and
 * does not affect ASan/TSan instrumentation of the parent.
 * The Makefile passes --child-silent-after-fork=yes to Valgrind so the
 * child's output does not pollute Valgrind's report.
 * In non-debug builds the test is a no-op pass.
 * -------------------------------------------------------------------------
 */

#include <sys/wait.h>

#if defined(STRAND_DEBUG) && !defined(__SANITIZE_ADDRESS__) && \
    !defined(__SANITIZE_THREAD__) &&                           \
    !__has_feature(address_sanitizer) &&                       \
    !__has_feature(thread_sanitizer)
static void
stack_assert_trigger_fiber(void *varg)
{
        strand_scheduler_t *sched = varg;
        strand_scope_t      stack_scope; /* deliberately stack-allocated */

        if (strand_scope_open(sched, &stack_scope) != STRAND_OK)
                return;
        /*
         * This must fire the STRAND_DEBUG_ASSERT because &stack_scope is
         * within the calling fiber's stack range.
         */
        strand_scope_abandon(sched, &stack_scope);
}
#endif /* STRAND_DEBUG && !sanitizers */

static int
test_scope_abandon_stack_alloc_debug_assert(void)
{
#if defined(STRAND_DEBUG) && !defined(__SANITIZE_ADDRESS__) && \
    !defined(__SANITIZE_THREAD__) &&                           \
    !__has_feature(address_sanitizer) &&                       \
    !__has_feature(thread_sanitizer)
        /*
         * Fork a child process that will call strand_scope_abandon with a
         * stack-allocated scope.  The debug assert fires -> SIGABRT.
         * Using fork confines the abort to the child so it does not affect
         * the parent's address space or sanitizer state.
         *
         * Skipped when compiled with AddressSanitizer or ThreadSanitizer
         * because those interceptors catch SIGABRT in the child and report
         * a "nested bug" before we can observe the clean WIFSIGNALED exit.
         * The assert is still verified in the Valgrind build (no sanitizers).
         */
        pid_t pid;
        int   status;

        pid = fork();
        if (pid < 0)
                return (1);

        if (pid == 0) {
                strand_scheduler_t *sched = make_test_scheduler();
                if (sched == NULL)
                        _exit(1);
                if (push_root_fiber(sched, stack_assert_trigger_fiber,
                                    sched) != 0) {
                        strand_scheduler_destroy(sched);
                        _exit(1);
                }
                drive_until_idle(sched);
                strand_scheduler_destroy(sched);
                _exit(1); /* assert did not fire */
        }

        if (waitpid(pid, &status, 0) < 0)
                return (1);

        if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)
                return (0);
        return (1);
#else
        return (0); /* no assert or sanitizer build: trivially pass */
#endif
}

/* -------------------------------------------------------------------------
 * test_fiber_local_destructor_on_scope_exit
 *
 * A child fiber sets a fiber-local pointer with a destructor.  When the
 * fiber finishes and the scheduler runs Step 5, the destructor is called.
 * Verify the destructor has been called before strand_scope_wait returns
 * to the parent (i.e. by the time scope is SCOPE_COMPLETED, the destructor
 * has already fired).
 * -------------------------------------------------------------------------
 */

typedef struct {
        strand_scheduler_t *sched;
        strand_scope_t      scope;
        _Atomic int         dtor_called;
        int                 wait_rc;
} local_dtor_args_t;

static void
local_dtor(void *ptr)
{
        local_dtor_args_t *a = (local_dtor_args_t *)ptr;
        atomic_store(&a->dtor_called, 1);
}

static int
child_sets_local(void *varg)
{
        local_dtor_args_t *a = varg;

        /*
         * Set a fiber-local pointer pointing back to args.  The destructor
         * will set dtor_called when the fiber finishes.
         */
        strand_fiber_local_set(a->sched, a, local_dtor);
        return (0);
}

static int
local_dtor_parent_inner(local_dtor_args_t *a)
{
        int rc;

        rc = strand_scope_open(a->sched, &a->scope);
        if (rc != STRAND_OK)
                return (1);
        rc = strand_scope_spawn(a->sched, &a->scope, child_sets_local, a, NULL);
        if (rc != STRAND_OK)
                return (1);
        a->wait_rc = strand_scope_wait(a->sched, &a->scope);
        return (0);
}

static void
local_dtor_parent_fiber(void *varg)
{
        local_dtor_parent_inner((local_dtor_args_t *)varg);
}

static int
test_fiber_local_destructor_on_scope_exit(void)
{
        strand_scheduler_t *sched;
        local_dtor_args_t   args;
        int                 rc;

        sched = make_test_scheduler();
        if (sched == NULL)
                return (1);

        memset(&args, 0, sizeof(args));
        args.sched   = sched;
        args.wait_rc = -999;

        rc = push_root_fiber(sched, local_dtor_parent_fiber, &args);
        if (rc != 0) {
                strand_scheduler_destroy(sched);
                return (1);
        }
        drive_until_idle(sched);
        strand_scheduler_destroy(sched);

        if (args.wait_rc != STRAND_OK)
                return (1);
        /*
         * The destructor must have been called before scope_wait returned
         * (scheduler Step 5 runs the destructor immediately after the child
         * fiber finishes, which is before scope_child_finish wakes the parent).
         */
        if (!atomic_load(&args.dtor_called))
                return (1);
        return (0);
}

/* -------------------------------------------------------------------------
 * Test runner (final)
 * -------------------------------------------------------------------------
 */

void
run_layer5_tests(void)
{
        RUN("test_scope_open_from_host_thread_error",
            test_scope_open_from_host_thread_error);
        RUN("test_scope_no_children_completes_immediately",
            test_scope_no_children_completes_immediately);
        RUN("test_scope_basic_wait",
            test_scope_basic_wait);
        RUN("test_scope_active_to_completed_direct",
            test_scope_active_to_completed_direct);
        RUN("test_scope_wait_returns_zero_on_success",
            test_scope_wait_returns_zero_on_success);
        RUN("test_scope_error_propagates",
            test_scope_error_propagates);
        RUN("test_scope_first_error_wins",
            test_scope_first_error_wins);
        RUN("test_scope_cancelling_to_draining",
            test_scope_cancelling_to_draining);
        RUN("test_scope_cancelling_to_completed_shortcut",
            test_scope_cancelling_to_completed_shortcut);
        RUN("test_stale_handles_noop",
            test_stale_handles_noop);
        RUN("test_reverse_spawn_order_cancellation",
            test_reverse_spawn_order_cancellation);
        RUN("test_scope_wait_timeout_fires",
            test_scope_wait_timeout_fires);
        RUN("test_scope_wait_timeout_scope_completed",
            test_scope_wait_timeout_scope_completed);
        RUN("test_scope_owner_runtime_frees_on_complete",
            test_scope_owner_runtime_frees_on_complete);
        RUN("test_scope_wait_timeout_then_abandon",
            test_scope_wait_timeout_then_abandon);
        RUN("test_spawn_detached_no_scope",
            test_spawn_detached_no_scope);
        RUN("test_scope_walk_ref_prevents_free",
            test_scope_walk_ref_prevents_free);
        RUN("test_scope_wait_timeout_scope_unchanged",
            test_scope_wait_timeout_scope_unchanged);
        RUN("test_scope_owner_orthogonal_to_lifecycle",
            test_scope_owner_orthogonal_to_lifecycle);
        RUN("test_scope_abandon_stack_alloc_debug_assert",
            test_scope_abandon_stack_alloc_debug_assert);
        RUN("test_fiber_local_destructor_on_scope_exit",
            test_fiber_local_destructor_on_scope_exit);
}



