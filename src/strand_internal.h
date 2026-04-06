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
 * ASan fiber stack switching hooks.
 * __sanitizer_start_switch_fiber must be called immediately before every
 * context swap so ASan tracks the active stack correctly.
 * __sanitizer_finish_switch_fiber must be called immediately after.
 * Without these hooks ASan reports valid fiber stack accesses as
 * stack-buffer-overflows. See TECH_STACK.md §7.2 and ARCHITECTURE.md §3.7.
 */
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define STRAND_ASAN_SWITCH_START(new_sp, new_sz, old_sp, old_sz)               \
	__sanitizer_start_switch_fiber(NULL, (new_sp), (new_sz))
#define STRAND_ASAN_SWITCH_FINISH()                                            \
	__sanitizer_finish_switch_fiber(NULL, NULL, NULL)
#else
#define STRAND_ASAN_SWITCH_START(new_sp, new_sz, old_sp, old_sz) ((void)0)
#define STRAND_ASAN_SWITCH_FINISH() ((void)0)
#endif

/*
 * TSan fiber switching hooks.
 * STRAND_TSAN_SWITCH must be called on the outgoing fiber immediately
 * before strand_context_swap to establish correct happens-before ordering.
 * STRAND_TSAN_CREATE / STRAND_TSAN_DESTROY manage the per-fiber TSan
 * handle (Task 2.7). Without these TSan produces both false positives and
 * false negatives across context switch boundaries.
 * See TECH_STACK.md §7.3 and ARCHITECTURE.md §3.7.
 */
#if defined(__SANITIZE_THREAD__)
#include <sanitizer/tsan_interface.h>
#define STRAND_TSAN_SWITCH(to_fiber)                                           \
	__tsan_switch_to_fiber((to_fiber)->tsan_fiber, 0)
#define STRAND_TSAN_CREATE(f) ((f)->tsan_fiber = __tsan_create_fiber(0))
#define STRAND_TSAN_DESTROY(f) __tsan_destroy_fiber((f)->tsan_fiber)
#else
#define STRAND_TSAN_SWITCH(to_fiber) ((void)0)
#define STRAND_TSAN_CREATE(f) ((void)0)
#define STRAND_TSAN_DESTROY(f) ((void)0)
#endif

/*
 * strand_fiber_t — Phase 2 fields (Task 2.4).
 *
 * Full scheduler descriptor added in Phase 3 (Task 3.1) by appending
 * fields after these. Offsets of these fields never change.
 *
 * The context field MUST remain first: assembly stubs in src/arch/
 * assume offset 0. The _Static_assert below enforces this.
 *
 * fp_ctrl encoding:
 *   x86_64 — low 32 bits hold MXCSR; upper 32 bits unused.
 *   AArch64 — low 32 bits hold FPCR, high 32 bits hold FPSR.
 *
 * tsan_fiber is always present as void* (8 bytes) regardless of whether
 * TSan is active — avoids ABI differences between TSan and non-TSan builds.
 * It is NULL in non-TSan builds and populated by STRAND_TSAN_CREATE in
 * Task 2.7.
 */
typedef struct strand_fiber {
	strand_context_t context; /* MUST be first — offset 0 */
	uint64_t fp_ctrl;         /* MXCSR (x86_64) or FPCR|FPSR (AArch64) */
	void *stack_base;         /* usable stack base (above guard page) */
	size_t stack_size;        /* usable stack size (excludes guard page) */
	void *tsan_fiber; /* __tsan_create_fiber handle; NULL if no TSan */
} strand_fiber_t;

/*
 * Compile-time layout assertions. See CODING_STANDARDS.md §1.3.
 */
_Static_assert(sizeof(strand_context_t) == STRAND_CONTEXT_SIZE,
               "strand_context_t size changed — update assembly stubs");
_Static_assert(offsetof(strand_fiber_t, context) == 0,
               "context must be first field — assembly stubs assume offset 0");
_Static_assert(
    1,
    "placeholder — replaced in Phase 3: sizeof(strand_fiber_handle_t) == 16");

#endif /* STRAND_INTERNAL_H */
