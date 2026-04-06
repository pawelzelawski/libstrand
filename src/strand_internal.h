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
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/strand.h"
#include "strand_context.h"

/*
 * Sanitizer feature detection.
 * Clang exposes sanitizer state via __has_feature(...), while some GCC
 * builds define __SANITIZE_ADDRESS__ / __SANITIZE_THREAD__.
 */
#if defined(__has_feature)
#define STRAND_HAS_FEATURE(x) __has_feature(x)
#else
#define STRAND_HAS_FEATURE(x) 0
#endif

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
#if defined(__SANITIZE_ADDRESS__) || STRAND_HAS_FEATURE(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define STRAND_ASAN_SWITCH_START(new_sp, new_sz)                               \
	__sanitizer_start_switch_fiber(NULL, (new_sp), (new_sz))
#define STRAND_ASAN_SWITCH_FINISH()                                            \
	__sanitizer_finish_switch_fiber(NULL, NULL, NULL)
#else
#define STRAND_ASAN_SWITCH_START(new_sp, new_sz) ((void)0)
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
#if defined(__SANITIZE_THREAD__) || STRAND_HAS_FEATURE(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
#define STRAND_TSAN_SWITCH(to_fiber)                                           \
	do {                                                                   \
		if ((to_fiber)->tsan_fiber != NULL)                            \
			__tsan_switch_to_fiber((to_fiber)->tsan_fiber, 0);     \
	} while (0)
#define STRAND_TSAN_CREATE(f) ((f)->tsan_fiber = __tsan_create_fiber(0))
#define STRAND_TSAN_BIND_CURRENT(f)                                            \
	((f)->tsan_fiber = __tsan_get_current_fiber())
#define STRAND_TSAN_DESTROY(f) __tsan_destroy_fiber((f)->tsan_fiber)
#else
#define STRAND_TSAN_SWITCH(to_fiber) ((void)0)
#define STRAND_TSAN_CREATE(f) ((void)0)
#define STRAND_TSAN_BIND_CURRENT(f) ((void)0)
#define STRAND_TSAN_DESTROY(f) ((void)0)
#endif

/*
 * Valgrind fiber stack registration.
 * STRAND_VG_STACK_REGISTER must be called after every mmap+mprotect to
 * register the usable portion of the stack with Valgrind, so that Valgrind
 * does not report fiber stack accesses as errors.  The registration returns
 * an opaque ID that must be passed to STRAND_VG_STACK_DEREGISTER at free.
 * These macros are no-ops when -DHAVE_VALGRIND is not set (OpenBSD builds,
 * or Linux builds without valgrind-devel) and are also no-ops at runtime
 * when the process is not running under Valgrind.
 * See TECH_STACK.md §7.1 and ARCHITECTURE.md §3.7.
 */
#ifdef HAVE_VALGRIND
#include <valgrind/valgrind.h>
#define STRAND_VG_STACK_REGISTER(base, top)                                    \
	((unsigned long)VALGRIND_STACK_REGISTER((base), (top)))
#define STRAND_VG_STACK_DEREGISTER(id) VALGRIND_STACK_DEREGISTER((id))
#else
#define STRAND_VG_STACK_REGISTER(base, top) (0UL)
#define STRAND_VG_STACK_DEREGISTER(id) ((void)0)
#endif

/*
 * fiber_state_t — fiber execution state machine.
 * See ARCHITECTURE.md §4.5 for all legal transitions and ownership rules.
 *
 * FIBER_CANCELLATION_PENDING is not a discrete state: it is a per-fiber
 * flag that coexists with FIBER_RUNNABLE or FIBER_RUNNING.  It is stored
 * as a separate field and not listed here.
 */
typedef enum {
	FIBER_NEW = 0,
	FIBER_RUNNABLE = 1,
	FIBER_RUNNING = 2,
	FIBER_PARKED_IO_READ = 3,
	FIBER_PARKED_IO_WRITE = 4,
	FIBER_PARKED_TIMER = 5,
	FIBER_PARKED_OFFLOAD = 6,
	FIBER_PARKED_CHANNEL = 7,
	FIBER_FINISHED = 8,
} fiber_state_t;

/*
 * strand_fiber_t — complete fiber descriptor (Phase 2 + Phase 3 Task 3.1).
 *
 * Layout invariants:
 *   - context MUST be at offset 0: assembly stubs in src/arch/ rely on it.
 *   - The _Static_assert below enforces this at compile time.
 *
 * fp_ctrl encoding:
 *   x86_64  — low 32 bits hold MXCSR; upper 32 bits unused.
 *   AArch64 — low 32 bits hold FPCR, high 32 bits hold FPSR.
 *
 * tsan_fiber is always present as void* regardless of whether TSan is
 * active — avoids ABI differences between TSan and non-TSan builds.
 * It is NULL in non-TSan builds.
 *
 * Dead-pool recycling: when a descriptor is reused from the dead pool,
 * generation is incremented and the descriptor is zeroed (except for the
 * new generation value) before being handed to the caller.  The next
 * field is repurposed as the dead-pool link while the fiber is in the
 * pool; it is overwritten during normal use.
 */
typedef struct strand_fiber {
	strand_context_t context;    /* MUST be first — offset 0 */
	uint64_t fp_ctrl;            /* MXCSR (x86_64) or FPCR|FPSR */
	_Atomic fiber_state_t state; /* see fiber_state_t above */
	uint64_t
	    generation;   /* ABA-protection counter; see ARCHITECTURE.md §4.6 */
	void *stack_base; /* usable stack base (above guard page) */
	size_t stack_size; /* usable stack size (excludes guard page) */
	void
	    *local_ptr; /* fiber-local storage slot; see ARCHITECTURE.md §4.7 */
	strand_destructor_t
	    local_dtor; /* destructor for local_ptr; called on FIBER_FINISHED */
	strand_scope_t *scope; /* owning scope; NULL if detached */
	struct strand_fiber
	    *next; /* intrusive link: run queue, scope lists, dead pool */
	void *tsan_fiber; /* __tsan_create_fiber handle; NULL if no TSan */
	unsigned long
	    valgrind_stack_id; /* VALGRIND_STACK_REGISTER id; 0 if unused */
} strand_fiber_t;

/*
 * Compile-time layout assertions. See CODING_STANDARDS.md §1.3.
 */
_Static_assert(sizeof(strand_context_t) == STRAND_CONTEXT_SIZE,
               "strand_context_t size changed — update assembly stubs");
_Static_assert(offsetof(strand_fiber_t, context) == 0,
               "context must be first field — assembly stubs assume offset 0");
_Static_assert(sizeof(strand_fiber_handle_t) == 16,
               "strand_fiber_handle_t size changed — ptr (8) + generation (8)");

#endif /* STRAND_INTERNAL_H */
