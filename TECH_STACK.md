# Tech Stack

## 1. Language

### C (C11)

libstrand is written in C11. No C++, no scripting languages, no code generation.

**Standard**: C11 (`-std=c11`)

**Why C11 over C99**:
- `_Atomic` and `<stdatomic.h>` for lock-free operations throughout the
  scheduler, inject queues, fiber handle generation counters, offload work
  item state, and scope lifecycle state - C11 atomics are the correct tool
  for this and avoid dependence on GCC/Clang intrinsics
- `_Static_assert` for compile-time checks on struct sizes, register save
  frame layout, and platform assumptions - for example, verifying that the
  context save region is the expected size before assembly stubs use it
- `_Alignas` / `_Alignof` for aligned stack allocations and cache-line
  alignment of hot scheduler structures
- Designated initialisers for static tables and configuration defaults
- Anonymous structs and unions in internal data structures

**Feature test macros** (defined in Makefile, not in source files):
```c
_POSIX_C_SOURCE=200809L   /* POSIX.1-2008 */
_XOPEN_SOURCE=700         /* XSI extensions */
```

Do not define `_GNU_SOURCE` - it pulls in non-portable extensions and
breaks OpenBSD builds.

---

## 2. Assembly

libstrand requires hand-written assembly for context switching. There is no
portable pure-C alternative - saving and restoring the stack pointer and
callee-saved registers requires direct register access.

### 2.1 Target Files

```
src/arch/x86_64/strand_context.S   - x86_64 context save and restore
src/arch/arm64/strand_context.S    - AArch64 context save and restore
```

Platform selection is handled in the Makefile via `$(ARCH)` detection.
Only the file for the current target architecture is compiled.

### 2.2 x86_64 (SysV AMD64 ABI)

Targets: Linux x86_64 and OpenBSD amd64.

**Callee-saved general-purpose registers saved and restored on every
context switch**:
```
rbx, rbp, r12, r13, r14, r15, rsp
```

**XMM registers**: Under the System V AMD64 ABI (used on both Linux and
OpenBSD), all XMM registers (XMM0–XMM15) are caller-saved. No XMM registers
are saved by the context switch. This is correct and intentional. Note: the
Windows x64 ABI differs (XMM6–XMM15 are callee-saved there), but libstrand
does not target Windows.

**Floating-point control register (MXCSR)**: Saved and restored on every
context switch via `stmxcsr` / `ldmxcsr`. Each fiber has its own MXCSR
state.

**x87 FP environment**: Not saved. Code that modifies x87 state (including
`long double` computations) across yield points is unsupported. This is
documented explicitly - programs using `long double` may see corrupted
floating-point behaviour after a yield.

**errno**: Saved and restored on every context switch by reading and writing
`errno` before and after the assembly stub. This is handled in the C wrapper
around the assembly, not in the assembly itself.

**Stack alignment**: 16 bytes, maintained on every switch.

### 2.3 AArch64 (AAPCS64)

Targets: Linux arm64 and OpenBSD arm64.

**Callee-saved general-purpose registers saved and restored**:
```
x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29 (frame pointer),
x30 (link register), sp
```

**Callee-saved FP/SIMD registers saved and restored**:
```
v8, v9, v10, v11, v12, v13, v14, v15
```
Only the lower 64 bits (d8–d15) need to be saved per AAPCS64 - the upper
64 bits of v8–v15 are not callee-saved.

**Floating-point control registers (FPCR and FPSR)**: Saved and restored on
every context switch via `mrs` / `msr`. Each fiber has its own FPCR and FPSR
state.

**errno**: Saved and restored in the C wrapper, same as x86_64.

**Stack alignment**: 16 bytes, maintained on every switch.

### 2.4 CFI Annotations

All assembly stubs carry correct DWARF CFI annotations:

```asm
strand_context_swap:
  .cfi_startproc
  /* ... context switch body ... */
  .cfi_endproc
```

For functions that save registers to the current stack frame, include matching
`.cfi_offset` and `.cfi_def_cfa_offset` directives. For context-switch stubs
that save state to explicit context structs, those stack-relative directives
may not apply, but the emitted FDE/unwind metadata must still be valid.

This ensures that stack unwinding, backtraces, gdb, perf, and crash
reporters all produce correct output on fiber stacks. Incorrect or missing
CFI annotations produce misleading stack traces and incorrect unwind during
exception handling. Every assembly stub is annotated correctly - this is
not optional.

---

## 3. External Dependencies

libstrand has **zero external library dependencies**. This is a hard
requirement, not a preference.

The only dependencies are the C standard library and POSIX interfaces
available on every supported platform without installation. There are no
vendored libraries, no submodules, and no package manager files.

**Runtime dependencies for embedders**:
- Linux: none beyond libc
- OpenBSD: none beyond libc

---

## 4. System Libraries

These are part of the C standard library or POSIX and require no
installation.

### 4.1 Standard C Library

```c
#include <stdint.h>     /* uint8_t, uint32_t, uint64_t, size_t */
#include <stddef.h>     /* offsetof, NULL, size_t */
#include <stdatomic.h>  /* _Atomic, atomic_load, atomic_store, atomic_compare_exchange */
#include <string.h>     /* memcpy, memset */
#include <stdlib.h>     /* malloc, free - fiber stack and descriptor allocation */
#include <errno.h>      /* errno - saved/restored on every context switch */
#include <assert.h>     /* assert - debug builds only */
```

### 4.2 Threads

```c
#include <pthread.h>    /* pthread_t, pthread_create - worker threads (Layer 4) */
                        /* pthread_mutex_t - inject queue lock, worker list lock */
```

libstrand uses pthreads only for creating and managing worker OS threads and
for protecting the small number of structures that require cross-thread
synchronisation (the inject queue, the worker registry, the offload pool work
queue). All per-fiber scheduling state is owned by a single worker thread and
requires no locking. `-lpthread` is required when linking.

### 4.3 Memory Management

```c
#include <sys/mman.h>   /* mmap, mprotect, munmap - stack allocation and guard pages */
```

Fiber stacks are allocated with `mmap(MAP_ANONYMOUS | MAP_PRIVATE)`. Guard
pages are installed at the low end of each stack with
`mprotect(PROT_NONE)` - a stack overflow hits the guard page and generates
a SIGSEGV rather than silently corrupting adjacent memory.

Stack memory is managed by the scheduler's stack cache. The stack allocator
never calls `malloc` - all stack memory goes through `mmap` and `munmap`.

### 4.4 Clocks and Timers

```c
#include <time.h>       /* clock_gettime(CLOCK_MONOTONIC) - timer heap deadlines */
```

`clock_gettime(CLOCK_MONOTONIC, ...)` is used exclusively for timer heap
deadlines (`strand_fiber_sleep_until`, scope timeouts). `CLOCK_MONOTONIC`
is correct because deadlines are relative to process uptime, not wall-clock
time. All internal timestamps are `uint64_t` nanoseconds since an arbitrary
epoch (first clock read at scheduler initialisation).

### 4.5 I/O Multiplexing - Linux

```c
#include <sys/epoll.h>  /* epoll_create1, epoll_ctl, epoll_wait - Layer 3 */
#include <sys/eventfd.h>/* eventfd - cross-worker wakeup */
#include <unistd.h>     /* read, write, close */
#include <fcntl.h>      /* fcntl, O_NONBLOCK, O_CLOEXEC, FD_CLOEXEC */
```

**Trigger mode**: `EPOLLET | EPOLLONESHOT` on all registered file descriptors.
Edge-triggered so a single edge is consumed per registration. One-shot so the
registration is automatically disabled after firing, preventing spurious
re-delivery before the fiber has re-armed.

**Cross-worker wakeup**: `eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)`. Writing
any value wakes a worker blocked in `epoll_wait`. The wakeup fd is drained
as a control event in Step 3 of `strand_scheduler_advance` - the bytes are
read and discarded, never interpreted as fiber waiter events.

All internal file descriptors are created with `O_CLOEXEC` /
`EFD_CLOEXEC`. They do not leak into child processes after `exec`.

### 4.6 I/O Multiplexing - OpenBSD

```c
#include <sys/event.h>  /* kqueue, kevent - Layer 3 */
#include <unistd.h>     /* pipe2, read, write, close */
#include <fcntl.h>      /* O_NONBLOCK, O_CLOEXEC */
```

**Trigger mode**: `EV_DISPATCH` without `EV_CLEAR`. EV_DISPATCH is
level-triggered with one-shot disable - the filter fires once and is
disabled until explicitly re-enabled with `EV_ENABLE`. On re-enable, if the
condition is still true, the filter fires again automatically. No
post-re-arm readiness check is needed on OpenBSD (unlike Linux with
EPOLLET).

**Cross-worker wakeup**: `pipe2(fds, O_CLOEXEC | O_NONBLOCK)`. OpenBSD does
not have `eventfd`. The write end is used to wake a blocked worker; the read
end is registered with kqueue. Bytes are drained as a control event in Step 3
of `strand_scheduler_advance`.

**CPU affinity**: `pthread_setaffinity_np` is available on Linux for pinning
workers to cores. OpenBSD does not provide CPU affinity APIs. This difference
is documented; the pinned model is advisory on OpenBSD.

---

## 5. Build System

### 5.1 Makefile Structure

One top-level Makefile. No per-directory Makefiles. Platform and architecture
detected at build time.

```
libstrand/
├── Makefile
├── src/
│   ├── arch/
│   │   ├── x86_64/   ← x86_64 assembly
│   │   └── arm64/    ← AArch64 assembly
│   └── *.c           ← all C source files
├── include/
│   └── strand.h      ← public header
├── tests/            ← unit and integration tests
├── bench/            ← performance benchmarks
└── tools/            ← optional developer tools
```

The output of `make` is a static library (`libstrand.a`). Embedders link
against `libstrand.a` and include `include/strand.h`.

### 5.2 Compiler

**Primary**: Clang (preferred on both platforms).

OpenBSD ships Clang as the system compiler. On Linux:
```sh
# Debian/Ubuntu
apt install clang

# RHEL/Fedora
dnf install clang

# Arch
pacman -S clang
```

Minimum supported version: Clang 11.0. Required for full C11 `_Atomic`
support, `-fsanitize=address,undefined`, and the sanitizer fiber hooks
(`__sanitizer_start_switch_fiber`, `__sanitizer_finish_switch_fiber`).

GCC is a supported secondary compiler for the C portions of the library.
The assembly stubs use `.S` syntax compatible with both the GNU assembler
(Linux) and LLVM's integrated assembler (OpenBSD). GCC does not support
the TSan fiber APIs - the `make test-tsan` target requires Clang.

### 5.3 Compiler Flags

```makefile
# Common flags - all builds
CFLAGS_BASE = -std=c11 -Wall -Wextra -Werror -fno-omit-frame-pointer

# Feature test macros - defined here, not in source
CFLAGS_FT   = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700

# Platform detection
UNAME := $(shell uname)
ifeq ($(UNAME), Linux)
    CFLAGS_OS = -DSTRAND_LINUX
    LDFLAGS   = -lpthread
endif
ifeq ($(UNAME), OpenBSD)
    CFLAGS_OS = -DSTRAND_OPENBSD
    LDFLAGS   = -lpthread
endif

# Architecture detection
ARCH := $(shell uname -m)
ifeq ($(ARCH), x86_64)
    ASM_SRC = src/arch/x86_64/strand_context.S
endif
ifeq ($(ARCH), aarch64)
    ASM_SRC = src/arch/arm64/strand_context.S
endif

# Development build - ASan + UBSan + debug assertions
CFLAGS_DEV  = $(CFLAGS_BASE) $(CFLAGS_FT) $(CFLAGS_OS) \
              -O1 -g -DSTRAND_DEBUG \
              -fsanitize=address,undefined

# Release build
CFLAGS_REL  = $(CFLAGS_BASE) $(CFLAGS_FT) $(CFLAGS_OS) \
              -O2 -DNDEBUG

# TSan build - mutually exclusive with ASan
CFLAGS_TSAN = $(CFLAGS_BASE) $(CFLAGS_FT) $(CFLAGS_OS) \
              -O1 -g -DSTRAND_DEBUG \
              -fsanitize=thread
```

`-fno-omit-frame-pointer` is required in all builds. Frame pointers are
essential for correct stack unwinding on fiber stacks, for perf profiling,
and for backtraces in crash reports. This flag is never relaxed.

`-DSTRAND_DEBUG` activates:
- O_NONBLOCK assertion at fd registration (checks the caller has set it)
- Generation counter validation logging
- Scheduler watchdog warnings for long-running fibers
- ASan fiber stack hooks (see §7.2)
- `scope_abandon` stack-range check

### 5.4 Build Targets

```makefile
make              # Debug build - same as make dev
make dev          # Development build - libstrand.a with ASan/UBSan
make release      # Explicit release build
make test-release # Build and run the suite with release flags
make test         # Build and run unit and integration tests (dev flags)
make test-real-clock # Exercise real-clock timer waits (dev flags)
make test-tsan    # Build and run tests under ThreadSanitizer (Clang only)
make bench        # Build and run performance benchmarks (release flags)
make valgrind     # Run tests under Valgrind (Linux only)
make lint         # Run clang-tidy and cppcheck
make format       # Run clang-format over C sources and headers
make clean        # Remove all build artifacts
make install      # Install libstrand.a and strand.h to $(PREFIX)
```

---

## 6. Test Harness

### 6.1 Structure

Tests are written in C using a minimal test harness defined in
`tests/test_harness.h`. No third-party test framework is used.

```c
/* tests/test_harness.h */
#define RUN(name, fn) do {                                    \
    int _r = (fn)();                                          \
    tests_run++;                                              \
    if (_r == 0) { tests_passed++; }                         \
    else { fprintf(stderr, "FAIL: %s\n", (name)); }          \
} while (0)
```

Each test function returns 0 on success and non-zero on failure. The test
binary exits with code 0 if all tests pass and 1 if any test fails.

```sh
make test
# output:
# 47/47 tests passed
```

### 6.2 Per-Layer Test Files

Tests are organised by layer in `tests/`:

```
tests/
├── test_harness.h
├── run_tests.c          ← test binary entry point, runs all suites
├── test_layer1.c        ← Layer 1: context switch correctness
├── test_layer2.c        ← Layer 2: scheduler modes, FIFO, budget
├── test_layer3.c        ← Layer 3: I/O parking, cancellation, re-arm
├── test_layer4.c        ← Layer 4: inject queue, cross-worker, worker selection
├── test_layer5.c        ← Layer 5: scopes, fiber handles, offload, fiber-local storage
└── test_integration.c  ← integration tests across multiple layers
```

See TESTING.md for the full test catalogue.

### 6.3 Benchmark Files

Performance benchmarks are in `bench/` and built separately under release
flags. They do not run as part of `make test`.

```
bench/
├── bench_context_switch.c   ← context switch round-trip latency
├── bench_fiber_spawn.c      ← fiber creation and teardown throughput
├── bench_io_roundtrip.c     ← I/O park and wake latency
├── bench_scheduler.c        ← scheduler throughput (fibers per second)
├── bench_multiworker.c      ← multi-worker scaling
├── bench_offload.c          ← offload pool throughput
└── bench_cross_worker.c     ← cross-worker wakeup latency
```

Benchmark output includes hardware context (CPU model, core count, clock
speed). Numbers are not comparable across machines without this context.

---

## 7. Development Tools

### 7.1 Valgrind (Linux only)

**Purpose**: Memory error detection - leaks, use-after-free, uninitialised
reads.

**Installation**: `apt install valgrind`

**Usage**:
```sh
make valgrind
# equivalent to:
valgrind --leak-check=full          \
         --show-leak-kinds=all      \
         --track-origins=yes        \
         --error-exitcode=1         \
         ./tests/run_tests
```

Valgrind requires fiber stack registration so that it does not report
accesses to fiber stacks as errors. libstrand registers and deregisters
every fiber stack using the Valgrind client request macros:

```c
#include <valgrind/valgrind.h>

/* At stack allocation: */
VALGRIND_STACK_REGISTER(stack_base, stack_base + stack_size);

/* At stack deallocation: */
VALGRIND_STACK_DEREGISTER(valgrind_stack_id);
```

These macros are no-ops in non-Valgrind builds. They are compiled in
unconditionally when the header is present - detected at build time via
`$(shell pkg-config --exists valgrind && echo 1)` or equivalent header
check.

All tests must pass Valgrind clean. Run on Linux before every commit.
Valgrind is not available on OpenBSD.  Use its functional, release-profile,
real-clock, and static-analysis gates with `MALLOC_OPTIONS=CFGJ`; this is not
a replacement for a sanitizer.

### 7.2 AddressSanitizer + UndefinedBehaviorSanitizer

**Purpose**: Runtime memory and undefined behaviour detection.

**Compiler flags**: `-fsanitize=address,undefined` (included in `make dev`)

Available in the supported Linux build.  OpenBSD's Clang toolchain does not
support the required AddressSanitizer flags, so `make dev` intentionally omits
ASan/UBSan there.

libstrand integrates the ASan fiber stack switching hooks so that ASan
correctly tracks the active stack across context switches:

```c
#if defined(__SANITIZE_ADDRESS__)
#  include <sanitizer/asan_interface.h>
#  define STRAND_ASAN_SWITCH_START(new_sp, new_size, old_sp, old_size) \
       __sanitizer_start_switch_fiber(NULL, (new_sp), (new_size))
#  define STRAND_ASAN_SWITCH_FINISH()                                   \
       __sanitizer_finish_switch_fiber(NULL, NULL, NULL)
#else
#  define STRAND_ASAN_SWITCH_START(new_sp, new_size, old_sp, old_size) ((void)0)
#  define STRAND_ASAN_SWITCH_FINISH()                                   ((void)0)
#endif
```

`__sanitizer_start_switch_fiber` is called immediately before every context
switch and `__sanitizer_finish_switch_fiber` is called immediately after.
This is required - without it, ASan incorrectly reports valid accesses to
the new stack as stack-buffer-overflows, producing false positives that
obscure real bugs.

Additional `STRAND_DEBUG` ASan behaviour:
- O_NONBLOCK check at fd registration
- Scope control block stack-range check in `strand_scope_abandon`

### 7.3 ThreadSanitizer

**Purpose**: Data race detection.

TSan and ASan are mutually exclusive - TSan runs as a separate target.

```sh
make test-tsan
```

libstrand integrates the TSan fiber switching APIs so that TSan correctly
tracks happens-before relationships across context switches:

```c
#if defined(__SANITIZE_THREAD__)
#  include <sanitizer/tsan_interface.h>
#  define STRAND_TSAN_SWITCH_START()  __tsan_switch_to_fiber(fiber->tsan_fiber, 0)
#  define STRAND_TSAN_CREATE(fiber)   (fiber)->tsan_fiber = __tsan_create_fiber(0)
#  define STRAND_TSAN_DESTROY(fiber)  __tsan_destroy_fiber((fiber)->tsan_fiber)
#else
#  define STRAND_TSAN_SWITCH_START()       ((void)0)
#  define STRAND_TSAN_CREATE(fiber)        ((void)0)
#  define STRAND_TSAN_DESTROY(fiber)       ((void)0)
#endif
```

Without TSan fiber integration, TSan cannot reason about the ordering of
operations across context switches and produces both false positives and
false negatives. The integration is required for TSan results to be
meaningful.

TSan is not a per-commit gate. Run at layer boundaries and before release.

**TSan availability**: Clang only. GCC TSan does not implement the fiber
APIs. The `make test-tsan` target fails gracefully when built with GCC.

### 7.4 clang-tidy

**Purpose**: Static analysis.

**Installation**: `apt install clang-tidy` (Linux) /
`pkg_add clang-tools-extra` (OpenBSD 7.9).

```sh
make lint
# or directly:
clang-tidy src/*.c -- $(CFLAGS_DEV) -I include/
```

`make lint` requires both clang-tidy and cppcheck and treats every
clang-tidy warning as an error.

### 7.5 cppcheck

**Purpose**: Additional static analysis, complementary to clang-tidy.

**Installation**: `apt install cppcheck` / `pkg_add cppcheck`

```sh
cppcheck --enable=all --error-exitcode=1 \
         --suppress=missingIncludeSystem \
         src/
```

### 7.6 clang-format

**Purpose**: Consistent code formatting.

**Configuration**: `.clang-format` in repository root. KNF-based style,
consistent with the author's other C libraries.

```sh
make format
# equivalent to:
clang-format -i src/*.c src/*.h include/strand.h
```

Assembly files (`.S`) are not processed by clang-format. Assembly
formatting conventions are described in CODING_STANDARDS.md §2.

CI builds and tests debug, real-clock, and release profiles on every target.
Formatting is checked before hand-off with `clang-format --dry-run --Werror`
on changed C sources and headers.

---

## 8. Dependency Summary

| Dependency | Version | Type | Purpose | Platforms |
|---|---|---|---|---|
| Clang | ≥ 11.0 | Build tool | Compilation, sanitizers | Linux, OpenBSD |
| GCC | ≥ 10.0 | Build tool | Secondary compiler (C only, no TSan fiber APIs) | Linux |
| libc | system | System lib | Standard C | Linux, OpenBSD |
| libpthread | system | System lib | Worker threads, mutex | Linux, OpenBSD |
| Valgrind | latest | Dev tool | Memory checking, stack registration | Linux only |
| clang-tidy | ≥ 11.0 | Dev tool | Static analysis | Linux, OpenBSD |
| cppcheck | latest | Dev tool | Static analysis | Linux, OpenBSD |

**Runtime dependencies for embedders**:
- Linux: libc, libpthread
- OpenBSD: libc, libpthread

**Development-only dependencies** (not required to embed the library):
- Valgrind (Linux only)
- clang-tidy, cppcheck

---

**See Also**: PROJECT.md, ARCHITECTURE.md, CODING_STANDARDS.md, DEVELOPMENT.md
