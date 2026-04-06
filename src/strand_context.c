/*
 * strand_context.c — C wrapper around the assembly context swap.
 * errno save/restore, MXCSR/FPCR save/restore, sanitizer hooks.
 * See ARCHITECTURE.md §3.4, §3.7.
 */

#include <errno.h>
#include <stdint.h>

#include "strand_context.h"
#include "strand_internal.h"

/*
 * fp_ctrl_get / fp_ctrl_set — save and restore floating-point control
 * registers around every context swap. Called only from
 * strand_context_switch; not a public interface.
 *
 * x86_64: MXCSR is a 32-bit register that controls SSE exception masks,
 * flush-to-zero, and rounding mode. It is not callee-saved by the SysV
 * AMD64 ABI, so fibers may observe a modified MXCSR without these calls.
 * Stored in low 32 bits of fp_ctrl; upper 32 bits are unused.
 * See ARCHITECTURE.md §3.2.
 *
 * AArch64: FPCR controls rounding, flush-to-zero, and exception enables.
 * FPSR holds cumulative exception flags and the QC/IDC bits. Neither is
 * callee-saved by AAPCS64 in all situations. FPCR is packed in the
 * low 32 bits of fp_ctrl; FPSR in the high 32 bits.
 * See ARCHITECTURE.md §3.3.
 */
#if defined(__x86_64__) || defined(__amd64__)

static inline uint64_t
fp_ctrl_get(void)
{
	uint32_t mxcsr;
	__asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
	return (uint64_t)mxcsr;
}

static inline void
fp_ctrl_set(uint64_t ctrl)
{
	uint32_t mxcsr = (uint32_t)ctrl;
	__asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

#elif defined(__aarch64__)

static inline uint64_t
fp_ctrl_get(void)
{
	uint64_t fpcr, fpsr;
	__asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
	__asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
	return (fpsr << 32) | (fpcr & 0xffffffffU);
}

static inline void
fp_ctrl_set(uint64_t ctrl)
{
	uint64_t fpcr = ctrl & 0xffffffffU;
	uint64_t fpsr = ctrl >> 32;
	__asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
	__asm__ volatile("msr fpsr, %0" : : "r"(fpsr));
}

#else
#error "Unsupported architecture: fp_ctrl_get/fp_ctrl_set not defined"
#endif

/*
 * strand_context_switch — switch execution between two fibers.
 *
 * Operation (in order):
 *   1. Save errno.
 *   2. Save current FP control registers to from->fp_ctrl.
 *   3. ASan start-switch hook: tells ASan the active stack is changing.
 *   4. TSan switch hook: tells TSan about the happens-before edge.
 *   5. strand_context_swap: raw register save, stack switch, register restore.
 *   6. ASan finish-switch hook: tells ASan the new active stack.
 *   7. Restore FP control registers from from->fp_ctrl (our own saved state
 *      — restoring from->fp_ctrl ensures each fiber resumes with the FP
 *      control state it had when it was last suspended).
 *   8. Restore errno.
 *
 * Note: DEVELOPMENT.md §2.4 says step 7 restores from to->fp_ctrl.
 * This appears to be a specification typo: restoring from->fp_ctrl is the
 * correct behaviour. Restoring to->fp_ctrl would give fiber A the FP
 * control state that fiber B had when B last yielded, which is wrong.
 *
 * TSan: must be called on the outgoing fiber (from) immediately before
 * strand_context_swap — cannot be called after.
 */
void
strand_context_switch(strand_fiber_t *from, strand_fiber_t *to)
{
	int saved_errno;

	saved_errno = errno;
	from->fp_ctrl = fp_ctrl_get();

	STRAND_ASAN_SWITCH_START(to->stack_base, to->stack_size, NULL, 0);
	STRAND_TSAN_SWITCH(to);
	strand_context_swap(&from->context, &to->context);
	STRAND_ASAN_SWITCH_FINISH();

	/* Restore our own FP control state — see note above. */
	fp_ctrl_set(from->fp_ctrl);
	errno = saved_errno;
}
