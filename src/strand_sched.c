/*
 * strand_sched.c -- fiber scheduler: advance, run, stop.
 * See ARCHITECTURE.md section 4.2.
 */

#include "../include/strand.h"
#include "strand_internal.h"
#include "strand_sched.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifdef STRAND_LINUX
#include <sys/eventfd.h>
#include <unistd.h>
#endif

#ifdef STRAND_OPENBSD
#include <unistd.h>
#include <fcntl.h>
#endif

#include "strand_fiber.h"

/* Initial capacity for the timer min-heap (grows by doubling). */
#define TIMER_HEAP_INITIAL_CAP ((size_t)8)

/* cache_idle_ns default: 5 seconds in nanoseconds. */
#define CACHE_IDLE_NS_DEFAULT ((uint64_t)5000000000ULL)

/*
 * wakeup_close -- close the platform wakeup fd(s) on a scheduler.
 * Safe to call even if the fds were never opened, because the fds are
 * initialised to -1 before the open attempt in strand_scheduler_create.
 */
static void
wakeup_close(strand_scheduler_t *sched)
{
#ifdef STRAND_LINUX
	if (sched->wakeup_fd != -1)
		close(sched->wakeup_fd);
#endif
#ifdef STRAND_OPENBSD
	if (sched->wakeup_pipe[0] != -1)
		close(sched->wakeup_pipe[0]);
	if (sched->wakeup_pipe[1] != -1)
		close(sched->wakeup_pipe[1]);
#endif
	(void)sched; /* suppress unused-parameter in neutral builds */
}

/*
 * run_queue_push -- append fiber to the tail of the run queue.
 */
void
run_queue_push(strand_scheduler_t *sched, strand_fiber_t *f)
{
	f->next = NULL;
	if (sched->run_tail != NULL)
		sched->run_tail->next = f;
	else
		sched->run_head = f;
	sched->run_tail = f;
	sched->run_queue_len++;
}

/*
 * run_queue_pop -- remove and return the head fiber from the run queue.
 * Returns NULL if the queue is empty.
 */
strand_fiber_t *
run_queue_pop(strand_scheduler_t *sched)
{
	strand_fiber_t *f;

	if (sched->run_head == NULL)
		return (NULL);

	f = sched->run_head;
	sched->run_head = f->next;
	if (sched->run_head == NULL)
		sched->run_tail = NULL;
	sched->run_queue_len--;
	f->next = NULL;
	return (f);
}

/*
 * strand_scheduler_create -- allocate and initialise a scheduler.
 */
strand_scheduler_t *
strand_scheduler_create(const strand_sched_config_t *cfg)
{
	strand_scheduler_t *sched;
	size_t budget, cache_cap, cache_floor;

	budget = (cfg != NULL && cfg->budget != 0)
	             ? cfg->budget
	             : STRAND_DEFAULT_SCHED_BUDGET;
	cache_cap = (cfg != NULL && cfg->cache_cap != 0)
	                ? cfg->cache_cap
	                : STRAND_DEFAULT_CACHE_CAP;
	cache_floor = (cfg != NULL && cfg->idle_floor != 0)
	                  ? cfg->idle_floor
	                  : STRAND_DEFAULT_CACHE_FLOOR;

	sched = calloc(1, sizeof(*sched));
	if (sched == NULL)
		return (NULL);

/*
 * Initialise wakeup fd sentinels before the open attempt so that
 * wakeup_close() is safe to call on any error path.
 * fd value 0 is valid on POSIX, so we cannot rely on calloc's zero.
 */
#ifdef STRAND_LINUX
	sched->wakeup_fd = -1;
#endif
#ifdef STRAND_OPENBSD
	sched->wakeup_pipe[0] = -1;
	sched->wakeup_pipe[1] = -1;
#endif

/*
 * Wakeup fd -- written by strand_scheduler_stop to interrupt a blocked
 * poll.  The bytes written are control signals, not I/O events; they
 * are drained and discarded in Step 3 of strand_scheduler_advance.
 * SAFETY: EFD_CLOEXEC and EFD_NONBLOCK (or O_CLOEXEC | O_NONBLOCK)
 * prevent leaks to child processes and blocking reads in the drain step.
 */
#ifdef STRAND_LINUX
	sched->wakeup_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (sched->wakeup_fd == -1) {
		free(sched);
		return (NULL);
	}
#endif
#ifdef STRAND_OPENBSD
	/*
	 * pipe2 is hidden behind __BSD_VISIBLE on OpenBSD when _POSIX_C_SOURCE
	 * or _XOPEN_SOURCE is set.  Use pipe + fcntl instead; both F_SETFD and
	 * F_SETFL are fully POSIX and always visible.
	 */
	if (pipe(sched->wakeup_pipe) == -1) {
		free(sched);
		return (NULL);
	}
	if (fcntl(sched->wakeup_pipe[0], F_SETFD, FD_CLOEXEC) == -1 ||
	    fcntl(sched->wakeup_pipe[1], F_SETFD, FD_CLOEXEC) == -1 ||
	    fcntl(sched->wakeup_pipe[0], F_SETFL, O_NONBLOCK) == -1 ||
	    fcntl(sched->wakeup_pipe[1], F_SETFL, O_NONBLOCK) == -1) {
		close(sched->wakeup_pipe[0]);
		close(sched->wakeup_pipe[1]);
		free(sched);
		return (NULL);
	}
#endif

	sched->timer_heap =
	    malloc(TIMER_HEAP_INITIAL_CAP * sizeof(*sched->timer_heap));
	if (sched->timer_heap == NULL) {
		wakeup_close(sched);
		free(sched);
		return (NULL);
	}
	sched->timer_heap_cap = TIMER_HEAP_INITIAL_CAP;
	sched->timer_heap_len = 0;

	sched->stack_cache = malloc(cache_cap * sizeof(*sched->stack_cache));
	if (sched->stack_cache == NULL) {
		free(sched->timer_heap);
		wakeup_close(sched);
		free(sched);
		return (NULL);
	}
	sched->cache_cap = cache_cap;
	sched->cache_len = 0;
	sched->cache_floor = cache_floor;
	sched->cache_idle_ns = CACHE_IDLE_NS_DEFAULT;
	sched->last_idle_ns = 0;

	sched->budget = budget;
	sched->run_head = NULL;
	sched->run_tail = NULL;
	sched->run_queue_len = 0;
	sched->current_fiber = NULL;
	sched->dead_pool = NULL;
	sched->poller = NULL;
	sched->inject_queue._placeholder = 0;
	atomic_init(&sched->stop_flag, 0);

	/*
	 * Bind TSan's current-thread fiber to scheduler_ctx so that context
	 * switches originating from the scheduler side are properly tracked.
	 * In non-TSan builds this is a no-op.
	 */
	strand_fiber_tsan_init(&sched->scheduler_ctx);
	strand_fiber_tsan_bind_current(&sched->scheduler_ctx);

	return (sched);
}

/*
 * strand_scheduler_destroy -- tear down and free a scheduler.
 * All fibers must be finished before calling this.
 */
void
strand_scheduler_destroy(strand_scheduler_t *sched)
{
	strand_fiber_t *f;
	size_t i;

	if (sched == NULL)
		return;

	strand_fiber_tsan_destroy(&sched->scheduler_ctx);

	wakeup_close(sched);

	free(sched->timer_heap);

	for (i = 0; i < sched->cache_len; i++) {
		strand_stack_slot_t *slot = &sched->stack_cache[i];
		stack_free(slot->base, slot->stack_size, slot->vg_id);
	}
	free(sched->stack_cache);

	f = sched->dead_pool;
	while (f != NULL) {
		strand_fiber_t *next = f->next;
		strand_fiber_tsan_destroy(f);
		free(f);
		f = next;
	}

	free(sched);
}
