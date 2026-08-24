/*
 * strand_fiber.c - fiber descriptor lifecycle, stack cache, fiber-local
 * storage. See ARCHITECTURE.md §4.5, §13.
 */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include "strand_context.h"
#include "strand_fiber.h"
#include "strand_internal.h"
#include "strand_inject.h"
#include "strand_sched.h"
#include "strand_poller.h"
#include "strand_offload.h"

/*
 * page_size() - system page size, cached after the first call.
 * sysconf(_SC_PAGESIZE) is idempotent; the one-time race in a multi-threaded
 * context is safe because both threads would write the same value.
 */
size_t
page_size(void)
{
	static size_t cached;
	long v;

	if (cached != 0)
		return cached;

	v = sysconf(_SC_PAGESIZE);
	if (v <= 0)
		return 0;

	cached = (size_t)v;
	return cached;
}

/*
 * stack_alloc - allocate a fiber stack with a guard page.
 *
 * Layout of the mmap region (low address to high address):
 *
 *   [guard page - PROT_NONE][usable stack - PROT_READ|PROT_WRITE]
 *   <-- PAGE_SIZE ----------><-- stack_size ---------------------->
 *
 * The stack grows downward; the fiber uses the top of the usable region.
 * Returns the mmap base on success, NULL on failure.
 * *vg_id_out receives the Valgrind stack registration ID.
 *
 * The caller is responsible for storing both the returned base pointer
 * and the Valgrind ID for later use with stack_free.  The usable base
 * (suitable for strand_context_init stack_top) is base + PAGE_SIZE +
 * stack_size (one past the top of the usable region).
 */
void *
stack_alloc(size_t stack_size, unsigned long *vg_id_out)
{
	size_t pgsz = page_size();
	size_t total;
	void *base;
	int mmap_flags;

	if (vg_id_out == NULL || pgsz == 0 || stack_size == 0)
		return NULL;
	if (SIZE_MAX - pgsz < stack_size)
		return NULL;
	total = stack_size + pgsz;
	mmap_flags = MAP_ANONYMOUS | MAP_PRIVATE;
#ifdef MAP_STACK
	/*
	 * Some kernels (notably OpenBSD arm64) enforce additional checks for
	 * SP-backed mappings. Marking the region as MAP_STACK avoids faults
	 * when this mapping is used as an execution stack.
	 */
	mmap_flags |= MAP_STACK;
#endif

	base = mmap(NULL, total, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
	if (base == MAP_FAILED)
		return NULL;

	if (mprotect(base, pgsz, PROT_NONE) != 0) {
		munmap(base, total);
		return NULL;
	}

	/*
	 * Register the usable portion - from just above the guard page to
	 * the top of the mapped region - with Valgrind.  This is a no-op when
	 * not running under Valgrind.  See TECH_STACK.md §7.1.
	 */
	*vg_id_out = STRAND_VG_STACK_REGISTER((char *)base + pgsz,
	                                      (char *)base + pgsz + stack_size);

	return base;
}

/*
 * stack_free - deregister the fiber stack from Valgrind and unmap it.
 *
 * base must be the mmap base returned by stack_alloc (not the usable base).
 * vg_id must be the Valgrind ID returned via stack_alloc's vg_id_out.
 */
void
stack_free(void *base, size_t stack_size, unsigned long vg_id)
{
	size_t pgsz = page_size();

	if (base == NULL || pgsz == 0 || stack_size == 0)
		return;
	if (SIZE_MAX - pgsz < stack_size)
		return;

	STRAND_VG_STACK_DEREGISTER(vg_id);
	munmap(base, stack_size + pgsz);
}
/*
 * strand_fiber_tsan_init - initialise the TSan fiber handle on a newly
 * allocated strand_fiber_t.  Must be called once per descriptor before any
 * context switch involving this fiber.  In non-TSan builds the macro is a
 * no-op.
 * See ARCHITECTURE.md §3.7 and TECH_STACK.md §7.3.
 */
void
strand_fiber_tsan_init(strand_fiber_t *f)
{
	if (f == NULL)
		return;

	f->tsan_fiber = NULL;
	STRAND_TSAN_CREATE(f);
}

/*
 * strand_fiber_tsan_destroy - destroy the TSan handle when a strand_fiber_t is
 * being freed or returned to the dead pool.  Must be called after the fiber
 * has finished and will never be switched to again.  In non-TSan builds the
 * macro is a no-op.
 * See ARCHITECTURE.md §3.7 and TECH_STACK.md §7.3.
 */
void
strand_fiber_tsan_destroy(strand_fiber_t *f)
{
	if (f == NULL)
		return;

	if (f->tsan_fiber != NULL)
		STRAND_TSAN_DESTROY(f);
	f->tsan_fiber = NULL;
}

/*
 * strand_fiber_tsan_bind_current - bind descriptor to current thread/fiber
 * TSan context. This is for scheduler/root contexts that represent the
 * currently running thread rather than a separately created fiber context.
 */
void
strand_fiber_tsan_bind_current(strand_fiber_t *f)
{
	if (f == NULL)
		return;

	f->tsan_fiber = NULL;
	STRAND_TSAN_BIND_CURRENT(f);
}

/*
 * strand_fiber_entry_start -- internal entry wrapper for spawned fibers.
 *
 * Runs the user entry function, then performs scheduler handoff so a normal
 * return from user code is treated as fiber completion instead of trapping in
 * the architecture trampoline guard instruction.
 */
void
strand_fiber_entry_start(void *varg)
{
	strand_fiber_t *f = (strand_fiber_t *)varg;
	strand_scheduler_t *sched;

	STRAND_DEBUG_ASSERT(f != NULL);
	sched = f->home_sched;
	STRAND_DEBUG_ASSERT(sched != NULL);

	if (f->entry_fn != NULL)
		f->entry_fn(f->entry_arg);

	/*
	 * ATOMIC: state transition to FINISHED is seq_cst by project default
	 * ordering for shared atomics (CODING_STANDARDS.md §4.1).
	 */
	atomic_store(&f->state, FIBER_FINISHED);

	/*
	 * Completion handoff mirrors test helper t35_switch_back: defer stack and
	 * fiber-local destructor processing to scheduler Step 5 after switch-back.
	 */
	sched->pending_free_base = (char *)f->stack_base - page_size();
	sched->pending_free_size = f->stack_size;
	sched->pending_free_vg_id = f->valgrind_stack_id;
	sched->pending_free_local_ptr = f->local_ptr;
	sched->pending_free_local_dtor = f->local_dtor;

	fiber_free(&sched->dead_pool, f);
	strand_context_switch(f, &sched->scheduler_ctx);

	STRAND_DEBUG_ASSERT(0);
	for (;;)
		;
}

/*
 * sched_stack_alloc - cache-aware stack allocator.
 *
 * Checks the top cache slot first.  If it has a matching size, pops and
 * returns it (LIFO cache hit).  Otherwise calls the raw stack_alloc.
 * Only the top slot is checked - different-sized stacks cannot be used
 * interchangeably (ARCHITECTURE.md §13).
 * See ARCHITECTURE.md §13.
 */
void *
sched_stack_alloc(strand_scheduler_t *sched, size_t stack_size,
                  unsigned long *vg_id_out)
{
	if (sched != NULL && sched->cache_len > 0) {
		strand_stack_slot_t *top =
		    &sched->stack_cache[sched->cache_len - 1];
		if (top->stack_size == stack_size) {
			/* Cache hit: pop the top entry (LIFO). */
			void *base = top->base;
			*vg_id_out = top->vg_id;
			sched->cache_len--;
			return (base);
		}
	}
	/* Cache miss or no scheduler: fall through to raw mmap. */
	return (stack_alloc(stack_size, vg_id_out));
}

/*
 * sched_stack_free - cache-aware stack release.
 *
 * Pushes the stack to the cache top (LIFO) if space is available.
 * If the cache is at capacity, calls stack_free immediately (overflow
 * policy: discard the incoming stack - ARCHITECTURE.md §13.3).
 * base must be the mmap base (includes the guard page at the low end).
 * See ARCHITECTURE.md §13.
 */
void
sched_stack_free(strand_scheduler_t *sched, void *base, size_t stack_size,
                 unsigned long vg_id)
{
	if (sched != NULL && sched->cache_len < sched->cache_cap) {
		strand_stack_slot_t *slot = &sched->stack_cache[sched->cache_len];
		slot->base = base;
		slot->stack_size = stack_size;
		slot->vg_id = vg_id;
		sched->cache_len++;
		return;
	}
	/* Cache full (overflow) or no scheduler: unmap immediately. */
	stack_free(base, stack_size, vg_id);
}


/*
 * Dead-pool path: pops the head entry from *dead_pool, increments the
 * generation counter (ABA protection), zeros the entire descriptor, then
 * writes the new generation back.  Outstanding handles with the old
 * generation will correctly return STRAND_HANDLE_STALE.
 *
 * Fresh-alloc path: malloc() + memset to zero + generation = 1.
 *
 * In both cases the caller must initialise context, stack, and state
 * before making the fiber runnable.
 *
 * Returns NULL on malloc failure; the dead-pool path never fails.
 * See ARCHITECTURE.md §4.6.
 */
strand_fiber_t *
fiber_alloc(strand_fiber_t **dead_pool)
{
	strand_fiber_t *f;

	if (dead_pool != NULL && *dead_pool != NULL) {
		uint64_t gen;

		f = *dead_pool;
		*dead_pool = f->next;
		/*
		 * Increment generation before zeroing: any handles with the
		 * old value will return STALE.  Store on the stack so the
		 * memset can safely clear the whole descriptor.
		 */
		gen = f->generation + 1;
		memset(f, 0, sizeof(*f));
		f->generation = gen;
		return (f);
	}

	f = malloc(sizeof(strand_fiber_t));
	if (f == NULL)
		return (NULL);
	memset(f, 0, sizeof(*f));
	f->generation = 1;
	return (f);
}

/*
 * fiber_free - return a finished fiber descriptor to the dead pool.
 *
 * The fiber stack must be freed via stack_free() before this call.
 * The next pointer is repurposed as the dead-pool intrusive link.
 * Generation is NOT incremented here; that happens at the next
 * fiber_alloc when the descriptor is taken from the pool.
 *
 * If dead_pool is NULL the descriptor is freed immediately via free().
 * See ARCHITECTURE.md §4.6.
 */
void
fiber_free(strand_fiber_t **dead_pool, strand_fiber_t *f)
{
	if (f == NULL)
		return;

	if (dead_pool != NULL) {
		f->next = *dead_pool;
		*dead_pool = f;
	} else {
		free(f);
	}
}

/* ---------------------------------------------------------------------------
 * strand_fiber_spawn - spawn a new fiber on a scheduler.
 *
 * Within-worker spawn path.  Host-thread spawning uses the inject queue
 * (see strand_runtime.c).
 *
 * Returns STRAND_OK on success; *out receives the handle.
 * Returns STRAND_ERR_SHUTDOWN if the scheduler has been stopped.
 * Returns STRAND_ERR_WRONGCTX if called from the host thread
 *   (sched->current_fiber is NULL - i.e. control is not inside a fiber).
 * Returns STRAND_ERR_NOMEM on allocation failure.
 * See ARCHITECTURE.md §4.5 and §4.6.
 * ---------------------------------------------------------------------------
 */
int
strand_fiber_spawn(strand_scheduler_t *sched, strand_fiber_fn_t fn, void *arg,
                   size_t stack_sz, strand_fiber_handle_t *out)
{
	strand_fiber_t *f;
	void *base;
	unsigned long vg_id;
	char *stack_top;
	size_t sz;

	if (atomic_load_explicit(&sched->stop_flag, memory_order_acquire))
		return (STRAND_ERR_SHUTDOWN);

	/*
	 * Only same-worker (within-fiber) spawning is supported here.
	 * current_fiber is NULL when the caller is the host thread / scheduler
	 * context.  Host-thread spawning uses the inject queue.
	 * Reading current_fiber without a lock is safe in the
	 * single-worker model.
	 */
	if (sched->current_fiber == NULL)
		return (STRAND_ERR_WRONGCTX);

	sz = (stack_sz != 0) ? stack_sz : STRAND_DEFAULT_STACK_SIZE;

	f = fiber_alloc(&sched->dead_pool);
	if (f == NULL)
		return (STRAND_ERR_NOMEM);

	base = sched_stack_alloc(sched, sz, &vg_id);
	if (base == NULL) {
		fiber_free(&sched->dead_pool, f);
		return (STRAND_ERR_NOMEM);
	}

	f->stack_base = (char *)base + page_size();
	f->stack_size = sz;
	f->valgrind_stack_id = vg_id;
	f->entry_fn = fn;
	f->entry_arg = arg;

	stack_top = (char *)f->stack_base + sz;
	strand_context_init(&f->context, stack_top, strand_fiber_entry_start, f);

	strand_fiber_tsan_init(f);

	f->home_sched = sched;
	atomic_store(&f->state, FIBER_NEW);
	atomic_store(&f->state, FIBER_RUNNABLE);

	run_queue_push(sched, f);

	if (out != NULL) {
		out->ptr = f;
		out->generation = f->generation;
	}

        return (STRAND_OK);
}

/* ---------------------------------------------------------------------------
 * strand_fiber_spawn_detached - spawn a fiber with no scope tracking.
 *
 * Identical to strand_fiber_spawn except f->scope is explicitly NULL.
 * fiber_alloc already zeroes the descriptor, so NULL is the default; this
 * function exists to make the explicit opt-out visible at the call site.
 *
 * STRONGLY DISCOURAGED.  Detached fibers are not tracked by any scope.
 * Errors are not propagated.  The fiber's lifetime is not bounded by any
 * enclosing scope.  Use of detached fibers makes correctness reasoning
 * significantly harder and precludes structured cleanup.
 * See ARCHITECTURE.md §7.7.
 *
 * Returns STRAND_OK on success.
 * Returns STRAND_ERR_SHUTDOWN if the scheduler has been stopped.
 * Returns STRAND_ERR_WRONGCTX if called from the host thread.
 * Returns STRAND_ERR_NOMEM on allocation failure.
 * ---------------------------------------------------------------------------
 */
int
strand_fiber_spawn_detached(strand_scheduler_t *sched, strand_fiber_fn_t fn,
                            void *arg, size_t stack_sz,
                            strand_fiber_handle_t *out)
{
	strand_fiber_t *f;
	void           *base;
	unsigned long   vg_id;
	char           *stack_top;
	size_t          sz;

	if (atomic_load_explicit(&sched->stop_flag, memory_order_acquire))
		return (STRAND_ERR_SHUTDOWN);

	if (sched->current_fiber == NULL)
		return (STRAND_ERR_WRONGCTX);

	sz = (stack_sz != 0) ? stack_sz : STRAND_DEFAULT_STACK_SIZE;

	f = fiber_alloc(&sched->dead_pool);
	if (f == NULL)
		return (STRAND_ERR_NOMEM);

	base = sched_stack_alloc(sched, sz, &vg_id);
	if (base == NULL) {
		fiber_free(&sched->dead_pool, f);
		return (STRAND_ERR_NOMEM);
	}

	f->stack_base        = (char *)base + page_size();
	f->stack_size        = sz;
	f->valgrind_stack_id = vg_id;
	f->entry_fn          = fn;
	f->entry_arg         = arg;
	f->scope             = NULL; /* explicit: detached, no scope tracking */

	stack_top = (char *)f->stack_base + sz;
	strand_context_init(&f->context, stack_top, strand_fiber_entry_start, f);

	strand_fiber_tsan_init(f);

	f->home_sched = sched;
	atomic_store(&f->state, FIBER_NEW);
	atomic_store(&f->state, FIBER_RUNNABLE);

	run_queue_push(sched, f);

	if (out != NULL) {
		out->ptr        = f;
		out->generation = f->generation;
	}

	return (STRAND_OK);
}

/* ---------------------------------------------------------------------------
 * strand_fiber_yield - voluntarily yield the current fiber.
 *
 * Transitions the calling fiber FIBER_RUNNING -> FIBER_RUNNABLE, appends it
 * to the run queue tail, then switches back to the scheduler.  Control
 * returns here when the scheduler next picks this fiber from the run queue.
 *
 * Must be called from inside a running fiber.  Debug builds assert this.
 * See ARCHITECTURE.md §4.5 and §11.2.
 * ---------------------------------------------------------------------------
 */
void
strand_fiber_yield(strand_scheduler_t *sched)
{
	strand_fiber_t *f;

	STRAND_DEBUG_ASSERT(sched != NULL);
	STRAND_DEBUG_ASSERT(sched->current_fiber != NULL);

	f = sched->current_fiber;

	/*
	 * Transition FIBER_RUNNING -> FIBER_RUNNABLE before re-queuing.
	 * The state is relaxed here: the scheduler, which is the only
	 * other observer, will see the updated state after the context
	 * switch returns (which implies a sequencing point).
	 */
	atomic_store_explicit(&f->state, FIBER_RUNNABLE, memory_order_relaxed);

	/* Append to tail so that the fiber is not re-run until all currently
	 * queued fibers have had a turn (FIFO fairness). */
	run_queue_push(sched, f);

	/* Switch back to the scheduler.  Execution resumes here when the
	 * scheduler next picks this fiber from the run queue. */
	strand_context_switch(f, &sched->scheduler_ctx);
}

/* ---------------------------------------------------------------------------
 * strand_fiber_sleep_until - park the current fiber until a deadline.
 *
 * Transitions FIBER_RUNNING -> FIBER_PARKED_TIMER, inserts (deadline_ns, f)
 * into the timer heap, then switches back to the scheduler.  Execution
 * resumes here when the scheduler's Step 2 pops the expired entry from the
 * heap and puts the fiber back into the run queue.
 *
 * On return, the cancel_pending flag is checked and cleared.  If it was set
 * (by a concurrent strand_fiber_cancel), STRAND_CANCELLED is
 * returned.  Otherwise STRAND_OK.
 *
 * Must be called from inside a running fiber.  Debug builds assert this.
 * See ARCHITECTURE.md §4.5 and §8.1.
 * ---------------------------------------------------------------------------
 */
int
strand_fiber_sleep_until(strand_scheduler_t *sched, uint64_t deadline_ns)
{
	strand_fiber_t *f;
	int cancelled;

	STRAND_DEBUG_ASSERT(sched != NULL);
	STRAND_DEBUG_ASSERT(sched->current_fiber != NULL);

	f = sched->current_fiber;

	/*
	 * Transition FIBER_RUNNING -> FIBER_PARKED_TIMER.
	 * The relaxed store is safe here: visibility to other threads is
	 * provided by the context switch that follows.
	 */
	atomic_store_explicit(&f->state, FIBER_PARKED_TIMER,
	                      memory_order_relaxed);

	/*
	 * Insert into the timer min-heap.  On allocation failure the fiber
	 * cannot park - return an error without switching context.
	 * (Extremely rare; heap only grows on capacity increase.)
	 */
	if (timer_heap_push(sched, deadline_ns, f) != 0) {
		atomic_store_explicit(&f->state, FIBER_RUNNING,
		                      memory_order_relaxed);
		return (STRAND_ERR_NOMEM);
	}

	/* Switch back to the scheduler.  Resumes here at timer expiry or
	 * when strand_fiber_cancel moves this fiber to FIBER_RUNNABLE. */
	strand_context_switch(f, &sched->scheduler_ctx);

	/*
	 * Check and clear the cancellation flag atomically.
	 * If strand_fiber_cancel set this flag before we resumed,
	 * return STRAND_CANCELLED so the caller can propagate cancellation.
	 * The exchange clears the flag so subsequent park calls start clean.
	 */
	cancelled = atomic_exchange_explicit(&f->cancel_pending, 0,
	                                     memory_order_acq_rel);
	return (cancelled ? STRAND_CANCELLED : STRAND_OK);
}

/* ---------------------------------------------------------------------------
 * strand_fiber_cancel - cancel a fiber by ABA-safe handle.
 *
 * Handles timer, run-queue, I/O-parked, offload-parked, and scope-parked
 * states.
 * See ARCHITECTURE.md §8.1 and the doc comment in include/strand.h.
 * ---------------------------------------------------------------------------
 */
int
strand_fiber_cancel(strand_fiber_handle_t handle)
{
	strand_fiber_t *f;
	strand_scheduler_t *sched;
	fiber_state_t state;
	int rc;

	rc = fiber_handle_validate(handle);
	if (rc != STRAND_OK)
		return (rc);

	f = handle.ptr;
	sched = f->home_sched;

	if (sched == NULL)
		return (STRAND_ERR_WRONGCTX);
	if (!pthread_equal(pthread_self(), atomic_load_explicit(
	    &sched->owner_thread, memory_order_acquire))) {
		inject_item_t item;

		/*
		 * SAFETY: cross-worker cancel is enqueue-only.  The request is
		 * pushed to the target scheduler's inject queue and processed in
		 * Step 1 of strand_scheduler_advance on the owning worker.
		 * Post-return fd-freedom guarantees do not apply until processed.
		 * See ARCHITECTURE.md §5.3.
		 */
		item.type = INJECT_CANCEL;
		item.u.cancel_handle = handle;
		if (!scheduler_inject_acquire(sched))
			return (STRAND_ERR_SHUTDOWN);
		if (inject_queue_try_push_release(&sched->inject_queue, &item) != 0) {
			scheduler_inject_release(sched);
			return (STRAND_EAGAIN);
		}
		scheduler_inject_release(sched);
		/* Wake the owner worker so Step 1 drains the item promptly. */
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
	 * Load the state once.  The fiber is pinned to this worker; no
	 * concurrent state change is possible on the same-worker path.
	 * Cross-worker cancellation uses the inject queue path above.
	 */
	/* ATOMIC: seq_cst default for shared state loads (CODING_STANDARDS.md §4.1). */
	state = atomic_load(&f->state);

	switch (state) {
	case FIBER_PARKED_TIMER:
		/*
		 * Remove from the timer heap so the fiber will not be woken
		 * by timer expiry.  Set cancel_pending so sleep_until returns
		 * STRAND_CANCELLED.  Transition to RUNNABLE and enqueue.
		 */
		timer_heap_remove(sched, f);
		atomic_store(&f->cancel_pending, 1);
		atomic_store(&f->state, FIBER_RUNNABLE);
		run_queue_push(sched, f);
		break;

	case FIBER_RUNNABLE:
	case FIBER_RUNNING:
		/*
		 * Set the FIBER_CANCELLATION_PENDING flag.  The fiber remains
		 * in the run queue (RUNNABLE) or continues executing (RUNNING)
		 * and must check cancel_pending at its next park point.
		 */
		atomic_store(&f->cancel_pending, 1);
		break;

	case FIBER_FINISHED:
	case FIBER_NEW:
		/* No-op - fiber is not in a cancellable state. */
		break;

	case FIBER_PARKED_IO_READ:
	case FIBER_PARKED_IO_WRITE:
		/*
		 * Remove the fiber from the fd waiter table, adjust the OS
		 * registration (MOD to remaining direction or DEL), and wake
		 * the fiber with STRAND_CANCELLED.
		 * See ARCHITECTURE.md §5.8.
		 */
		poller_cancel_io(sched, f);
		break;

        case FIBER_PARKED_OFFLOAD:
                /*
                 * ATOMIC: CAS(PENDING -> CANCELLED) with seq_cst.
                 * This races with the offload thread's CAS(PENDING -> RESULT_CLAIMED).
                 * Exactly one side wins.  See ARCHITECTURE.md 6.6.
                 *
                 * If CANCELLED wins:
                 *   - Set cancel_pending so strand_fiber_offload returns
                 *     STRAND_CANCELLED on resume.
                 *   - Transition fiber to FIBER_RUNNABLE and push to run queue.
                 *   - Decrement fiber-side refcount (offload_item_release).
                 *     The offload thread will still run fn to completion but
                 *     will skip the result_slot write and inject.
                 *
                 * If RESULT_CLAIMED already won:
                 *   - DO NOT touch refcount.  The fiber-side reference is still
                 *     live; strand_fiber_offload will release it on resume.
                 *   - DO NOT push the fiber - the inject will do that.
                 *
                 * ATOMIC: the fiber's home_sched is already this worker (same-
                 * worker path).  Accessing f->offload_item requires that the
                 * item pointer was set before FIBER_PARKED_OFFLOAD was stored
                 * (guaranteed by strand_fiber_offload ordering).
                 */
                {
                        strand_offload_item_t *oi = f->offload_item;
                        offload_state_t expected_state = OFFLOAD_PENDING;

                        /*
                         * offload_item is set by strand_fiber_offload before
                         * transitioning to PARKED_OFFLOAD (same worker thread,
                         * no race).  SAFETY: same-worker cancel path only.
                         */
                        if (oi == NULL)
                                break; /* defensive; should not happen */

                        if (atomic_compare_exchange_strong_explicit(
                                &oi->state,
                                &expected_state,
                                OFFLOAD_CANCELLED,
                                memory_order_seq_cst,
                                memory_order_seq_cst)) {
                                /*
                                 * CANCELLED CAS won.  Wake fiber with cancel
                                 * result and release fiber-side refcount.
                                 */
                                atomic_store(&f->cancel_pending, 1);
                                atomic_store(&f->state, FIBER_RUNNABLE);
                                run_queue_push(sched, f);
                                offload_item_release(oi);
                        }
                        /*
                         * else: RESULT_CLAIMED already won.
                         * ATOMIC: RESULT_CLAIMED wins; fiber-side reference is
                         * still live and will be released by strand_fiber_offload
                         * on resume.  Do not touch refcount here.
                         * The inject completion will push the fiber to RUNNABLE.
                         */
                }
                break;

        case FIBER_PARKED_CHANNEL:
                /*
                 * Placeholder - handled when channels are implemented.
                 */
                break;

        case FIBER_PARKED_SCOPE:
                /*
                 * Fiber is parked in strand_scope_wait.  Wake it with
                 * cancel_pending set so scope_wait returns STRAND_CANCELLED.
                 * See ARCHITECTURE.md §7.4.
                 */
                atomic_store(&f->cancel_pending, 1);
                atomic_store(&f->state, FIBER_RUNNABLE);
                run_queue_push(sched, f);
                break;
	}

	return (STRAND_OK);
}

/* ---------------------------------------------------------------------------
 * strand_fiber_local_set - store a fiber-local pointer with destructor.
 *
 * Sets local_ptr and local_dtor on the currently running fiber.  The
 * scheduler calls the destructor (if non-NULL) with the stored pointer
 * immediately after the fiber completes and the context switch returns -
 * see the pending_free processing block in strand_scheduler_advance.
 *
 * Must be called from inside a fiber.  Debug builds assert; release builds
 * treat a non-fiber call as a no-op.
 * See ARCHITECTURE.md §4.7.
 * ---------------------------------------------------------------------------
 */
void
strand_fiber_local_set(strand_scheduler_t *sched, void *ptr,
                       strand_destructor_t dtor)
{
	strand_fiber_t *f;

	if (sched == NULL)
		return;

	if (sched->current_fiber == NULL) {
		STRAND_DEBUG_ASSERT(0 && "strand_fiber_local_set called from non-fiber context");
		return;
	}

	f = sched->current_fiber;
	f->local_ptr  = ptr;
	f->local_dtor = dtor;
}

/* ---------------------------------------------------------------------------
 * strand_fiber_local_get - retrieve the fiber-local pointer.
 *
 * Returns current_fiber->local_ptr.  Returns NULL if called from the host
 * thread (debug builds assert).
 * See ARCHITECTURE.md §4.7.
 * ---------------------------------------------------------------------------
 */
void *
strand_fiber_local_get(strand_scheduler_t *sched)
{
	if (sched == NULL)
		return (NULL);

	if (sched->current_fiber == NULL) {
		STRAND_DEBUG_ASSERT(0 && "strand_fiber_local_get called from non-fiber context");
		return (NULL);
	}

	return (sched->current_fiber->local_ptr);
}
