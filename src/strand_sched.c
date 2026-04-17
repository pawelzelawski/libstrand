/*
 * strand_sched.c -- fiber scheduler: advance, run, stop.
 * See ARCHITECTURE.md section 4.2.
 */

#include "../include/strand.h"
#include "strand_internal.h"
#include "strand_sched.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* clock_gettime, CLOCK_MONOTONIC */

#ifdef STRAND_LINUX
#include <sys/eventfd.h>
#include <unistd.h>
#endif

#ifdef STRAND_OPENBSD
#include <fcntl.h>
#include <unistd.h>
#endif

#include <limits.h>
#include <pthread.h>

#include "strand_context.h"
#include "strand_fiber.h"
#include "strand_inject.h"
#include "strand_poller.h"

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

	{
		size_t inject_cap = (cfg != NULL && cfg->inject_cap != 0)
		    ? cfg->inject_cap
		    : STRAND_DEFAULT_INJECT_CAP;
		if (inject_queue_init(&sched->inject_queue, inject_cap) != 0) {
			free(sched);
			return (NULL);
		}
	}

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
		inject_queue_destroy(&sched->inject_queue);
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
		inject_queue_destroy(&sched->inject_queue);
		free(sched);
		return (NULL);
	}
	if (fcntl(sched->wakeup_pipe[0], F_SETFD, FD_CLOEXEC) == -1 ||
	    fcntl(sched->wakeup_pipe[1], F_SETFD, FD_CLOEXEC) == -1 ||
	    fcntl(sched->wakeup_pipe[0], F_SETFL, O_NONBLOCK) == -1 ||
	    fcntl(sched->wakeup_pipe[1], F_SETFL, O_NONBLOCK) == -1) {
		close(sched->wakeup_pipe[0]);
		close(sched->wakeup_pipe[1]);
		inject_queue_destroy(&sched->inject_queue);
		free(sched);
		return (NULL);
	}
#endif

	sched->timer_heap =
	    malloc(TIMER_HEAP_INITIAL_CAP * sizeof(*sched->timer_heap));
	if (sched->timer_heap == NULL) {
		wakeup_close(sched);
		inject_queue_destroy(&sched->inject_queue);
		free(sched);
		return (NULL);
	}
	sched->timer_heap_cap = TIMER_HEAP_INITIAL_CAP;
	sched->timer_heap_len = 0;

	sched->stack_cache = malloc(cache_cap * sizeof(*sched->stack_cache));
	if (sched->stack_cache == NULL) {
		free(sched->timer_heap);
		wakeup_close(sched);
		inject_queue_destroy(&sched->inject_queue);
		free(sched);
		return (NULL);
	}
	sched->cache_cap = cache_cap;
	sched->cache_len = 0;
	sched->cache_floor = cache_floor;
	sched->cache_idle_ns = CACHE_IDLE_NS_DEFAULT;
	sched->last_idle_ns = 0;
	sched->pending_free_base  = NULL;
	sched->pending_free_size  = 0;
	sched->pending_free_vg_id = 0;
	sched->pending_free_local_ptr  = NULL;
	sched->pending_free_local_dtor = NULL;

	sched->budget = budget;
	sched->run_head = NULL;
	sched->run_tail = NULL;
	sched->run_queue_len = 0;
	sched->current_fiber = NULL;
	sched->dead_pool = NULL;
	sched->poller = poller_create(0);
	if (sched->poller == NULL) {
		inject_queue_destroy(&sched->inject_queue);
		wakeup_close(sched);
		free(sched->timer_heap);
		free(sched->stack_cache);
		free(sched);
		return NULL;
	}

	/*
	 * Register the wakeup fd with the poller so that a write by
	 * strand_scheduler_stop unblocks a blocked epoll_wait / kevent.
	 * See ARCHITECTURE.md §5 and DEVELOPMENT.md Task 4.5.
	 */
#ifdef STRAND_LINUX
	if (poller_register_wakeup_fd(sched->poller, sched->wakeup_fd) !=
	    STRAND_OK) {
		poller_destroy(sched->poller);
		inject_queue_destroy(&sched->inject_queue);
		wakeup_close(sched);
		free(sched->timer_heap);
		free(sched->stack_cache);
		free(sched);
		return NULL;
	}
#endif
#ifdef STRAND_OPENBSD
	if (poller_register_wakeup_fd(sched->poller,
	    sched->wakeup_pipe[0]) != STRAND_OK) {
		poller_destroy(sched->poller);
		inject_queue_destroy(&sched->inject_queue);
		wakeup_close(sched);
		free(sched->timer_heap);
		free(sched->stack_cache);
		free(sched);
		return NULL;
	}
#endif
	atomic_init(&sched->stop_flag, 0);
	sched->owner_thread = pthread_self();

	/*
	 * Bind TSan's current-thread fiber to scheduler_ctx so that context
	 * switches originating from the scheduler side are properly tracked.
	 * bind_current is the correct call here — scheduler_ctx IS the OS
	 * thread, not an independently created fiber.  Do NOT call
	 * strand_fiber_tsan_init first: init creates a fresh fiber that
	 * bind_current would immediately overwrite and leak.
	 * In non-TSan builds this is a no-op.
	 */
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

	/*
	 * scheduler_ctx was bound via strand_fiber_tsan_bind_current, NOT
	 * created via strand_fiber_tsan_init (__tsan_create_fiber).  Calling
	 * __tsan_destroy_fiber on a fiber returned by __tsan_get_current_fiber
	 * corrupts TSan's internal state.  Just NULL the pointer.
	 */
	sched->scheduler_ctx.tsan_fiber = NULL;

	/*
	 * Scheduler destroy requires no active fibers; pending cross-worker
	 * inject items are stale at this point and can be discarded.
	 */
	inject_queue_discard_all(&sched->inject_queue);
	inject_queue_destroy(&sched->inject_queue);

	wakeup_close(sched);

	poller_destroy(sched->poller);
	sched->poller = NULL;

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

/* ---------------------------------------------------------------------------
 * Timer min-heap helpers.
 * Binary heap keyed on deadline_ns (ascending).  Parent at (i-1)/2,
 * children at 2*i+1 and 2*i+2.
 * ---------------------------------------------------------------------------
 */

/*
 * now_ns -- return a CLOCK_MONOTONIC timestamp in nanoseconds.
 *
 * STRAND_TEST_CLOCK: in test builds, reads the global strand_test_clock_ns
 * instead of calling clock_gettime so tests can advance time without sleeping.
 * See TESTING.md section 2.3.
 */
#ifdef STRAND_TEST_CLOCK
uint64_t strand_test_clock_ns;
static uint64_t
now_ns(void)
{
	return (strand_test_clock_ns);
}
#else
static uint64_t
now_ns(void)
{
	struct timespec ts;
	/* CLOCK_MONOTONIC is declared in <time.h>; clang-tidy cannot resolve
	 * it through glibc's private <bits/time.h> indirection. */
	// NOLINTNEXTLINE(misc-include-cleaner)
	(void)clock_gettime(CLOCK_MONOTONIC, &ts); /* cannot fail in practice */
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}
#endif /* STRAND_TEST_CLOCK */

/*
 * heap_swap -- exchange two entries in the timer heap.
 */
static void
heap_swap(strand_timer_entry_t *a, strand_timer_entry_t *b)
{
	strand_timer_entry_t tmp = *a;
	*a = *b;
	*b = tmp;
}

/*
 * heap_sift_up -- restore heap property after inserting at index i.
 */
static void
heap_sift_up(strand_scheduler_t *sched, size_t i)
{
	strand_timer_entry_t *h = sched->timer_heap;
	while (i > 0) {
		size_t parent = (i - 1) / 2;
		if (h[parent].deadline_ns <= h[i].deadline_ns)
			break;
		heap_swap(&h[parent], &h[i]);
		i = parent;
	}
}

/*
 * heap_sift_down -- restore heap property after replacing root.
 */
static void
heap_sift_down(strand_scheduler_t *sched, size_t i)
{
	strand_timer_entry_t *h = sched->timer_heap;
	size_t n = sched->timer_heap_len;
	for (;;) {
		size_t smallest = i;
		size_t left = 2 * i + 1;
		size_t right = 2 * i + 2;
		if (left < n && h[left].deadline_ns < h[smallest].deadline_ns)
			smallest = left;
		if (right < n && h[right].deadline_ns < h[smallest].deadline_ns)
			smallest = right;
		if (smallest == i)
			break;
		heap_swap(&h[i], &h[smallest]);
		i = smallest;
	}
}

/*
 * timer_heap_push -- insert (deadline_ns, fiber) into the min-heap.
 * Grows the heap array by doubling when full.
 * Returns 0 on success, -1 on allocation failure.
 */
int
timer_heap_push(strand_scheduler_t *sched, uint64_t deadline_ns,
                strand_fiber_t *f)
{
	if (sched->timer_heap_len == sched->timer_heap_cap) {
		strand_timer_entry_t *new_heap;
		size_t new_cap = sched->timer_heap_cap * 2;
		new_heap = realloc(sched->timer_heap,
		                   new_cap * sizeof(*sched->timer_heap));
		if (new_heap == NULL)
			return (-1);
		sched->timer_heap = new_heap;
		sched->timer_heap_cap = new_cap;
	}
	sched->timer_heap[sched->timer_heap_len].deadline_ns = deadline_ns;
	sched->timer_heap[sched->timer_heap_len].fiber = f;
	heap_sift_up(sched, sched->timer_heap_len);
	sched->timer_heap_len++;
	return (0);
}

/*
 * timer_heap_pop_min -- remove and return the entry with the smallest
 * deadline_ns.  Caller must ensure timer_heap_len > 0.
 */
strand_timer_entry_t
timer_heap_pop_min(strand_scheduler_t *sched)
{
	strand_timer_entry_t min = sched->timer_heap[0];
	sched->timer_heap_len--;
	if (sched->timer_heap_len > 0) {
		sched->timer_heap[0] = sched->timer_heap[sched->timer_heap_len];
		heap_sift_down(sched, 0);
	}
	return (min);
}

/*
 * timer_heap_peek_deadline -- return the smallest deadline_ns without
 * removing it, or UINT64_MAX if the heap is empty.
 */
uint64_t
timer_heap_peek_deadline(const strand_scheduler_t *sched)
{
	if (sched->timer_heap_len == 0)
		return (UINT64_MAX);
	return (sched->timer_heap[0].deadline_ns);
}

/*
 * timer_heap_remove -- remove the entry whose fiber pointer matches f.
 *
 * Linear scan to locate the entry; replaces it with the last element to
 * avoid leaving a hole, then restores heap order by sifting up then down.
 * Sifting both directions handles the general case regardless of whether
 * the replacement value is smaller or larger than the removed entry's
 * neighbours.
 *
 * Returns 1 if found and removed, 0 if f was not in the heap.
 */
int
timer_heap_remove(strand_scheduler_t *sched, strand_fiber_t *f)
{
	size_t i;
	strand_timer_entry_t *h = sched->timer_heap;
	size_t n = sched->timer_heap_len;

	/* Linear scan — heap is sorted by deadline, not by fiber pointer. */
	for (i = 0; i < n; i++) {
		if (h[i].fiber == f)
			break;
	}
	if (i == n)
		return (0); /* not found */

	/*
	 * Replace with the last entry and shrink the heap.  If i already
	 * points to the last entry, no data movement is needed.
	 */
	sched->timer_heap_len--;
	if (i < sched->timer_heap_len) {
		h[i] = h[sched->timer_heap_len];
		/*
		 * Sift up first: if the replacement is smaller than its
		 * parent, bubble it up.  Then sift down: if it is larger
		 * than a child, bubble it down.  Only one of the two will
		 * actually move the element; the other is a no-op.
		 */
		heap_sift_up(sched, i);
		heap_sift_down(sched, i);
	}
	return (1);
}

/* ---------------------------------------------------------------------------
 * strand_scheduler_advance -- one nonblocking scheduler pass.
 * See ARCHITECTURE.md section 4.2 for the five-step specification.
 * ---------------------------------------------------------------------------
 */

/*
 * wakeup_drain -- drain all bytes from the wakeup fd/pipe and discard them.
 *
 * SAFETY: the bytes written to the wakeup fd by strand_scheduler_stop (or
 * cross-worker signals) are plain control signals.  They carry no data about
 * which fiber to run and must NOT be interpreted as fd readiness events.
 * All bytes are read here and thrown away.  See ARCHITECTURE.md section 4.2
 * Step 3.
 */
static void
wakeup_drain(strand_scheduler_t *sched)
{
#ifdef STRAND_LINUX
	uint64_t buf;
	while (read(sched->wakeup_fd, &buf, sizeof(buf)) > 0)
		;
	/* EAGAIN / EWOULDBLOCK is expected when the fd is empty. */
#endif
#ifdef STRAND_OPENBSD
	char buf[64];
	while (read(sched->wakeup_pipe[0], buf, sizeof(buf)) > 0)
		;
#endif
	(void)sched; /* suppress unused-parameter in neutral builds */
}

sched_result_t
strand_scheduler_advance(strand_scheduler_t *sched, uint64_t *next_deadline_ns)
{
	int progress = 0;
	uint64_t t;
	size_t ran;
	strand_fiber_t *f;

	/* Phase 3 same-worker bookkeeping: current caller owns scheduler access. */
	sched->owner_thread = pthread_self();

	/*
	 * Rebind scheduler_ctx's TSan fiber handle to the current thread.
	 * strand_scheduler_advance may be called from any thread (e.g. the
	 * worker thread in strand_scheduler_run, or the main thread after a
	 * stop).  The TSan handle must always match the actual OS thread that
	 * is about to context-switch, otherwise TSan asserts when a fiber
	 * switches back to the scheduler context and TSan sees a cross-thread
	 * fiber transition.  In non-TSan builds this is a no-op.
	 */
	STRAND_TSAN_BIND_CURRENT(&sched->scheduler_ctx);

	/*
	 * Step 1: drain injected cross-worker items (bounded MPSC ring buffer).
	 * Handles INJECT_CANCEL items by calling strand_fiber_cancel.
	 * Additional item types (INJECT_SPAWN, INJECT_OFFLOAD_COMPLETE) are
	 * added in Phase 5 Tasks 5.4 and 5.8.
	 * See ARCHITECTURE.md §6.3.
	 */
	inject_queue_drain(sched);

	/*
	 * Step 2: expire timers.
	 * Pop every heap entry whose deadline <= now and move the fiber to
	 * the run queue tail.  Each expiry counts as progress.
	 */
	t = now_ns();
	while (sched->timer_heap_len > 0 &&
	       timer_heap_peek_deadline(sched) <= t) {
		strand_timer_entry_t entry = timer_heap_pop_min(sched);
		atomic_store_explicit(&entry.fiber->state, FIBER_RUNNABLE,
		                      memory_order_relaxed);
		run_queue_push(sched, entry.fiber);
		progress = 1;
	}

	/*
	 * Step 3: drain wakeup fd.
	 * SAFETY: bytes written here are control signals from
	 * strand_scheduler_stop, not fiber waiter events.  They are read and
	 * discarded; no fiber is woken as a result.  See ARCHITECTURE.md
	 * section 4.2 Step 3.
	 */
	wakeup_drain(sched);

	/*
	 * Step 4: poll I/O with zero timeout (non-blocking).
	 * Delivers any ready events to waiting fibers via poller_deliver_event,
	 * pushing woken fibers onto the run queue for Step 5.
	 * See ARCHITECTURE.md §4.2 and §5.
	 */
	poller_poll(sched, 0);

	/*
	 * Step 5: run up to budget fibers.
	 * Set current_fiber before the context switch so that functions called
	 * from within the fiber (yield, sleep_until, etc.) can find the running
	 * fiber.  Clear it immediately when the fiber suspends or finishes and
	 * control returns here.
	 */
	for (ran = 0; ran < sched->budget; ran++) {
		f = run_queue_pop(sched);
		if (f == NULL)
			break;
		atomic_store_explicit(&f->state, FIBER_RUNNING,
		                      memory_order_relaxed);
		sched->current_fiber = f;
		strand_context_switch(&sched->scheduler_ctx, f);
		sched->current_fiber = NULL;

		/*
		 * Pending stack free and fiber-local destructor —
		 * ARCHITECTURE.md §13, §4.2 Step 5, §4.7.
		 *
		 * The fiber stored its stack info and local destructor in
		 * pending_free_* before switching back so it could not
		 * munmap its own execution stack or run the destructor on
		 * the fiber's stack.  We are now back on the scheduler's
		 * stack; both operations are safe here.
		 *
		 * Destructor is called before the stack is freed so the
		 * destructor body can still refer to fiber-stack memory
		 * without risk (it runs on the scheduler stack, not the
		 * fiber stack, but the mapping still exists until after
		 * sched_stack_free).
		 */
		if (sched->pending_free_local_dtor != NULL) {
			sched->pending_free_local_dtor(
			    sched->pending_free_local_ptr);
			sched->pending_free_local_dtor = NULL;
			sched->pending_free_local_ptr  = NULL;
		}
		if (sched->pending_free_base != NULL) {
			sched_stack_free(sched,
			                 sched->pending_free_base,
			                 sched->pending_free_size,
			                 sched->pending_free_vg_id);
			sched->pending_free_base = NULL;
		}
		progress = 1;
	}

	/*
	 * Idle reclamation — ARCHITECTURE.md §13.4.
	 *
	 * When the run queue is empty (idle), track the start of the idle
	 * period using the timestamp already computed at Step 2 (t).  If the
	 * queue has been empty for at least cache_idle_ns nanoseconds and the
	 * cache holds more than cache_floor stacks, reclaim the excess from
	 * the top (LIFO) down to cache_floor and reset the idle clock.
	 *
	 * When the queue is non-empty (busy), reset last_idle_ns to zero so
	 * the idle timer restarts fresh after the next burst.
	 *
	 * In STRAND_TEST_CLOCK builds, t == strand_test_clock_ns, so tests
	 * can trigger reclamation by advancing the mock clock past the
	 * threshold without sleeping.
	 */
	if (sched->run_queue_len == 0) {
		if (sched->last_idle_ns == 0)
			sched->last_idle_ns = t;
		if (sched->cache_len > sched->cache_floor &&
		    t - sched->last_idle_ns >= sched->cache_idle_ns) {
			while (sched->cache_len > sched->cache_floor) {
				strand_stack_slot_t *slot =
				    &sched->stack_cache[sched->cache_len - 1];
				stack_free(slot->base, slot->stack_size,
				           slot->vg_id);
				sched->cache_len--;
			}
			sched->last_idle_ns = t;
		}
	} else {
		sched->last_idle_ns = 0;
	}

	if (progress) {
		if (next_deadline_ns != NULL)
			*next_deadline_ns = timer_heap_peek_deadline(sched);
		return (SCHED_PROGRESS);
	}

	if (next_deadline_ns != NULL)
		*next_deadline_ns = timer_heap_peek_deadline(sched);
	return (SCHED_IDLE);
}

/* ---------------------------------------------------------------------------
 * strand_scheduler_next_deadline -- return next timer deadline (nanoseconds).
 * Returns UINT64_MAX if no timers are pending.
 * ---------------------------------------------------------------------------
 */
uint64_t
strand_scheduler_next_deadline(const strand_scheduler_t *sched)
{
	return (timer_heap_peek_deadline(sched));
}

/* ---------------------------------------------------------------------------
 * strand_scheduler_get_fd -- return the scheduler activity fd for host loops.
 *
 * In Phase 4 this is the internal poller fd (epoll on Linux, kqueue on
 * OpenBSD), not the raw wakeup channel.  The poller fd becomes readable when
 * any scheduler event is pending: libstrand-managed I/O readiness, wakeup
 * control writes (stop/cross-worker), or other poller-delivered activity.
 *
 * Host loops should monitor this fd and call strand_scheduler_advance when it
 * fires.  See ARCHITECTURE.md §4.2 and §5.
 * ---------------------------------------------------------------------------
 */
int
strand_scheduler_get_fd(const strand_scheduler_t *sched)
{
	if (sched == NULL || sched->poller == NULL)
		return (-1);
	return (sched->poller->pollfd);
}

/* ---------------------------------------------------------------------------
 * strand_scheduler_stop -- signal the scheduler to stop.
 *
 * SAFETY: atomic_store with release ordering ensures all prior writes
 * (run queue state, fiber states) are visible to the worker before it
 * observes stop_flag == 1.  The atomic store alone cannot interrupt a
 * worker blocked in epoll_wait/kevent — the wakeup fd write below is
 * required for that.  See ARCHITECTURE.md §4.2.
 *
 * SAFETY: writing to the wakeup fd unblocks any thread blocked in poll/
 * epoll_wait/kevent inside strand_scheduler_run.  The written bytes are
 * plain control signals; strand_scheduler_advance Step 3 drains and
 * discards them unconditionally.  Safe to call from any thread.
 * See ARCHITECTURE.md §4.2.
 * ---------------------------------------------------------------------------
 */
void
strand_scheduler_stop(strand_scheduler_t *sched)
{
	/*
	 * SAFETY: release ordering pairs with the acquire load in
	 * strand_scheduler_run, ensuring the stop is visible before the
	 * next advance call inspects the flag.
	 */
	atomic_store_explicit(&sched->stop_flag, 1, memory_order_release);

	/*
	 * SAFETY: write to the wakeup fd to interrupt a worker blocked in
	 * poll/epoll_wait/kevent.  Without this write the worker would block
	 * indefinitely even after the flag is set.  The bytes are control
	 * signals only; they are drained and discarded in Step 3 of the
	 * next strand_scheduler_advance call.  See ARCHITECTURE.md §4.2.
	 */
#ifdef STRAND_LINUX
	{
		uint64_t val = 1;
		(void)write(sched->wakeup_fd, &val, sizeof(val));
	}
#endif
#ifdef STRAND_OPENBSD
	{
		char val = 1;
		(void)write(sched->wakeup_pipe[1], &val, sizeof(val));
	}
#endif
}

/* ---------------------------------------------------------------------------
 * strand_scheduler_run -- blocking worker-mode loop.
 *
 * Calls strand_scheduler_advance in a loop.  When advance returns
 * SCHED_IDLE the worker blocks in poll() on the wakeup fd until either
 * the next timer deadline expires or strand_scheduler_stop writes a byte
 * to the wakeup fd.  Returns only after stop_flag is set.
 *
 * In Phase 4 the idle wait is replaced with the real poller (epoll/kqueue
 * fd from strand_poller_t) so that I/O readiness also wakes the worker.
 * See ARCHITECTURE.md §4.2.
 * ---------------------------------------------------------------------------
 */
void
strand_scheduler_run(strand_scheduler_t *sched)
{
	/*
	 * In Phase 3, no context switches to user fibers happen in this test,
	 * so no TSan rebinding is needed here.  Multi-worker TSan setup is
	 * deferred to Phase 5 (Task 5.3) where the worker-thread lifecycle
	 * is fully defined.
	 */

	/* Worker mode establishes scheduler ownership for same-worker APIs. */
	sched->owner_thread = pthread_self();

	/*
	 * Rebind scheduler_ctx's TSan fiber handle to this worker thread.
	 * See the comment in strand_scheduler_advance for the full rationale.
	 * In non-TSan builds this is a no-op.
	 */
	STRAND_TSAN_BIND_CURRENT(&sched->scheduler_ctx);

	for (;;) {
		uint64_t next_ns;
		sched_result_t rc;
		int timeout_ms;

		if (atomic_load_explicit(&sched->stop_flag,
		                         memory_order_acquire))
			return;

		rc = strand_scheduler_advance(sched, &next_ns);

		if (atomic_load_explicit(&sched->stop_flag,
		                         memory_order_acquire))
			return;

		if (rc != SCHED_IDLE)
			continue;

		/*
		 * Idle: compute timeout from the next timer deadline, then
		 * block on the poller.  strand_scheduler_stop writes to the
		 * wakeup fd (registered with the poller in Task 4.5), which
		 * unblocks epoll_wait / kevent.  I/O readiness on any fiber
		 * fd also unblocks the wait and delivers events.
		 * See ARCHITECTURE.md §4.2.
		 */
		if (next_ns == UINT64_MAX) {
			timeout_ms = -1; /* block indefinitely until woken */
		} else {
			uint64_t t_now = now_ns();
			if (t_now >= next_ns) {
				timeout_ms = 0;
			} else {
				uint64_t diff_ms =
				    (next_ns - t_now) / 1000000ULL;
				timeout_ms = (diff_ms > (uint64_t)INT_MAX)
				                 ? INT_MAX
				                 : (int)diff_ms;
			}
		}

		poller_poll(sched, timeout_ms);
	}
}
