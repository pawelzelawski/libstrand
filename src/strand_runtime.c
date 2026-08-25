/*
 * strand_runtime.c - multi-worker runtime: worker registry and lifecycle.
 *
 * Implements Tasks 5.2, 5.3, 5.4:
 *   5.2 - strand_runtime_t struct, init/destroy, spinlock, shutdown_flag
 *   5.3 - strand_worker_start: scheduler + pthread; CPU affinity on Linux
 *   5.4 - strand_runtime_spawn: host-thread fiber spawn via inject queue
 *
 * Design decisions:
 *   - Worker list is a heap-allocated pointer array; size fixed at init.
 *   - Spinlock is a CAS-based _Atomic int - held only at spawn time
 *     (low-frequency host-thread path); no mutex overhead needed.
 *   - Round-robin counter (_Atomic uint32_t) is accessed without the
 *     spinlock; a torn increment at worst selects a suboptimal worker, not
 *     an incorrect one.
 *   - Fiber allocation at spawn time uses fiber_alloc(NULL) (malloc) and
 *     stack_alloc() (mmap) so the target worker's dead_pool and stack_cache
 *     are never touched from the host thread.  The fiber is injected via
 *     INJECT_SPAWN; inject_queue_drain pushes it to the run queue.
 *   - CPU affinity: Linux only - pthread_setaffinity_np.  OpenBSD does not
 *     provide CPU affinity APIs; cfg->cpu_affinity is silently ignored there.
 *
 * See ARCHITECTURE.md §6.1, §6.2, §6.4.
 */

#include "strand_runtime.h"
#include "strand_context.h"
#include "strand_fiber.h"
#include "strand_inject.h"
#include "strand_sched.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#ifdef STRAND_LINUX
#include <stdint.h>
#include <pthread.h>
#include <sched.h>  /* cpu_set_t, CPU_ZERO, CPU_SET */
#endif

/* -------------------------------------------------------------------------
 * Runtime lifetime helpers
 * -------------------------------------------------------------------------
 */

/*
 * runtime_lock - acquire the mutex protecting the worker registry.
 */
void
runtime_lock(strand_runtime_t *rt)
{
	(void)pthread_mutex_lock(&rt->mutex);
}

void
runtime_unlock(strand_runtime_t *rt)
{
	(void)pthread_mutex_unlock(&rt->mutex);
}

static int
runtime_begin(strand_runtime_t *rt)
{
	runtime_lock(rt);
	if (rt->shutdown_flag) {
		runtime_unlock(rt);
		return (0);
	}
	rt->active_ops++;
	runtime_unlock(rt);
	return (1);
}

static void
runtime_end(strand_runtime_t *rt)
{
	runtime_lock(rt);
	rt->active_ops--;
	if (rt->active_ops == 0)
		(void)pthread_cond_broadcast(&rt->idle_cond);
	runtime_unlock(rt);
}

/* -------------------------------------------------------------------------
 * Worker selection
 * -------------------------------------------------------------------------
 */

/*
 * runtime_select_worker - round-robin worker selection.
 *
 * Called with the spinlock held.  Increments rr_counter atomically and
 * searches for a non-stopped worker starting at counter % worker_count.
 * Returns NULL if worker_count == 0 or all workers have stop_flag set.
 *
 * SAFETY: rr_counter is _Atomic and incremented without the spinlock being
 * required for correctness - only the final slot selection needs the list
 * to be stable.  We always hold the spinlock here so worker_count cannot
 * change under us during the walk.
 * See ARCHITECTURE.md §6.4.
 */
strand_worker_t *
runtime_select_worker(strand_runtime_t *rt)
{
	uint32_t base, i;
	strand_worker_t *w;

	if (rt->worker_count == 0)
		return (NULL);

	base = atomic_fetch_add(&rt->rr_counter, 1u);

	for (i = 0; i < (uint32_t)rt->worker_count; i++) {
		w = rt->workers[(base + i) % rt->worker_count];
		if (!atomic_load(&w->sched->stop_flag))
			return (w);
	}
	return (NULL); /* all workers stopped */
}

/* -------------------------------------------------------------------------
 * Worker thread entry point
 * -------------------------------------------------------------------------
 */

static void *
worker_thread_fn(void *arg)
{
	strand_worker_t *w = arg;

	strand_scheduler_run(w->sched);
	return (NULL);
}

/* -------------------------------------------------------------------------
 * Public API - Tasks 5.2, 5.3, 5.4
 * -------------------------------------------------------------------------
 */

/*
 * strand_runtime_init - allocate and initialise a multi-worker runtime.
 */
strand_runtime_t *
strand_runtime_init(const strand_runtime_config_t *cfg)
{
	strand_runtime_t *rt;
	size_t            cap;

	cap = (cfg != NULL && cfg->max_workers != 0)
	    ? cfg->max_workers
	    : STRAND_DEFAULT_MAX_WORKERS;

	rt = calloc(1, sizeof(*rt));
	if (rt == NULL)
		return (NULL);

	rt->workers = calloc(cap, sizeof(strand_worker_t *));
	if (rt->workers == NULL) {
		free(rt);
		return (NULL);
	}

	rt->workers_cap  = cap;
	rt->worker_count = 0;
	atomic_init(&rt->rr_counter,    0u);
	if (pthread_mutex_init(&rt->mutex, NULL) != 0) {
		free(rt->workers);
		free(rt);
		return (NULL);
	}
	if (pthread_cond_init(&rt->idle_cond, NULL) != 0) {
		pthread_mutex_destroy(&rt->mutex);
		free(rt->workers);
		free(rt);
		return (NULL);
	}
	rt->shutdown_flag = 0;
	rt->active_ops = 0;
	return (rt);
}

/*
 * strand_runtime_destroy - stop all workers, join all threads, free memory.
 *
 * Follows the shutdown sequence from ARCHITECTURE.md §6.2:
 *   1. strand_scheduler_stop on every worker.
 *   2. pthread_join on every worker thread.
 *   3. strand_scheduler_destroy + free worker descriptor.
 * Safe to call even if workers were never started or are already stopped.
 */
void
strand_runtime_destroy(strand_runtime_t *rt)
{
	size_t i;

	if (rt == NULL)
		return;

	/* Stop new operations and wait for in-progress spawns to release refs. */
	runtime_lock(rt);
	rt->shutdown_flag = 1;
	while (rt->active_ops != 0)
		(void)pthread_cond_wait(&rt->idle_cond, &rt->mutex);
	runtime_unlock(rt);

	/* Step 2: stop all workers. */
	for (i = 0; i < rt->worker_count; i++)
		strand_scheduler_stop(rt->workers[i]->sched);

	/* Step 3: join and free each worker. */
	for (i = 0; i < rt->worker_count; i++) {
		strand_worker_t *w = rt->workers[i];
		/*
		 * Use strand_worker_join so the CAS-based joined flag prevents
		 * double-joining a worker already joined by strand_worker_join.
		 */
		strand_worker_join(w);
		strand_scheduler_destroy(w->sched);
		free(w);
	}

	free(rt->workers);
	pthread_cond_destroy(&rt->idle_cond);
	pthread_mutex_destroy(&rt->mutex);
	free(rt);
}

/*
 * strand_worker_start - create and register a new worker thread.
 *
 * Allocate strand_worker_t, create scheduler, spawn pthread.
 * On Linux: apply CPU affinity if cfg->cpu_affinity >= 0.
 * On OpenBSD: cpu_affinity silently ignored.
 */
strand_worker_t *
strand_worker_start(strand_runtime_t *rt, const strand_worker_config_t *cfg)
{
	strand_worker_t *w;
	int              rc;

	if (rt == NULL)
		return (NULL);

	if (!runtime_begin(rt))
		return (NULL);

	w = calloc(1, sizeof(*w));
	if (w == NULL) {
		runtime_end(rt);
		return (NULL);
	}

	w->runtime = rt;
	w->sched   = strand_scheduler_create(
	    (cfg != NULL) ? cfg->sched_cfg : NULL);
	if (w->sched == NULL) {
		free(w);
		runtime_end(rt);
		return (NULL);
	}

	/* Register before spawning thread so the worker is visible. */
	runtime_lock(rt);
	if (rt->worker_count >= rt->workers_cap) {
		runtime_unlock(rt);
		strand_scheduler_destroy(w->sched);
		free(w);
		runtime_end(rt);
		return (NULL);
	}
	rt->workers[rt->worker_count++] = w;
	runtime_unlock(rt);

	rc = pthread_create(&w->thread, NULL, worker_thread_fn, w);
	if (rc != 0) {
		/* Remove from registry on thread creation failure. */
		runtime_lock(rt);
		rt->worker_count--;
		rt->workers[rt->worker_count] = NULL;
		runtime_unlock(rt);
		strand_scheduler_destroy(w->sched);
		free(w);
		runtime_end(rt);
		return (NULL);
	}

#ifdef STRAND_LINUX
	/*
	 * CPU affinity - Linux only.
	 * pthread_setaffinity_np is not available on OpenBSD; skip silently.
	 * A failure here is non-fatal: we log nothing (no I/O in library code)
	 * and continue.  The fiber model remains correct on any CPU.
	 * See ARCHITECTURE.md §6.1.
	 */
	if (cfg != NULL && cfg->cpu_affinity >= 0) {
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET((size_t)cfg->cpu_affinity, &cpuset);
		(void)pthread_setaffinity_np(w->thread, sizeof(cpuset), &cpuset);
	}
#endif

	runtime_end(rt);
	return (w);
}

/*
 * strand_worker_stop - signal a worker to stop.
 */
void
strand_worker_stop(strand_worker_t *w)
{
	if (w == NULL)
		return;
	/*
	 * Stop the scheduler only.  We do NOT set the runtime shutdown_flag
	 * here because stopping one worker must not prevent spawning to other
	 * live workers.  strand_runtime_spawn detects "all workers stopped"
	 * when runtime_select_worker returns NULL.
	 * See ARCHITECTURE.md §6.2.
	 */
	strand_scheduler_stop(w->sched);
}

/*
 * strand_worker_join - wait for a worker thread to exit.
 */
void
strand_worker_join(strand_worker_t *w)
{
	if (w == NULL)
		return;
	/*
	 * Guard against double-join: strand_runtime_destroy also joins all
	 * workers.  If the caller already joined via strand_worker_join,
	 * destroy must skip.  Use a CAS so that exactly one caller joins.
	 */
	int expected = 0;
	if (atomic_compare_exchange_strong_explicit(
	    &w->joined, &expected, 1,
	    memory_order_acq_rel, memory_order_relaxed))
		pthread_join(w->thread, NULL);
}

/*
 * strand_worker_get_scheduler - return the scheduler owned by a worker.
 * See ARCHITECTURE.md §12.
 */
strand_scheduler_t *
strand_worker_get_scheduler(strand_worker_t *w)
{
	if (w == NULL)
		return (NULL);
	return (w->sched);
}

/*
 * strand_runtime_spawn - spawn a fiber from the host thread.
 *
 * Check shutdown/no-workers, select worker (round-robin or
 * explicit), allocate fiber on the host thread, inject INJECT_SPAWN.
 *
 * Fiber allocation uses fiber_alloc(NULL) (malloc) and stack_alloc()
 * (mmap) - both thread-safe.  The target worker's dead_pool and
 * stack_cache are never touched from the host thread.
 * See ARCHITECTURE.md §6.4.
 */
int
strand_runtime_spawn(strand_runtime_t *rt, strand_fiber_fn_t fn,
    void *arg, size_t stack_sz, strand_worker_t *worker,
    strand_fiber_handle_t *out)
{
	strand_worker_t *target;
	strand_fiber_t  *f;
	void            *stack_base;
	unsigned long    vg_id;
	inject_item_t    item;
	size_t           i;
	int              found;

	if (rt == NULL)
		return (STRAND_ERR_NOMEM);

	if (!runtime_begin(rt))
		return (STRAND_ERR_SHUTDOWN);

	if (stack_sz == 0)
		stack_sz = STRAND_DEFAULT_STACK_SIZE;

	/* Select target worker. */
	if (worker != NULL) {
		/*
		 * Explicit override: worker must belong to this runtime and
		 * must not be stopped.
		 * See ARCHITECTURE.md §6.4.
		 */
		runtime_lock(rt);
		found = 0;
		for (i = 0; i < rt->worker_count; i++) {
			if (rt->workers[i] == worker) {
				found = 1;
				break;
			}
		}
		if (!found) {
			runtime_unlock(rt);
			runtime_end(rt);
			return (STRAND_ERR_WRONGCTX);
		}
		if (atomic_load(&worker->sched->stop_flag)) {
			runtime_unlock(rt);
			runtime_end(rt);
			return (STRAND_ERR_SHUTDOWN);
		}
		runtime_unlock(rt);
		target = worker;
	} else {
		runtime_lock(rt);
		if (rt->worker_count == 0) {
			runtime_unlock(rt);
			runtime_end(rt);
			return (STRAND_ERR_NO_WORKERS);
		}
		target = runtime_select_worker(rt);
		runtime_unlock(rt);
		if (target == NULL) {
			runtime_end(rt);
			return (STRAND_ERR_SHUTDOWN);
		}
	}
	if (!scheduler_inject_acquire(target->sched)) {
		runtime_end(rt);
		return (STRAND_ERR_SHUTDOWN);
	}

	/*
	 * Allocate fiber descriptor and stack on the host thread.
	 * fiber_alloc(NULL) uses malloc; stack_alloc uses mmap - both safe
	 * to call from any thread.  We pass NULL for dead_pool so we never
	 * touch the target worker's per-scheduler dead_pool.
	 */
	f = fiber_alloc(NULL);
	if (f == NULL) {
		scheduler_inject_release(target->sched);
		runtime_end(rt);
		return (STRAND_ERR_NOMEM);
	}

	stack_base = stack_alloc(stack_sz, &vg_id);
	if (stack_base == NULL) {
		fiber_free(NULL, f);
		scheduler_inject_release(target->sched);
		runtime_end(rt);
		return (STRAND_ERR_NOMEM);
	}

	f->stack_base        = (char *)stack_base + page_size();
	f->stack_size        = stack_sz;
	f->valgrind_stack_id = vg_id;
	f->entry_fn          = fn;
	f->entry_arg         = arg;
	f->home_sched        = target->sched;
	f->scope             = NULL;

	strand_context_init(&f->context,
	    (char *)f->stack_base + stack_sz,
	    strand_fiber_entry_start, f);
	strand_fiber_tsan_init(f);

	/*
	 * Set state to FIBER_RUNNABLE before injecting.  The fiber is not
	 * visible to anyone else until it is on the run queue (after drain).
	 */
	atomic_store(&f->state, FIBER_RUNNABLE);

	/* Build and push the inject item. */
	item.type    = INJECT_SPAWN;
	item.u.fiber = f;
	if (inject_queue_try_push_release(&target->sched->inject_queue, &item) != 0) {
		stack_free((char *)f->stack_base - page_size(), f->stack_size,
		    f->valgrind_stack_id);
		strand_fiber_tsan_destroy(f);
		free(f);
		scheduler_inject_release(target->sched);
		runtime_end(rt);
		return (STRAND_EAGAIN);
	}

	/* Wake the target worker so Step 1 runs promptly. */
#ifdef STRAND_LINUX
	{
		uint64_t v = 1;
		(void)write(target->sched->wakeup_fd, &v, sizeof(v));
	}
#endif
#ifdef STRAND_OPENBSD
	{
		char v = 1;
		(void)write(target->sched->wakeup_pipe[1], &v, sizeof(v));
	}
#endif

	if (out != NULL) {
		out->ptr        = f;
		out->generation = f->generation;
	}
	scheduler_inject_release(target->sched);
	runtime_end(rt);
	return (STRAND_OK);
}
