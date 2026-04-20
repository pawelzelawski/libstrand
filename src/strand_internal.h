#ifndef STRAND_INTERNAL_H
#define STRAND_INTERNAL_H

/*
 * strand_internal.h - internal shared types and compile-time assertions.
 * NOT included by embedders. Internal use only.
 *
 * Complete struct definitions, _Static_assert size/offset checks, and
 * internal macros are added here as each phase lands.
 */

#include <assert.h>
#include <pthread.h>
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
 * STRAND_DEBUG_ASSERT - active in debug builds (-DSTRAND_DEBUG).
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
 * handle. Without these TSan produces both false positives and
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
 * scope_lifecycle_t - scope lifecycle state machine (ARCHITECTURE.md 7.3).
 *
 * SCOPE_ACTIVE      - open; children running; no failure; no cancellation.
 * SCOPE_CANCELLING  - cancellation initiated; children being cancelled.
 * SCOPE_DRAINING    - all children notified; waiting for last completions.
 * SCOPE_COMPLETED   - all children finished; safe to inspect first_error.
 */
typedef enum {
        SCOPE_ACTIVE     = 0,
        SCOPE_CANCELLING = 1,
        SCOPE_DRAINING   = 2,
        SCOPE_COMPLETED  = 3,
} scope_lifecycle_t;

/*
 * owner_flag values for strand_scope_t.
 *
 * OWNER_CALLER  - the programmer's code owns the control block memory.
 *                 strand_scope_wait and strand_scope_wait_timeout require
 *                 this; strand_scope_abandon transitions away from it.
 * OWNER_RUNTIME - strand_scope_abandon was called; the runtime will free
 *                 the control block when live_child_count reaches zero and
 *                 lifecycle is SCOPE_COMPLETED and walk_ref_count is zero.
 */
#define OWNER_CALLER  0
#define OWNER_RUNTIME 1

/*
 * strand_scope_t - structured concurrency scope control block.
 * See ARCHITECTURE.md 7.2 for field semantics and the walk reference rule.
 *
 * Memory ownership:
 *   OWNER_CALLER  - caller-allocated; freed by the caller after scope_wait.
 *   OWNER_RUNTIME - heap-allocated or caller-allocated but abandoned;
 *                   freed by the runtime when all three free conditions
 *                   hold simultaneously (see ARCHITECTURE.md 7.3).
 *
 * Thread safety:
 *   All _Atomic fields may be accessed from any worker thread.
 *   spawn_list_head is written only at spawn time (single worker) and
 *   read only during the cancellation walk; the walk_ref_count protocol
 *   ensures no concurrent modification.
 *   cancellation_flag is set once under lifecycle CAS protection and
 *   thereafter read-only.
 */
typedef struct strand_scope {
        /*
         * CONCURRENT: accessed from the owner fiber (strand_scope_open,
         * strand_scope_wait, strand_scope_cancel, strand_scope_abandon) and
         * from child fibers in scope_child_finish and scope_walk_cancel.
         * All transitions use atomic_compare_exchange with seq_cst so every
         * reader observes a consistent ordering of state changes.
         * See ARCHITECTURE.md 7.3.
         */
        _Atomic scope_lifecycle_t lifecycle;

        /*
         * CONCURRENT: written by the owner fiber via strand_scope_abandon
         * (OWNER_CALLER -> OWNER_RUNTIME, seq_cst store) and read by
         * scope_child_finish and scope_walk_cancel to determine whether to
         * free the control block.  Atomic store/load with seq_cst.
         */
        _Atomic int owner_flag;

        /*
         * CONCURRENT: decremented by any worker thread when a child fiber
         * finishes (scope_child_finish).  Incremented by the spawning fiber
         * at strand_scope_spawn time (single worker, but other threads may
         * concurrently decrement).  seq_cst fetch_add / fetch_sub.
         * Reaching zero triggers SCOPE_COMPLETED transition.
         */
        _Atomic int live_child_count;

        /*
         * CONCURRENT: incremented by scope_walk_cancel before traversing
         * the spawn-order list; decremented after the walk completes.
         * The control block must not be freed while this is non-zero, even
         * if SCOPE_COMPLETED and OWNER_RUNTIME.  seq_cst fetch_add / fetch_sub.
         * See ARCHITECTURE.md 7.3 (walk reference rule).
         */
        _Atomic int walk_ref_count;

        /*
         * CONCURRENT: CAS-set exactly once by the first failing child fiber
         * in scope_child_finish.  Subsequent failures discard their error
         * (CAS will fail because the value is already non-zero).
         * seq_cst compare_exchange_strong.  See ARCHITECTURE.md 7.5.
         */
        _Atomic int first_error;

        /*
         * spawn_list_head - intrusive singly-linked list of spawned child
         * fibers in spawn order, linked via strand_fiber_t.next at spawn
         * time.  Used by scope_walk_cancel to cancel siblings in
         * reverse-spawn order.
         * Written only at spawn time (single worker thread).
         * Read only during cancellation walk; walk_ref_count prevents
         * concurrent structural modification.
         * Not _Atomic: guarded by the walk reference rule.
         */
        strand_fiber_t *spawn_list_head;

		/*
		 * CONCURRENT: parent_fiber_ptr and parent_fiber_generation form the
		 * ABA-safe handle of the fiber blocked in strand_scope_wait.
		 * Updated by strand_scope_open / strand_scope_abandon and read by
		 * scope_child_finish when lifecycle reaches SCOPE_COMPLETED.
		 * These fields are atomic because strand_scope_abandon may be called
		 * from the host thread while child completion runs on the owner worker.
		 * seq_cst store/load keeps ordering consistent with lifecycle/owner
		 * transitions.
		 */
		_Atomic(strand_fiber_t *) parent_fiber_ptr;
		_Atomic uint64_t parent_fiber_generation;

        /*
         * cancellation_flag - set to 1 when the scope enters SCOPE_CANCELLING
         * state.  Thereafter read-only; used as a fast non-atomic check by
         * code running on the owner worker where seq_cst ordering is
         * already established by prior atomic operations.
         * Written once under lifecycle CAS protection; read-only after.
         */
        int cancellation_flag;
} strand_scope_t;

/*
 * fiber_state_t - fiber execution state machine.
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
        FIBER_PARKED_SCOPE   = 8, /* parked in strand_scope_wait */
        FIBER_FINISHED       = 9,
} fiber_state_t;

/*
 * strand_fiber_t - complete fiber descriptor.
 *
 * Layout invariants:
 *   - context MUST be at offset 0: assembly stubs in src/arch/ rely on it.
 *   - The _Static_assert below enforces this at compile time.
 *
 * fp_ctrl encoding:
 *   x86_64  - low 32 bits hold MXCSR; upper 32 bits unused.
 *   AArch64 - low 32 bits hold FPCR, high 32 bits hold FPSR.
 *
 * tsan_fiber is always present as void* regardless of whether TSan is
 * active - avoids ABI differences between TSan and non-TSan builds.
 * It is NULL in non-TSan builds.
 *
 * Dead-pool recycling: when a descriptor is reused from the dead pool,
 * generation is incremented and the descriptor is zeroed (except for the
 * new generation value) before being handed to the caller.  The next
 * field is repurposed as the dead-pool link while the fiber is in the
 * pool; it is overwritten during normal use.
 */
typedef struct strand_fiber {
	strand_context_t context;    /* MUST be first - offset 0 */
	uint64_t fp_ctrl;            /* MXCSR (x86_64) or FPCR|FPSR */
	_Atomic fiber_state_t state; /* see fiber_state_t above */
	strand_fiber_fn_t entry_fn;  /* user entry function */
	void *entry_arg;             /* user entry argument */
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
            *next; /* intrusive link: run queue, dead pool */
        /*
         * scope_next - intrusive link for the scope's spawn-order list.
         * Separate from next so the run queue and scope list can coexist.
         * Set by strand_scope_spawn; traversed by scope_walk_cancel.
         * Spawn list is prepended (newest at head): forward traversal =
         * reverse spawn order for the cancellation walk (ARCHITECTURE.md 7.6).
         */
        struct strand_fiber *scope_next;
	void *tsan_fiber; /* __tsan_create_fiber handle; NULL if no TSan */
	unsigned long
	    valgrind_stack_id; /* VALGRIND_STACK_REGISTER id; 0 if unused */
        /*
         * offload_item - pointer to the in-flight strand_offload_item_t while
         * the fiber is in FIBER_PARKED_OFFLOAD state.  Set by
         * strand_fiber_offload before transitioning to PARKED_OFFLOAD; cleared
         * to NULL on resume.  Used by strand_fiber_cancel to locate the work
         * item for the CANCELLED CAS without scanning any pool data structure.
         * Valid only while state == FIBER_PARKED_OFFLOAD.
         */
        struct strand_offload_item *offload_item;
        /*
         * cancel_pending - set by strand_fiber_cancel when the fiber is
	 * FIBER_RUNNABLE or FIBER_RUNNING (FIBER_CANCELLATION_PENDING flag).
	 * Also consulted by strand_fiber_sleep_until on return to detect a
	 * cancellation that raced with timer expiry.
	 * See ARCHITECTURE.md §4.5 and §8.1.
	 */
	_Atomic int cancel_pending;
	/*
	 * parked_fd - fd this fiber is parked on when state is
	 * FIBER_PARKED_IO_READ or FIBER_PARKED_IO_WRITE.
	 * Used by strand_fiber_cancel to find the fd table entry
	 * without scanning the full poller table.
	 * Meaningful only while fiber is in a PARKED_IO state; value is
	 * undefined otherwise.
	 */
	int parked_fd;
	/*
	 * io_result - result code set by the waker before pushing this fiber
	 * to the run queue from an IO wait.  Read by strand_fiber_wait_readable
	 * / strand_fiber_wait_writable on return from strand_context_switch.
	 * Values: STRAND_OK (ready), STRAND_CANCELLED, STRAND_ERR_IO.
	 * See ARCHITECTURE.md §5.3 and §5.6.
	 */
	int io_result;
	/*
	 * home_sched - the scheduler that owns this fiber.
	 * Set at spawn time (strand_fiber_spawn) and used by
	 * strand_fiber_cancel to access the run queue and timer heap without
	 * requiring the caller to supply a scheduler pointer.
	 * This identifies the fiber's pinned
	 * worker for cross-worker inject routing.
	 * Zeroed by fiber_alloc on descriptor reuse; set again at next spawn.
	 */
	struct strand_scheduler *home_sched;
} strand_fiber_t;

/*
 * Compile-time layout assertions. See CODING_STANDARDS.md §1.3.
 */
_Static_assert(sizeof(strand_context_t) == STRAND_CONTEXT_SIZE,
               "strand_context_t size changed - update assembly stubs");
_Static_assert(offsetof(strand_fiber_t, context) == 0,
               "context must be first field - assembly stubs assume offset 0");
_Static_assert(sizeof(strand_fiber_handle_t) == 16,
               "strand_fiber_handle_t size changed - ptr (8) + generation (8)");

/*
 * inject_item_type_t - discriminator for items carried by the inject queue.
 * See ARCHITECTURE.md §6.3.
 *
 * INJECT_CANCEL - cross-worker fiber cancel request; payload is cancel_handle.
 * INJECT_SPAWN  - host-thread fiber spawn; payload is a fully-initialised
 *                 strand_fiber_t * to be pushed onto the worker's run queue.
 * INJECT_OFFLOAD_COMPLETE - offload thread finished; payload is the completed
 *                 strand_offload_item_t *.  The fiber's home worker resumes
 *                 the parked fiber with the result already written to
 *                 result_slot.
 * INJECT_SCOPE_CANCEL - cross-thread scope cancel request.  Payload is a
 *                 strand_scope_t * that must be cancelled on the owning
 *                 worker thread.
 */
typedef enum {
        INJECT_CANCEL = 0,
        INJECT_SPAWN  = 1,
        INJECT_OFFLOAD_COMPLETE = 2,
		INJECT_SCOPE_CANCEL = 3,
} inject_item_type_t;

/*
 * inject_item_t - one item in the bounded MPSC inject ring buffer.
 * The type field selects which union member is valid.
 */
typedef struct inject_item {
        inject_item_type_t     type;
        union {
                strand_fiber_handle_t  cancel_handle; /* INJECT_CANCEL */
                struct strand_fiber   *fiber;         /* INJECT_SPAWN  */
                struct strand_offload_item *offload;  /* INJECT_OFFLOAD_COMPLETE */
				struct strand_scope   *scope;         /* INJECT_SCOPE_CANCEL */
        } u;
} inject_item_t;

/*
 * inject_slot_t - one slot in the ring buffer array.
 *
 * sequence - atomic sequence number used by the Vyukov MPSC algorithm.
 *   Initial value for slot[i] is i.
 *   After producer writes:   sequence = pos + 1  (release)
 *   After consumer reads:    sequence = pos + capacity (release)
 * See strand_inject.c for the full protocol.
 */
typedef struct inject_slot {
	_Atomic size_t sequence;
	inject_item_t  item;
} inject_slot_t;

/*
 * strand_inject_queue_t - bounded MPSC ring buffer, one per worker.
 * See ARCHITECTURE.md §6.3 and strand_inject.c.
 *
 * slots    - heap-allocated ring of inject_slot_t; capacity must be power of 2.
 * capacity - number of slots; always a power of 2 >= 1.
 * mask     - capacity - 1; used instead of modulo.
 * head     - atomic producer position; multiple producers increment atomically.
 * tail     - consumer position; owned exclusively by the worker thread.
 *
 * Thread safety:
 *   head: written by any thread via atomic_fetch_add.
 *   tail: read/written only by the owning worker thread; plain size_t.
 *   slots[i].sequence: the synchronisation point - release on write,
 *     acquire on read.  See CODING_STANDARDS.md §4.2.
 */
typedef struct strand_inject_queue {
	inject_slot_t    *slots;
	size_t            capacity;
	size_t            mask;
	_Atomic size_t    head;
	size_t            tail;
} strand_inject_queue_t;

/*
 * strand_stack_slot_t - one entry in the per-scheduler stack cache.
 *
 * base       - mmap base (bottom of the allocation, includes guard page).
 * stack_size - usable stack size (excludes the guard page).
 * vg_id      - Valgrind stack registration ID; passed to
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
 * strand_timer_entry_t - one entry in the per-scheduler timer min-heap.
 * Keyed on deadline_ns; ties are broken by insertion order (stable).
 * See ARCHITECTURE.md §4.2 Step 2.
 */
typedef struct strand_timer_entry {
	uint64_t deadline_ns;
	strand_fiber_t *fiber;
} strand_timer_entry_t;

/*
 * strand_scheduler_t - complete single-worker fiber scheduler.
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
 *   - The inject queue has its own memory-ordering protocol.
 *
 * Wakeup fd:
 *   - Linux: eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK) - single int.
 *   - OpenBSD: pipe2([0]=read, [1]=write, O_CLOEXEC | O_NONBLOCK).
 *   - strand_scheduler_stop writes one byte to the write side to interrupt
 *     a blocked epoll_wait/kevent.  Step 3 of advance drains and discards
 *     all bytes.  See ARCHITECTURE.md §4.2.
 */
typedef struct strand_scheduler {
	/*
	 * Scheduler's own execution context.  When running inside advance,
	 * context switches save here and resume the scheduler after the fiber
	 * returns or yields.  stack_base and stack_size are 0/NULL - the
	 * scheduler runs on the OS thread stack and does not own a fiber stack.
	 */
	strand_fiber_t scheduler_ctx;

	/* Run queue - FIFO intrusive singly-linked list via fiber->next.
	 * run_head is the next fiber to run; run_tail is where new fibers
	 * are appended.  Both are NULL when the queue is empty. */
	strand_fiber_t *run_head;
	strand_fiber_t *run_tail;
	size_t run_queue_len;

	/* Budget - max fibers to run per advance call.  Default 64. */
	size_t budget;

	/* Currently executing fiber.  NULL when control is in the scheduler. */
	strand_fiber_t *current_fiber;

	/* Timer min-heap - binary heap keyed on deadline_ns (ascending).
	 * Dynamically allocated; grows by doubling when full. */
	strand_timer_entry_t *timer_heap;
	size_t timer_heap_len;
	size_t timer_heap_cap;

	/* Wakeup fd - used by strand_scheduler_stop to interrupt a blocked
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

	/* Inject queue - bounded MPSC ring buffer.
	 * Capacity set from cfg->inject_cap at create time.
	 * See ARCHITECTURE.md §6.3 and strand_inject.c. */
	strand_inject_queue_t inject_queue;

	/* I/O poller.  See ARCHITECTURE.md §5. */
	strand_poller_t *poller;

	/* Stack cache - LIFO, per-scheduler.  Only stacks of matching size
	 * are cached; non-matching stacks are munmap'd immediately.
	 * See ARCHITECTURE.md §13 for cap, floor, and idle reclamation. */
	strand_stack_slot_t *stack_cache;
	size_t cache_len;

	/*
	 * Pending stack free - written by a completing fiber (via
	 * t35_switch_back in tests; via the trampoline in production) to
	 * communicate its stack back to the scheduler.  The fiber MUST NOT
	 * call munmap on its own stack while still executing on it; instead
	 * it stores the mmap base/size/vg_id here and the scheduler calls
	 * sched_stack_free immediately after the context switch returns
	 * (Step 5 of strand_scheduler_advance).
	 * NULL in pending_free_base means no pending free.
	 * SAFETY: only ever written from within the running fiber (single
	 * threaded per scheduler) and read/cleared by the scheduler on the
	 * same thread immediately after the switch - no synchronisation
	 * needed.
	 */
	void         *pending_free_base;
	size_t        pending_free_size;
	unsigned long pending_free_vg_id;
	/*
	 * Pending fiber-local destructor - copied from the completing fiber's
	 * local_dtor/local_ptr before the context switch so the scheduler can
	 * invoke the destructor after the switch, while on its own stack.
	 * pending_free_local_dtor == NULL means no destructor to call.
	 */
	void                *pending_free_local_ptr;
	strand_destructor_t  pending_free_local_dtor;
	size_t cache_cap;
	size_t cache_floor;
	uint64_t cache_idle_ns; /* idle reclamation timeout (ns) */
	uint64_t last_idle_ns;  /* last observed idle timestamp */

	/* Dead pool - reusable strand_fiber_t descriptors linked via next.
	 * fiber_alloc pops from here when available; fiber_free pushes here. */
	strand_fiber_t *dead_pool;

	/*
	 * Debug watchdog threshold (nanoseconds).  In STRAND_DEBUG builds,
	 * if a fiber runs for longer than this without yielding, a warning
	 * is emitted to stderr.  Default STRAND_DEFAULT_WATCHDOG_NS (100 ms).
	 * Watchdog does not preempt - warning only.
	 * See ARCHITECTURE.md §11.2.
	 */
	uint64_t watchdog_threshold_ns;

	/* Owner thread for same-worker operations.
	 * Set at create and refreshed by scheduler entry points. */
	pthread_t owner_thread;
} strand_scheduler_t;

/*
 * strand_worker_t - one worker thread and its scheduler.
 *
 * sched    - heap-allocated scheduler owned by this worker.
 * thread   - the OS thread running strand_scheduler_run.
 * runtime  - back-pointer to the owning runtime (for shutdown checks).
 *
 * Thread safety: read-only after strand_worker_start returns.
 * strand_worker_stop / strand_worker_join may be called from any thread.
 */
typedef struct strand_worker {
        strand_scheduler_t *sched;
        pthread_t           thread;
        struct strand_runtime *runtime;
        /*
         * joined - set to 1 after pthread_join completes on this worker.
         * Prevents strand_runtime_destroy from double-joining a worker
         * that was already joined by strand_worker_join.
         * Protected by the runtime spinlock during destroy; set by join.
         */
        _Atomic int         joined;
} strand_worker_t;

/*
 * strand_runtime_t - multi-worker runtime registry.
 *
 * workers[]      - fixed-size array of registered worker pointers.
 * worker_count   - number of registered workers; protected by spinlock.
 * workers_cap    - capacity of workers[] (set at init from max_workers).
 * spinlock       - CAS-based spinlock protecting workers[] and worker_count.
 *                  Round-robin counter is accessed WITHOUT the spinlock.
 * rr_counter     - _Atomic round-robin position; incremented on each
 *                  host-thread spawn; mod worker_count gives the slot.
 *                  Accessed without the spinlock - a torn read at best
 *                  selects a suboptimal worker, not a wrong one.
 * shutdown_flag  - set atomically when the first worker is stopped.
 *                  All spawn paths check this before proceeding.
 *
 * See ARCHITECTURE.md §6.1, §6.2, §6.4.
 * See ARCHITECTURE.md §6.1, §6.2, §6.4.
 */
typedef struct strand_runtime {
	strand_worker_t   **workers;
	size_t              worker_count;
	size_t              workers_cap;
	/*
	 * SAFETY: spinlock protects workers[] and worker_count.
	 * Acquire before reading or modifying either; release after.
	 * rr_counter and shutdown_flag are _Atomic and accessed lock-free.
	 */
	_Atomic int         spinlock;
	_Atomic uint32_t    rr_counter;
	_Atomic int         shutdown_flag;
} strand_runtime_t;

#endif /* STRAND_INTERNAL_H */
