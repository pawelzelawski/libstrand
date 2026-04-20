/*
 * tools/strand_inspect.c - scheduler state inspector.
 *
 * Standalone diagnostic tool.  Prints current state of a scheduler
 * object: run queue depth, timer heap size, inject queue depth,
 * fiber count by state, wakeup fd type, stack cache depth.
 *
 * Intended for use in test harnesses and debug builds where the
 * scheduler object is accessible.
 *
 * Usage:
 *   #include "strand_inspect.c"   (in a test or debug harness)
 * or:
 *   strand_inspect_dump(sched, stdout);
 *
 * Built by `make tools`.  Links against libstrand.a.
 * See REPOSITORY_STRUCTURE.md §6 and DEVELOPMENT.md Task 7.4.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../include/strand.h"
#include "../src/strand_internal.h"

/*
 * state_name - human-readable label for a fiber_state_t value.
 */
static const char *
state_name(int s)
{
	switch (s) {
	case FIBER_NEW:            return "NEW";
	case FIBER_RUNNABLE:       return "RUNNABLE";
	case FIBER_RUNNING:        return "RUNNING";
	case FIBER_PARKED_IO_READ: return "PARKED_IO_READ";
	case FIBER_PARKED_IO_WRITE:return "PARKED_IO_WRITE";
	case FIBER_PARKED_TIMER:   return "PARKED_TIMER";
	case FIBER_PARKED_OFFLOAD: return "PARKED_OFFLOAD";
	case FIBER_PARKED_CHANNEL: return "PARKED_CHANNEL";
	case FIBER_PARKED_SCOPE:   return "PARKED_SCOPE";
	case FIBER_FINISHED:       return "FINISHED";
	default:                   return "UNKNOWN";
	}
}

/*
 * count_dead_pool - walk the dead pool linked list and count entries.
 */
static size_t
count_dead_pool(const strand_scheduler_t *sched)
{
	size_t n = 0;
	const strand_fiber_t *f = sched->dead_pool;

	while (f != NULL) {
		n++;
		f = f->next;
	}
	return (n);
}

/*
 * count_fibers_by_state - walk the run queue and count fibers in each state.
 *
 * Only the run queue is directly walkable.  Timer-parked and IO-parked
 * fibers are tracked by their respective counts.  This function counts
 * fibers on the run queue (RUNNABLE state) and reports the timer heap
 * size for PARKED_TIMER.
 */

/*
 * strand_inspect_dump - print a human-readable scheduler state snapshot.
 *
 * Output goes to the given FILE* (typically stdout or stderr).
 * Safe to call from the scheduler's owner thread while no advance is
 * in progress.  Not thread-safe against concurrent advance calls.
 */
void
strand_inspect_dump(const strand_scheduler_t *sched, FILE *fp)
{
	if (sched == NULL) {
		fprintf(fp, "strand_inspect: NULL scheduler\n");
		return;
	}

	fprintf(fp, "=== strand_inspect: scheduler %p ===\n",
	    (const void *)sched);

	/* Run queue */
	fprintf(fp, "  run_queue_len     : %zu\n", sched->run_queue_len);
	fprintf(fp, "  budget            : %zu\n", sched->budget);

	/* Timer heap */
	fprintf(fp, "  timer_heap_len    : %zu\n", sched->timer_heap_len);
	fprintf(fp, "  timer_heap_cap    : %zu\n", sched->timer_heap_cap);

	/* Inject queue depth: head - tail gives pending items. */
	{
		size_t head = atomic_load_explicit(
		    &((strand_inject_queue_t *)&sched->inject_queue)->head,
		    memory_order_relaxed);
		size_t tail = sched->inject_queue.tail;
		size_t depth = head - tail;

		fprintf(fp, "  inject_queue_depth: %zu / %zu\n",
		    depth, sched->inject_queue.capacity);
	}

	/* Stack cache */
	fprintf(fp, "  stack_cache_len   : %zu\n", sched->cache_len);
	fprintf(fp, "  stack_cache_cap   : %zu\n", sched->cache_cap);
	fprintf(fp, "  stack_cache_floor : %zu\n", sched->cache_floor);

	/* Dead pool */
	fprintf(fp, "  dead_pool_len     : %zu\n", count_dead_pool(sched));

	/* Wakeup fd type */
#ifdef STRAND_LINUX
	fprintf(fp, "  wakeup_fd         : %d (eventfd)\n",
	    sched->wakeup_fd);
#endif
#ifdef STRAND_OPENBSD
	fprintf(fp, "  wakeup_pipe       : [%d, %d] (pipe)\n",
	    sched->wakeup_pipe[0], sched->wakeup_pipe[1]);
#endif

	/* Stop flag */
	fprintf(fp, "  stop_flag         : %d\n",
	    atomic_load_explicit(
	        &((strand_scheduler_t *)sched)->stop_flag,
	        memory_order_relaxed));

	/* Current fiber */
	fprintf(fp, "  current_fiber     : %p\n",
	    (const void *)sched->current_fiber);

	/* Fiber count by state - walk run queue */
	{
		size_t counts[10] = {0};
		const strand_fiber_t *f = sched->run_head;

		while (f != NULL) {
			int st = atomic_load_explicit(
			    &((strand_fiber_t *)f)->state,
			    memory_order_relaxed);
			if (st >= 0 && st <= 9)
				counts[st]++;
			f = f->next;
		}
		fprintf(fp, "  fibers on run queue by state:\n");
		for (int i = 0; i <= 9; i++) {
			if (counts[i] > 0)
				fprintf(fp, "    %-18s: %zu\n",
				    state_name(i), counts[i]);
		}
	}

	/* Timer heap fibers by state */
	{
		size_t counts[10] = {0};

		for (size_t i = 0; i < sched->timer_heap_len; i++) {
			const strand_fiber_t *f =
			    sched->timer_heap[i].fiber;
			if (f == NULL)
				continue;
			int st = atomic_load_explicit(
			    &((strand_fiber_t *)f)->state,
			    memory_order_relaxed);
			if (st >= 0 && st <= 9)
				counts[st]++;
		}
		fprintf(fp, "  fibers on timer heap by state:\n");
		for (int i = 0; i <= 9; i++) {
			if (counts[i] > 0)
				fprintf(fp, "    %-18s: %zu\n",
				    state_name(i), counts[i]);
		}
	}

#ifdef STRAND_DEBUG
	fprintf(fp, "  watchdog_ns       : %" PRIu64 " ms\n",
	    (uint64_t)(sched->watchdog_threshold_ns / 1000000ULL));
#endif

	fprintf(fp, "=== end ===\n");
}

/*
 * main - standalone entry point.
 *
 * Creates a default scheduler, dumps its empty state, and destroys it.
 * Useful as a smoke test for the tool build.
 */
int
main(void)
{
	strand_sched_config_t cfg = {
		.budget     = 64,
		.inject_cap = 256,
		.cache_cap  = 8,
		.idle_floor = 2,
	};
	strand_scheduler_t *sched = strand_scheduler_create(&cfg);

	if (sched == NULL) {
		fprintf(stderr, "strand_inspect: failed to create scheduler\n");
		return (1);
	}

	strand_inspect_dump(sched, stdout);
	strand_scheduler_destroy(sched);
	return (0);
}

