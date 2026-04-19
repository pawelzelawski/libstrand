/*
 * tests/test_layer5.c - Layer 5 (Phase 6) test suite.
 *
 * Tasks 6.2, 6.3, 6.5: strand_scope_open, strand_scope_spawn,
 * scope_child_finish, strand_scope_wait.
 *
 * Tests covered here (happy-path, no cancellation):
 *   test_scope_open_from_host_thread_error
 *   test_scope_basic_wait
 *   test_scope_active_to_completed_direct
 *   test_scope_no_children_completes_immediately
 *   test_scope_wait_returns_zero_on_success
 *   test_scope_error_propagates  (first_error CAS; walk stub = no sibling
 *                                  cancel, but error code returns correctly)
 *   test_scope_first_error_wins
 *
 * See DEVELOPMENT.md "Tests for Phase 6" and TESTING.md §5.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test_harness.h"
#include "../include/strand.h"
#include "../src/strand_internal.h"
#include "../src/strand_sched.h"
#include "../src/strand_fiber.h"

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
 * Note: Task 6.4 (cancellation walk) is not yet implemented, so sibling
 * cancellation does NOT happen.  All children run.  The first_error CAS
 * is exercised; scope_wait must return the error code.
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
         * Without Task 6.4 the sibling is NOT cancelled; it runs to
         * completion.  Verify it ran (no early termination).
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

/* -------------------------------------------------------------------------
 * Test runner
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
}

