/* Real-clock timer regression; built without STRAND_TEST_CLOCK. */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../include/strand.h"
#include "../src/strand_context.h"
#include "../src/strand_fiber.h"
#include "../src/strand_internal.h"
#include "../src/strand_sched.h"

static uint64_t
clock_ns(clockid_t clock_id)
{
	struct timespec ts;

	(void)clock_gettime(clock_id, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

typedef struct {
	strand_scheduler_t *sched;
	uint64_t deadline;
	_Atomic int done;
	int rc;
} timer_state_t;

static void
timer_fiber(void *arg)
{
	timer_state_t *state = arg;

	state->rc = strand_fiber_sleep_until(state->sched, state->deadline);
	atomic_store_explicit(&state->done, 1, memory_order_release);
	strand_scheduler_stop(state->sched);
}

static int
push_timer_fiber(strand_scheduler_t *sched, timer_state_t *state)
{
	strand_fiber_t *fiber;
	void *base;
	unsigned long vg_id;

	fiber = fiber_alloc(&sched->dead_pool);
	if (fiber == NULL)
		return (-1);
	base = sched_stack_alloc(sched, STRAND_DEFAULT_STACK_SIZE, &vg_id);
	if (base == NULL) {
		fiber_free(&sched->dead_pool, fiber);
		return (-1);
	}
	fiber->stack_base = (char *)base + page_size();
	fiber->stack_size = STRAND_DEFAULT_STACK_SIZE;
	fiber->valgrind_stack_id = vg_id;
	fiber->entry_fn = timer_fiber;
	fiber->entry_arg = state;
	fiber->home_sched = sched;
	strand_context_init(&fiber->context,
	                    (char *)fiber->stack_base + fiber->stack_size,
	                    strand_fiber_entry_start, fiber);
	strand_fiber_tsan_init(fiber);
	atomic_store_explicit(&fiber->state, FIBER_RUNNABLE,
	                      memory_order_relaxed);
	run_queue_push(sched, fiber);
	return (0);
}

static void *
run_scheduler(void *arg)
{
	strand_scheduler_run(arg);
	return (NULL);
}

int
main(void)
{
	static const uint64_t delays[] = {100000ULL, 500000ULL, 1000000ULL,
	                                  10000000ULL};
	uint64_t cpu_start, elapsed, wall_start, cpu_used;
	unsigned int i, repeat;

	cpu_start = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	wall_start = clock_ns(CLOCK_MONOTONIC);
	for (repeat = 0; repeat < 20; repeat++) {
		for (i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
			strand_scheduler_t *sched;
			timer_state_t state;
			pthread_t thread;

			sched = strand_scheduler_create(NULL);
			if (sched == NULL)
				return (1);
			memset(&state, 0, sizeof(state));
			state.sched = sched;
			state.deadline = clock_ns(CLOCK_MONOTONIC) + delays[i];
			state.rc = STRAND_ERR_IO;
			atomic_init(&state.done, 0);
			if (push_timer_fiber(sched, &state) != 0 ||
			    pthread_create(&thread, NULL, run_scheduler,
			                   sched) != 0)
				return (1);
			(void)pthread_join(thread, NULL);
			if (!atomic_load_explicit(&state.done,
			                          memory_order_acquire) ||
			    state.rc != STRAND_OK)
				return (1);
			strand_scheduler_destroy(sched);
		}
	}
	elapsed = clock_ns(CLOCK_MONOTONIC) - wall_start;
	cpu_used = clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
	if (cpu_used > elapsed / 2 + 20000000ULL) {
		fprintf(
		    stderr, "real-clock timer spin: cpu=%llu ns wall=%llu ns\n",
		    (unsigned long long)cpu_used, (unsigned long long)elapsed);
		return (1);
	}
	return (0);
}
