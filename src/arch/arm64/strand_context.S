/*
 * strand_context.S — fiber context save and restore (AArch64 AAPCS64)
 *
 * void strand_context_swap(strand_context_t *old, strand_context_t *new);
 *   x0 = old   (first argument under AAPCS64)
 *   x1 = new   (second argument under AAPCS64)
 *
 * Saves callee-saved GPRs, sp, and callee-saved FP/SIMD lower halves to
 * *old, loads them from *new, and returns via the restored link register
 * (x30) — resuming execution at the address that suspended that context,
 * or the fabricated entry point from strand_context_init.
 *
 * Calling convention: AAPCS64 (Linux arm64, OpenBSD arm64).
 * Callee-saved GPRs: x19-x28, x29 (fp), x30 (lr).
 * Callee-saved FP/SIMD lower halves: d8-d15.
 * FPCR/FPSR: saved and restored by the C wrapper, not here.
 * See ARCHITECTURE.md §3.3.
 *
 * strand_context_t layout (AArch64), STRAND_CONTEXT_SIZE = 168:
 *   offset   0 — x19
 *   offset   8 — x20
 *   offset  16 — x21
 *   offset  24 — x22
 *   offset  32 — x23
 *   offset  40 — x24
 *   offset  48 — x25
 *   offset  56 — x26
 *   offset  64 — x27
 *   offset  72 — x28
 *   offset  80 — x29 (frame pointer)
 *   offset  88 — x30 (link register)
 *   offset  96 — sp
 *   offset 104 — d8
 *   offset 112 — d9
 *   offset 120 — d10
 *   offset 128 — d11
 *   offset 136 — d12
 *   offset 144 — d13
 *   offset 152 — d14
 *   offset 160 — d15
 *
 * CFI: registers are saved directly to struct memory, not onto the current
 * stack; standard .cfi_offset (CFA-relative) does not apply. The required
 * .cfi_startproc/.cfi_endproc pair ensures a valid FDE is emitted.
 * The unwinder cannot follow execution through a context switch — this is
 * an inherent limitation of all fiber context switch implementations.
 * See ARCHITECTURE.md §3.6.
 */

#include "../../strand_asm.h"

	.text
	.globl	strand_context_swap
	.type	strand_context_swap, @function
strand_context_swap:
	.cfi_startproc

	/*
	 * Save callee-saved GPRs to *old (x0).
	 * Offsets match strand_context_t definition in strand_context.h.
	 * STRAND_CONTEXT_SIZE = 168; _Static_assert in strand_internal.h
	 * verifies the struct layout has not drifted from these offsets.
	 * Use scalar str/ldr forms to avoid potential strict-alignment traps
	 * on platforms that enforce stronger alignment for paired accesses.
	 */
	str	x19, [x0, #0]
	str	x20, [x0, #8]
	str	x21, [x0, #16]
	str	x22, [x0, #24]
	str	x23, [x0, #32]
	str	x24, [x0, #40]
	str	x25, [x0, #48]
	str	x26, [x0, #56]
	str	x27, [x0, #64]
	str	x28, [x0, #72]
	str	x29, [x0, #80]	/* fp */
	str	x30, [x0, #88]	/* lr */

	/*
	 * Save sp — cannot be used directly as stp/str source in all
	 * encodings; move to a scratch register (x2, caller-saved) first.
	 */
	mov	x2, sp
	str	x2, [x0, #96]

	/*
	 * Save callee-saved FP/SIMD lower 64-bit halves (d8-d15) to *old.
	 * Only the lower 64 bits of v8-v15 are callee-saved under AAPCS64.
	 * Use scalar str/ldr forms here to avoid potential strict-alignment
	 * traps on platforms that treat 16-byte paired FP accesses more
	 * strictly than 8-byte scalar accesses.
	 */
	str	d8,  [x0, #104]
	str	d9,  [x0, #112]
	str	d10, [x0, #120]
	str	d11, [x0, #128]
	str	d12, [x0, #136]
	str	d13, [x0, #144]
	str	d14, [x0, #152]
	str	d15, [x0, #160]

	/*
	 * Switch stacks: load sp from *new (x1).
	 * After this instruction the processor executes on the new stack.
	 */
	ldr	x2, [x1, #96]
	mov	sp, x2

	/*
	 * Restore callee-saved FP/SIMD halves from *new.
	 */
	ldr	d8,  [x1, #104]
	ldr	d9,  [x1, #112]
	ldr	d10, [x1, #120]
	ldr	d11, [x1, #128]
	ldr	d12, [x1, #136]
	ldr	d13, [x1, #144]
	ldr	d14, [x1, #152]
	ldr	d15, [x1, #160]

	/*
	 * Restore callee-saved GPRs from *new.
	 * x29 (fp) and x30 (lr) are restored last so that x1 remains
	 * valid as the base pointer throughout all the loads above.
	 */
	ldr	x19, [x1, #0]
	ldr	x20, [x1, #8]
	ldr	x21, [x1, #16]
	ldr	x22, [x1, #24]
	ldr	x23, [x1, #32]
	ldr	x24, [x1, #40]
	ldr	x25, [x1, #48]
	ldr	x26, [x1, #56]
	ldr	x27, [x1, #64]
	ldr	x28, [x1, #72]
	ldr	x29, [x1, #80]	/* fp */
	ldr	x30, [x1, #88]	/* lr */

	/*
	 * Return — branches to the restored link register (x30).
	 * On first entry to a fabricated context this jumps to the fiber
	 * entry function set up by strand_context_init; on a resume it
	 * returns to the strand_context_swap call site that suspended
	 * this context.
	 */
	ret

	.cfi_endproc
	.size	strand_context_swap, .-strand_context_swap

/*
 * strand_fiber_trampoline — first landing point for a newly fabricated
 * fiber context.  Branched to (via ret in strand_context_swap) with:
 *   x19 = fiber entry function pointer (callee-saved, restored by swap)
 *   x20 = arg pointer              (callee-saved, restored by swap)
 *   sp  = stack_top aligned to 16 bytes
 *
 * Moves arg into x0 (the AAPCS64 first-argument register) from x20, then
 * calls entry(arg) via blr so that entry sees a standard function-call
 * frame.  sp is already 16-byte aligned; blr does not modify sp, so the
 * AAPCS64 stack-alignment requirement is satisfied on entry to the fiber
 * function.
 *
 * The `brk #0` after blr is a guard: the fiber entry function must not
 * return.  If it does, brk #0 raises SIGTRAP immediately rather than
 * continuing with a stale link register.
 */
	.text
	.globl	strand_fiber_trampoline
	.type	strand_fiber_trampoline, %function
strand_fiber_trampoline:
	.cfi_startproc
	mov	x0, x20			/* arg -> AAPCS64 first argument */
	blr	x19			/* call entry(arg) */
	brk	#0			/* fiber entry must not return */
	.cfi_endproc
	.size	strand_fiber_trampoline, .-strand_fiber_trampoline
