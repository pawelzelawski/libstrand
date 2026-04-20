/*
 * bench/bench_common.h - shared utilities for libstrand benchmarks.
 *
 * Provides:
 *   bench_now_ns()          - CLOCK_MONOTONIC timestamp in nanoseconds
 *   bench_print_hw()        - print CPU model and core count
 *   bench_print_result()    - formatted ns/op + Mop/s output line
 *   bench_done_t            - fiber-to-host completion signal
 *
 * See DEVELOPMENT.md 7.2, REPOSITORY_STRUCTURE.md 5.
 */
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef STRAND_LINUX
#include <unistd.h>
#endif


/*
 * bench_now_ns - return a CLOCK_MONOTONIC timestamp in nanoseconds.
 */
static inline uint64_t
bench_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * bench_print_hw - print CPU model and online core count.
 * Platform-specific: /proc/cpuinfo on Linux, sysctl on OpenBSD.
 */
static inline void
bench_print_hw(void)
{
#ifdef STRAND_LINUX
	char  line[256];
	FILE *fp;

	fp = fopen("/proc/cpuinfo", "r");
	if (fp != NULL) {
		while (fgets(line, sizeof(line), fp) != NULL) {
			if (strncmp(line, "model name", 10) == 0) {
				char *p = strchr(line, ':');
				if (p != NULL)
					printf("CPU:  %s", p + 2);
				break;
			}
		}
		fclose(fp);
	}
	printf("CPUs: %ld online\n", sysconf(_SC_NPROCESSORS_ONLN));
#endif
#ifdef STRAND_OPENBSD
	/*
	 * Use popen(sysctl -n) to avoid pulling in <sys/sysctl.h>, which
	 * requires BSD-visible types (u_long) that are hidden when
	 * _POSIX_C_SOURCE / _XOPEN_SOURCE are defined.  popen is declared
	 * in <stdio.h> which is already included above.
	 */
	{
		char  buf[128];
		FILE *fp;

		fp = popen("sysctl -n hw.model 2>/dev/null", "r");
		if (fp != NULL) {
			if (fgets(buf, sizeof(buf), fp) != NULL)
				printf("CPU:  %s", buf);
			pclose(fp);
		}
		fp = popen("sysctl -n hw.ncpu 2>/dev/null", "r");
		if (fp != NULL) {
			if (fgets(buf, sizeof(buf), fp) != NULL)
				printf("CPUs: %s", buf);
			pclose(fp);
		}
	}
#endif
	printf("\n");
}

/*
 * bench_print_result - print one benchmark result line.
 *
 * label      - benchmark variant description (≤40 chars looks best)
 * ops        - number of operations measured
 * elapsed_ns - total wall time in nanoseconds
 */
static inline void
bench_print_result(const char *label, uint64_t ops, uint64_t elapsed_ns)
{
	uint64_t ns_per_op;
	uint64_t mops;

	if (elapsed_ns == 0)
		elapsed_ns = 1;
	ns_per_op = elapsed_ns / ops;
	/* Kop/s = ops * 1e6 / elapsed_ns  (ops/ns * 1e6 = ops/us = Kop/s) */
	mops = ops * 1000000ULL / elapsed_ns;
	printf("  %-42s  %8" PRIu64 " ns/op   %8" PRIu64 " Kop/s\n",
	    label, ns_per_op, mops);
}

/*
 * bench_done_t - mutex+condvar completion latch.
 *
 * A fiber running on a worker thread calls bench_done_signal() to wake
 * the host thread blocked in bench_done_wait().  Use bench_done_reset()
 * to reuse the same latch for successive iterations.
 */
typedef struct bench_done {
	pthread_mutex_t mu;
	pthread_cond_t  cv;
	int             done;
} bench_done_t;

static inline void
bench_done_init(bench_done_t *d)
{
	pthread_mutex_init(&d->mu, NULL);
	pthread_cond_init(&d->cv, NULL);
	d->done = 0;
}

static inline void
bench_done_destroy(bench_done_t *d)
{
	pthread_mutex_destroy(&d->mu);
	pthread_cond_destroy(&d->cv);
}

static inline void
bench_done_signal(bench_done_t *d)
{
	pthread_mutex_lock(&d->mu);
	d->done = 1;
	pthread_cond_signal(&d->cv);
	pthread_mutex_unlock(&d->mu);
}

static inline void
bench_done_wait(bench_done_t *d)
{
	pthread_mutex_lock(&d->mu);
	while (!d->done)
		pthread_cond_wait(&d->cv, &d->mu);
	pthread_mutex_unlock(&d->mu);
}

static inline void
bench_done_reset(bench_done_t *d)
{
	pthread_mutex_lock(&d->mu);
	d->done = 0;
	pthread_mutex_unlock(&d->mu);
}

#endif /* BENCH_COMMON_H */

