/*
 * strand_fiber.c — fiber descriptor lifecycle, stack cache, fiber-local
 * storage. See ARCHITECTURE.md §4.5, §13.
 */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "strand_context.h"
#include "strand_fiber.h"
#include "strand_internal.h"
#include "strand_sched.h"

/*
 * page_size() — system page size, cached after the first call.
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
 * stack_alloc — allocate a fiber stack with a guard page.
 *
 * Layout of the mmap region (low address to high address):
 *
 *   [guard page — PROT_NONE][usable stack — PROT_READ|PROT_WRITE]
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
	 * Register the usable portion — from just above the guard page to
	 * the top of the mapped region — with Valgrind.  This is a no-op when
	 * not running under Valgrind.  See TECH_STACK.md §7.1.
	 */
	*vg_id_out = STRAND_VG_STACK_REGISTER((char *)base + pgsz,
	                                      (char *)base + pgsz + stack_size);

	return base;
}

/*
 * stack_free — deregister the fiber stack from Valgrind and unmap it.
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
 * strand_fiber_tsan_init — initialise the TSan fiber handle on a newly
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
 * strand_fiber_tsan_destroy — destroy the TSan handle when a strand_fiber_t is
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
 * strand_fiber_tsan_bind_current — bind descriptor to current thread/fiber
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
 * fiber_alloc — allocate a fiber descriptor from the dead pool or malloc.
 *
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
 * fiber_free — return a finished fiber descriptor to the dead pool.
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
 * strand_fiber_spawn — spawn a new fiber on a scheduler.
 *
 * Phase 3 within-worker path only.  Host-thread spawning (via the inject
 * queue) is added in Phase 5 (Task 5.4).
 *
 * Returns STRAND_OK on success; *out receives the handle.
 * Returns STRAND_ERR_SHUTDOWN if the scheduler has been stopped.
 * Returns STRAND_ERR_WRONGCTX if called from the host thread
 *   (sched->current_fiber is NULL — i.e. control is not inside a fiber).
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
	 * Phase 3: only same-worker (within-fiber) spawning is supported.
	 * current_fiber is NULL when the caller is the host thread / scheduler
	 * context.  Host-thread spawning via the inject queue is Task 5.4.
	 * Reading current_fiber without a lock is safe in Phase 3's
	 * single-worker model.
	 */
	if (sched->current_fiber == NULL)
		return (STRAND_ERR_WRONGCTX);

	sz = (stack_sz != 0) ? stack_sz : STRAND_DEFAULT_STACK_SIZE;

	f = fiber_alloc(&sched->dead_pool);
	if (f == NULL)
		return (STRAND_ERR_NOMEM);

	base = stack_alloc(sz, &vg_id);
	if (base == NULL) {
		fiber_free(&sched->dead_pool, f);
		return (STRAND_ERR_NOMEM);
	}

	f->stack_base = (char *)base + page_size();
	f->stack_size = sz;
	f->valgrind_stack_id = vg_id;

	stack_top = (char *)f->stack_base + sz;
	strand_context_init(&f->context, stack_top, fn, arg);

	strand_fiber_tsan_init(f);

	atomic_store_explicit(&f->state, FIBER_NEW, memory_order_relaxed);
	atomic_store_explicit(&f->state, FIBER_RUNNABLE, memory_order_relaxed);

	run_queue_push(sched, f);

	if (out != NULL) {
		out->ptr = f;
		out->generation = f->generation;
	}

	return (STRAND_OK);
}

/* ---------------------------------------------------------------------------
 * strand_fiber_yield — voluntarily yield the current fiber.
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
 * strand_fiber_sleep_until — park the current fiber until a deadline.
 *
 * Transitions FIBER_RUNNING -> FIBER_PARKED_TIMER, inserts (deadline_ns, f)
 * into the timer heap, then switches back to the scheduler.  Execution
 * resumes here when the scheduler's Step 2 pops the expired entry from the
 * heap and puts the fiber back into the run queue.
 *
 * On return, the cancel_pending flag is checked and cleared.  If it was set
 * (by a concurrent strand_fiber_cancel — Task 3.8), STRAND_CANCELLED is
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
	 * cannot park — return an error without switching context.
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
	 * If strand_fiber_cancel (Task 3.8) set this flag before we resumed,
	 * return STRAND_CANCELLED so the caller can propagate cancellation.
	 * The exchange clears the flag so subsequent park calls start clean.
	 */
	cancelled = atomic_exchange_explicit(&f->cancel_pending, 0,
	                                     memory_order_acq_rel);
	return (cancelled ? STRAND_CANCELLED : STRAND_OK);
}

