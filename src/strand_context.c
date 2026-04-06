/*
 * strand_context.c — C wrapper around the assembly context swap.
 * errno save/restore, MXCSR/FPCR save/restore, sanitizer hooks.
 * See ARCHITECTURE.md §3.4, §3.7.
 */

#include <errno.h>
#include <stdint.h>

#include "../include/strand.h"
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

#elif defined(__aarch64__) || defined(__arm64__)

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
	int asan_track;

	saved_errno = errno;
	from->fp_ctrl = fp_ctrl_get();
	asan_track = (from->stack_base == NULL || from->stack_size == 0);

	if (asan_track)
		STRAND_ASAN_SWITCH_START(to->stack_base, to->stack_size);
	STRAND_TSAN_SWITCH(to);
	strand_context_swap(&from->context, &to->context);
	if (asan_track)
		STRAND_ASAN_SWITCH_FINISH();

	/* Restore our own FP control state — see note above. */
	fp_ctrl_set(from->fp_ctrl);
	errno = saved_errno;
}

/*
 * strand_fiber_trampoline — defined in the arch-specific strand_context.S.
 * First landing point for a newly fabricated context. Sets up the
 * ABI argument register from a callee-saved register, then calls the
 * fiber entry function.  Not part of the public API.
 */
void strand_fiber_trampoline(void);

/*
 * strand_context_init — fabricate an initial saved-register state.
 *
 * After this call, the first strand_context_swap to *ctx will begin
 * execution at entry(arg).  See strand_context.h for the full contract.
 */

#if defined(__x86_64__) || defined(__amd64__)

void
strand_context_init(strand_context_t *ctx, void *stack_top,
                    strand_fiber_fn_t entry, void *arg)
{
	uint64_t *sp;

	/*
	 * Write the trampoline address as the fabricated return address at
	 * stack_top - 8.  strand_context_swap's retq pops this value,
	 * advancing rsp to stack_top before jumping to the trampoline.
	 *
	 * stack_top must be 16-byte aligned (caller's responsibility).
	 * After retq: rsp = stack_top (16-byte aligned).
	 * After trampoline's `call *%rbx`: rsp = stack_top - 8
	 * (rsp%16 == 8), satisfying the SysV AMD64 ABI on entry to entry().
	 */
	sp = (uint64_t *)stack_top;
	sp[-1] = (uint64_t)(uintptr_t)strand_fiber_trampoline;

	/*
	 * rbx: entry — callee-saved; restored by strand_context_swap;
	 *      read by the trampoline and called via `call *%rbx`.
	 * r12: arg  — callee-saved; restored by strand_context_swap;
	 *      moved into rdi (SysV AMD64 first argument) by the trampoline.
	 * rsp: points at the fabricated return address (stack_top - 8).
	 */
	ctx->rbx = (uint64_t)(uintptr_t)entry;
	ctx->r12 = (uint64_t)(uintptr_t)arg;
	ctx->rsp = (uint64_t)(uintptr_t)(sp - 1);
	ctx->rbp = 0;
	ctx->r13 = 0;
	ctx->r14 = 0;
	ctx->r15 = 0;
}

#elif defined(__aarch64__) || defined(__arm64__)

void
strand_context_init(strand_context_t *ctx, void *stack_top,
                    strand_fiber_fn_t entry, void *arg)
{
	uint64_t sp;

	/*
	 * Align sp to 16 bytes — AAPCS64 requires 16-byte alignment at all
	 * times.  The caller should provide an aligned stack_top; we enforce
	 * the alignment defensively.
	 *
	 * Leave one 16-byte slot below stack_top so the first callee prologue
	 * push (stp x29, x30, [sp, #-16]!) always lands inside writable stack
	 * memory on platforms with stricter stack-boundary handling.
	 */
	sp = (uint64_t)(uintptr_t)stack_top & ~(uint64_t)15;
	sp -= 16;
	ctx->sp = sp;

	/*
	 * x30 (lr): trampoline address — strand_context_swap's ret branches
	 *           here on first context entry.
	 * x19: entry — callee-saved; read by the trampoline and called via
	 *      blr x19.
	 * x20: arg  — callee-saved; moved into x0 (AAPCS64 first argument)
	 *      by the trampoline before calling entry.
	 */
	ctx->x30 = (uint64_t)(uintptr_t)strand_fiber_trampoline;
	ctx->x19 = (uint64_t)(uintptr_t)entry;
	ctx->x20 = (uint64_t)(uintptr_t)arg;

	/* Clear remaining callee-saved registers. */
	ctx->x21 = 0;
	ctx->x22 = 0;
	ctx->x23 = 0;
	ctx->x24 = 0;
	ctx->x25 = 0;
	ctx->x26 = 0;
	ctx->x27 = 0;
	ctx->x28 = 0;
	ctx->x29 = 0; /* frame pointer */
	ctx->d8 = 0;
	ctx->d9 = 0;
	ctx->d10 = 0;
	ctx->d11 = 0;
	ctx->d12 = 0;
	ctx->d13 = 0;
	ctx->d14 = 0;
	ctx->d15 = 0;
}

#else
#error "Unsupported architecture: strand_context_init not defined"
#endif
