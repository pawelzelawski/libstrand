# Architecture

## 1. Library Overview

### 1.1 Design Model

libstrand is a concurrency library, not a framework. The direction of control
flow always remains with the programmer's code. The programmer calls
libstrand; libstrand does not call the programmer back from hidden threads.

**Precision by layer:**
- Layers 1–2: fully guest-capable. The programmer creates a scheduler object
  and drives it explicitly from their own event loop.
- Layer 3: guest-capable with a defined integration contract. The scheduler
  exposes a single fd and a deadline query so the host loop can incorporate
  it with minimal changes.
- Layers 4–5: accurately described as an embeddable runtime. The programmer
  starts and stops workers explicitly; the library owns those threads only
  between `strand_scheduler_run` and its return.

### 1.2 What the Library Does Not Do

libstrand does not wrap, replace, or intercept standard library functions. It
does not intercept or handle signals. It does not migrate fibers between
workers. It does not implement work stealing or preemption. It does not modify
fd flags. It does not register `pthread_atfork` handlers. It does not
implement garbage collection. It has no mandatory third-party dependencies. It
does not require a non-standard build system.

### 1.3 Design Influences and Rejected Alternatives

libstrand borrows from four concurrency traditions:

- **Goroutines (Go runtime):** Stackful fiber primitive, cooperative
  scheduling, I/O parking. The clearest existence proof that the fiber model
  is correct for connection-oriented workloads.
- **Structured concurrency (Trio, Swift Concurrency):** Scoped task lifetimes,
  guaranteed cleanup, first-error propagation.
- **Actors:** Mailbox-based communication concepts and supervision ideas.
  Actor-inspired, not equivalent to Erlang actors - libstrand does not
  implement a full actor model.
- **Thread-per-core (Seastar):** Per-worker ownership, explicit cross-worker
  coordination, no work stealing.

**Async/await explicitly rejected.** The C ecosystem has several alternatives
to stackful fibers. All were evaluated and rejected:

- **Async/await with futures:** Reintroduces callback chains in the form of
  continuation composition. Does not solve the state machine explosion problem.
- **Protothreads:** Macro-based stackless coroutines. Produce unreadable code
  with severe constraints on local variable use.
- **Source-level code generators:** Add mandatory build complexity and a
  non-standard compilation step. Incompatible with the zero-build-dependency
  requirement.
- **io_uring:** Linux-only (requires kernel 5.1+) and would permanently
  exclude OpenBSD. Out of scope.

The stackful fiber model is the only option that preserves sequential code
structure, works on both target platforms, requires no build-time transformation,
and composes naturally with existing C code.

### 1.4 API Philosophy

**Explicit over implicit.** Every ownership decision, every lifecycle
constraint, every threading rule is stated in the API rather than inferred
from conventions.

**Additive not transformative.** The library adds fiber concurrency to a C
program without restructuring it. The programmer's existing event loop, their
existing fd management, their existing error handling patterns are all
preserved.

**C-idiomatic.** The API uses plain structs, integer error codes, and explicit
handle passing. No hidden allocation, no opaque vtables, no macro-heavy DSL.

**No overpromising names.** `strand_fiber_spawn` not `strand_fiber_go`.
Names reflect what the operation does, not what the programmer hopes it
implies.

---

## 2. The Five Layers

```
┌──────────────────────────────────────────────────────────┐
│  Layer 5 - Coordination Primitives                       │
│  Scopes, cancellation, fiber-local storage, timers,      │
│  channels (deferred), mutexes (deferred)                 │
├──────────────────────────────────────────────────────────┤
│  Layer 4 - Multi-Worker Runtime                          │
│  Multiple workers, inject queues, fiber_spawn routing,   │
│  offload pool                                            │
├──────────────────────────────────────────────────────────┤
│  Layer 3 - I/O Integration                               │
│  epoll (Linux) / kqueue (OpenBSD), fd parking,           │
│  edge-triggered one-shot, re-arm protocol                │
├──────────────────────────────────────────────────────────┤
│  Layer 2 - Fiber Scheduler                               │
│  Cooperative scheduling, run queue, timer heap,          │
│  guest and worker operating modes                        │
├──────────────────────────────────────────────────────────┤
│  Layer 1 - Execution Contexts                            │
│  x86_64 and AArch64 context save/restore, guard pages,   │
│  CFI annotations, sanitizer hooks                        │
└──────────────────────────────────────────────────────────┘
```

Each layer is independently usable and testable. Lower layers can be used
without the upper layers. Each layer is built and tested completely before the
next layer begins implementation.

---

## 3. Layer 1 - Execution Contexts

### 3.1 Overview

Layer 1 is raw context save and restore. It knows nothing about scheduling,
fibers, or I/O. It provides one operation: switch from the current execution
context to another. Everything above Layer 1 is built in C.

**No global state.** Layer 1 has no global variables, no static mutable state,
and no thread-local state of its own. It can be used in any context without
hidden side effects or initialization order dependencies.

Context switching is implemented in hand-written assembly. There is no
portable pure-C alternative - saving and restoring the stack pointer requires
direct register access.

### 3.2 x86_64 Context Save/Restore (SysV AMD64 ABI)

Targets: Linux x86_64, OpenBSD amd64.

**Callee-saved GPRs saved and restored on every switch:**
```
rbx, rbp, r12, r13, r14, r15, rsp
```

**XMM registers:** Under the System V AMD64 ABI (Linux and OpenBSD), all XMM
registers are caller-saved. No XMM registers are saved by the context switch.
This is correct. The Windows x64 ABI is different (XMM6–XMM15 are
callee-saved there) but libstrand does not target Windows.

**Stack alignment:** 16 bytes on entry to the switched-to context.

### 3.3 AArch64 Context Save/Restore (AAPCS64)

Targets: Linux arm64, OpenBSD arm64.

**Callee-saved GPRs saved and restored:**
```
x19–x28, x29 (frame pointer), x30 (link register), sp
```

**Callee-saved FP/SIMD registers saved and restored:**
```
v8–v15  (lower 64 bits only - d8–d15 per AAPCS64)
```

**Stack alignment:** 16 bytes.

### 3.4 errno and Floating-Point Control Registers

**errno:** Saved and restored on every context switch. Handled in the C
wrapper around the assembly stub, not in the assembly itself. Each fiber
sees its own errno state.

**MXCSR (x86_64):** Saved and restored via `stmxcsr` / `ldmxcsr` on every
context switch. Each fiber has its own MXCSR state.

**FPCR and FPSR (AArch64):** Saved and restored via `mrs` / `msr` on every
context switch. Each fiber has its own FPCR and FPSR state.

**x87 FP environment:** Not saved. Code that modifies x87 state (including
`long double` computations) across yield points is unsupported. This must be
stated explicitly and prominently in all user-facing documentation: programs
using `long double` may see corrupted floating-point behaviour after a yield.

### 3.5 Guard Pages

Fiber stacks are allocated with `mmap(MAP_ANONYMOUS | MAP_PRIVATE)`. A guard
page is installed at the low end of each stack with `mprotect(PROT_NONE)`. A
stack overflow hits the guard page and generates SIGSEGV rather than silently
corrupting adjacent memory.

### 3.6 CFI Annotations

All assembly stubs carry correct DWARF CFI annotations. At minimum this means
`.cfi_startproc` / `.cfi_endproc` with a valid FDE. Where registers are
saved to the current stack frame, matching `.cfi_offset` /
`.cfi_def_cfa_offset` directives are required. For context-switch stubs that
save registers into explicit context structs (not CFA-relative stack slots),
CFI must still emit valid unwind metadata but `.cfi_offset` entries may not be
applicable.
Correct annotations are required for:
- Stack unwinding in gdb and lldb
- `perf` profiling with frame pointers
- Crash reporter backtraces on fiber stacks
- LSAN stack scanning

Missing or incorrect CFI annotations produce misleading backtraces and
incorrect unwind through exception handling. Every assembly stub is annotated
completely. This is not optional and is verified at each layer boundary test.

### 3.7 Sanitizer Hooks

The context switch calls sanitizer hooks immediately before and after every
switch. These hooks are compiled in conditionally and are no-ops in non-
sanitizer builds.

**ASan hooks** (required to prevent false positives when switching stacks):
```c
/* called immediately before switch, passing destination stack */
__sanitizer_start_switch_fiber(NULL, new_stack_bottom, new_stack_size);

/* called immediately after switch completes */
__sanitizer_finish_switch_fiber(NULL, NULL, NULL);
```

Without these, ASan incorrectly reports accesses to the new stack as
stack-buffer-overflows.

**TSan hooks** (required for correct happens-before tracking):
```c
/* called on the outgoing fiber before switch */
__tsan_switch_to_fiber(incoming_fiber->tsan_fiber, 0);
```

Each fiber has a `tsan_fiber` handle created at spawn via
`__tsan_create_fiber(0)` and destroyed at fiber teardown via
`__tsan_destroy_fiber`. Without TSan integration, TSan produces both false
positives and false negatives across context switch boundaries.

**Valgrind stack registration** (required to suppress false positives on
fiber stacks):
```c
/* at stack allocation: */
VALGRIND_STACK_REGISTER(stack_base, stack_base + stack_size);

/* at stack deallocation: */
VALGRIND_STACK_DEREGISTER(valgrind_stack_id);
```

---

## 4. Layer 2 - Fiber Scheduler

### 4.1 Overview

Layer 2 is a cooperative scheduler built on Layer 1. One scheduler object
manages one OS thread and any number of fibers. The scheduler object is
explicit and caller-owned - there is no global scheduler state.

### 4.2 Scheduler Operating Modes

**`strand_scheduler_advance(sched)` - guest mode:**

Nonblocking. Performs exactly one pass through the scheduler work pipeline and
returns. Never blocks the caller. Used when the programmer drives the scheduler
from their own event loop.

**Steps performed in order:**
1. Drain inject queue - move injected fibers/events to local run queue (FIFO)
2. Process expired timers - move fibers whose deadline has passed to run queue
3. Drain wakeup fd/pipe - read and discard all bytes written by
   `strand_scheduler_stop` or cross-worker signals. These bytes are control
   signals, not fiber waiter events, and must not be interpreted as fd
   readiness.
4. Poll I/O with zero timeout - `epoll_wait`/`kevent` with `timeout=0`, move
   ready fibers to run queue
5. Run up to budget fibers - FIFO order, new arrivals go to tail, default
   budget 64 (configurable)

**Returns:** `SCHED_PROGRESS` if any work was done, or `SCHED_IDLE` with the
next timer deadline as a `uint64_t` nanosecond timestamp (`UINT64_MAX` if no
pending timers).

**`strand_scheduler_run(sched)` - worker mode:**

Blocking. Runs `strand_scheduler_advance` in a loop. When the run queue is
empty after all steps, blocks in `epoll_wait`/`kevent` with the next timer
deadline as the timeout, or indefinitely if no pending timers. Never returns
until `strand_scheduler_stop` is called and the stop flag is observed.

**`strand_scheduler_stop(sched)`:**

Sets an atomic stop flag AND writes to the worker's wakeup eventfd (Linux) or
pipe (OpenBSD). Both steps are required - the atomic flag alone does not
interrupt a worker blocked indefinitely in `epoll_wait`/`kevent`. Safe to call
from any thread. The written bytes are drained in Step 3 of the next
`strand_scheduler_advance` call as control events.

**`strand_scheduler_next_deadline(sched)`:**

Returns the next timer deadline as a `uint64_t` nanosecond timestamp, or
`UINT64_MAX` if none. Called by the host loop to compute its own `epoll_wait`
timeout.

**`strand_scheduler_get_fd(sched)`:**

Returns the file descriptor the host loop must monitor for scheduler activity.
The host loop adds this fd to its own epoll/kqueue instance.

### 4.3 Host Loop Integration Pattern

The correct pattern for integrating a libstrand scheduler into an existing
event loop:

```c
while (running) {
    uint64_t deadline = strand_scheduler_next_deadline(sched);
    int timeout_ms    = deadline_to_ms(deadline);  /* caller computes */

    epoll_wait(host_epoll, events, max_events, timeout_ms);

    for (int i = 0; i < nfds; i++) {
        if (events[i].data.fd == strand_scheduler_get_fd(sched)) {
            strand_scheduler_advance(sched);
        } else {
            /* handle host application events */
        }
    }

    /*
     * Always call strand_scheduler_advance after every epoll_wait return,
     * regardless of which fds fired. Timer expiry requires processing even
     * when no I/O event arrived. The double call when the scheduler fd also
     * fired is intentional and harmless - it processes timers and injected
     * work more eagerly.
     */
    strand_scheduler_advance(sched);
}
```

The unconditional trailing call is required. A timer deadline may expire
during the `epoll_wait` that is woken by an unrelated application fd. Without
the trailing call, timer-fired fibers would not be run until the next
iteration.

### 4.4 Fairness and Budget

The run queue is FIFO. New arrivals (from inject drain, timer expiry, I/O
readiness) are appended to the tail. The scheduler runs up to `budget` fibers
per `strand_scheduler_advance` call, then returns. Fibers that yield
explicitly are re-added to the tail.

Default budget: 64 fibers per `strand_scheduler_advance` call. Configurable
at scheduler initialisation.

Inject queue items are always drained before any locally queued fiber runs
(Step 1 before Step 5). This ensures that cross-worker cancellations and
completions are processed promptly.

### 4.5 Fiber State Machine

**States:**

```
FIBER_NEW
FIBER_RUNNABLE
FIBER_RUNNING
FIBER_PARKED_IO_READ
FIBER_PARKED_IO_WRITE
FIBER_PARKED_TIMER
FIBER_PARKED_OFFLOAD
FIBER_PARKED_CHANNEL
FIBER_CANCELLATION_PENDING   (flag coexisting with RUNNABLE or RUNNING)
FIBER_FINISHED
```

**Legal transitions:**

```
FIBER_NEW
  -> FIBER_RUNNABLE          stack fabrication complete

FIBER_RUNNABLE
  -> FIBER_RUNNING           scheduler picks this fiber
  -> FIBER_RUNNABLE          cancellation flag set; stays in queue

FIBER_RUNNING
  -> FIBER_RUNNABLE          fiber calls strand_fiber_yield explicitly
  -> FIBER_PARKED_IO_READ    fiber calls strand_fiber_wait_readable(fd)
  -> FIBER_PARKED_IO_WRITE   fiber calls strand_fiber_wait_writable(fd)
  -> FIBER_PARKED_TIMER      fiber calls strand_fiber_sleep_until(deadline)
  -> FIBER_PARKED_OFFLOAD    fiber calls strand_fiber_offload(fn, arg, result)
  -> FIBER_PARKED_CHANNEL    fiber calls channel_send or channel_recv and blocks
  -> FIBER_FINISHED          fiber function returns

FIBER_PARKED_IO_READ / FIBER_PARKED_IO_WRITE
  -> FIBER_RUNNABLE          fd ready (poller fires), or strand_fiber_cancel called

FIBER_PARKED_TIMER
  -> FIBER_RUNNABLE          deadline expires, or strand_fiber_cancel called

FIBER_PARKED_OFFLOAD
  -> FIBER_RUNNABLE          offload thread wins RESULT_CLAIMED CAS - result injected
  -> FIBER_RUNNABLE          strand_fiber_cancel wins CANCELLED CAS - cancel result
  Note: if RESULT_CLAIMED wins before cancel arrives, fiber resumes normally
        with the offload result regardless of the subsequent cancel attempt.

FIBER_PARKED_CHANNEL
  -> FIBER_RUNNABLE          channel operation becomes possible, or cancel called

FIBER_FINISHED
  -> destroyed               scheduler cleanup; scope reference released
```

**Ownership per state:**

| State | Owner |
|---|---|
| FIBER_NEW | creating thread |
| FIBER_RUNNABLE | run queue |
| FIBER_RUNNING | executing worker |
| FIBER_PARKED_IO_READ / IO_WRITE | poller registry |
| FIBER_PARKED_TIMER | timer heap |
| FIBER_PARKED_OFFLOAD | offload pool work item |
| FIBER_PARKED_CHANNEL | channel wait queue |
| FIBER_FINISHED | scheduler pending cleanup |

### 4.6 Fiber Handle ABA Protection

```c
typedef struct {
    fiber_t  *ptr;
    uint64_t  generation;
} strand_fiber_handle_t;
```

**Generation counter:** Stored in the fiber descriptor as `uint64_t`.
Incremented when a descriptor is taken from the dead pool for reuse. Initial
value 1 on first allocation.

**Why 64-bit:** At 10 million fiber creations per second a 32-bit counter
wraps in approximately 7 minutes - achievable under benchmark conditions and
in busy production systems. A 64-bit counter at the same rate takes
approximately 58,000 years to wrap. The correctness benefit eliminates
generation counter overflow as a practical concern.

**Validation on every API use:**
```c
if (handle.ptr == NULL)                          return STRAND_HANDLE_INVALID;
if (handle.ptr->generation != handle.generation) return STRAND_HANDLE_STALE;
```

**Stale handle behaviour:** No-op. Returns `STRAND_HANDLE_STALE`. This
applies everywhere: `strand_fiber_cancel`, sibling cancellation walks,
scope child list traversal.

**Descriptor lifetime:** Fiber descriptors are never freed while the runtime
runs - they are recycled through a dead pool. Stale handle pointers are always
safe to dereference for generation validation. Dead pool size is bounded by
peak concurrent fiber count.

**Handle creation:** Only via `strand_fiber_spawn`. Manual handle construction
is not part of the API.

### 4.7 TLS Safety

Fibers pinned to a worker preserve stable OS thread identity. They do not make
thread-local storage fiber-local.

Multiple fibers on the same worker share `__thread` variables,
`pthread_getspecific` values, per-thread library state, and `pthread_self()`
identity.

**Handled automatically on every context switch:**
- errno
- MXCSR (x86_64)
- FPCR and FPSR (AArch64)

**Shared and not automatically handled (documented footguns):**
- OpenSSL ERR state
- `pthread_getspecific` values
- `__thread` request context
- `uselocale()`
- Any library that caches per-request state in TLS

**Standard C library functions that are not fiber-safe:**

| Function | Problem | Safe alternative |
|---|---|---|
| `strtok` | static pointer to string being tokenised | `strtok_r` |
| `asctime`, `ctime` | return pointer to static buffer | `asctime_r`, `ctime_r` |
| `rand` | per-thread seed state | `rand_r` or fiber-local PRNG |
| `getc_unlocked` and unlocked stdio | may share stream state | locked variants |

The general rule: any C standard library function whose documentation mentions
"static storage" or "not thread-safe" is not fiber-safe either. Prefer
reentrant (`_r` suffix) variants throughout fiber code.

**Fiber-local storage** is provided as an explicit opt-in:
`strand_fiber_local_set(ptr)` and `strand_fiber_local_get()`. Single `void*`
slot per fiber with an optional destructor called on fiber completion.

---

## 5. Layer 3 - I/O Integration

### 5.1 Overview

Layer 3 connects the scheduler to the OS I/O polling interface. It wraps
I/O parking - the fiber parks until an fd becomes ready - not I/O syscalls.
The library never calls `read`, `write`, `send`, `recv`, or any I/O syscall
on behalf of the fiber.

**The library never modifies fd flags.** O_NONBLOCK is always the caller's
responsibility. Calling `strand_fiber_wait_readable` or
`strand_fiber_wait_writable` on a blocking fd is undefined behaviour - the
worker thread will block entirely. Debug builds assert O_NONBLOCK at
registration.

### 5.2 fd Lifecycle Contract

**Before calling `strand_fiber_wait_readable` or `strand_fiber_wait_writable`:**
- fd must be open with O_NONBLOCK set
- No other fiber registered as waiter on this fd in the same direction
- fd must not also be registered with the host loop's own epoll instance

**During a wait call:**
- fd must not be closed or dup'd in a way that affects the registration
- No other fiber must register the same fd in the same direction

**After the wait call returns:**
- The library has removed or disabled its registration for this direction
- fd is fully free only when both directions have no active waiters
- Caller must drain until EAGAIN before calling wait again

**Registration state tracking (Linux):**

EPOLLONESHOT disables but does not delete. Three states are tracked per fd:
```
REGISTERED_ACTIVE    - currently registered and armed
REGISTERED_DISABLED  - fired and disabled by EPOLLONESHOT; re-arm uses MOD
NOT_REGISTERED       - never registered or DEL'd; re-arm uses ADD
```

**fd handoff between event loops:** Requires explicit deregistration from the
source loop before registration with the destination.

**Regular file I/O:** `epoll_ctl` returns `EPERM` for regular files - kernel
limitation. Use the offload pool for regular file I/O.

### 5.3 Waiter Model

**Per-fd waiter tracking:** One read waiter and one write waiter per fd,
tracked separately. Both are allowed simultaneously on the same fd.

**Multiple waiters in the same direction:** Forbidden. Returns an error on
violation.

**Event identity:** Each fd registration has an fd-plus-generation token
stored in `epoll_event.data.u64` (Linux) or `kevent.udata` (OpenBSD). Delivery
resolves the token through the current fd table and validates its generation.
This makes stale events harmless across table growth, cancellation, and fd
number reuse; no kernel event retains a pointer into table storage.

**Cancellation race - two-part rule:**

- **Same-worker cancellation:** Readiness wins. The poller drains in Step 4
  before the run queue executes in Step 5, so a ready event is detected before
  the cancelled fiber runs.
- **Cross-worker cancellation:** Cancel wins. The inject queue is drained in
  Step 1 before the I/O poller runs in Step 4, so a cancel injected by another
  worker is processed before the poller checks the fd.

This behavioural difference is intentional and documented accurately.

**fd closed while waiter registered:** Undefined behaviour. Caller contract.
Debug builds use a per-fd generation counter to detect this case and assert.

### 5.4 Linux: EPOLLET | EPOLLONESHOT

All file descriptors registered with the internal poller use
`EPOLLET | EPOLLONESHOT`. Edge-triggered so a single transition edge is
consumed per registration. One-shot so the registration is automatically
disabled after firing, preventing spurious re-delivery before the fiber has
re-armed.

**Re-arm operation depends on registration state:**
- `REGISTERED_DISABLED` (fired, disabled by EPOLLONESHOT): re-arm uses
  `epoll_ctl MOD`
- `NOT_REGISTERED` (never registered, or after `DEL`): re-arm uses
  `epoll_ctl ADD`

Using `ADD` on a `REGISTERED_DISABLED` fd returns `EEXIST`. Using `MOD` on
a `NOT_REGISTERED` fd returns `ENOENT`. The registration state must be
tracked accurately to use the correct operation.

**Registration mask reflects active waiters:**

| Active waiters | Registration mask |
|---|---|
| Read only | `EPOLLIN \| EPOLLET \| EPOLLONESHOT` |
| Write only | `EPOLLOUT \| EPOLLET \| EPOLLONESHOT` |
| Both | `EPOLLIN \| EPOLLOUT \| EPOLLET \| EPOLLONESHOT` |

On event firing: check returned flags. Wake read waiter if `EPOLLIN` set.
Wake write waiter if `EPOLLOUT` set.

### 5.5 Linux: Post-Re-arm Readiness Check

When one direction fires and the other direction still has an active waiter,
re-arming with `EPOLLET` requires an immediate readiness check. EPOLLET only
fires on state transitions - if the remaining direction was already ready
before the MOD call, no new edge will occur and the waiter would park forever.

**Required sequence after re-arming the remaining direction:**
1. Call `epoll_ctl MOD` to re-register interest in just the remaining
   direction with `EPOLLONESHOT`
2. Immediately call `epoll_wait` with `timeout=0` on the internal poller
3. Process **all events returned by this zero-timeout poll** through the
   normal event delivery path - not only the rearmed fd. Other fds on the
   internal poller may also have events ready; discarding them would lose
   those events permanently since EPOLLONESHOT has already consumed them.
4. If the rearmed fd fired: wake the remaining waiter immediately
5. If the rearmed fd did not fire: waiter remains parked, will wake on the
   next genuine edge

**EPOLLONESHOT prevents double-processing:** An event consumed by the zero-
timeout poll in step 2 cannot appear in a subsequent step-4 poll in the same
`strand_scheduler_advance` call - EPOLLONESHOT has disabled the registration.

**This check is Linux-specific.** OpenBSD's EV_DISPATCH is level-triggered:
re-enabling with EV_ENABLE fires automatically if the condition is still true.
No post-re-arm check is needed on OpenBSD.

**Cancellation mid-check:** If a cross-worker cancel arrives after the MOD
call but before the zero-timeout `epoll_wait` returns, the readiness check may
wake the waiter before the cancel is processed. This window behaves like
same-worker readiness-wins, which is consistent with the stated rule.

### 5.6 Linux: Error and Hangup Events

`EPOLLERR` and `EPOLLHUP` are delivered by the kernel regardless of whether
they appear in the registered interest mask. `EPOLLERR` wakes every waiter
with an error result. For `EPOLLHUP`, a read waiter is woken successfully when
`EPOLLIN` is also present, so the caller can drain bytes buffered before the
close; a subsequent nonblocking read observes EOF. An empty read hangup and
all write hangups wake with an error result, so no fiber remains parked.

**Why these flags are not in the interest mask:** `EPOLLERR` and `EPOLLHUP`
are delivered unconditionally by the kernel - adding them to the registration
mask is unnecessary but harmless. The critical requirement is in the event
delivery code: it must inspect these flags in returned events and wake affected
waiters. Omitting the inspection - not the registration - is the bug to avoid.

### 5.7 OpenBSD: EV_DISPATCH

All file descriptors registered with the internal poller use `EV_DISPATCH`
without `EV_CLEAR`.

- Level-triggered with one-shot disable: filter fires once and is automatically
  disabled until explicitly re-enabled with `EV_ENABLE`
- On re-enable, if the condition is still true, the filter fires again
  automatically - no post-re-arm readiness check is needed
- `EVFILT_READ` and `EVFILT_WRITE` are independent filters; read and write
  waiters on the same fd use separate kevent registrations

**EV_EOF:** When an `EVFILT_READ` event has `EV_EOF` set but its `data` count
is positive, the waiting fiber is woken successfully to drain those buffered
bytes. With no buffered bytes, and for write-filter EOF events, the waiter is
woken with an EOF/error result.

### 5.8 Cancellation of I/O Waiter

**Cancel one direction:** `epoll_ctl MOD` to the remaining direction only. No
post-cancel readiness check needed.

**Cancel both directions:** `epoll_ctl DEL` (Linux) or delete both filters
(OpenBSD). fd is fully free for re-use after both directions are cleared.

**Cross-worker cancel:** Enqueued to the target worker's inject queue; returns
after enqueueing. The fd is not safe to close or re-register until the cancel
has been processed (i.e., confirmed via scope completion or other
synchronisation). The same-worker fd-free-after-return guarantee does not
apply cross-worker.

---

## 6. Layer 4 - Multi-Worker Runtime

### 6.1 Per-Worker Pinned Model

Each worker thread owns its scheduler, I/O poller, timer heap, and all fibers
assigned to it. No fiber ever migrates between workers. Cross-worker
interaction happens only through:
- **Inject queues:** bounded MPSC queues, one per worker
- **Wakeup fds:** one per worker (eventfd on Linux, pipe on OpenBSD)

This model eliminates migration races, requires no per-fiber locking, and
matches the natural load-balancing point for connection-oriented workloads -
connection acceptance time. Work stealing is not implemented and not planned.

**CPU affinity:** On Linux, workers can be pinned to cores via
`pthread_setaffinity_np`. OpenBSD does not provide CPU affinity APIs; the
pinned model is implemented but advisory there.

### 6.2 Worker Registration and Shutdown

Workers must be registered with the runtime before host-thread
`strand_fiber_spawn` is called. Calling `strand_fiber_spawn` from the host
thread before any workers are registered returns an error.

**Shutdown sequence:**
1. Call `strand_scheduler_stop` on all workers
2. Wait for each `strand_scheduler_run` call to return on each worker thread
3. Tear down the runtime only after all workers have returned

Once shutdown begins - after the first `strand_scheduler_stop` call - both
host-thread and running-fiber `strand_fiber_spawn` return an error. The same
atomic shutdown flag is checked on all spawn paths. Stopped workers are
excluded from round-robin selection.

Spawning during shutdown would create fibers that cannot complete cleanly as
the runtime tears down. Returning an error on spawn is the correct behaviour.

### 6.3 Inject Queue

Each worker has one bounded inject queue. Items enqueued by other threads
(cross-worker cancel, offload completion, host-thread spawn, scope
signals) are drained in Step 1 of `strand_scheduler_advance`.

**Capacity:** Configurable at runtime initialisation. Must be sized
appropriately for expected peak cross-worker operation rate.

**Overflow behaviour:** When an enqueue attempt finds the queue full, the
calling thread blocks briefly using exponential backoff until space is
available. Silent drops are never permitted - every injected item must
eventually be processed. Full blocking is acceptable because inject queue
overflow indicates extreme system load; a brief wait is preferable to
correctness failure.

**Memory ordering:** Inject enqueue uses release semantics; inject dequeue
uses acquire semantics. This establishes the happens-before chain required for
offload result visibility (see §6.6).

### 6.3.1 Cross-Worker Wakeup Latency - Platform Characteristics

The cross-worker cancel→resume path (inject queue enqueue + wakeup fd write +
target worker wakes from poll + inject drain + fiber dispatch) has different
latency characteristics on the two target platforms:

- **OpenBSD (kqueue + pipe):** ~3 µs cross-worker wakeup. The OpenBSD
  scheduler wakes a thread blocked in `kevent` with low latency; pipe write
  is a simple kernel operation.

- **Linux (epoll + eventfd):** ~7 µs on multi-core machines. The additional
  latency comes from the CFS scheduler's wakeup path: after the eventfd write
  makes the target worker runnable, CFS must schedule it onto a core. On
  machines with many cores this includes IPI delivery and scheduler queue
  insertion. This is a known structural characteristic of the Linux CFS
  scheduler, not a defect in libstrand.

**Practical impact:** Cross-worker wakeup latency is only relevant when
cross-worker cancellation is on the hot path at very high frequency. For the
target workload - connection-oriented protocol daemons - cross-worker cancel
is an exceptional event (connection teardown, timeout-triggered cleanup), not
a per-request operation. The ~7 µs latency is not a bottleneck in practice.

**Potential future optimization:** A spin-before-block strategy - spinning on
the inject queue for a configurable duration before entering `epoll_wait` -
could reduce Linux cross-worker latency to ~100–300 ns (inject queue pop +
dispatch latency) at the cost of CPU burn during idle periods. This
optimization is not implemented in v0.1.0. If user feedback indicates
cross-worker cancellation frequency is a real production bottleneck, this can
be added as an opt-in configuration parameter (`spin_before_block_ns`) in a
future version without architectural changes.

### 6.4 fiber_spawn Worker Selection

**From a running fiber:** New fiber assigned to the same worker as the
spawning fiber. Returns an error if shutdown has begun.

**From the host thread:** Round-robin across registered workers, with explicit
worker override. Stopped workers are excluded from round-robin. Returns an
error if no workers are registered or if shutdown has begun. The worker list
and round-robin counter are protected by a lightweight spinlock accessed only
at spawn time from the host thread.

### 6.5 Blocking Syscall Offload

Some syscalls block the calling thread regardless of fd state:
`getaddrinfo`, regular file I/O (`read`/`write` on regular files), `fsync`,
`waitpid`, and blocking library calls.

Calling these from a fiber stalls the entire worker thread. The offload pool
addresses this:

```c
int strand_fiber_offload(blocking_fn_t fn, void *arg, void *result_slot);
```

The fiber transitions to `FIBER_PARKED_OFFLOAD`. A plain OS thread from the
pool runs `fn(arg)`. When complete, the result is injected back to the fiber's
home worker and the fiber transitions to `FIBER_RUNNABLE`.

**Offload pool is optional.** If not initialised, `strand_fiber_offload`
returns `STRAND_NO_OFFLOAD_POOL`.

**Pool full behaviour:** Returns `EAGAIN` immediately. The caller must yield
before retrying. Calling `strand_fiber_offload` in a tight retry loop without
yielding is a liveness violation - it spins the worker and starves other
fibers. The correct pattern:

```c
while ((rc = strand_fiber_offload(blocking_fn, &arg, &result)) == EAGAIN) {
    strand_fiber_yield();   /* must yield before retry */
}
```

This pattern must be shown explicitly in all documentation. Callers that
ignore it violate the cooperative scheduling contract.

**Offload threads** must not call any fiber or scheduler APIs.

### 6.6 Offload Work Item Lifecycle

Each `strand_fiber_offload` call allocates a runtime-owned work item:

```
Contents:
  - function pointer
  - arg pointer (not copied - points to caller's data)
  - result_slot pointer
  - refcount starting at 2 (one for fiber, one for offload thread)
  - atomic completion state: PENDING | RESULT_CLAIMED | CANCELLED
```

**Atomic exclusive claim eliminates the result_slot write race:**

```
offload thread runs fn(arg):
  -> CAS(state, PENDING -> RESULT_CLAIMED)
  -> if CAS succeeds:
       write result_slot
       inject completion to fiber's home worker
       fiber transitions PARKED_OFFLOAD -> RUNNABLE normally
  -> if CAS fails (state already CANCELLED):
       skip result_slot write entirely
  -> decrement refcount; if zero: free work item

strand_fiber_cancel on FIBER_PARKED_OFFLOAD:
  -> CAS(state, PENDING -> CANCELLED)
  -> if CAS succeeds:
       fiber transitions to RUNNABLE with cancellation result immediately
       decrement refcount; if zero: free work item
  -> if CAS fails (state already RESULT_CLAIMED):
       offload thread will inject result normally
       fiber will resume with the offload result - cancellation does not override
       DO NOT decrement refcount here - fiber-side reference is still live

fiber resumes after normal offload completion:
  -> reads result from result_slot
  -> runtime decrements fiber-side refcount at resume point - exactly once
  -> if refcount reaches zero: free work item
```

**Refcount ownership rule:** The fiber-side reference is released exactly
once - at fiber resume after normal offload completion, or when
`strand_fiber_cancel`'s CANCELLED CAS succeeds. When the CAS fails because
RESULT_CLAIMED already won, `strand_fiber_cancel` must not touch the refcount.

**Arg lifetime:** Programmer's responsibility. Must remain valid until `fn`
completes even if the waiting fiber is cancelled. Heap-allocate arg data when
cancellation is possible and the arg points to fiber-local or stack memory.

**Result slot:** Safe under cancellation - the atomic CAS prevents any write
after CANCELLED.

### 6.7 Offload Memory Ordering

The result_slot write must be visible to the fiber before it reads the
injected result. The required ordering chain:

1. Offload thread writes `result_slot` (relaxed or stronger)
2. Offload thread performs `CAS(PENDING -> RESULT_CLAIMED)` with **release**
   semantics - publishes the result_slot write
3. Offload thread enqueues completion to inject queue with **release** semantics
4. Fiber's worker drains inject queue with **acquire** semantics - establishes
   happens-before with the enqueue
5. Fiber reads `result_slot` - acquire from step 4 ensures visibility

Using seq_cst for the CAS and inject operations is correct and simpler. The
minimum required is release on write side and acquire on read side.

---

## 7. Layer 5 - Coordination Primitives

*The first implementation phase delivers Layers 1–4 plus structured
concurrency scopes (§7.1–§7.7) and fiber-local storage (§7.8). Channels,
mutexes, and additional timer APIs are deferred to a subsequent
specification and implementation phase.*

### 7.1 Structured Concurrency Scopes - Overview

A scope is a lifetime container for a set of fibers. The invariant: the
code that opens a scope cannot proceed past scope exit until all fibers
spawned under that scope have completed. This eliminates the use-after-free
class of bug that arises when fibers outlive the state they reference.

**Scope operations:**
- `strand_scope_open(scope)` - initialise scope, set OWNER_CALLER
- `strand_scope_spawn(scope, fn, arg)` - spawn a child fiber under this scope.
  Child fibers spawned through `strand_scope_spawn` use
  `strand_scope_fiber_fn_t` (returns `int`). Return 0 for success, non-zero
  for failure. The runtime trampoline captures the return value and calls
  `scope_child_finish` on fiber exit. Fibers spawned via `strand_fiber_spawn`
  (not scope-tracked) continue to use `strand_fiber_fn_t` (returns `void`).
- `strand_scope_wait(scope)` - block until SCOPE_COMPLETED; terminal
- `strand_scope_wait_timeout(scope, deadline)` - block until completed or
  deadline; non-terminal if timeout fires
- `strand_scope_cancel(scope)` - initiate cancellation; non-terminal
- `strand_scope_abandon(scope)` - transfer ownership to runtime; terminal for
  the caller

**`strand_scope_open` from a host thread is forbidden.** The host thread has
no fiber context and cannot call `strand_scope_wait`.

### 7.2 Scope Control Block

```
Fields:
  lifecycle_state    - atomic enum: ACTIVE | CANCELLING | DRAINING | COMPLETED
  owner_flag         - atomic enum: OWNER_CALLER | OWNER_RUNTIME
  live_child_count   - atomic integer; decremented when a child fiber finishes
  walk_ref_count     - atomic integer; incremented when a cancellation walk
                       begins, decremented when it ends; control block not freed
                       until this is zero AND lifecycle is COMPLETED
  first_error        - atomic, CAS-set once by the first failing child
  spawn_order_list   - intrusive linked list of strand_fiber_handle_t, in spawn
                       order; used for reverse-order cancellation walk
  parent_fiber       - handle of fiber to notify at SCOPE_COMPLETED;
                       set to null by strand_scope_abandon
  cancellation_flag  - set when scope enters CANCELLING state
```

### 7.3 Scope Lifecycle State Machine

**Two orthogonal dimensions:**

**Lifecycle state** - what the scope is doing:
```
SCOPE_ACTIVE      - open; children running; no failure; no cancellation
SCOPE_CANCELLING  - cancellation initiated; children being cancelled cooperatively
SCOPE_DRAINING    - all children have been sent cancellation; waiting for last completions
SCOPE_COMPLETED   - all children finished
```

**Owner flag** - who is responsible for the control block:
```
OWNER_CALLER   - programmer's code owns the control block
OWNER_RUNTIME  - strand_scope_abandon was called; runtime owns cleanup
```

These dimensions are orthogonal. Any lifecycle state can have either owner
flag. Valid combinations include `SCOPE_CANCELLING + OWNER_RUNTIME` (scope
abandoned while a child was failing) and `SCOPE_DRAINING + OWNER_RUNTIME`
(abandoned scope waiting for last children).

**Lifecycle transitions:**

```
SCOPE_ACTIVE
  -> SCOPE_CANCELLING   first child fails (first-error CAS succeeds),
                        or strand_scope_cancel called
  -> SCOPE_COMPLETED    last child finishes without failure or cancellation
                        (live_child_count reaches zero while state is ACTIVE)

SCOPE_CANCELLING
  -> SCOPE_DRAINING     cancellation walk through spawn-order list completes
                        (all children have been sent cancellation signals)
  -> SCOPE_COMPLETED    last child finishes before walk completes
                        (live_child_count reaches zero while CANCELLING)
                        Walk continues but remaining stale handles return
                        STRAND_HANDLE_STALE - walk is idempotent and safe.

SCOPE_DRAINING
  -> SCOPE_COMPLETED    last child finishes
                        (live_child_count reaches zero while DRAINING)

SCOPE_COMPLETED
  -> freed              if OWNER_RUNTIME: runtime frees control block
                        if OWNER_CALLER: caller may safely destroy it
```

**Owner flag transition:**
```
OWNER_CALLER -> OWNER_RUNTIME   strand_scope_abandon called - atomic store
```

**Walk reference rule:** When a cancellation walk begins, the runtime
increments `walk_ref_count`. The control block is not freed until both:
(1) `live_child_count` is zero AND lifecycle is `SCOPE_COMPLETED`, AND
(2) `walk_ref_count` is zero.

When the walk completes, it decrements `walk_ref_count`. If both conditions
are then satisfied, the runtime frees the block. This prevents use-after-free
if the last child completes and triggers `OWNER_RUNTIME` free while the walk
is still iterating the spawn-order list.

### 7.4 Scope Exit Variants

**`strand_scope_wait(scope)` - terminal:**
Blocks until `SCOPE_COMPLETED`. No follow-up required or permitted. When it
returns, the scope is in `SCOPE_COMPLETED` and `OWNER_CALLER`. The caller may
then destroy it.

**`strand_scope_wait_timeout(scope, deadline)` - non-terminal if timeout fires:**
Blocks until `SCOPE_COMPLETED` or `deadline` passes. If the scope completed:
equivalent to `strand_scope_wait` - terminal. If the timeout fired: scope
remains in whatever lifecycle state it was in at timeout. The caller must
follow with `strand_scope_wait` or `strand_scope_abandon`. The scope is not
affected by the timeout return - it continues in its current lifecycle state.

**`strand_scope_cancel(scope)` - non-terminal:**
Initiates cancellation: transitions scope to `SCOPE_CANCELLING` if currently
`SCOPE_ACTIVE`. Returns immediately. Requires follow-up with
`strand_scope_wait` or `strand_scope_abandon`.

**`strand_scope_abandon(scope)` - terminal for the caller:**
Sets `owner_flag` to `OWNER_RUNTIME` atomically. Sets `parent_fiber` to null.
The scope pointer is invalid for the caller after this call. Children continue
running. When `live_child_count` reaches zero and lifecycle reaches
`SCOPE_COMPLETED`, the runtime frees the control block.

**Allocation requirement for `strand_scope_abandon`:** The control block must
be heap-allocated. Stack-allocated scope control blocks are valid only when
`strand_scope_wait` is used exclusively and the scope is destroyed before the
stack unwinds. Passing a stack-allocated control block to
`strand_scope_abandon` is undefined behaviour. Debug builds assert that the
scope pointer is not within the current fiber's stack range.

### 7.5 Error Propagation

First error wins via atomic CAS on `first_error`. Subsequent child failures
are discarded - `first_error` is written exactly once. The parent receives one
error code at `strand_scope_wait`.

### 7.6 Sibling Cancellation

When the first child fails, the scope transitions to `SCOPE_CANCELLING` and
initiates a cancellation walk. The walk cancels children in reverse spawn
order. Cancellation is cooperative - `strand_fiber_cancel` is called on each
child handle, which transitions the child fiber to `FIBER_RUNNABLE` with a
cancellation result at its next park point. Fibers that never yield or park
cannot be cancelled. Handles validated via generation counter - stale handles
are no-ops.

**Structured concurrency guarantee (honest statement):** All cooperative
children spawned through this scope that reach cancellation points will
complete or observe cancellation. Non-cooperative children (fibers that spin
without yielding) can hang scope exit indefinitely. This weaker guarantee must
be stated accurately in all user-facing documentation.

### 7.7 Detached Fibers

Explicit opt-in via `strand_fiber_spawn_detached`. Detached fibers are not
tracked by any scope. Errors are not propagated. The lifetime of a detached
fiber is not bounded by any enclosing scope. Use of detached fibers is
**strongly discouraged** - they make correctness reasoning significantly
harder and preclude structured cleanup.

### 7.8 Fiber-Local Storage

Single `void*` slot per fiber.

```c
void  strand_fiber_local_set(void *ptr, strand_destructor_t dtor);
void *strand_fiber_local_get(void);
```

The destructor (if non-null) is called with the stored pointer when the fiber
completes. Useful for per-fiber allocations or cleanup that must run
regardless of how the fiber exits.

---

## 8. Cancellation

### 8.1 `strand_fiber_cancel(handle)`

Validates the handle (null check, generation check). Then:

| Fiber state | Action |
|---|---|
| `FIBER_PARKED_IO_READ` or `IO_WRITE` | Remove or update epoll/kqueue registration. Transition to `FIBER_RUNNABLE` with cancellation result. |
| `FIBER_PARKED_TIMER` | Remove from timer heap. Transition to `FIBER_RUNNABLE` with cancellation result. |
| `FIBER_PARKED_OFFLOAD` | CAS on work item state. If CANCELLED wins: transition to `FIBER_RUNNABLE` with cancellation result. If RESULT_CLAIMED already won: no-op - fiber will resume normally with the offload result. |
| `FIBER_PARKED_CHANNEL` | Remove from channel wait queue. Transition to `FIBER_RUNNABLE` with cancellation result. |
| `FIBER_RUNNABLE` or `FIBER_RUNNING` | Set `FIBER_CANCELLATION_PENDING` flag. |
| `FIBER_FINISHED` | No-op. |
| Stale handle | No-op. Returns `STRAND_HANDLE_STALE`. |

**Same-worker call:** Operation applied immediately. For I/O waiters, the
registration is removed and the fd is free for the cancelled direction when
the call returns.

**Cross-worker call:** Enqueued to the target worker's inject queue. Returns
after enqueueing. Post-return guarantees (fd freedom, state visibility) hold
only for same-worker calls. Cross-worker fd freedom requires confirmation via
scope completion or other explicit synchronisation.

---

## 9. Bootstrap and Shutdown

### 9.1 Runtime Lifecycle

```
strand_runtime_init(config)
  -> allocate runtime context
  -> no workers started yet

strand_worker_start(runtime, worker_config)
  -> create OS thread
  -> thread calls strand_scheduler_run internally
  -> worker registered and available for fiber_spawn

[program runs: fiber_spawn, scope operations, I/O, offload]

strand_scheduler_stop(worker_sched)   [call for each worker]
  -> sets atomic stop flag
  -> writes to wakeup fd/pipe
  -> both steps required

[wait for each strand_scheduler_run to return on each worker thread]

strand_runtime_destroy(runtime)
  -> tear down after all workers have returned
```

### 9.2 Main Thread Role

The main thread is responsible for initialisation, shutdown, and scheduler
management. It does not protect against memory corruption or undefined
behaviour inside fibers. It is a stable execution context, not a supervisor.

---

## 10. Platform Specifics

### 10.1 OpenBSD Differences

| Feature | Linux | OpenBSD |
|---|---|---|
| I/O poll | epoll | kqueue |
| Wakeup fd | eventfd | pipe2 with O_CLOEXEC \| O_NONBLOCK |
| Trigger mode | EPOLLET \| EPOLLONESHOT | EV_DISPATCH |
| Post-re-arm check | Required | Not required |
| CPU affinity | pthread_setaffinity_np | Not available |
| String functions | strlcpy/strlcat not in glibc | In libc |

### 10.2 VM Area Limits (Linux)

Linux `vm.max_map_count` defaults to 65530 VMAs. Each mmap-allocated fiber
stack consumes one VMA. The stack cache bounds VMA count to the peak watermark
by caching stacks rather than unmapping them after each fiber completes. The
cache must have a configurable hard cap to prevent unbounded VMA accumulation.

Stack cache hard cap, reclamation policy, and default stack size: **TBD -
see §13.**

### 10.3 fork() and exec()

| Scenario | Behaviour |
|---|---|
| `fork()` before `strand_runtime_init` | Fully supported |
| `fork()` then immediate `exec()` | Supported - all internal fds have FD_CLOEXEC |
| `fork()` after init without `exec()` | **Undefined behaviour** |
| Subprocess spawning from fibers | Via `strand_fiber_offload` for both `fork` and `waitpid` |

`pthread_atfork` is not registered. `fork()` after runtime init without
`exec()` leaves worker threads, epoll/kqueue instances, and pipe/eventfd
state in the child in an inconsistent state. This is explicitly unsupported
and must be documented clearly.

All internal file descriptors are created with `O_CLOEXEC` / `EFD_CLOEXEC`
/ `FD_CLOEXEC` to prevent leaking into child processes after `exec`.

### 10.4 Signal Handling

**Fiber APIs inside signal handlers:** Forbidden. Explicitly documented.

**Signal handlers on fiber stacks:** Must be async-signal-safe. The scheduler
may deliver a signal to a fiber's OS thread while that fiber is running. The
signal handler executes on the fiber's stack unless `sigaltstack` is used.

**Recommended:** Set up an alternate signal stack with `sigaltstack` to ensure
signal handlers never execute on a fiber stack.

**EINTR:** User code handles EINTR in the normal POSIX way, before calling
the fiber wait primitives.

---

## 11. Cooperative Scheduling - Honest Assessment

### 11.1 Real Costs

- Scheduler starvation from uncooperative fiber loops
- Poor tail-latency under uncooperative workloads
- Weak cancellation responsiveness if fibers do not reach park points
- No mechanism to force-stop a truly non-cooperative fiber

### 11.2 Mitigations

**Debug watchdog:** Debug builds include a configurable maximum fiber run time
(wall time between yields). Exceeding the threshold emits a warning. Not a
hard limit - does not preempt the fiber.

**Explicit yield contracts:** Every API that parks a fiber is also an implicit
yield point and a cancellation point. Code that loops without calling any
parking API must call `strand_fiber_yield()` periodically to cooperate with
the scheduler.

**Instrumentation hooks:** Production monitoring hooks for scheduler latency
and fiber run time are planned. Specifics deferred.

---

## 12. Thread Safety Matrix

| Operation | Running fiber | Host thread | Other worker thread | Signal handler | Offload thread |
|---|---|---|---|---|---|
| `strand_fiber_spawn` | Yes (same worker) | Yes (round-robin; error if no workers or shutdown) | No | No | No |
| `strand_fiber_spawn_detached` | Yes | Yes (round-robin; error if no workers or shutdown) | No | No | No |
| `strand_fiber_cancel` | Yes | Yes | Yes (enqueue-only †) | No | No |
| `strand_fiber_wait_readable` | Yes | No | No | No | No |
| `strand_fiber_wait_writable` | Yes | No | No | No | No |
| `strand_fiber_sleep_until` | Yes | No | No | No | No |
| `strand_fiber_offload` | Yes | No | No | No | No |
| `strand_fiber_yield` | Yes | No | No | No | No |
| `strand_fiber_local_get` | Yes | No | No | No | No |
| `strand_fiber_local_set` | Yes | No | No | No | No |
| `strand_scope_open` | Yes | No ‡ | No | No | No |
| `strand_scope_spawn` | Yes | No | No | No | No |
| `strand_scope_wait` | Yes | No | No | No | No |
| `strand_scope_wait_timeout` | Yes | No | No | No | No |
| `strand_scope_cancel` | Yes | Yes | Yes (enqueue-only †) | No | No |
| `strand_scope_abandon` | Yes | Yes | No | No | No |
| `strand_scheduler_advance` | No | Yes | No | No | No |
| `strand_scheduler_run` | No | Yes (one call per worker thread) | No | No | No |
| `strand_scheduler_stop` | No | Yes | Yes | No | No |
| `strand_scheduler_next_deadline` | No | Yes | No | No | No |
| `strand_scheduler_get_fd` | No | Yes | No | No | No |
| `strand_worker_get_scheduler` | No | Yes | No | No | No |
| `strand_fiber_self_scheduler` | Yes | Returns NULL | Returns NULL | No | No |

**† Enqueue-only:** Returns after enqueueing to the target worker's inject
queue, not after the operation is applied. Post-return guarantees (fd freedom,
state visibility) hold only for same-worker calls.

**‡ `strand_scope_open` from the host thread is forbidden.** The host thread
has no fiber context and cannot call `strand_scope_wait`.

**No fiber API may be called from a signal handler.**

**Offload threads must not call any fiber or scheduler API.**

---

## 13. Stack Cache

The scheduler maintains a per-worker stack cache to avoid the cost of
`mmap`/`munmap` on every fiber creation and teardown. When a fiber completes,
its stack is returned to the cache rather than unmapped. When a new fiber is
spawned, the cache is checked first before calling `mmap`.

### 13.1 Default Stack Size

**Default: 64 KB.** Per-spawn override is supported - the spawn API accepts
an optional `size_t stack_size` parameter where `0` means use the default.

64 KB is the right default for the target workload. Protocol handling logic,
I/O waiting, and distributed system coordination code does not have deep call
stacks. 8–16 KB is too aggressive - a single function using a modest local
buffer or a string formatting call can exhaust it. 256 KB–1 MB is safe but
wastes memory at scale: at 10,000 concurrent fibers, 256 KB per stack is
2.5 GB reserved before a byte of data is processed. 64 KB sits in the
practical sweet spot. The guard page catches overflows at any size.

Callers who know their fibers will use more - or can guarantee they will use
less - pass an explicit `stack_size` at spawn time.

### 13.2 Cache Hard Cap

**Default: 64 stacks per worker.** Configurable at runtime initialisation.

The cap is expressed as a fixed count per worker, not a memory budget. A
memory budget is harder to reason about when stack sizes vary per fiber. At
the default 64 KB stack size, 64 cached stacks is 4 MB per worker - modest
and predictable. A deployment running 4 workers retains at most 256 stacks
across the runtime, enough to absorb burst workloads without retaining memory
indefinitely.

If large stacks are used (e.g. 1 MB), 64 cached stacks is 64 MB per worker -
significant but bounded and explicit. The configurable cap allows reducing this
at initialisation.

The cap bounds VMA consumption on Linux - see §10.2.

### 13.3 Cache Overflow Policy

**When the cache is at its hard cap and a completed fiber's stack would be
returned to the cache: free the incoming stack immediately via `munmap`.**

The cache retains its current contents unchanged. The incoming stack is
released. This is the simplest correct behaviour - evicting an existing cached
stack in favour of the incoming one adds complexity without correctness benefit,
since there is no basis for preferring one cached stack over another.

### 13.4 Idle Reclamation

When a worker's run queue has been empty for **5 seconds** and the cache holds
more than **8 stacks**, the worker shrinks the cache to 8 stacks by freeing
the excess in LIFO order (most recently cached first).

- The 5-second idle threshold is long enough to absorb burst patterns without
  reclaiming during normal load variation between requests.
- The idle floor of 8 stacks keeps warm capacity available for quick
  resumption after an idle period without retaining the full burst allocation.
- LIFO order frees the most recently cached stacks first - these are coldest
  in terms of access recency and least likely to be reused soon.

Both the idle timeout and the idle floor are configurable at runtime
initialisation.

### 13.5 Allocation Failure

**`strand_fiber_spawn` returns an error code when the cache is empty and
`mmap` fails.**

This is consistent with the library's explicit error handling philosophy
throughout. The caller decides whether to retry, reduce concurrency, or
propagate the failure upward. OOM during fiber spawn is not categorically
different from any other resource exhaustion - a well-written server should
handle it gracefully rather than crash.

**Exception:** If the library cannot allocate its own internal control
structures at initialisation time, that is a fatal condition and `abort` is
appropriate. Internal structures are not optional and cannot be partially
initialised. This applies only to `strand_runtime_init` - not to
`strand_fiber_spawn`.

### 13.6 Summary

| Parameter | Default | Configurable |
|---|---|---|
| Default stack size | 64 KB | Yes - per-spawn override via `stack_size` parameter |
| Cache hard cap | 64 stacks per worker | Yes - at runtime init |
| Cache overflow policy | Free incoming stack immediately | No |
| Idle reclamation trigger | Run queue empty for 5 s, cache above 8 stacks | Yes - timeout and floor at init |
| Idle reclamation target | Shrink to 8 stacks, LIFO order | Yes - floor at init |
| Spawn allocation failure | Return error code | N/A |
| Init allocation failure | Abort | N/A |

---

## 14. Open Items

Items explicitly deferred from the architecture phase that affect
implementation. Each must be resolved before the relevant layer is
implemented.

| Item | Blocks | Notes |
|---|---|---|
| Full API naming | All layers | `strand_fiber_spawn` confirmed; complete surface deferred |
| Layer 5 full scope (channels, mutexes, additional timer APIs) | Layer 5 phase 2 | Scopes and fiber-local storage in phase 1; remainder deferred |
| Repository structure | Phase 1 | Covered in REPOSITORY_STRUCTURE.md |
| License choice | Pre-release | ISC preferred per project philosophy; not yet confirmed |
| FOSS strategy, community | Pre-release | Not yet discussed |

**Confirmed out-of-scope decisions (not deferred - permanently excluded):**

| Decision | Rationale |
|---|---|
| io_uring | Linux-only; requires kernel 5.1+. Would permanently exclude OpenBSD. Not a target for any phase. |

---

**See Also**: PROJECT.md, TECH_STACK.md, CODING_STANDARDS.md, DEVELOPMENT.md, TESTING.md
