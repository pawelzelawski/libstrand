#ifndef STRAND_CONTEXT_H
#define STRAND_CONTEXT_H

/*
 * strand_context.h — Layer 1 internal interface.
 * strand_context_t definition, strand_context_swap(), strand_context_init().
 * See ARCHITECTURE.md §3.
 */

#include "../include/strand.h"

/*
 * strand_context_t — register save area for one execution context.
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
 * XMM registers are caller-saved under SysV AMD64 — not saved here.
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

#elif defined(__aarch64__)

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

#endif /* STRAND_CONTEXT_H */
