/*
 * bench/bench_offload.c - blocking syscall offload pool throughput.
 *
 * Measures offload submissions/completions per second and pool size
 * sensitivity (1, 2, 4, 8, 16 threads).
 *
 * The scheduler runs in worker mode (strand_scheduler_run on a pthread).
 * drive_to_idle() cannot be used: it exits the advance loop when the run
 * queue empties, but the fiber is FIBER_PARKED_OFFLOAD waiting for the
 * offload thread's inject.  Worker mode ensures the inject drain (Step 1)
 * fires on every cycle and wakes the fiber when the offload completes.
 *
 * See REPOSITORY_STRUCTURE.md 5, DEVELOPMENT.md 7.2, ARCHITECTURE.md 6.5.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/strand.h"
#include "../src/strand_sched.h"
#include "bench_common.h"
#define OFFLOAD_ITERS 5000UL
static void
noop_fn(void *arg, void *result_slot)
{
(void)arg;
*(int *)result_slot = 0;
}
typedef struct {
strand_offload_pool_t *pool;
bench_done_t           done;
uint64_t               t0;
uint64_t               t1;
} offload_bench_t;
static void
offload_fiber(void *arg)
{
offload_bench_t    *b     = arg;
strand_scheduler_t *sched = strand_sched_current_tls;
int                 result;
uint64_t            i;
int                 rc;
b->t0 = bench_now_ns();
for (i = 0; i < OFFLOAD_ITERS; i++) {
do {
rc = strand_fiber_offload(sched, b->pool,
    noop_fn, NULL, &result);
if (rc == STRAND_EAGAIN)
strand_fiber_yield(sched);
} while (rc == STRAND_EAGAIN);
}
b->t1 = bench_now_ns();
bench_done_signal(&b->done);
}
static void
run_pool_size(size_t pool_threads)
{
strand_runtime_t      *rt;
strand_worker_t       *w;
strand_offload_pool_t *pool;
offload_bench_t        bench;
uint64_t               elapsed;
char                   label[64];
rt = strand_runtime_init(NULL);
if (rt == NULL) {
fprintf(stderr, "strand_runtime_init failed\n");
return;
}
w = strand_worker_start(rt, NULL);
if (w == NULL) {
fprintf(stderr, "strand_worker_start failed\n");
strand_runtime_destroy(rt);
return;
}
pool = strand_offload_pool_init(pool_threads);
if (pool == NULL) {
fprintf(stderr, "strand_offload_pool_init failed\n");
strand_runtime_destroy(rt);
return;
}
memset(&bench, 0, sizeof(bench));
bench.pool = pool;
bench_done_init(&bench.done);
if (strand_runtime_spawn(rt, offload_fiber, &bench,
    STRAND_DEFAULT_STACK_SIZE, w, NULL) != STRAND_OK) {
fprintf(stderr, "strand_runtime_spawn failed\n");
strand_offload_pool_destroy(pool);
strand_runtime_destroy(rt);
bench_done_destroy(&bench.done);
return;
}
bench_done_wait(&bench.done);
elapsed = bench.t1 - bench.t0;
snprintf(label, sizeof(label),
    "offload pool=%zu thread(s) (ns/offload)", pool_threads);
bench_print_result(label, OFFLOAD_ITERS, elapsed);
strand_offload_pool_destroy(pool);
strand_runtime_destroy(rt);
bench_done_destroy(&bench.done);
}
int
main(void)
{
static const size_t pool_sizes[] = { 1, 2, 4, 8, 16 };
size_t              i;
printf("=== bench_offload ===\n");
printf("    %zu sequential offload calls (no-op fn) per run\n",
    OFFLOAD_ITERS);
printf("\n");
bench_print_hw();
for (i = 0; i < sizeof(pool_sizes) / sizeof(pool_sizes[0]); i++)
run_pool_size(pool_sizes[i]);
printf("\n");
return (0);
}
