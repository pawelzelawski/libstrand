#ifndef STRAND_INTERNAL_H
#define STRAND_INTERNAL_H

/*
 * strand_internal.h — internal shared types and compile-time assertions.
 * NOT included by embedders. Internal use only.
 *
 * Complete struct definitions, _Static_assert size/offset checks, and
 * internal macros are added here as each phase lands.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/strand.h"
#include "strand_context.h"

/*
 * STRAND_DEBUG_ASSERT — active in debug builds (-DSTRAND_DEBUG).
 * Use for invariant checks that must not run in production.
 */
#ifdef STRAND_DEBUG
#define STRAND_DEBUG_ASSERT(cond) assert(cond)
#else
#define STRAND_DEBUG_ASSERT(cond) ((void)0)
#endif

/*
 * Compile-time layout assertions. See CODING_STANDARDS.md §1.3.
 * strand_context_t check is live — Task 2.1 complete.
 * strand_fiber_t check is a placeholder until Phase 3, Task 3.1.
 */
_Static_assert(sizeof(strand_context_t) == STRAND_CONTEXT_SIZE,
               "strand_context_t size changed — update assembly stubs");
_Static_assert(1, "placeholder — replaced in Phase 3 (Task 3.1): "
                  "offsetof(strand_fiber_t, context) == 0");
_Static_assert(
    1,
    "placeholder — replaced in Phase 3: sizeof(strand_fiber_handle_t) == 16");

#endif /* STRAND_INTERNAL_H */
