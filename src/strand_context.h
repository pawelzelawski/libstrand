#ifndef STRAND_CONTEXT_H
#define STRAND_CONTEXT_H

/*
 * strand_context.h - Layer 1 internal interface.
 * strand_context_t definition, strand_context_swap(), strand_context_init().
 * See ARCHITECTURE.md §3.
 */

#include "../include/strand.h"

/*
 * strand_context_t - register save area for one execution context.
 *
 * Contains all callee-saved registers for the current architecture plus
 * the stack pointer. Sized to hold exactly one save-and-restore sequence.
 *
 * errno and floating-point control registers (MXCSR on x86_64,
 * FPCR/FPSR on AArch64) are saved by the C wrapper, not here.
 * See ARCHITECTURE.md §3.
 *
 * STRAND_CONTEXT_SIZE must equal sizeof(strand_context_t). The assembly
 * stubs in src/arch/ use this constant directly. The _Static_assert in
 * strand_internal.h verifies the match at compile time.
 */
#if defined(__x86_64__) || defined(__amd64__)

/*
 * System V AMD64 ABI callee-saved GPRs: rbx, rbp, r12-r15, rsp.
 * XMM registers are caller-saved under SysV AMD64 - not saved here.
 * MXCSR is saved by strand_context_switch. See ARCHITECTURE.md §3.2.
 */
typedef struct strand_context {
	uint64_t rbx;
	uint64_t rbp;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t rsp;
} strand_context_t;

#define STRAND_CONTEXT_SIZE 56 /* 7 * 8 bytes */

#elif defined(__aarch64__) || defined(__arm64__)

/*
 * AAPCS64 callee-saved GPRs: x19-x28, x29 (fp), x30 (lr), sp.
 * Callee-saved FP/SIMD lower halves: d8-d15.
 * FPCR/FPSR are saved by strand_context_switch. See ARCHITECTURE.md §3.3.
 */
typedef struct strand_context {
	uint64_t x19;
	uint64_t x20;
	uint64_t x21;
	uint64_t x22;
	uint64_t x23;
	uint64_t x24;
	uint64_t x25;
	uint64_t x26;
	uint64_t x27;
	uint64_t x28;
	uint64_t x29; /* frame pointer */
	uint64_t x30; /* link register */
	uint64_t sp;
	uint64_t d8;
	uint64_t d9;
	uint64_t d10;
	uint64_t d11;
	uint64_t d12;
	uint64_t d13;
	uint64_t d14;
	uint64_t d15;
} strand_context_t;

#define STRAND_CONTEXT_SIZE 168 /* 21 * 8 bytes */

#else
#error "Unsupported architecture: strand_context_t not defined for this target"
#endif

/*
 * strand_context_swap(old, new) - the core context switch primitive.
 *
 * Saves callee-saved registers and rsp into *old, then restores them
 * from *new and returns - resuming execution on the new stack.
 *
 * Must only be called from the C wrapper strand_context_switch, which
 * handles errno, MXCSR/FPCR, and sanitizer hooks.  Do not call directly.
 *
 * WARNING: x87 FP state (including long double) is NOT saved across a
 * context switch.  Programs that modify x87 state across a fiber yield
 * point may observe corrupted floating-point behaviour.  See
 * ARCHITECTURE.md §3.4.
 *
 * Implemented in assembly: src/arch/x86_64/strand_context.S (x86_64)
 *                           src/arch/arm64/strand_context.S  (AArch64)
 */
void strand_context_swap(strand_context_t *old, strand_context_t *new);
/*
 * strand_context_init(ctx, stack_top, entry, arg) - fabricate an initial
 * saved-register state so that the first strand_context_swap to *ctx
 * begins execution at entry(arg).
 *
 * stack_top must be the address one byte past the top of the usable stack
 * (i.e. the highest valid address + 1) and must be 16-byte aligned.
 *
 * On x86_64: writes the address of an internal trampoline as the fabricated
 * return address at stack_top - 8; stores entry in rbx and arg in r12
 * (both callee-saved, so strand_context_swap restores them); sets rsp to
 * stack_top - 8; clears rbp, r13, r14, r15 to zero.  The trampoline moves
 * arg into rdi (SysV AMD64 first-argument register) and calls entry.
 *
 * On AArch64: aligns sp to 16 bytes; stores the trampoline in x30 (lr),
 * entry in x19, and arg in x20 (all callee-saved); clears remaining
 * callee-saved registers to zero.  The trampoline moves arg into x0
 * (AAPCS64 first-argument register) and calls entry.
 *
 * The fiber entry function must not return.  It must transfer control back
 * to the scheduler (or another context) via strand_context_switch before
 * returning, otherwise behaviour is undefined.
 */
void strand_context_init(strand_context_t *ctx, void *stack_top,
                         strand_fiber_fn_t entry, void *arg);

/*
 * strand_context_switch(from, to) - the public Layer 1 interface.
 *
 * Wraps strand_context_swap with:
 *   - errno save and restore
 *   - MXCSR (x86_64) or FPCR/FPSR (AArch64) save and restore
 *   - ASan fiber stack switching hooks
 *   - TSan fiber switching hook
 *
 * All sanitizer hooks are no-ops in non-sanitizer builds.
 * See ARCHITECTURE.md §3.4, ARCHITECTURE.md §3.7.
 */
void strand_context_switch(strand_fiber_t *from, strand_fiber_t *to);
#endif /* STRAND_CONTEXT_H */
