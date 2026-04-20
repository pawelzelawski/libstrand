/*
 * bench/bench_cross_worker.c - cross-worker wakeup latency.
 *
 * Measures the time from when the host thread calls strand_fiber_cancel
 * on a sleeping fiber to when that fiber resumes on its home worker.
 *
 * Methodology:
 *   A generation counter protocol avoids the condvar-reset race:
 *   - _Atomic uint64_t sleep_gen: incremented by the fiber each time it
 *     calls strand_fiber_sleep_until (just before parking).
 *   - _Atomic uint64_t wake_gen: incremented by the fiber each time it
 *     wakes from a cancel.
 *   - Host spins briefly until sleep_gen > wake_gen (fiber is parked),
 *     records T0, issues strand_fiber_cancel, then spins until
 *     wake_gen increments (fiber woke), reads T1.
 *   - Latency = T1 - T0 covers: inject-queue enqueue + worker wakeup
 *     (eventfd write) + worker epoll_wait return + inject drain + fiber
 *     dispatch.
 *
 * See REPOSITORY_STRUCTURE.md 5, DEVELOPMENT.md 7.2, ARCHITECTURE.md 6.3.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef STRAND_LINUX
#include <unistd.h>
#endif
#include "../include/strand.h"
#include "../src/strand_sched.h"
#include "bench_common.h"
#define CW_ITERS 10000UL
#define FAR_NS   (60ULL * 1000000000ULL)  /* 60 s - never fires naturally */
typedef struct {
_Atomic uint64_t sleep_gen;   /* incremented just before each sleep */
_Atomic uint64_t wake_gen;    /* incremented just after each cancel wake */
_Atomic uint64_t t1;          /* fiber wake timestamp */
_Atomic int      quit;
} cw_state_t;
static void
cw_fiber(void *arg)
{
cw_state_t         *s     = arg;
strand_scheduler_t *sched = strand_sched_current_tls;
uint64_t            deadline;
while (!atomic_load_explicit(&s->quit, memory_order_acquire)) {
deadline = bench_now_ns() + FAR_NS;
/* Signal host that we are about to park. */
atomic_fetch_add_explicit(&s->sleep_gen, 1,
    memory_order_release);
strand_fiber_sleep_until(sched, deadline);
/* Record wake time and signal host we are awake. */
atomic_store_explicit(&s->t1, bench_now_ns(),
    memory_order_relaxed);
atomic_fetch_add_explicit(&s->wake_gen, 1,
    memory_order_release);
}
}
/* spin_until_eq: spin until *a == target, with a short pause each iteration
 * to avoid saturating the memory bus when both threads share a core. */
static void
spin_until_eq(const _Atomic uint64_t *a, uint64_t target)
{
while (atomic_load_explicit(a, memory_order_acquire) != target) {
#ifdef STRAND_LINUX
/* pause hint reduces power and memory contention on x86 */
__asm__ volatile("pause" ::: "memory");
#endif
}
}
int
main(void)
{
strand_runtime_t      *rt;
strand_worker_t       *w;
cw_state_t             state;
strand_fiber_handle_t  handle;
uint64_t               t0, t1;
uint64_t               total_ns = 0;
uint64_t               i;
uint64_t               cur_sleep, cur_wake;
int                    rc;
printf("=== bench_cross_worker ===\n");
bench_print_hw();
memset(&state, 0, sizeof(state));
atomic_init(&state.sleep_gen, 0);
atomic_init(&state.wake_gen,  0);
atomic_init(&state.t1,        0);
atomic_init(&state.quit,      0);
rt = strand_runtime_init(NULL);
if (rt == NULL) {
fprintf(stderr, "strand_runtime_init failed\n");
return (1);
}
w = strand_worker_start(rt, NULL);
if (w == NULL) {
fprintf(stderr, "strand_worker_start failed\n");
strand_runtime_destroy(rt);
return (1);
}
rc = strand_runtime_spawn(rt, cw_fiber, &state,
    STRAND_DEFAULT_STACK_SIZE, w, &handle);
if (rc != STRAND_OK) {
fprintf(stderr, "strand_runtime_spawn failed: %d\n", rc);
strand_runtime_destroy(rt);
return (1);
}
cur_wake = 0;
for (i = 0; i < CW_ITERS; i++) {
/* Wait until fiber has incremented sleep_gen past wake_gen. */
cur_sleep = cur_wake + 1;
spin_until_eq(&state.sleep_gen, cur_sleep);
/* Record T0 and issue cross-worker cancel. */
t0 = bench_now_ns();
strand_fiber_cancel(handle);
/* Wait for fiber to record its wake time. */
cur_wake = cur_sleep;
spin_until_eq(&state.wake_gen, cur_wake);
t1 = atomic_load_explicit(&state.t1, memory_order_acquire);
if (t1 > t0)
total_ns += t1 - t0;
}
/* Tell fiber to exit on its next loop. */
atomic_store_explicit(&state.quit, 1, memory_order_release);
strand_fiber_cancel(handle);   /* wake it one last time */
strand_runtime_destroy(rt);
bench_print_result("cross-worker cancel->resume (ns/wakeup)",
    CW_ITERS, total_ns);
printf("\n");
return (0);
}
