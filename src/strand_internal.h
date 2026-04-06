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
 * STRAND_CONTEXT_SIZE — size of the register save area in strand_context_t.
 * Placeholder: replaced with sizeof(strand_context_t) in Phase 2 (Task 2.1).
 * The assembly stubs in src/arch/ depend on this value — must be kept in sync.
 * See CODING_STANDARDS.md §1.3.
 */
#define STRAND_CONTEXT_SIZE 0

/*
 * _Static_assert placeholders — replaced with real checks as struct
 * definitions land in Phase 2 onward. See CODING_STANDARDS.md §1.3.
 */
_Static_assert(1, "placeholder — replaced in Phase 2: "
                  "sizeof(strand_context_t) == STRAND_CONTEXT_SIZE");
_Static_assert(1, "placeholder — replaced in Phase 2: "
                  "offsetof(strand_fiber_t, context) == 0");
_Static_assert(
    1,
    "placeholder — replaced in Phase 3: sizeof(strand_fiber_handle_t) == 16");

#endif /* STRAND_INTERNAL_H */
