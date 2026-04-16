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
 *
 * The save pointer is passed by address to start_switch_fiber so that ASan
 * preserves the caller's fake-stack frame across the switch.  Using NULL
 * discards the old fake stack, causing ASan to poison the calling context's
 * local variables and produce false SIGSEGV when a fiber dereferences a
 * pointer into the caller's frame (e.g. a user arg passed as a local-var
 * address).  This save/restore matches the documented correct usage of
 * __sanitizer_start_switch_fiber for resumable contexts.
 */
#if defined(__SANITIZE_ADDRESS__) || STRAND_HAS_FEATURE(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define STRAND_ASAN_SWITCH_START(save, new_sp, new_sz)                         \
	__sanitizer_start_switch_fiber(&(save), (new_sp), (new_sz))
#define STRAND_ASAN_SWITCH_FINISH(save)                                        \
	__sanitizer_finish_switch_fiber((save), NULL, NULL)
#else
#define STRAND_ASAN_SWITCH_START(save, new_sp, new_sz) ((void)(save))
#define STRAND_ASAN_SWITCH_FINISH(save) ((void)(save))
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
	/*
	 * cancel_pending — set by strand_fiber_cancel when the fiber is
	 * FIBER_RUNNABLE or FIBER_RUNNING (FIBER_CANCELLATION_PENDING flag).
	 * Also consulted by strand_fiber_sleep_until on return to detect a
	 * cancellation that raced with timer expiry.
	 * See ARCHITECTURE.md §4.5 and §8.1.
	 */
	_Atomic int cancel_pending;
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

/*
 * strand_inject_queue_t — Phase 3 stub.
 * Replaced with a real bounded MPSC ring buffer in Phase 5 (Task 5.1).
 * The stub has no fields; inject_queue_drain is a no-op in Phase 3.
 */
typedef struct strand_inject_queue {
	int _placeholder; /* zero-size structs are not valid in C11 */
} strand_inject_queue_t;

/*
 * strand_stack_slot_t — one entry in the per-scheduler stack cache.
 *
 * base       — mmap base (bottom of the allocation, includes guard page).
 * stack_size — usable stack size (excludes the guard page).
 * vg_id      — Valgrind stack registration ID; passed to
 *              STRAND_VG_STACK_DEREGISTER on free.
 *
 * The guard page is always PAGE_SIZE bytes at base; usable stack is
 * base + PAGE_SIZE for stack_size bytes.
 */
typedef struct strand_stack_slot {
	void *base;
	size_t stack_size;
	unsigned long vg_id;
} strand_stack_slot_t;

/*
 * strand_timer_entry_t — one entry in the per-scheduler timer min-heap.
 * Keyed on deadline_ns; ties are broken by insertion order (stable).
 * See ARCHITECTURE.md §4.2 Step 2.
 */
typedef struct strand_timer_entry {
	uint64_t deadline_ns;
	strand_fiber_t *fiber;
} strand_timer_entry_t;

/*
 * strand_scheduler_t — complete single-worker fiber scheduler (Task 3.2).
 *
 * Ownership:
 *   - All strand_fiber_t descriptors on the run queue or timer heap are
 *     owned by this scheduler.
 *   - current_fiber is the exclusively-running fiber; all others are parked.
 *   - The dead pool holds reusable descriptors; generation is incremented
 *     by fiber_alloc when a descriptor is taken from the pool.
 *
 * Thread safety:
 *   - This struct is NOT thread-safe.  All fields are owned by the worker
 *     thread that calls strand_scheduler_advance / strand_scheduler_run.
 *   - stop_flag is the only field modified from other threads; it is
 *     _Atomic for that reason.
 *   - The inject queue will have its own memory-ordering protocol in Phase 5.
 *
 * Wakeup fd:
 *   - Linux: eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK) — single int.
 *   - OpenBSD: pipe2([0]=read, [1]=write, O_CLOEXEC | O_NONBLOCK).
 *   - strand_scheduler_stop writes one byte to the write side to interrupt
 *     a blocked epoll_wait/kevent.  Step 3 of advance drains and discards
 *     all bytes.  See ARCHITECTURE.md §4.2.
 */
typedef struct strand_scheduler {
	/*
	 * Scheduler's own execution context.  When running inside advance,
	 * context switches save here and resume the scheduler after the fiber
	 * returns or yields.  stack_base and stack_size are 0/NULL — the
	 * scheduler runs on the OS thread stack and does not own a fiber stack.
	 */
	strand_fiber_t scheduler_ctx;

	/* Run queue — FIFO intrusive singly-linked list via fiber->next.
	 * run_head is the next fiber to run; run_tail is where new fibers
	 * are appended.  Both are NULL when the queue is empty. */
	strand_fiber_t *run_head;
	strand_fiber_t *run_tail;
	size_t run_queue_len;

	/* Budget — max fibers to run per advance call.  Default 64. */
	size_t budget;

	/* Currently executing fiber.  NULL when control is in the scheduler. */
	strand_fiber_t *current_fiber;

	/* Timer min-heap — binary heap keyed on deadline_ns (ascending).
	 * Dynamically allocated; grows by doubling when full. */
	strand_timer_entry_t *timer_heap;
	size_t timer_heap_len;
	size_t timer_heap_cap;

	/* Wakeup fd — used by strand_scheduler_stop to interrupt a blocked
	 * worker.  Bytes written here are control signals; drained in Step 3
	 * of strand_scheduler_advance.  See ARCHITECTURE.md §4.2. */
#ifdef STRAND_LINUX
	int wakeup_fd;
#endif
#ifdef STRAND_OPENBSD
	int wakeup_pipe[2]; /* [0]=read, [1]=write */
#endif

	/* SAFETY: stop_flag is written from any thread via
	 * strand_scheduler_stop. Use atomic_store/atomic_load with the
	 * appropriate memory order. Reading with acquire and writing with
	 * release ensures the stop is visible before the next advance call
	 * inspects the flag. */
	_Atomic int stop_flag;

	/* Inject queue — Phase 3 stub; no-op drain in Step 1.
	 * Replaced with real MPSC ring buffer in Phase 5 (Task 5.1). */
	strand_inject_queue_t inject_queue;

	/* I/O poller — NULL in Phase 3; initialised in Phase 4 (Task 4.1). */
	strand_poller_t *poller;

	/* Stack cache — LIFO, per-scheduler.  Only stacks of matching size
	 * are cached; non-matching stacks are munmap'd immediately.
	 * See ARCHITECTURE.md §13 for cap, floor, and idle reclamation. */
	strand_stack_slot_t *stack_cache;
	size_t cache_len;
	size_t cache_cap;
	size_t cache_floor;
	uint64_t cache_idle_ns; /* idle reclamation timeout (ns) */
	uint64_t last_idle_ns;  /* last observed idle timestamp */

	/* Dead pool — reusable strand_fiber_t descriptors linked via next.
	 * fiber_alloc pops from here when available; fiber_free pushes here. */
	strand_fiber_t *dead_pool;
} strand_scheduler_t;

#endif /* STRAND_INTERNAL_H */
