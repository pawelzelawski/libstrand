/*
 * tests/run_tests.c - test binary entry point
 *
 * Runs all test suites in order. Add suite runner calls here as
 * new test files are introduced in each phase.
 *
 * Exit code: 0 if all tests passed, 1 if any test failed.
 *
 * See TECH_STACK.md §6.2.
 */

#include <stdio.h>

#include "test_harness.h"

int tests_run = 0;
int tests_passed = 0;

void run_layer1_tests(void);
void run_layer2_tests(void);
void run_layer3_tests(void);
void run_layer4_tests(void);
void run_layer5_tests(void);
void run_integration_tests(void);

int
main(void)
{
        run_layer1_tests();
        run_layer2_tests();
        run_layer3_tests();
        run_layer4_tests();
        run_layer5_tests();
        run_integration_tests();

	printf("%d/%d tests passed\n", tests_passed, tests_run);
	return (tests_passed == tests_run) ? 0 : 1;
}
