/*
 * tests/test_harness.h - minimal test harness
 *
 * Each test function has the signature:
 *   static int test_foo(void);
 * returning 0 on success, non-zero on failure.
 *
 * Usage in run_tests.c:
 *   RUN("test_foo", test_foo);
 *
 * See TECH_STACK.md §6.1.
 */

#ifndef STRAND_TEST_HARNESS_H
#define STRAND_TEST_HARNESS_H

#include <stdio.h>

/*
 * Counters incremented by RUN(). Defined in run_tests.c.
 */
extern int tests_run;
extern int tests_passed;

/*
 * RUN(name, fn) - run one test function and record the result.
 */
#define RUN(name, fn)                                                          \
	do {                                                                   \
		int _r = (fn)();                                               \
		tests_run++;                                                   \
		if (_r == 0) {                                                 \
			tests_passed++;                                        \
		} else {                                                       \
			fprintf(stderr, "FAIL: %s\n", (name));                 \
		}                                                              \
	} while (0)

#endif /* STRAND_TEST_HARNESS_H */
