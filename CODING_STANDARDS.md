# Coding Standards

## 1. Code Style

### 1.1 OpenBSD KNF (Kernel Normal Form)

libstrand follows OpenBSD's Kernel Normal Form style. This is the style used
throughout the OpenBSD base system and is required for all source files in
`src/` and `include/`.

**Indentation**:
- Tabs for indentation, 8-character display width
- Spaces only for alignment within a line, never for indentation
- Never mix tabs and spaces for indentation

```c
/* Correct */
static int
scheduler_run_one(strand_scheduler_t *sched)
{
	strand_fiber_t *f;

	if (sched->run_queue_len == 0)
		return STRAND_IDLE;
	f = run_queue_pop(sched);
	return fiber_resume(sched, f);
}

/* Wrong - spaces used for indentation */
static int
scheduler_run_one(strand_scheduler_t *sched)
{
    strand_fiber_t *f;   /* spaces, not tabs */
}
```

**Braces**:
```c
/* Functions: opening brace on its own line */
static void
fiber_park_io(strand_scheduler_t *sched, strand_fiber_t *f, int fd, int dir)
{
	/* body */
}

/* Control structures: opening brace on same line */
if (f->state == FIBER_PARKED_IO_READ) {
	if (poller_cancel_read(sched->poller, f) != STRAND_OK)
		return STRAND_ERR_INTERNAL;
}

/* Single-statement bodies: no braces, indented on next line */
if (handle.ptr == NULL)
	return STRAND_HANDLE_INVALID;

/* Loop with single statement */
for (i = 0; i < sched->inject_queue.len; i++)
	run_queue_push(sched, inject_queue_pop(&sched->inject_queue));
```

**Line length**: Maximum 80 characters. Break long lines at logical points,
aligning continuation with the opening parenthesis or using one extra tab:

```c
/* Break at logical point, align with opening paren */
rc = strand_fiber_spawn(runtime, fiber_fn,
    arg, stack_size, &handle);

/* Extra tab indent for continuation */
return strand_err(STRAND_ERR_CANCELLED,
	"fiber cancelled while parked on fd %d", fd);
```

**Naming conventions**:
```c
/* Variables and function parameters: lowercase with underscores */
size_t          stack_size;
uint64_t        generation;
int             fd;
strand_fiber_t *fiber;

/* Public API functions: strand_ prefix, lowercase with underscores */
int  strand_fiber_spawn(strand_runtime_t *, strand_fiber_fn_t, void *,
         size_t, strand_fiber_handle_t *);
int  strand_scheduler_advance(strand_scheduler_t *);
void strand_fiber_cancel(strand_fiber_handle_t);

/* Internal functions: module prefix, no strand_ prefix */
static int  fiber_resume(strand_scheduler_t *, strand_fiber_t *);
static void run_queue_push(strand_scheduler_t *, strand_fiber_t *);
static int  poller_arm(strand_poller_t *, int fd, int mask);

/* Constants and macros: uppercase with underscores, STRAND_ prefix */
#define STRAND_DEFAULT_STACK_SIZE   (64 * 1024)
#define STRAND_DEFAULT_CACHE_CAP    64
#define STRAND_SCHED_BUDGET_DEFAULT 64
#define STRAND_HANDLE_INVALID       (-1)
#define STRAND_HANDLE_STALE         (-2)

/* Structs and typedefs: lowercase, _t suffix */
typedef struct strand_fiber        strand_fiber_t;
typedef struct strand_scheduler    strand_scheduler_t;
typedef struct strand_scope        strand_scope_t;
typedef struct strand_fiber_handle strand_fiber_handle_t;

/* Enums: uppercase values with STRAND_ or module-specific prefix */
typedef enum {
	FIBER_NEW              = 0,
	FIBER_RUNNABLE         = 1,
	FIBER_RUNNING          = 2,
	FIBER_PARKED_IO_READ   = 3,
	FIBER_PARKED_IO_WRITE  = 4,
	FIBER_PARKED_TIMER     = 5,
	FIBER_PARKED_OFFLOAD   = 6,
	FIBER_PARKED_CHANNEL   = 7,
	FIBER_FINISHED         = 8,
} fiber_state_t;

typedef enum {
	SCOPE_ACTIVE      = 0,
	SCOPE_CANCELLING  = 1,
	SCOPE_DRAINING    = 2,
	SCOPE_COMPLETED   = 3,
} scope_lifecycle_t;
```

**Spacing**:
```c
/* Space after keywords, not after function names */
if (condition)                    /* correct */
if(condition)                     /* wrong */
fiber_resume(sched, f)            /* correct */
fiber_resume (sched, f)           /* wrong */

/* No space inside parentheses */
if (n > 0)                        /* correct */
if ( n > 0 )                      /* wrong */

/* Space around binary operators */
deadline = now + timeout_ns;
h = (fd * 2654435761u) >> (32 - table_bits);

/* No space for unary operators */
ptr  = &entry;
val  = *ptr;
mask = ~EPOLLET;
```

**Return type on its own line**:
```c
/* Correct */
static int
fiber_park_timer(strand_scheduler_t *sched, strand_fiber_t *f,
    uint64_t deadline_ns)
{
	/* ... */
}

/* Wrong */
static int fiber_park_timer(strand_scheduler_t *sched, strand_fiber_t *f,
    uint64_t deadline_ns)
{
	/* ... */
}
```

### 1.2 File Organisation

**Public header** (`include/strand.h`):

The public header exposes only what embedders need. Internal types, internal
function declarations, and implementation details are never declared here. The
header is self-contained - an embedder includes only `strand.h` and nothing
else.

```c
#ifndef STRAND_H
#define STRAND_H

/*
 * strand.h - libstrand public API
 *
 * Initialise a runtime with strand_runtime_init().
 * Spawn fibers with strand_fiber_spawn().
 * Wait for scopes with strand_scope_wait().
 * Drive a scheduler from a host loop with strand_scheduler_advance().
 *
 * See ARCHITECTURE.md for the full five-layer design.
 * See DEVELOPMENT.md for the phased implementation plan.
 */

#include <stddef.h>
#include <stdint.h>

/* ... public types, constants, and function declarations ... */

#endif /* STRAND_H */
```

**Internal headers** (e.g. `src/strand_fiber.h`):

```c
#ifndef STRAND_FIBER_H
#define STRAND_FIBER_H

/*
 * strand_fiber.h - fiber descriptor, state machine, and run queue (internal)
 * See ARCHITECTURE.md §4.5 for the fiber state machine.
 * See ARCHITECTURE.md §4.6 for fiber handle ABA protection.
 */

#include "../include/strand.h"
#include "strand_internal.h"

/* ... internal types and function declarations ... */

#endif /* STRAND_FIBER_H */
```

**Source files** (`.c`):
```c
/*
 * strand_sched.c - fiber scheduler: Layers 2 operating modes
 *
 * Implements strand_scheduler_advance (guest mode) and
 * strand_scheduler_run (worker mode). See ARCHITECTURE.md §4.2.
 */

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "../include/strand.h"
#include "strand_internal.h"
#include "strand_fiber.h"
#include "strand_sched.h"
```

**Include order** (within each group, alphabetical):
```c
/* 1. System headers */
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* 2. OS-specific headers */
#ifdef STRAND_LINUX
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif
#ifdef STRAND_OPENBSD
#include <sys/event.h>
#endif

/* 3. Threading */
#include <pthread.h>

/* 4. Public library header */
#include "../include/strand.h"

/* 5. Internal headers */
#include "strand_fiber.h"
#include "strand_internal.h"
#include "strand_poller.h"
#include "strand_sched.h"
```

### 1.3 Static Assertions

Use `_Static_assert` to catch layout changes at compile time. Place them
in the internal header where the type is defined.

```c
/* In strand_internal.h */
_Static_assert(sizeof(strand_fiber_handle_t) == 16,
    "strand_fiber_handle_t size changed - ptr (8) + generation (8)");
_Static_assert(sizeof(strand_context_t) == STRAND_CONTEXT_SIZE,
    "strand_context_t size changed - update assembly stubs");
_Static_assert(offsetof(strand_fiber_t, context) == 0,
    "context must be first field - assembly stubs assume offset 0");
```

The assertion on `strand_context_t` is especially critical - the assembly
stubs in `src/arch/` use hardcoded offsets into the context struct. Any
change to the struct layout that is not reflected in the assembly will
produce silent register corruption.

---

## 2. Assembly Conventions

### 2.1 File Naming and Structure

Assembly files use the `.S` extension (uppercase) to invoke the C
preprocessor, enabling `#include`, `#ifdef`, and macro use.

```
src/arch/x86_64/strand_context.S
src/arch/arm64/strand_context.S
```

Each file opens with a standard header:

```asm
/*
 * strand_context.S - fiber context save and restore (x86_64 SysV ABI)
 *
 * See ARCHITECTURE.md §3.2 for register set and ABI rationale.
 * See ARCHITECTURE.md §3.6 for CFI annotation requirements.
 *
 * Calling convention: SysV AMD64 (Linux and OpenBSD).
 * Callee-saved GPRs: rbx, rbp, r12-r15.
 * XMM registers: all caller-saved under SysV - none saved here.
 * MXCSR: saved and restored in the C wrapper, not here.
 */

#include "../strand_asm.h"   /* shared macro definitions */
```

### 2.2 CFI Annotations

Every assembly function must carry complete DWARF CFI annotations.
Missing or incorrect annotations produce misleading backtraces and broken
unwind through signal handlers and crash reporters. This is not optional.

When a function saves registers on the current stack frame, include matching
`.cfi_offset` / `.cfi_def_cfa_offset` directives. For context-switch stubs
that save registers to explicit context structs (not CFA-relative stack
slots), emit valid `.cfi_startproc` / `.cfi_endproc` unwind metadata and use
stack-relative directives only where they are semantically correct.

```asm
.globl strand_context_switch
.type  strand_context_switch, @function
strand_context_switch:
	.cfi_startproc
	/* save callee-saved registers */
	pushq   %rbp
	.cfi_def_cfa_offset 16
	.cfi_offset rbp, -16
	pushq   %rbx
	.cfi_def_cfa_offset 24
	.cfi_offset rbx, -24
	/* ... remaining registers ... */

	/* switch stack */
	movq    %rsp, (%rdi)    /* save current sp to *old_sp */
	movq    (%rsi), %rsp    /* load new sp from *new_sp */

	/* restore callee-saved registers */
	popq    %rbx
	.cfi_restore rbx
	popq    %rbp
	.cfi_restore rbp
	retq
	.cfi_endproc
.size strand_context_switch, .-strand_context_switch
```

### 2.3 Platform Preprocessor Guards

When a `.S` file contains platform-specific code, use the preprocessor
macros defined at build time (`STRAND_LINUX`, `STRAND_OPENBSD`). Never use
`__linux__` or `__OpenBSD__` directly in assembly files - use the Makefile-
defined macros for consistency with the C sources.

### 2.4 No Logic in Assembly

Assembly stubs perform pure register save/restore and stack pointer swap.
No conditional logic, no memory allocation, no function calls to C code,
no errno access. All logic lives in the C wrappers that call the assembly
stubs. This keeps the assembly auditable and the CFI annotations tractable.

---

## 3. Memory Management

### 3.1 Stack Memory

Fiber stacks are allocated with `mmap` and freed with `munmap`. Direct
`malloc` for stack memory is forbidden - stacks must be `mmap`-allocated so
that guard pages can be installed with `mprotect(PROT_NONE)`.

```c
/* Correct - mmap-allocated stack with guard page */
static strand_stack_t *
stack_alloc(size_t stack_size)
{
	void           *mem;
	strand_stack_t *s;

	mem = mmap(NULL, stack_size + PAGE_SIZE,
	    PROT_READ | PROT_WRITE,
	    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (mem == MAP_FAILED)
		return NULL;

	/* guard page at the low end */
	if (mprotect(mem, PAGE_SIZE, PROT_NONE) != 0) {
		munmap(mem, stack_size + PAGE_SIZE);
		return NULL;
	}

	s              = (strand_stack_t *)mem;
	s->base        = (uint8_t *)mem + PAGE_SIZE;
	s->size        = stack_size;
	s->total_size  = stack_size + PAGE_SIZE;
	return s;
}

/* Wrong - malloc cannot have a guard page installed */
uint8_t *stack = malloc(stack_size);
```

**Stack cache:** Completed fiber stacks are returned to the per-worker
stack cache rather than unmapped immediately. The stack allocator checks
the cache before calling `mmap`. See ARCHITECTURE.md §13 for cache policy.

### 3.2 Control Structure Memory

Fiber descriptors, scheduler objects, scope control blocks, work items, and
other runtime control structures are allocated with `malloc` and freed with
`free`. These are not stacks and do not require `mmap`.

Every `malloc` call in the library must check for NULL:

```c
f = malloc(sizeof(strand_fiber_t));
if (f == NULL)
	return STRAND_ERR_NOMEM;
```

**NULL after free:** Set pointers to NULL after freeing to prevent
accidental use-after-free:

```c
free(item);
item = NULL;
```

**Exception:** Fiber descriptors are never freed while the runtime runs -
they are recycled through the dead pool. Do not free fiber descriptors
directly; return them to the dead pool. See ARCHITECTURE.md §4.6.

### 3.3 Resource Cleanup with goto

Use the `goto cleanup` pattern for functions that acquire multiple
resources. This ensures cleanup on every error path without duplication.

```c
static strand_scheduler_t *
scheduler_create(const strand_sched_config_t *cfg)
{
	strand_scheduler_t *sched;
	int                 rc = STRAND_ERR_NOMEM;

	sched = malloc(sizeof(strand_scheduler_t));
	if (sched == NULL)
		goto cleanup;

	sched->poller = poller_create();
	if (sched->poller == NULL)
		goto cleanup;

	sched->timer_heap = timer_heap_create(cfg->timer_cap);
	if (sched->timer_heap == NULL)
		goto cleanup;

	sched->inject_queue = inject_queue_create(cfg->inject_cap);
	if (sched->inject_queue == NULL)
		goto cleanup;

	rc = STRAND_OK;

cleanup:
	if (rc != STRAND_OK) {
		scheduler_destroy(sched);
		return NULL;
	}
	return sched;
}
```

This pattern is mandatory for any function that acquires more than one
resource before it can succeed.

---

## 4. Atomic Operations

libstrand uses C11 `_Atomic` and `<stdatomic.h>` throughout. Incorrect
use of atomics produces data races that are silent, non-deterministic, and
platform-dependent. Follow these rules without exception.

### 4.1 Default Memory Order: seq_cst

**Use `memory_order_seq_cst` as the default for all atomic operations.**
seq_cst is the strongest and simplest ordering. It is always correct. Use
it unless there is a specific documented reason to use a weaker order and
the weaker order has been explicitly verified against the architecture
document.

```c
/* Correct - seq_cst default via non-explicit form */
atomic_store(&f->state, FIBER_RUNNABLE);
state = atomic_load(&f->state);

/* Also correct - explicit seq_cst */
atomic_store_explicit(&f->state, FIBER_RUNNABLE, memory_order_seq_cst);

/* Requires justification - document why this is safe */
atomic_store_explicit(&item->result_written, 1, memory_order_release);
```

### 4.2 Documented Exceptions to seq_cst

Two places in the library use explicit acquire/release ordering for
documented reasons. These are the only permitted deviations from seq_cst.
Every such deviation must have a `/* ATOMIC: ... */` comment explaining the
ordering requirement.

**Offload result visibility chain** (ARCHITECTURE.md §6.7):

```c
/*
 * ATOMIC: release on CAS - publishes the result_slot write to the
 * fiber's home worker. The acquire on inject dequeue establishes the
 * happens-before chain that makes result_slot visible to the fiber.
 * See ARCHITECTURE.md §6.7 for the full ordering chain.
 */
success = atomic_compare_exchange_strong_explicit(
    &item->state,
    &expected,            /* PENDING */
    OFFLOAD_RESULT_CLAIMED,
    memory_order_release, /* success order */
    memory_order_relaxed  /* failure order */
);
if (success)
	inject_queue_push_release(&sched->inject_queue, item);

/*
 * ATOMIC: acquire on inject dequeue - pairs with the release on
 * inject_queue_push above, establishing happens-before with the
 * result_slot write. See ARCHITECTURE.md §6.7.
 */
item = inject_queue_pop_acquire(&sched->inject_queue);
result = item->result_slot;   /* visible due to acquire above */
```

**Inject queue enqueue/dequeue:** The inject queue implementation uses
release on enqueue and acquire on dequeue. This is an internal
implementation detail of the inject queue module - callers use the queue
via its API and do not need to apply additional ordering.

### 4.3 CAS Discipline

All compare-and-swap operations must handle both success and failure
explicitly. Never assume a CAS succeeds.

```c
/*
 * Attempt to transition scope from ACTIVE to CANCELLING.
 * If another thread won the CAS first, that is correct - scope is
 * already in the right direction.
 */
expected = SCOPE_ACTIVE;
atomic_compare_exchange_strong(&scope->lifecycle,
    &expected, SCOPE_CANCELLING);
/* expected now holds the actual state regardless of outcome */
```

**Three-state offload CAS** - the PENDING → RESULT_CLAIMED and
PENDING → CANCELLED transitions are exclusive. Only one party wins. The
losing party must observe the winner's state and act accordingly:

```c
expected = OFFLOAD_PENDING;
if (atomic_compare_exchange_strong(&item->state,
    &expected, OFFLOAD_CANCELLED)) {
	/* we won - transition fiber to runnable with cancel result */
} else {
	/*
	 * Lost the CAS - expected now holds the actual state.
	 * If RESULT_CLAIMED: offload thread won; do not touch refcount;
	 * fiber will resume with the offload result normally.
	 * See ARCHITECTURE.md §6.6.
	 */
	assert(expected == OFFLOAD_RESULT_CLAIMED);
}
```

### 4.4 Non-Atomic Fields

Fields not declared `_Atomic` must never be read or written from multiple
threads without synchronisation. Per-worker scheduler fields (run queue,
timer heap, poller state, fiber descriptor internals) are owned by exactly
one worker thread and require no atomic access. Do not add spurious atomic
qualifiers to single-owner fields - it obscures the thread ownership model
and adds unnecessary memory barriers.

```c
/* Correct - single-owner field, no atomic needed */
sched->run_queue_len++;    /* called only from owner worker */

/* Wrong - unnecessary atomic on single-owner field */
atomic_fetch_add(&sched->run_queue_len, 1);   /* misleads the reader */
```

### 4.5 CONCURRENT Comments

Mark all accesses to shared atomic fields with a `/* CONCURRENT: ... */`
comment at the field's declaration, explaining which threads access it and
what ordering is required.

```c
typedef struct strand_scope {
	/*
	 * CONCURRENT: accessed from owner fiber and from sibling fibers
	 * calling strand_fiber_cancel. All transitions use seq_cst.
	 * See ARCHITECTURE.md §7.3.
	 */
	_Atomic scope_lifecycle_t  lifecycle;

	/*
	 * CONCURRENT: decremented by any worker when a child fiber finishes.
	 * seq_cst. Reaching zero triggers SCOPE_COMPLETED transition.
	 */
	_Atomic int                live_child_count;

	/*
	 * CONCURRENT: CAS-set once by the first failing child.
	 * Subsequent writes discarded. seq_cst.
	 */
	_Atomic int                first_error;
} strand_scope_t;
```

---

## 5. Error Handling

### 5.1 Error Returns

Every function that can fail returns an error code. Callers must check
return values. `void` return is permitted only for functions that cannot
fail (pure computations, no allocation, no system calls).

```c
/* Public API: return STRAND_OK (0) on success, STRAND_ERR_* on failure */
int rc = strand_fiber_spawn(runtime, fn, arg, 0, &handle);
if (rc != STRAND_OK) {
	/* handle error */
}

/* Internal functions: same convention */
if (poller_arm(sched->poller, fd, EPOLLIN) != STRAND_OK)
	return STRAND_ERR_INTERNAL;
```

Do not silently discard return values. If a return value is intentionally
ignored, cast to `(void)`:

```c
(void)clock_gettime(CLOCK_MONOTONIC, &ts);  /* cannot fail in practice */
```

### 5.2 No Assertions on Caller API Misuse in Release Builds

`assert()` is for internal invariants - conditions that must hold given
correct internal logic. API precondition violations from library callers
(e.g. calling `strand_fiber_wait_readable` from a host thread, passing NULL
where a non-NULL pointer is required) return error codes in release builds.
In debug builds (`STRAND_DEBUG`), they may additionally assert with a
diagnostic message.

```c
/* Correct - internal invariant: programmer error if violated */
assert(sched->run_queue_len >= 0 &&
    "run_queue_len underflowed: impossible internal state");

/* Correct - API precondition: error return in release, assert in debug */
if (sched->current_fiber == NULL) {
	STRAND_DEBUG_ASSERT(0, "fiber API called from non-fiber context");
	return STRAND_ERR_NOT_IN_FIBER;
}

/* Wrong - assert on a caller-controllable condition in production path */
assert(fd >= 0);   /* caller could pass -1; return STRAND_ERR_INVALID instead */
```

`STRAND_DEBUG_ASSERT(cond, msg)` expands to `assert(cond && msg)` in debug
builds and to `((void)0)` in release builds.

### 5.3 Resource Cleanup on Error

All error paths must release any resources acquired before the failure. Use
`goto cleanup` for multi-resource acquisition (see §3.3). Single-resource
functions may return directly after the NULL check.

### 5.4 Cooperative Scheduling Contract Violations

The cooperative scheduling contract requires that fibers yield regularly.
The following patterns are liveness violations - they are not API errors
that the library can detect and return, but they are explicitly documented
and must be called out in comments when a pattern is at risk:

```c
/*
 * WARNING: calling strand_fiber_offload in a tight loop without yielding
 * is a liveness violation - it starves other fibers on this worker.
 * Always yield before retrying on STRAND_EAGAIN.
 * See ARCHITECTURE.md §6.5.
 */
while ((rc = strand_fiber_offload(fn, &arg, &result)) == STRAND_EAGAIN)
	strand_fiber_yield();   /* required - never remove this yield */
```

---

## 6. Safety Practices

### 6.1 fd Lifecycle Rules

All fd-related code must respect the lifecycle contract stated in
ARCHITECTURE.md §5.2. The critical rules:

- Never call `strand_fiber_wait_readable` or `strand_fiber_wait_writable`
  on a blocking fd. The worker thread will block entirely.
- Debug builds assert O_NONBLOCK at fd registration. Release builds do not
  check - it is the caller's responsibility.
- An fd is not fully free for re-use until both read and write waiters are
  cleared. Do not close or dup an fd while a waiter is registered.
- Cross-worker cancel does not immediately free the fd. The fd is safe to
  reuse only after the cancel has been confirmed via scope completion or
  other synchronisation.

```c
/*
 * SAFETY: assert O_NONBLOCK in debug builds. Blocking fds stall the
 * entire worker thread. Callers must set O_NONBLOCK before calling.
 * See ARCHITECTURE.md §5.2.
 */
#ifdef STRAND_DEBUG
{
	int flags = fcntl(fd, F_GETFL);
	STRAND_DEBUG_ASSERT(flags & O_NONBLOCK,
	    "fd passed to fiber_wait_readable without O_NONBLOCK");
}
#endif
```

### 6.2 SAFETY Comments

Mark safety-critical invariants explicitly so they are never accidentally
removed during refactoring. The comment format is `/* SAFETY: ... */` on
its own line before the guarded code.

```c
/*
 * SAFETY: drain wakeup fd as a control event - do not interpret as a
 * fiber waiter event. Bytes are written by strand_scheduler_stop and
 * cross-worker signals; they carry no fd readiness information.
 * See ARCHITECTURE.md §4.2, Step 3.
 */
if (events[i].data.fd == sched->wakeup_fd) {
	drain_wakeup_fd(sched->wakeup_fd);
	continue;
}

/*
 * SAFETY: process ALL events from the zero-timeout post-re-arm poll,
 * not only the rearmed fd. Events for other fds are consumed from the
 * kernel queue and would be lost if discarded here.
 * See ARCHITECTURE.md §5.5.
 */
for (int i = 0; i < n; i++)
	poller_deliver_event(sched, &events[i]);

/*
 * SAFETY: validate fiber handle before use. Stale handles must be
 * no-ops. Do not skip this check - ABA protection depends on it.
 * See ARCHITECTURE.md §4.6.
 */
if (handle.ptr == NULL)
	return STRAND_HANDLE_INVALID;
if (handle.ptr->generation != handle.generation)
	return STRAND_HANDLE_STALE;

/*
 * SAFETY: scope_abandon requires a heap-allocated control block.
 * Stack-allocated scope + abandon = use-after-free when stack unwinds.
 * See ARCHITECTURE.md §7.4.
 */
#ifdef STRAND_DEBUG
STRAND_DEBUG_ASSERT(!ptr_in_fiber_stack(strand_current_fiber(), scope),
    "strand_scope_abandon called with stack-allocated scope");
#endif
```

### 6.3 TLS Footguns

Code that calls standard C library functions across yield points must use
reentrant variants. The following functions maintain internal state that is
not fiber-local and must not be called across yield points:

| Forbidden | Use instead |
|---|---|
| `strtok` | `strtok_r` |
| `asctime`, `ctime` | `asctime_r`, `ctime_r` |
| `rand` | `rand_r` or fiber-local PRNG |
| `getc_unlocked`, `putc_unlocked` | Locked variants |

The general rule: any function documented as "not thread-safe" is also not
fiber-safe. Prefer `_r`-suffixed reentrant variants throughout fiber code.

When using third-party libraries that maintain per-thread state (OpenSSL ERR
state, `uselocale`, custom TLS-based request context), ensure that state is
not shared across fibers on the same worker or that all accesses occur within
a single fiber without yields.

### 6.4 Integer Safety

Use fixed-width types for all size calculations and timer arithmetic.
Check for overflow before arithmetic on values that could be large.

```c
/* Use sized types */
uint64_t  deadline_ns;
size_t    stack_size;
uint64_t  generation;

/* Check for overflow before timer arithmetic */
if (timeout_ns > UINT64_MAX - now_ns)
	deadline_ns = UINT64_MAX;   /* saturate */
else
	deadline_ns = now_ns + timeout_ns;
```

### 6.5 fork() Safety

After `strand_runtime_init`, `fork()` without an immediate `exec()` is
undefined behaviour. Do not add any code paths that call `fork()` from
inside the library after initialisation. Subprocess spawning from fibers
must use `strand_fiber_offload` to run `fork()`/`exec()`/`waitpid()` on
an offload thread.

---

## 7. Documentation

### 7.1 Function Comments

Every non-trivial public function requires a block comment describing what
it does, its parameters, return values, and any threading or lifetime
constraints. Internal static helpers require a brief comment unless the
function name is entirely self-explanatory.

```c
/*
 * strand_fiber_offload - run a blocking function on an offload thread.
 *
 * Parks the calling fiber and submits fn(arg) to the offload pool.
 * Resumes the fiber when fn completes; the return value of fn is written
 * to *result_slot before the fiber is woken.
 *
 * fn:          blocking function to run on an offload thread
 * arg:         passed to fn - must remain valid until fn completes,
 *              even if the calling fiber is cancelled (heap-allocate
 *              if the calling fiber's lifetime may not cover fn's)
 * result_slot: receives the return value of fn; must remain valid until
 *              the fiber resumes
 *
 * Returns STRAND_OK on success.
 * Returns STRAND_EAGAIN if the offload pool is full - caller must yield
 *         before retrying (see ARCHITECTURE.md §6.5).
 * Returns STRAND_ERR_CANCELLED if the fiber was cancelled before fn
 *         completed AND the offload thread had not yet claimed the result.
 * Returns STRAND_ERR_NOT_IN_FIBER if called from a non-fiber context.
 *
 * Must be called only from a running fiber. Not safe from host thread,
 * signal handlers, or offload threads.
 */
int
strand_fiber_offload(strand_offload_fn_t fn, void *arg, void *result_slot);
```

### 7.2 Architecture Cross-Reference Comments

When implementing scheduler, poller, scope, or offload logic, reference
the relevant section of ARCHITECTURE.md.

```c
/*
 * Step 3 of strand_scheduler_advance: drain wakeup fd as a control event.
 * See ARCHITECTURE.md §4.2.
 */

/*
 * Fiber handle validation - generation counter ABA protection.
 * See ARCHITECTURE.md §4.6.
 */

/*
 * Post-re-arm readiness check - required for EPOLLET on Linux only.
 * Process all events returned by the zero-timeout poll, not only the
 * rearmed fd. See ARCHITECTURE.md §5.5.
 */

/*
 * Scope cancellation walk - reverse spawn order, cooperative.
 * Hold walk_ref_count for duration of walk; release on completion.
 * See ARCHITECTURE.md §7.3.
 */
```

### 7.3 TODO and FIXME

```c
/* TODO: hash index for fd table when fd count exceeds linear threshold */
/* FIXME: idle reclamation timer not yet wired into scheduler advance */
/* NOTE: inject queue len is uint32_t - cannot exceed UINT32_MAX entries;
 *       the configurable cap must be validated at init to stay within this */
/* NOTE: STRAND_OK == 0; STRAND_IDLE and error codes are negative.
 *       Do not compare rc > 0 to mean success - use rc == STRAND_OK */
```

---

## 8. Pre-Commit Checklist

Before every commit:

- [ ] Compiles without warnings on Linux (`-Wall -Wextra -Wpedantic`)
- [ ] Compiles without warnings on OpenBSD (`-Wall -Wextra -Wpedantic`)
- [ ] All tests pass (`make test`)
- [ ] Valgrind clean on Linux (`make valgrind`)
- [ ] ASan/UBSan clean on both platforms (`make dev && make test`)
- [ ] clang-format clean (`make format`)
- [ ] No direct `malloc`/`free` for stack memory in `src/` - stacks must
      use `mmap`/`munmap` - verify with:
      `grep -n "malloc" src/*.c | grep -i stack`
- [ ] Every `malloc` call checks for NULL return
- [ ] Every `mmap` call checks for `MAP_FAILED` return
- [ ] `SAFETY:` comment present on every new safety-critical check
- [ ] `ATOMIC:` comment present on every non-seq_cst atomic operation
- [ ] `CONCURRENT:` comment present on every new `_Atomic` field declaration
- [ ] All CAS operations handle both success and failure paths explicitly
- [ ] No data races on non-atomic fields (per-worker fields accessed only
      from owner worker; shared fields are `_Atomic`)
- [ ] Fiber handle validation (null check + generation check) present on
      every function that accepts a `strand_fiber_handle_t`
- [ ] `strand_scope_abandon` debug check for stack-allocated scope in place
- [ ] `goto cleanup` pattern used for all multi-resource acquisition
- [ ] No calls to TLS-unsafe stdlib functions across yield points - use
      `_r` variants (strtok_r, asctime_r, ctime_r, rand_r)
- [ ] No blocking fd passed to `strand_fiber_wait_readable` or
      `strand_fiber_wait_writable` - debug assert for O_NONBLOCK in place
- [ ] Assembly stubs emit valid CFI/FDE unwind info (`.cfi_startproc`,
	  `.cfi_endproc`, and stack-relative `.cfi_offset` directives where
	  applicable)
- [ ] `_Static_assert` present for any struct whose layout the assembly
      stubs depend on
- [ ] ASan fiber switch hooks present at every context switch
      (`__sanitizer_start_switch_fiber` before, `__sanitizer_finish_switch_fiber` after)
- [ ] TSan fiber hooks present at every context switch (`__tsan_switch_to_fiber`)
- [ ] Valgrind stack registration macros called at stack alloc and dealloc
- [ ] New public functions have complete doc comment blocks
- [ ] New scheduler/poller/scope logic has ARCHITECTURE.md cross-reference
- [ ] No `FIXME` added without a comment explaining what is deferred and why

---

**See Also**: PROJECT.md, ARCHITECTURE.md, TECH_STACK.md, DEVELOPMENT.md
