/*
 * strand_context.S — fiber context save and restore (x86_64 SysV AMD64 ABI)
 *
 * void strand_context_swap(strand_context_t *old, strand_context_t *new);
 *   rdi = old   (first argument under SysV AMD64)
 *   rsi = new   (second argument under SysV AMD64)
 *
 * Saves callee-saved GPRs and rsp to *old, loads them from *new, and
 * returns — resuming execution at the address stored at the top of the
 * new stack (the return address from the call that suspended that context,
 * or the fabricated entry point from strand_context_init).
 *
 * Calling convention: SysV AMD64 (Linux x86_64, OpenBSD amd64).
 * Callee-saved GPRs: rbx, rbp, r12-r15.
 * XMM registers: all caller-saved under SysV AMD64 — none saved here.
 * This is correct. The Windows x64 ABI differs (XMM6-XMM15 callee-saved)
 * but libstrand does not target Windows. See ARCHITECTURE.md §3.2.
 * MXCSR: saved and restored by the C wrapper (strand_context_switch).
 *
 * strand_context_t layout (x86_64), STRAND_CONTEXT_SIZE = 56:
 *   offset  0 — rbx
 *   offset  8 — rbp
 *   offset 16 — r12
 *   offset 24 — r13
 *   offset 32 — r14
 *   offset 40 — r15
 *   offset 48 — rsp
 *
 * CFI: registers are saved directly to struct memory, not onto the current
 * stack; standard .cfi_offset (CFA-relative) does not apply. The required
 * .cfi_startproc/.cfi_endproc pair ensures a valid FDE is emitted so that
 * readelf --debug-dump=frames shows correct unwind info for this function.
 * The unwinder cannot follow execution through a context switch — this is
 * an inherent limitation of all fiber context switch implementations and is
 * expected. See ARCHITECTURE.md §3.6.
 */

#include "../../strand_asm.h"

	.text
	.globl	strand_context_swap
	.type	strand_context_swap, @function
strand_context_swap:
	.cfi_startproc

	/*
	 * Save callee-saved GPRs to *old (rdi).
	 * Offsets match strand_context_t definition in strand_context.h.
	 * STRAND_CONTEXT_SIZE = 56 bytes; _Static_assert in strand_internal.h
	 * verifies the struct layout has not drifted from these offsets.
	 */
	movq	%rbx, 0(%rdi)
	movq	%rbp, 8(%rdi)
	movq	%r12, 16(%rdi)
	movq	%r13, 24(%rdi)
	movq	%r14, 32(%rdi)
	movq	%r15, 40(%rdi)

	/*
	 * Save rsp — on entry rsp points at the return address pushed by
	 * the 'call' instruction.  Saving it here captures the full call
	 * frame so that a future swap back will resume at the call site.
	 */
	movq	%rsp, 48(%rdi)

	/*
	 * Switch stacks: load rsp from *new (rsi).
	 * After this instruction the processor executes on the new stack.
	 */
	movq	48(%rsi), %rsp

	/*
	 * Restore callee-saved GPRs from *new.
	 */
	movq	0(%rsi), %rbx
	movq	8(%rsi), %rbp
	movq	16(%rsi), %r12
	movq	24(%rsi), %r13
	movq	32(%rsi), %r14
	movq	40(%rsi), %r15

	/*
	 * Return — pops the return address from the restored stack and
	 * transfers control there.  On first entry to a fabricated context
	 * this jumps to the fiber entry function set up by strand_context_init;
	 * on a resume it returns to the strand_context_swap call site that
	 * suspended this context.
	 */
	retq

	.cfi_endproc
	.size	strand_context_swap, .-strand_context_swap

/*
 * strand_fiber_trampoline — first landing point for a newly fabricated
 * fiber context.  Entered via the ret in strand_context_swap with:
 *   rbx = fiber entry function pointer (callee-saved, restored by swap)
 *   r12 = arg pointer              (callee-saved, restored by swap)
 *   rsp = stack_top  (16-byte aligned — ret popped the fabricated address)
 *
 * Moves arg into rdi (the SysV AMD64 first-argument register), then calls
 * entry(arg) via `call` so that entry observes a correctly aligned stack:
 * rsp is 16-byte aligned here; `call *%rbx` pushes an 8-byte return
 * address making rsp%16 == 8 on entry to the fiber function, satisfying
 * the SysV AMD64 ABI requirement.
 *
 * The `ud2` after the call is a guard: the fiber entry function must not
 * return.  If it does, ud2 raises SIGILL immediately rather than
 * continuing with a corrupted or empty stack.
 */
	.text
	.globl	strand_fiber_trampoline
	.type	strand_fiber_trampoline, @function
strand_fiber_trampoline:
	.cfi_startproc
	movq	%r12, %rdi		/* arg -> SysV AMD64 first argument */
	call	*%rbx			/* call entry(arg) */
	ud2				/* fiber entry must not return */
	.cfi_endproc
	.size	strand_fiber_trampoline, .-strand_fiber_trampoline
