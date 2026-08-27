# Repository Structure

## 1. Top-Level Layout

```
libstrand/
├── Makefile                    # Build orchestration: dev, release, test, bench, lint, install
├── .clang-format               # Code formatting rules (KNF-based)
├── .clang-tidy                 # Static analysis configuration
├── README.md                   # Embedder-facing introduction and quickstart
├── PROJECT.md                  # Overview, goals, scope, design philosophy
├── ARCHITECTURE.md             # Full technical architecture - layers, state machines,
│                               #   platform specifics, data structures, decision rationale
├── TECH_STACK.md               # Build system, compiler flags, assembly conventions, tools
├── CODING_STANDARDS.md         # C style, assembly conventions, atomics, safety patterns
├── REPOSITORY_STRUCTURE.md     # This file
├── DEVELOPMENT.md              # Phased build plan, milestones, task breakdown
├── TESTING.md                  # Test strategy, per-layer test catalogue, CI approach
├── include/                    # Public header
├── src/                        # Library source files (C and assembly)
├── tests/                      # Unit and integration tests
├── bench/                      # Performance benchmarks
└── tools/                      # Developer tools
```

The build output is `libstrand.a`. No binary is installed. `make install`
installs exactly two files: `libstrand.a` into `$(LIBDIR)` and
`include/strand.h` into `$(INCLUDEDIR)`.

---

## 2. include/ - Public Header

```
include/
│
└── strand.h            # The only header an embedder includes.
                        # Self-contained: pulls in only <stddef.h> and <stdint.h>.
                        # Declares all public types, constants, and functions.
                        # Does NOT expose any internal types, struct layouts, or
                        #   implementation details.
                        # See ARCHITECTURE.md for full API documentation.
                        # See README.md for a quickstart integration example.
```

`strand.h` is the entire public surface of the library. An embedder adds
`-I/usr/local/include` (or wherever installed) and writes
`#include <strand.h>`. Nothing else is needed.

---

## 3. src/ - Library Source Files

Library source is organised by layer. Each layer is a set of `.c` files with
corresponding internal `.h` files. Assembly lives in `src/arch/`. The public
API (`include/strand.h`) is the only header embedders ever include - internal
headers are never installed and never included by callers.

```
src/
│
│   - Internal shared definitions -
│
├── strand_internal.h   # Internal shared types, forward declarations,
│                       #   and compile-time assertions.
│                       # Complete definitions of all internal structs:
│                       #   strand_fiber_t, strand_scheduler_t,
│                       #   strand_scope_t, strand_worker_t,
│                       #   strand_runtime_t, strand_offload_item_t,
│                       #   strand_inject_queue_t, strand_timer_heap_t.
│                       # All _Static_assert size and offset checks.
│                       #   Critically: sizeof(strand_context_t) ==
│                       #   STRAND_CONTEXT_SIZE and offsetof(strand_fiber_t,
│                       #   context) == 0 - assembly stubs depend on these.
│                       # STRAND_DEBUG_ASSERT macro definition.
│                       # NOT included by embedders - internal only.
│                       # See CODING_STANDARDS.md §1.3 for assertion rules.
│
│   - Layer 1: Execution Contexts -
│
├── arch/
│   ├── x86_64/
│   │   └── strand_context.S    # x86_64 context save and restore.
│   │                           # Saves and restores: rbx, rbp, r12–r15, rsp.
│   │                           # XMM registers NOT saved - all caller-saved
│   │                           #   under SysV AMD64 ABI. See ARCHITECTURE.md §3.2.
│   │                           # Complete CFI annotations throughout.
│   │                           # Exports: strand_context_swap(old, new).
│   │                           # See ARCHITECTURE.md §3.6 for CFI requirements.
│   │                           # See CODING_STANDARDS.md §2.2 for assembly rules.
│   │
│   └── arm64/
│       └── strand_context.S    # AArch64 context save and restore.
│                               # Saves and restores: x19–x29, x30 (lr), sp.
│                               # FP/SIMD: d8–d15 (lower 64 bits of v8–v15).
│                               # FPCR and FPSR saved/restored in C wrapper.
│                               # See ARCHITECTURE.md §3.3.
│
├── strand_context.c    # C wrapper around the assembly context swap.
│                       # Saves and restores errno before and after every switch.
│                       # Saves and restores MXCSR (x86_64) or FPCR/FPSR (AArch64)
│                       #   via inline intrinsics or platform headers.
│                       # Calls sanitizer hooks at every switch:
│                       #   __sanitizer_start_switch_fiber (before),
│                       #   __sanitizer_finish_switch_fiber (after),
│                       #   __tsan_switch_to_fiber (TSan builds).
│                       # Valgrind stack registration/deregistration macros called
│                       #   at stack alloc and dealloc - not in this file but
│                       #   documented here as the Layer 1 boundary.
│                       # strand_context_init(): fabricate initial context for a
│                       #   new fiber stack so that strand_context_swap to it
│                       #   begins execution at the fiber entry function.
│                       # See ARCHITECTURE.md §3.4, §3.7.
│
├── strand_context.h    # Layer 1 internal interface.
│                       # strand_context_t definition - register save area.
│                       # strand_context_swap(), strand_context_init() declarations.
│                       # STRAND_CONTEXT_SIZE constant (used in _Static_assert).
│
│   - Layer 2: Fiber Scheduler -
│
├── strand_fiber.c      # Fiber descriptor lifecycle and stack cache.
│                       # Fiber descriptor allocation and initialisation:
│                       #   fiber_alloc() - from dead pool if available, else malloc.
│                       #   fiber_free() - return descriptor to dead pool.
│                       # Generation counter increment on descriptor reuse.
│                       # Stack allocation: stack_alloc() via mmap + mprotect guard.
│                       # Stack cache: per-scheduler cache, cap 64 stacks (default),
│                       #   idle reclamation after 5s, floor 8 stacks.
│                       #   See ARCHITECTURE.md §13.
│                       # fiber_local_set(), fiber_local_get(): single void* slot
│                       #   with optional destructor called on fiber completion.
│                       # strand_fiber_yield() implementation: transition RUNNING
│                       #   -> RUNNABLE, append to run queue tail, context switch.
│                       # strand_fiber_cancel() implementation: all parked state
│                       #   transitions, CAS for PARKED_OFFLOAD, enqueue for
│                       #   cross-worker cancel. See ARCHITECTURE.md §8.1.
│                       # strand_fiber_spawn() and strand_fiber_spawn_detached():
│                       #   worker selection, descriptor init, stack fabrication,
│                       #   inject to target worker. See ARCHITECTURE.md §6.4.
│                       # Public API: strand_fiber_spawn, strand_fiber_spawn_detached,
│                       #   strand_fiber_cancel, strand_fiber_yield,
│                       #   strand_fiber_local_set, strand_fiber_local_get.
│
├── strand_fiber.h      # Fiber descriptor and run queue internal interface.
│                       # fiber_state_t enum (FIBER_NEW through FIBER_FINISHED).
│                       # strand_fiber_handle_t struct (ptr + generation).
│                       # fiber_alloc(), fiber_free(), stack_alloc(), stack_free().
│                       # fiber_handle_validate() inline - null check + generation.
│                       # run_queue_push(), run_queue_pop(), run_queue_len().
│
├── strand_sched.c      # Fiber scheduler: Layer 2 operating modes.
│                       # strand_scheduler_advance(): five-step nonblocking pass.
│                       #   Step 1: inject queue drain - move to run queue FIFO.
│                       #   Step 2: timer heap - expire deadlines, move to run queue.
│                       #   Step 3: wakeup fd drain - read and discard control bytes.
│                       #   Step 4: I/O poll timeout=0 - move ready fibers to queue.
│                       #   Step 5: run up to budget fibers (default 64).
│                       #   Returns SCHED_PROGRESS or SCHED_IDLE + next deadline.
│                       # strand_scheduler_run(): blocking worker mode loop.
│                       #   Calls advance in a loop; blocks in epoll_wait/kevent
│                       #   when idle; returns only when stop flag is set.
│                       # strand_scheduler_stop(): atomic stop flag + wakeup fd write.
│                       #   Both steps required. Safe from any thread.
│                       # strand_scheduler_next_deadline(): next timer deadline ns.
│                       # strand_scheduler_get_fd(): scheduler fd for host loop.
│                       # Timer heap: binary min-heap, keyed on uint64_t deadline_ns.
│                       #   timer_heap_push(), timer_heap_pop(), timer_heap_peek().
│                       # strand_fiber_sleep_until(): park fiber on timer heap.
│                       # Public API: strand_scheduler_advance, strand_scheduler_run,
│                       #   strand_scheduler_stop, strand_scheduler_next_deadline,
│                       #   strand_scheduler_get_fd, strand_fiber_sleep_until.
│                       # See ARCHITECTURE.md §4.2.
│
├── strand_sched.h      # Scheduler internal interface.
│                       # strand_scheduler_t forward declaration (defined in
│                       #   strand_internal.h).
│                       # Timer heap types and operations.
│                       # scheduler_advance_step_*() internal function declarations.
│
│   - Layer 3: I/O Integration -
│
├── strand_poller.c     # I/O multiplexing: epoll (Linux) and kqueue (OpenBSD).
│                       # fd waiter table: open-addressed hash table.
│                       #   Per entry: fd key, read waiter pointer, write waiter
│                       #   pointer, current event mask, registration state
│                       #   (REGISTERED_ACTIVE / REGISTERED_DISABLED /
│                       #   NOT_REGISTERED), per-registration token,
│                       #   debug generation counter.
│                       # strand_fiber_wait_readable(), strand_fiber_wait_writable():
│                       #   register fd, park fiber, context switch to scheduler.
│                       # Linux: EPOLLET | EPOLLONESHOT on all registrations.
│                       #   Post-re-arm readiness check: zero-timeout epoll_wait
│                       #   after MOD; all returned events processed via normal
│                       #   delivery path. See ARCHITECTURE.md §5.5.
│                       # OpenBSD: EV_DISPATCH without EV_CLEAR.
│                       #   No post-re-arm check needed. See ARCHITECTURE.md §5.7.
│                       # EPOLLERR/EPOLLHUP (Linux): wake any waiter on affected fd
│                       #   with error result regardless of interest mask.
│                       # EV_EOF (OpenBSD): wake EVFILT_READ waiter with EOF result.
│                       # Cancellation of one direction: MOD to remaining direction.
│                       # Cancellation of both directions: DEL.
│                       # Platform-specific code guarded by STRAND_LINUX /
│                       #   STRAND_OPENBSD preprocessor macros.
│                       # Wakeup fd: eventfd (Linux) or pipe (OpenBSD).
│                       #   Created with FD_CLOEXEC / O_CLOEXEC | O_NONBLOCK.
│                       # Debug build: O_NONBLOCK assertion at fd registration.
│                       # Public API: strand_fiber_wait_readable,
│                       #   strand_fiber_wait_writable.
│                       # See ARCHITECTURE.md §5.
│
├── strand_poller.h     # Poller internal interface.
│                       # strand_poller_t forward declaration.
│                       # poller_create(), poller_destroy().
│                       # poller_arm(), poller_rearm(), poller_cancel().
│                       # poller_poll(): zero-timeout poll for scheduler Step 4.
│                       # poller_deliver_event(): process one returned event,
│                       #   wake affected waiters.
│                       # fd_waiter_table_t and related types.
│
│   - Layer 4: Multi-Worker Runtime -
│
├── strand_inject.c     # Inject queue: bounded MPSC queue, one per worker.
│                       # Fixed-capacity ring buffer allocated at scheduler init.
│                       # inject_queue_push(): fail fast with STRAND_EAGAIN
│                       #   when full or closed.
│                       #   Release memory ordering on enqueue.
│                       # inject_queue_pop(): acquire memory ordering on dequeue.
│                       #   Establishes happens-before with result_slot writes
│                       #   in the offload completion path.
│                       # inject_queue_drain(): move all queued items to the
│                       #   scheduler's run queue - called in advance Step 1.
│                       # See ARCHITECTURE.md §6.3, §6.7.
│
├── strand_inject.h     # Inject queue internal interface.
│                       # strand_inject_queue_t definition.
│                       # inject_queue_push(), inject_queue_pop(),
│                       #   inject_queue_drain(), inject_queue_len().
│
├── strand_runtime.c    # Multi-worker runtime: worker registry and lifecycle.
│                       # strand_runtime_init(): allocate runtime context,
│                       #   initialise worker list and round-robin counter.
│                       # strand_worker_start(): create OS thread, register worker,
│                       #   thread calls strand_scheduler_run internally.
│                       # strand_runtime_destroy(): called after all workers have
│                       #   returned from strand_scheduler_run.
│                       # Worker registry: array of worker pointers protected by
│                       #   lightweight spinlock, accessed only at spawn time from
│                       #   host thread. Round-robin counter for host-thread spawn.
│                       # Shutdown: atomic shutdown flag checked on all spawn paths.
│                       #   Stopped workers excluded from round-robin.
│                       # strand_fiber_spawn() host-thread path: round-robin worker
│                       #   selection, inject to target worker, error if no workers
│                       #   registered or if shutdown begun.
│                       # CPU affinity: pthread_setaffinity_np on Linux when
│                       #   worker_config.cpu_affinity is set. Not available on
│                       #   OpenBSD - documented difference.
│                       # Public API: strand_runtime_init, strand_runtime_destroy,
│                       #   strand_worker_start.
│                       # See ARCHITECTURE.md §6.1, §6.2.
│
├── strand_runtime.h    # Runtime and worker internal interface.
│                       # strand_runtime_t, strand_worker_t forward declarations.
│                       # runtime_get_worker_roundrobin(): select next worker.
│                       # runtime_is_shutdown(): check atomic shutdown flag.
│
├── strand_offload.c    # Blocking syscall offload pool.
│                       # Optional pool of plain OS threads. Not initialised by
│                       #   default - strand_offload_pool_init() required.
│                       # Work queue: mutex-protected list of pending work items.
│                       # strand_fiber_offload(): allocate work item (refcount=2,
│                       #   state=PENDING), park fiber, enqueue work item.
│                       # Offload thread: run blocking_fn(arg), CAS to
│                       #   RESULT_CLAIMED, write result_slot, inject completion
│                       #   to fiber's home worker.
│                       # CAS semantics: RESULT_CLAIMED and CANCELLED are exclusive.
│                       #   Only one party wins. See ARCHITECTURE.md §6.6.
│                       # Memory ordering: release on CAS and inject enqueue;
│                       #   acquire on inject dequeue. See ARCHITECTURE.md §6.7.
│                       # EAGAIN when pool is full - caller must yield before retry.
│                       # strand_offload_pool_init(), strand_offload_pool_destroy().
│                       # Public API: strand_fiber_offload, strand_offload_pool_init,
│                       #   strand_offload_pool_destroy.
│                       # See ARCHITECTURE.md §6.5, §6.6.
│
├── strand_offload.h    # Offload pool internal interface.
│                       # strand_offload_pool_t, strand_offload_item_t.
│                       # offload_item_alloc(), offload_item_release().
│                       # offload_pool_submit(), offload_pool_worker_thread().
│
│   - Layer 5: Coordination Primitives -
│
├── strand_scope.c      # Structured concurrency scopes.
│                       # strand_scope_open(): initialise control block,
│                       #   SCOPE_ACTIVE, OWNER_CALLER, live_child_count=0.
│                       # strand_scope_spawn(): spawn child fiber tracked by scope,
│                       #   append handle to spawn-order list, increment
│                       #   live_child_count.
│                       # strand_scope_wait(): block until SCOPE_COMPLETED.
│                       #   Terminal - no follow-up.
│                       # strand_scope_wait_timeout(): block until SCOPE_COMPLETED
│                       #   or deadline. Non-terminal if timeout fires.
│                       # strand_scope_cancel(): transition ACTIVE -> CANCELLING,
│                       #   initiate cancellation walk. Non-terminal.
│                       # strand_scope_abandon(): set OWNER_RUNTIME atomically,
│                       #   null parent_fiber. Terminal for caller.
│                       #   Debug check: scope pointer not in current fiber stack.
│                       # Lifecycle state machine: ACTIVE -> CANCELLING ->
│                       #   DRAINING -> COMPLETED. See ARCHITECTURE.md §7.3.
│                       # Cancellation walk: reverse spawn order, handle validation
│                       #   via generation counter, walk_ref_count held for
│                       #   duration. See ARCHITECTURE.md §7.3.
│                       # First error: CAS-set once by first failing child.
│                       # OWNER_RUNTIME cleanup: free control block when both
│                       #   live_child_count==0 and walk_ref_count==0.
│                       # Public API: strand_scope_open, strand_scope_spawn,
│                       #   strand_scope_wait, strand_scope_wait_timeout,
│                       #   strand_scope_cancel, strand_scope_abandon.
│                       # See ARCHITECTURE.md §7.
│
└── strand_scope.h      # Scope internal interface.
                        # strand_scope_t forward declaration (defined in
                        #   strand_internal.h).
                        # scope_child_finish(): called by scheduler when a
                        #   fiber tracked by a scope reaches FIBER_FINISHED.
                        #   Decrements live_child_count; triggers COMPLETED
                        #   transition if count reaches zero.
                        # scope_walk_cancel(): cancellation walk implementation.
```

---

## 4. tests/ - Unit and Integration Tests

```
tests/
│
├── test_harness.h      # Minimal test harness - no external framework.
│                       # RUN(name, fn) macro: calls fn(), tracks pass/fail.
│                       # Each test function returns 0 on success, non-zero on
│                       #   failure. Test binary exits 0 if all pass, 1 if any fail.
│                       # Prints PASS: name / FAIL: name per test.
│                       # Final line: "N/M tests passed".
│
├── run_tests.c         # Test binary entry point.
│                       # Calls RUN() for every test function across all suites.
│                       # Prints summary and exits with pass/fail code.
│                       # Links against libstrand.a - verifies exported symbols.
│
├── test_layer1.c       # Layer 1: execution context correctness.
│                       # Context switch preserves all callee-saved GPRs.
│                       # Context switch preserves errno across switch.
│                       # MXCSR (x86_64) preserved across switch.
│                       # FPCR/FPSR (AArch64) preserved across switch.
│                       # Stack pointer correct after switch.
│                       # 16-byte stack alignment maintained.
│                       # Guard page present: stack overflow generates SIGSEGV
│                       #   (tested in a subprocess to catch the signal safely).
│                       # Round-trip: switch to new context, execute function,
│                       #   switch back, verify registers unchanged.
│
├── test_layer2.c       # Layer 2: scheduler operating modes.
│                       # strand_scheduler_advance is nonblocking - returns
│                       #   without blocking even with no work.
│                       # strand_scheduler_run blocks until strand_scheduler_stop.
│                       # strand_scheduler_stop writes wakeup fd - blocked worker
│                       #   actually returns (not just sets flag).
│                       # Wakeup fd bytes drained as control events - not
│                       #   misinterpreted as fiber waiter events (Step 3).
│                       # FIFO run queue order - fibers execute in spawn order.
│                       # Budget limiting - advance runs at most N fibers per call.
│                       # Timer expiry - fiber_sleep_until wakes at correct time.
│                       # Inject drain before run queue - injected fibers run first.
│                       # Stack cache: stack reused on second fiber spawn.
│                       # Stack cache cap: at cap, incoming stack freed immediately.
│                       # Idle reclamation: cache shrinks to floor after idle period.
│                       # fiber_spawn returns STRAND_ERR_SHUTDOWN after stop called.
│
├── test_layer3.c       # Layer 3: I/O integration.
│                       # fiber_wait_readable parks until fd becomes readable.
│                       # fiber_wait_writable parks until fd becomes writable.
│                       # Edge-triggered one-shot: does not re-fire without re-arm.
│                       # Post-re-arm readiness check (Linux): if remaining direction
│                       #   already ready when MOD is called, waiter wakes
│                       #   immediately without waiting for next edge.
│                       # All events from zero-timeout poll processed - not only
│                       #   the rearmed fd. Test: two fds ready simultaneously;
│                       #   verify both waiters wake.
│                       # EV_DISPATCH (OpenBSD): filter fires on re-enable if
│                       #   condition still true - no post-re-arm check needed.
│                       # Simultaneous read/write on same fd: both waiters wake
│                       #   when both directions fire.
│                       # Cancel one direction: other direction remains registered.
│                       # Cancel both directions: fd fully free.
│                       # Same-worker cancel: readiness wins - fd ready + cancel
│                       #   in same advance call, fiber wakes with ready result.
│                       # Cross-worker cancel: cancel wins - injected cancel
│                       #   processed in Step 1 before I/O polled in Step 4.
│                       # EPOLLERR/EPOLLHUP (Linux): fiber woken with error result.
│                       # EV_EOF (OpenBSD): EVFILT_READ waiter woken with EOF result.
│                       # Blocking fd assertion: debug build asserts O_NONBLOCK.
│
├── test_layer4.c       # Layer 4: multi-worker runtime.
│                       # Inject queue: items delivered to target worker.
│                       # Cross-worker wakeup: target worker wakes from epoll_wait.
│                       # Round-robin worker selection across registered workers.
│                       # Explicit worker override in fiber_spawn.
│                       # Error return when no workers registered.
│                       # Error return when shutdown begun.
│                       # Stopped workers excluded from round-robin.
│                       # Offload pool: blocking_fn runs on separate thread.
│                       # Offload result delivered to originating fiber.
│                       # Offload EAGAIN when pool full.
│                       # Offload yield-before-retry: correct liveness pattern.
│                       # RESULT_CLAIMED wins before cancel: fiber resumes with
│                       #   offload result, not cancellation result.
│                       # CANCELLED wins: fiber resumes with cancellation result;
│                       #   result_slot not written.
│                       # Offload arg lifetime: arg remains valid until fn returns
│                       #   even after fiber cancellation.
│                       # Inject queue overflow: fail-fast STRAND_EAGAIN.
│
├── test_layer5.c       # Layer 5: scopes, handles, fiber-local storage.
│                       # Scope lifecycle: all four state transitions.
│                       # Owner flag orthogonal to lifecycle: all valid combinations
│                       #   (CANCELLING + OWNER_RUNTIME, DRAINING + OWNER_RUNTIME).
│                       # scope_open from host thread returns error.
│                       # scope_wait is terminal: no follow-up after return.
│                       # scope_wait_timeout: scope state after timeout is whatever
│                       #   state it was in when timeout fired - scope unaffected.
│                       # scope_abandon: OWNER_RUNTIME set; caller pointer invalid.
│                       # scope_abandon debug check: stack-allocated scope asserts.
│                       # Reverse-spawn-order cancellation verified.
│                       # First-error-wins: second child failure discarded.
│                       # CANCELLING -> COMPLETED shortcut: last child finishes
│                       #   before walk completes - scope completes; walk
│                       #   continues safely (stale handles are no-ops).
│                       # walk_ref_count: control block not freed while walk active.
│                       # Stale handle no-ops: all API calls on stale handles
│                       #   return STRAND_HANDLE_STALE without side effects.
│                       # Generation counter 64-bit: wrap not a concern.
│                       # Fiber-local storage: set and get across yield points.
│                       # Fiber-local destructor: called on fiber completion.
│
└── test_integration.c  # Integration tests spanning multiple layers.
                        # Echo server: accept, per-connection fiber, read/write loop.
                        # Producer-consumer: fiber pipeline with back-pressure.
                        # Scope with parallel fan-out: N child fibers, first error
                        #   propagates, siblings cancelled.
                        # Producer-consumer pipeline: fiber chain passing work
                        #   downstream; verify ordering, back-pressure, clean shutdown.
                        # scope_wait_timeout then scope_abandon: correct lifecycle.
                        # strand_fiber_offload with getaddrinfo: result delivered.
                        # strand_fiber_offload cancel race: RESULT_CLAIMED and
                        #   CANCELLED outcomes both exercised.
                        # strand_fiber_offload EAGAIN and retry: fill pool; verify
                        #   EAGAIN; yield; retry; verify eventual success.
                        # Subprocess via fork-then-exec through offload pool.
                        # Multi-worker accept distribution: connections spread across
                        #   workers via round-robin spawn from host thread.
                        # Simultaneous read/write on same fd with post-re-arm
                        #   readiness check: both waiters wake in same advance call.
                        # strand_scheduler_stop interrupts indefinitely blocked
                        #   worker - verify return within bounded time.
                        # Guest mode: scheduler_advance drives fibers from a host
                        #   event loop; verify correct timer and I/O integration.
```

---

## 5. bench/ - Performance Benchmarks

Benchmarks are built separately under release flags. They do not run as part
of `make test`. Each benchmark prints results with full hardware context
(CPU model, core count, clock speed) so numbers are not compared across
machines without this context.

```
bench/
│
├── bench_context_switch.c  # Context switch round-trip latency.
│                           # Measures: two-fiber ping-pong, ns per switch.
│                           # Also measures: switch with errno save/restore,
│                           #   switch with MXCSR/FPCR save/restore.
│
├── bench_fiber_spawn.c     # Fiber creation and teardown throughput.
│                           # Measures: fibers spawned per second.
│                           # Stack cache warm vs cold path.
│
├── bench_io_roundtrip.c    # I/O park and wake latency.
│                           # Measures: write to pipe, fiber wakes, ns elapsed.
│                           # Linux epoll and OpenBSD kqueue separately.
│
├── bench_scheduler.c       # Scheduler throughput.
│                           # Measures: fibers run per second, one worker.
│                           # Budget sensitivity: vary budget 8..256.
│
├── bench_multiworker.c     # Multi-worker scaling.
│                           # Measures: throughput vs worker count (1..N).
│                           # Per-worker pinned model - no migration overhead.
│
├── bench_offload.c         # Offload pool throughput and latency.
│                           # Measures: blocking_fn submissions per second.
│                           # Pool size sensitivity.
│
└── bench_cross_worker.c    # Cross-worker wakeup latency.
                            # Measures: inject to target worker, fiber wakes,
                            #   ns from inject to fiber resume.
```

---

## 6. tools/ - Developer Tools

```
tools/
│
└── strand_inspect.c    # Scheduler state inspector.
                        # Standalone diagnostic tool. Prints current state
                        #   of a scheduler object: run queue depth, timer
                        #   heap size, inject queue depth, fiber count by
                        #   state, wakeup fd type, stack cache depth.
                        # Intended for use in test harnesses and debug builds
                        #   where the scheduler object is accessible.
                        # Built from tools/strand_inspect.c only - links
                        #   against libstrand.a.
                        # Built by `make tools`.
```

---

## 7. Top-Level Files

### Makefile

Single top-level Makefile. No per-directory Makefiles. Platform and
architecture detected at build time.

```makefile
# Key targets
make              # same as make release - builds libstrand.a
make dev          # debug build with ASan/UBSan and STRAND_DEBUG=1
make release      # optimised build
make test         # build and run tests/run_tests (dev flags)
make test-tsan    # build and run tests with ThreadSanitizer (Clang only)
make valgrind     # run tests under Valgrind (Linux only)
make bench        # build and run all benchmarks (release flags)
make tools        # build tools/strand_inspect
make lint         # clang-tidy + cppcheck on src/
make format       # clang-format on src/*.c src/*.h include/strand.h
make clean        # remove build artifacts
make install      # install libstrand.a and include/strand.h to PREFIX
```

Platform detected via `$(shell uname)`. Architecture detected via
`$(shell uname -m)`. The correct assembly file from `src/arch/` is selected
at build time. The `make install` target installs exactly two files -
`libstrand.a` into `$(LIBDIR)` and `strand.h` into `$(INCLUDEDIR)` - and
nothing else.

`make dev` defines `-DSTRAND_DEBUG=1` which enables:
- O_NONBLOCK assertion at fd registration
- scope_abandon stack-range check
- Scheduler watchdog warnings for long-running fibers
- Debug generation counter logging

### .clang-format

KNF-based formatting rules:

```yaml
BasedOnStyle: LLVM
IndentWidth: 8
UseTab: ForIndentation
BreakBeforeBraces: Linux     # functions: own line; control: same line
ColumnLimit: 80
AllowShortFunctionsOnASingleLine: None
AllowShortIfStatementsOnASingleLine: Never
```

Assembly files (`.S`) are not processed by clang-format.

### .clang-tidy

Static analysis checks:

```yaml
Checks: >
  clang-analyzer-*,
  cert-*,
  bugprone-*,
  performance-*,
  portability-*,
  -cert-err33-c,
  -bugprone-easily-swappable-parameters
```

---

## 8. Layer-to-File Mapping

| Layer | Description | Primary files |
|---|---|---|
| Layer 1 | Execution contexts | `src/arch/x86_64/strand_context.S`, `src/arch/arm64/strand_context.S`, `src/strand_context.c` |
| Layer 2 | Fiber scheduler | `src/strand_fiber.c`, `src/strand_sched.c` |
| Layer 3 | I/O integration | `src/strand_poller.c` |
| Layer 4 | Multi-worker runtime | `src/strand_runtime.c`, `src/strand_inject.c`, `src/strand_offload.c` |
| Layer 5 | Coordination primitives | `src/strand_scope.c` |
| Shared | Internal definitions | `src/strand_internal.h` |
| Public | Embedder API | `include/strand.h` |

---

## 9. Naming Conventions Across Files

Function names are prefixed by their module. This makes `grep` and code
navigation unambiguous across the codebase.

| Module | File | Internal prefix | Example |
|---|---|---|---|
| Context switch | `src/strand_context.c` | `context_` | `context_swap()` |
| Fiber descriptor | `src/strand_fiber.c` | `fiber_` | `fiber_alloc()` |
| Run queue | `src/strand_fiber.c` | `run_queue_` | `run_queue_push()` |
| Stack cache | `src/strand_fiber.c` | `stack_` | `stack_alloc()` |
| Scheduler | `src/strand_sched.c` | `scheduler_` | `scheduler_advance_step1()` |
| Timer heap | `src/strand_sched.c` | `timer_heap_` | `timer_heap_push()` |
| Poller | `src/strand_poller.c` | `poller_` | `poller_arm()` |
| fd waiter table | `src/strand_poller.c` | `fd_table_` | `fd_table_lookup()` |
| Inject queue | `src/strand_inject.c` | `inject_queue_` | `inject_queue_push()` |
| Runtime | `src/strand_runtime.c` | `runtime_` | `runtime_get_worker_roundrobin()` |
| Offload pool | `src/strand_offload.c` | `offload_` | `offload_item_alloc()` |
| Scope | `src/strand_scope.c` | `scope_` | `scope_walk_cancel()` |

Public API functions (declared in `include/strand.h`) use the `strand_`
prefix throughout. No internal function name begins with `strand_` - that
prefix is reserved exclusively for the public API.

| Public namespace | Functions |
|---|---|
| `strand_fiber_` | `strand_fiber_spawn`, `strand_fiber_spawn_detached`, `strand_fiber_cancel`, `strand_fiber_yield`, `strand_fiber_sleep_until`, `strand_fiber_wait_readable`, `strand_fiber_wait_writable`, `strand_fiber_offload`, `strand_fiber_local_get`, `strand_fiber_local_set` |
| `strand_scope_` | `strand_scope_open`, `strand_scope_spawn`, `strand_scope_wait`, `strand_scope_wait_timeout`, `strand_scope_cancel`, `strand_scope_abandon` |
| `strand_scheduler_` | `strand_scheduler_advance`, `strand_scheduler_run`, `strand_scheduler_stop`, `strand_scheduler_next_deadline`, `strand_scheduler_get_fd` |
| `strand_runtime_` | `strand_runtime_init`, `strand_runtime_destroy` |
| `strand_worker_` | `strand_worker_start` |
| `strand_offload_pool_` | `strand_offload_pool_init`, `strand_offload_pool_destroy` |

---

**See Also**: PROJECT.md, ARCHITECTURE.md, TECH_STACK.md, CODING_STANDARDS.md,
DEVELOPMENT.md, TESTING.md
