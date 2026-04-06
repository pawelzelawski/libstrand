/*
 * tests/test_layer1.c — Layer 1 (Phase 2) test suite.
 *
 * Tests the raw context switch in isolation: strand_context_switch,
 * strand_context_init, stack allocation with guard pages, register
 * preservation, errno save/restore, FP control register save/restore,
 * and documentation correctness.
 *
 * No scheduler, no run queue — purely Layer 1 primitives.
 * See DEVELOPMENT.md §"Tests for Phase 2".
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_harness.h"
#include "../src/strand_context.h"
#include "../src/strand_fiber.h"
#include "../src/strand_internal.h"

/* Default fiber stack size used by all tests. */
#define TEST_STACK_SIZE (64 * 1024)

/* -------------------------------------------------------------------------
 * Shared state between test "scheduler" and fiber functions.
 *
 * Each test that switches contexts declares a local strand_fiber_t for the
 * "scheduler" slot (saves the calling context into it) and a struct
 * test_ctx that carries the fiber's strand_fiber_t plus a pre-allocated
 * stack.  The fiber function switches back to sched_fiber when done.
 * -------------------------------------------------------------------------
 */

/*
 * A minimal fiber fixture: the fiber descriptor plus its stack backing.
 * set_up_fiber() initialises ctx_fiber and allocates a stack;
 * tear_down_fiber() frees it.
 */
struct test_fiber {
	strand_fiber_t fiber;
	void *stack_base;  /* mmap base (guard page at base) */
	size_t stack_size; /* usable stack size */
	unsigned long vg_id;
};

static void
set_up_sched_fiber(strand_fiber_t *sched)
{
	memset(sched, 0, sizeof(*sched));
	strand_fiber_tsan_bind_current(sched);
}

static void
tear_down_sched_fiber(strand_fiber_t *sched)
{
	(void)sched;
}

/*
 * set_up_fiber — allocate a stack and initialise a fiber context so that
 * the first strand_context_switch to it begins at entry(arg).
 * Returns 0 on success, -1 on failure.
 */
static int
set_up_fiber(struct test_fiber *tf, strand_fiber_fn_t entry, void *arg)
{
	char *stack_top;

	memset(tf, 0, sizeof(*tf));
	tf->stack_size = TEST_STACK_SIZE;
	tf->stack_base = stack_alloc(tf->stack_size, &tf->vg_id);
	if (tf->stack_base == NULL)
		return (-1);
	tf->fiber.stack_base = (char *)tf->stack_base + page_size();
	tf->fiber.stack_size = tf->stack_size;
	strand_fiber_tsan_init(&tf->fiber);

	/* Usable top: one past the highest usable byte. */
	stack_top = (char *)tf->fiber.stack_base + tf->fiber.stack_size;

	strand_context_init(&tf->fiber.context, stack_top, entry, arg);
	return (0);
}

static void
tear_down_fiber(struct test_fiber *tf)
{
	strand_fiber_tsan_destroy(&tf->fiber);
	stack_free(tf->stack_base, tf->stack_size, tf->vg_id);
}

/* =========================================================================
 * test_context_switch_returns
 *
 * Switch from A (main thread context) to B; B switches back; verify
 * control returns to A.
 * =========================================================================
 */

static strand_fiber_t *g_sched_01; /* pointer to sched slot for fiber B */
static struct test_fiber g_tf_01;
static int g_switch_01_entered;

static void
fiber_01(void *arg)
{
	(void)arg;
	g_switch_01_entered = 1;
	strand_context_switch(&g_tf_01.fiber, g_sched_01);
	/* Must not be reached — fiber must not return to trampoline. */
	abort();
}

static int
test_context_switch_returns(void)
{
	strand_fiber_t sched;

	g_sched_01 = &sched;
	g_switch_01_entered = 0;

	set_up_sched_fiber(&sched);
	if (set_up_fiber(&g_tf_01, fiber_01, NULL) != 0)
		return (tear_down_sched_fiber(&sched), 1);

	strand_context_switch(&sched, &g_tf_01.fiber);

	tear_down_fiber(&g_tf_01);
	tear_down_sched_fiber(&sched);
	return (g_switch_01_entered == 1) ? 0 : 1;
}

/* =========================================================================
 * test_context_gpr_preserved
 *
 * Verify that all callee-saved GPRs are preserved across a round-trip
 * context switch.  We exploit the compiler's own reliance on callee-save
 * semantics: put the expected sentinel values in many local variables
 * (which -O1 stores in callee-saved registers across the call to
 * strand_context_switch), then verify they are unchanged after the
 * switch returns.  Any corruption of callee-saved registers by
 * strand_context_swap would make the post-switch comparison wrong.
 *
 * The noinline attribute ensures the locals span the call boundary and
 * the sentinels cannot be optimised away.
 * =========================================================================
 */

static strand_fiber_t *g_sched_02;
static struct test_fiber g_tf_02;

static void
fiber_02(void *arg)
{
	(void)arg;
	strand_context_switch(&g_tf_02.fiber, g_sched_02);
	abort();
}

static int __attribute__((noinline))
do_gpr_switch(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
              uint64_t f, strand_fiber_t *sched)
{
	strand_context_switch(sched, &g_tf_02.fiber);
	/*
	 * If strand_context_swap corrupted any callee-saved register the
	 * compiler reloaded a-f from, the comparison below fails.
	 */
	return (a == 0xAA00000000000001ULL && b == 0xBB00000000000002ULL &&
	        c == 0xCC00000000000003ULL && d == 0xDD00000000000004ULL &&
	        e == 0xEE00000000000005ULL && f == 0xFF00000000000006ULL)
	           ? 0
	           : 1;
}

static int
test_context_gpr_preserved(void)
{
	strand_fiber_t sched;

	g_sched_02 = &sched;
	set_up_sched_fiber(&sched);
	if (set_up_fiber(&g_tf_02, fiber_02, NULL) != 0)
		return (tear_down_sched_fiber(&sched), 1);

	int result =
	    do_gpr_switch(0xAA00000000000001ULL, 0xBB00000000000002ULL,
	                  0xCC00000000000003ULL, 0xDD00000000000004ULL,
	                  0xEE00000000000005ULL, 0xFF00000000000006ULL, &sched);

	tear_down_fiber(&g_tf_02);
	tear_down_sched_fiber(&sched);
	return result;
}

/* =========================================================================
 * test_context_errno_preserved
 *
 * Set errno to a known value; switch to B (which changes errno); switch
 * back; verify errno in A is restored.
 * =========================================================================
 */

static strand_fiber_t *g_sched_03;
static struct test_fiber g_tf_03;

static void
fiber_03(void *arg)
{
	(void)arg;
	errno = EBADF; /* different from the sentinel below */
	strand_context_switch(&g_tf_03.fiber, g_sched_03);
	abort();
}

static int
test_context_errno_preserved(void)
{
	strand_fiber_t sched;
	int saved;

	g_sched_03 = &sched;
	set_up_sched_fiber(&sched);
	if (set_up_fiber(&g_tf_03, fiber_03, NULL) != 0)
		return (tear_down_sched_fiber(&sched), 1);

	errno = EPERM; /* sentinel */
	strand_context_switch(&sched, &g_tf_03.fiber);
	saved = errno;

	tear_down_fiber(&g_tf_03);
	tear_down_sched_fiber(&sched);
	return (saved == EPERM) ? 0 : 1;
}

/* =========================================================================
 * test_context_mxcsr_preserved (x86_64) / test_context_fpcr_preserved
 * (AArch64)
 *
 * Set a non-default FP control register value; switch to B (which sets a
 * different value); switch back; verify the original value is restored.
 * =========================================================================
 */

static strand_fiber_t *g_sched_04;
static struct test_fiber g_tf_04;

#if defined(__x86_64__) || defined(__amd64__)

static uint32_t
read_mxcsr(void)
{
	uint32_t v;
	__asm__ volatile("stmxcsr %0" : "=m"(v));
	return v;
}

static void
write_mxcsr(uint32_t v)
{
	__asm__ volatile("ldmxcsr %0" : : "m"(v));
}

static void
fiber_04(void *arg)
{
	(void)arg;
	/* Switching MXCSR FTZ flag (bit 15) — harmless test mutation. */
	write_mxcsr(read_mxcsr() | 0x8000u);
	strand_context_switch(&g_tf_04.fiber, g_sched_04);
	abort();
}

static int
test_context_mxcsr_preserved(void)
{
	strand_fiber_t sched;
	uint32_t before, after;

	g_sched_04 = &sched;
	set_up_sched_fiber(&sched);
	if (set_up_fiber(&g_tf_04, fiber_04, NULL) != 0)
		return (tear_down_sched_fiber(&sched), 1);

	before = read_mxcsr();
	/* Ensure FTZ is cleared in our own context before the switch. */
	write_mxcsr(before & ~0x8000u);
	before = read_mxcsr();

	strand_context_switch(&sched, &g_tf_04.fiber);
	after = read_mxcsr();

	/* Restore default MXCSR regardless of test outcome. */
	write_mxcsr(before);

	tear_down_fiber(&g_tf_04);
	tear_down_sched_fiber(&sched);
	return (after == before) ? 0 : 1;
}

#elif defined(__aarch64__)

static uint64_t
read_fpcr(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, fpcr" : "=r"(v));
	return v;
}

static void
write_fpcr(uint64_t v)
{
	__asm__ volatile("msr fpcr, %0" : : "r"(v));
}

static void
fiber_04(void *arg)
{
	(void)arg;
	/* Set DN (default NaN) bit in FPCR — bit 25, harmless mutation. */
	write_fpcr(read_fpcr() | (1UL << 25));
	strand_context_switch(&g_tf_04.fiber, g_sched_04);
	abort();
}

static int
test_context_fpcr_preserved(void)
{
	strand_fiber_t sched;
	uint64_t before, after;

	g_sched_04 = &sched;
	set_up_sched_fiber(&sched);
	if (set_up_fiber(&g_tf_04, fiber_04, NULL) != 0)
		return (tear_down_sched_fiber(&sched), 1);

	before = read_fpcr();
	/* Ensure DN bit is clear in our own FPCR. */
	write_fpcr(before & ~(1UL << 25));
	before = read_fpcr();

	strand_context_switch(&sched, &g_tf_04.fiber);
	after = read_fpcr();

	write_fpcr(before); /* restore */

	tear_down_fiber(&g_tf_04);
	tear_down_sched_fiber(&sched);
	return (after == before) ? 0 : 1;
}

#else
#error "FP control test: unsupported architecture"
#endif

/* =========================================================================
 * test_context_stack_alignment
 *
 * Verify the stack pointer is correctly aligned inside the fiber function.
 *
 * x86_64 (SysV AMD64): at the call-site sp%16==0, the `call` instruction
 * pushes 8 bytes so sp%16==8 at function entry; the standard prologue
 * (`push rbp`) then decrements sp by another 8, leaving sp%16==0 inside
 * the function body — which is what this test measures.
 *
 * AArch64 (AAPCS64): sp must be 16-byte aligned at all times; the
 * prologue (`stp x29,x30,[sp,#-N]!`) preserves alignment, so sp%16==0
 * inside the function body.
 *
 * Both architectures: we verify sp%16==0 inside the fiber function body,
 * which is the observable consequence of correct ABI alignment at entry.
 * =========================================================================
 */

static strand_fiber_t *g_sched_05;
static struct test_fiber g_tf_05;
static uintptr_t g_sp_alignment_05;

static void
fiber_05(void *arg)
{
	uintptr_t sp;
	(void)arg;

#if defined(__x86_64__) || defined(__amd64__)
	__asm__ volatile("movq %%rsp, %0" : "=r"(sp));
#elif defined(__aarch64__)
	__asm__ volatile("mov %0, sp" : "=r"(sp));
#else
	sp = 0;
#endif
	g_sp_alignment_05 = sp;
	strand_context_switch(&g_tf_05.fiber, g_sched_05);
	abort();
}

static int
test_context_stack_alignment(void)
{
	strand_fiber_t sched;

	g_sched_05 = &sched;
	g_sp_alignment_05 = 0;
	set_up_sched_fiber(&sched);
	if (set_up_fiber(&g_tf_05, fiber_05, NULL) != 0)
		return (tear_down_sched_fiber(&sched), 1);

	strand_context_switch(&sched, &g_tf_05.fiber);
	tear_down_fiber(&g_tf_05);
	tear_down_sched_fiber(&sched);

	/* sp inside the function body (after prologue) must be 16-byte aligned
	 * on both x86_64 and AArch64. */
	return (g_sp_alignment_05 % 16 == 0) ? 0 : 1;
}

/* =========================================================================
 * test_context_init_entry_fires
 *
 * Fabricate a fiber context with strand_context_init; switch to it; verify
 * the entry function was called (sets a flag); switch back.
 * =========================================================================
 */

static strand_fiber_t *g_sched_06;
static struct test_fiber g_tf_06;
static int g_entry_fired_06;

static void
fiber_06(void *arg)
{
	(void)arg;
	g_entry_fired_06 = 1;
	strand_context_switch(&g_tf_06.fiber, g_sched_06);
	abort();
}

static int
test_context_init_entry_fires(void)
{
	strand_fiber_t sched;

	g_sched_06 = &sched;
	g_entry_fired_06 = 0;
	set_up_sched_fiber(&sched);
	if (set_up_fiber(&g_tf_06, fiber_06, NULL) != 0)
		return (tear_down_sched_fiber(&sched), 1);

	strand_context_switch(&sched, &g_tf_06.fiber);
	tear_down_fiber(&g_tf_06);
	tear_down_sched_fiber(&sched);
	return (g_entry_fired_06 == 1) ? 0 : 1;
}

/* =========================================================================
 * test_context_init_arg_delivered
 *
 * Verify the arg pointer passed to strand_context_init arrives as the
 * first argument of the fiber entry function.
 * =========================================================================
 */

static strand_fiber_t *g_sched_07;
static struct test_fiber g_tf_07;
static void *g_received_arg_07;

static void
fiber_07(void *arg)
{
	g_received_arg_07 = arg;
	strand_context_switch(&g_tf_07.fiber, g_sched_07);
	abort();
}

static int
test_context_init_arg_delivered(void)
{
	strand_fiber_t sched;
	static int sentinel = 0xdeadbeef;

	g_sched_07 = &sched;
	g_received_arg_07 = NULL;
	set_up_sched_fiber(&sched);
	if (set_up_fiber(&g_tf_07, fiber_07, &sentinel) != 0)
		return (tear_down_sched_fiber(&sched), 1);

	strand_context_switch(&sched, &g_tf_07.fiber);
	tear_down_fiber(&g_tf_07);
	tear_down_sched_fiber(&sched);
	return (g_received_arg_07 == &sentinel) ? 0 : 1;
}

/* =========================================================================
 * test_guard_page_present
 *
 * In a subprocess, allocate a stack and deliberately write one byte below
 * the guard page boundary.  Expect SIGSEGV.  The parent waits for the
 * subprocess; the test passes if the child terminated by signal (not by
 * normal exit).
 * =========================================================================
 */

static int
test_guard_page_present(void)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return (1); /* fork failed */

	if (pid == 0) {
		/* Child: allocate a stack and write into the guard page. */
		unsigned long vg_id;
		void *base;
		volatile char *guard;

		base = stack_alloc(TEST_STACK_SIZE, &vg_id);
		if (base == NULL)
			_exit(2);

		/*
		 * Reset SIGSEGV to the default action so the OS terminates
		 * the child via signal rather than ASan intercepting it.
		 * Without this, ASan's signal handler catches the fault and
		 * calls _exit(), which the parent would see as a normal exit.
		 */
		signal(SIGSEGV, SIG_DFL);

		/*
		 * The guard page occupies [base, base + page_size()).
		 * Write to the last byte of the guard page to trigger SIGSEGV.
		 */
		guard = (volatile char *)base + page_size() - 1;
		*guard = 'x'; /* SIGSEGV expected here */

		/*
		 * If we reach here the guard page was not effective.
		 * Exit normally so the parent knows the test failed.
		 */
		_exit(0);
	}

	/* Parent: wait for child. */
	if (waitpid(pid, &status, 0) < 0)
		return (1);

	/*
	 * Test passes only if the child was terminated by a signal.
	 * A normal exit (exit code 0 or 2) means the guard was absent.
	 */
	return (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) ? 0 : 1;
}

/* =========================================================================
 * test_stack_alloc_free_roundtrip
 *
 * Allocate and free a stack; verify the operations complete without error.
 * Valgrind will flag any leak or deregistration mismatch.
 * =========================================================================
 */

static int
test_stack_alloc_free_roundtrip(void)
{
	unsigned long vg_id;
	void *base;

	base = stack_alloc(TEST_STACK_SIZE, &vg_id);
	if (base == NULL)
		return (1);

	stack_free(base, TEST_STACK_SIZE, vg_id);
	return (0);
}

/* =========================================================================
 * test_x87_unsupported_documented
 *
 * Open strand_context.h as a text file and verify it contains an explicit
 * warning that x87 FP state (including long double) is not saved across
 * context switches.  This is a documentation correctness test.
 * =========================================================================
 */

static int
test_x87_unsupported_documented(void)
{
	FILE *f;
	char line[256];
	int found_x87 = 0;
	int found_ldouble = 0;

	f = fopen("src/strand_context.h", "r");
	if (f == NULL) {
		/* Try from project root via relative path. */
		f = fopen("../src/strand_context.h", "r");
	}
	if (f == NULL)
		return (1);

	while (fgets(line, sizeof(line), f) != NULL) {
		if (strstr(line, "x87") != NULL)
			found_x87 = 1;
		if (strstr(line, "long double") != NULL)
			found_ldouble = 1;
	}
	fclose(f);

	return (found_x87 && found_ldouble) ? 0 : 1;
}

/* =========================================================================
 * Suite runner — called from run_tests.c
 * =========================================================================
 */

void
run_layer1_tests(void)
{
	RUN("context_switch_returns", test_context_switch_returns);
	RUN("context_gpr_preserved", test_context_gpr_preserved);
	RUN("context_errno_preserved", test_context_errno_preserved);
#if defined(__x86_64__) || defined(__amd64__)
	RUN("context_mxcsr_preserved", test_context_mxcsr_preserved);
#elif defined(__aarch64__)
	RUN("context_fpcr_preserved", test_context_fpcr_preserved);
#endif
	RUN("context_stack_alignment", test_context_stack_alignment);
	RUN("context_init_entry_fires", test_context_init_entry_fires);
	RUN("context_init_arg_delivered", test_context_init_arg_delivered);
	RUN("guard_page_present", test_guard_page_present);
	RUN("stack_alloc_free_roundtrip", test_stack_alloc_free_roundtrip);
	RUN("x87_unsupported_documented", test_x87_unsupported_documented);
}
