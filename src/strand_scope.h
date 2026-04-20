#ifndef STRAND_SCOPE_H
#define STRAND_SCOPE_H

/*
 * strand_scope.h - structured concurrency scope internal interface.
 * See ARCHITECTURE.md §7.
 */

#include <stdlib.h>

#include "../include/strand.h"
#include "strand_internal.h"

/* strand_scope_t defined in strand_internal.h. */

/*
 * scope_trampoline_t - heap-allocated per-spawn context for scope-tracked
 * fibers.  Passed as entry_arg to scope_fiber_trampoline (a void-returning
 * function compatible with strand_fiber_fn_t).  Freed at the start of
 * scope_fiber_trampoline after all fields are copied to locals.
 *
 * Keeping the trampoline context separate from strand_fiber_t avoids
 * adding a scope-specific function pointer to the hot fiber descriptor.
 * The allocation cost (one small malloc per scope spawn) is negligible
 * relative to the fiber stack allocation.
 */
typedef struct scope_trampoline {
        strand_scope_t          *scope;
        strand_scope_fiber_fn_t  fn;
        void                    *arg;
        strand_scheduler_t      *sched;
} scope_trampoline_t;

/*
 * scope_child_finish - called by scope_fiber_trampoline after the user
 * function returns.  Performs first-error CAS, decrements live_child_count,
 * and (when the count reaches zero) transitions lifecycle to SCOPE_COMPLETED
 * and wakes the parent fiber if it is parked in FIBER_PARKED_SCOPE.
 *
 * retval == 0  : child succeeded.
 * retval != 0  : child failed; CAS first_error; initiate cancellation walk.
 *
 * Must be called from the completing fiber's trampoline, on the same worker
 * thread as the scope's parent.  sched is the owning scheduler.
 */
void scope_child_finish(strand_scheduler_t *sched, strand_scope_t *scope,
                        int retval);

#endif /* STRAND_SCOPE_H */
