# Testing Strategy

## 1. Overview

Testing operates at three levels:

| Level | What | When | Tools |
|---|---|---|---|
| Unit | Individual components in isolation, one layer at a time | During each phase, before moving on | Plain C test programs, Valgrind, ASan/UBSan, TSan |
| Integration | Cross-layer behaviour, realistic usage patterns | Phase 7, after all unit tests pass | Same test binary, integration test suite |
| Performance | Latency and throughput baselines | Phase 7, under release flags | Benchmark suite in `bench/` |

The rule is simple: **a phase is not done until its tests pass on both Linux
and OpenBSD, on both x86_64 and ARM64**. Do not accumulate untested code. Each
phase is small enough that bugs are easy to find when caught immediately.

**No external test framework.** Plain C programs with a minimal assertion
macro. The harness is described in TECH_STACK.md §6. Every test function
returns 0 on success and non-zero on failure. The test binary exits with code
0 if all tests pass and 1 if any fail.

---

## 2. Unit Testing

### 2.1 Test Harness

See TECH_STACK.md §6.1 for the `RUN(name, fn)` macro definition and the test
binary contract. The structure is:

```
tests/
├── test_harness.h       ← RUN macro, tests_run and tests_passed counters
├── run_tests.c          ← binary entry point - calls RUN() for every suite
├── test_layer1.c        ← Layer 1: context switch
├── test_layer2.c        ← Layer 2: scheduler
├── test_layer3.c        ← Layer 3: I/O integration
├── test_layer4.c        ← Layer 4: multi-worker runtime
├── test_layer5.c        ← Layer 5: scopes and coordination
└── test_integration.c  ← cross-layer integration tests
```

`make test` builds all test files against `libstrand.a` and runs the binary.
This verifies that exported symbols are correct - tests do not link directly
against source files.

### 2.2 Per-Layer Test Scope

Every layer has a dedicated test file. The scope of each file is strictly
bounded - a Layer 2 test does not test I/O, and a Layer 3 test does not test
scope lifecycle. This makes failures easy to localise.

| Test file | What it tests | What it does NOT test |
|---|---|---|
| `test_layer1.c` | Context switch, registers, errno, FP control, guard pages, sanitizer hooks | Scheduling, fibers, I/O |
| `test_layer2.c` | Scheduler modes, run queue, timer heap, stack cache, fiber-local storage | Real I/O, multi-worker |
| `test_layer3.c` | fd parking, epoll/kqueue, re-arm, cancellation, error events | Multi-worker, scopes |
| `test_layer4.c` | Inject queue, worker selection, offload pool, CAS outcomes | Scope lifecycle |
| `test_layer5.c` | Scope state machine, walk reference rule, error propagation | Network I/O |
| `test_integration.c` | End-to-end realistic scenarios across all layers | None - cross-layer |

### 2.3 Scheduler and Fiber Test Patterns

Unlike a zero-I/O library, testing libstrand requires actually running the
scheduler. Tests follow a consistent setup pattern:

```c
/*
 * Standard single-worker test setup.
 * Creates a scheduler, runs test fibers via strand_scheduler_advance,
 * tears down after assertions.
 */
static strand_scheduler_t *
make_test_scheduler(void)
{
    strand_sched_config_t cfg = {
        .budget      = 64,
        .inject_cap  = 256,
        .cache_cap   = 8,
        .idle_floor  = 2,
    };
    return strand_scheduler_create(&cfg);
}

/*
 * Run the scheduler for up to max_ticks advance calls.
 * Returns when run queue empties or tick limit hit.
 * Used by tests that need to drive fibers to completion.
 */
static void
run_until_idle(strand_scheduler_t *sched, int max_ticks)
{
    for (int i = 0; i < max_ticks; i++) {
        int rc = strand_scheduler_advance(sched);
        if (rc == SCHED_IDLE)
            break;
    }
}
```

Tests that need timer behaviour pass a mock clock rather than sleeping:

```c
/*
 * STRAND_TEST_CLOCK: when defined, strand_now_ns() reads a global
 * test_clock_ns instead of calling clock_gettime. Tests advance
 * time by writing to test_clock_ns directly.
 * No test may use sleep() or usleep() for timing.
 */
extern uint64_t strand_test_clock_ns;   /* writable by tests */
```

### 2.4 Pipe-Based I/O Test Patterns

Layer 3 tests need real file descriptors. Tests use `pipe2(O_NONBLOCK)` pairs
as controllable I/O sources:

```c
static void
make_test_pipe(int *rd, int *wr)
{
    int fds[2];
    pipe2(fds, O_NONBLOCK | O_CLOEXEC);
    *rd = fds[0];
    *wr = fds[1];
}

/*
 * Park a fiber on the read end of a pipe.
 * Write one byte from outside the scheduler to trigger readiness.
 * Run advance - fiber must wake.
 */
static int
test_wait_readable_wakes(void)
{
    strand_scheduler_t *sched = make_test_scheduler();
    int rd, wr;
    make_test_pipe(&rd, &wr);

    int flag = 0;
    strand_fiber_spawn(sched, fiber_wait_read_fn, &(test_args){rd, &flag}, 0, NULL);
    strand_scheduler_advance(sched);    /* fiber parks on rd */
    write(wr, "x", 1);                 /* trigger readiness */
    strand_scheduler_advance(sched);    /* fiber wakes, sets flag */

    assert(flag == 1);
    close(rd); close(wr);
    strand_scheduler_destroy(sched);
    return 1;
}
```

### 2.5 Cross-Worker Test Patterns

Layer 4 tests spin up real worker threads and offload pool threads.  Tests
use predicate-based wait helpers with generous timeouts rather than fixed-
duration `nanosleep` loops.  Fixed sleeps fail non-deterministically under
Valgrind (20-50× slowdown) and on slow CI runners.

**Core helpers** (defined at the top of `test_layer4.c`):

```c
typedef int (*wait_pred_fn)(void *);

/*
 * wait_until_pred -- poll a predicate with 1 ms sleep between checks.
 * If drive_sched is non-NULL, calls strand_scheduler_advance each
 * iteration (for tests that drive a scheduler from the main thread).
 * Returns 1 when the predicate becomes true, 0 on timeout.
 */
static int
wait_until_pred(wait_pred_fn pred, void *ctx, uint64_t timeout_ms,
    strand_scheduler_t *drive_sched);

/*
 * wait_atomic_at_least -- wait for an _Atomic int to reach a target.
 * Built on wait_until_pred.
 */
static int
wait_atomic_at_least(_Atomic int *value, int target,
    uint64_t timeout_ms, strand_scheduler_t *drive_sched);

/*
 * wait_fiber_state -- wait for a fiber handle's state to match.
 * Used to confirm a fiber has parked (e.g. FIBER_PARKED_OFFLOAD)
 * before exercising a cancel race.
 */
static int
wait_fiber_state(strand_fiber_handle_t *handle, fiber_state_t expected,
    uint64_t timeout_ms, strand_scheduler_t *drive_sched);
```

**Timeout guidelines:**

| Context | Recommended timeout |
|---|---|
| Worker thread executing a trivial fiber | 15 000 ms |
| Offload thread picking up / completing work | 15 000 ms |
| Offload arg-outlives-cancel (long spin loop) | 60 000 ms |

These values are deliberately large.  On native hardware each wait
completes in milliseconds; the generous ceiling exists solely for
Valgrind.  Tests that time out under Valgrind indicate a real hang, not
a slow machine.

**Rules for cross-worker / offload tests:**

1. **Never assume a fixed sleep is long enough.**  Use a predicate wait
   that checks the observable condition (atomic flag, pool state, fiber
   state) rather than hoping N milliseconds is sufficient.
2. **Use observable flags to gate race set-up.**  When a test needs an
   offload thread to have picked up an item before cancelling, add an
   `_Atomic int` flag that the offload function sets on entry
   (e.g. `g_cancel_fn_started`).  Wait for it with
   `wait_atomic_at_least` before proceeding.
3. **Always release spinning offload functions on failure paths.**  If a
   test exits early (timeout, assertion), set the release flag so that
   `strand_offload_pool_destroy` does not deadlock on a blocked thread.
   Use a `goto cleanup` pattern when multiple exit paths exist.
4. **Pass `drive_sched` when the test thread is the scheduler.**  The
   offload tests (single-threaded scheduler driven by the test thread)
   must pass the scheduler to `wait_until_pred` so that fibers are
   advanced while waiting.  Runtime tests (real worker threads) pass
   `NULL`.
5. **Match predicate to actual invariant.**  A pool with capacity 2 and
   one blocker has `in_flight=1`, not `count + in_flight >= capacity`.
   Use `pred_pool_has_inflight` (not `pred_pool_full`) when waiting for
   a single item to be picked up.

### 2.6 What Gets Unit Tests

Every module listed in REPOSITORY_STRUCTURE.md §3 has unit tests:

| Module | Test file |
|---|---|
| `strand_context.c` + assembly | `test_layer1.c` |
| `strand_fiber.c` | `test_layer2.c` |
| `strand_sched.c` | `test_layer2.c` |
| `strand_poller.c` | `test_layer3.c` |
| `strand_inject.c` | `test_layer4.c` |
| `strand_runtime.c` | `test_layer4.c` |
| `strand_offload.c` | `test_layer4.c` |
| `strand_scope.c` | `test_layer5.c` |

What does **not** need dedicated unit tests:
- `strand_internal.h` - tested implicitly by every test that links the library
- Build system files and config files

See DEVELOPMENT.md for the specific named test cases required per phase.

---

## 3. Memory Testing

### 3.1 Valgrind (Linux)

Run after every phase on Linux:

```sh
make dev
make valgrind
# equivalent to:
valgrind --leak-check=full          \
         --show-leak-kinds=all      \
         --track-origins=yes        \
         --error-exitcode=1         \
         ./tests/run_tests
```

libstrand registers and deregisters every fiber stack with Valgrind via
`VALGRIND_STACK_REGISTER` / `VALGRIND_STACK_DEREGISTER`. Without these,
Valgrind reports false positives on every fiber stack access. See
TECH_STACK.md §7.1 for the registration pattern.

All tests must pass Valgrind clean before the phase is considered complete.
No suppression file is needed - libstrand has no external library dependencies
that would require suppressions.

### 3.2 AddressSanitizer and UndefinedBehaviorSanitizer

Run on both platforms after every phase:

```sh
make dev   # compiles with -fsanitize=address,undefined
make test
```

ASan requires the fiber stack switching hooks to be in place or it reports
false positives on every context switch. See TECH_STACK.md §7.2 for the
`__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber`
integration. Verify in Phase 2 that the hooks suppress false positives before
the scheduler is built.

Debug build additionally enables:
- O_NONBLOCK assertion at fd registration
- `scope_abandon` stack-range check
- Scheduler watchdog warnings

### 3.3 ThreadSanitizer

Run on Linux at each phase boundary and before release:

```sh
make test-tsan
```

TSan requires the fiber-aware hooks to be in place or it reports false
positives across every context switch. See TECH_STACK.md §7.3 for the
`__tsan_switch_to_fiber` / `__tsan_create_fiber` / `__tsan_destroy_fiber`
integration. Verify in Phase 2 that the hooks are in place.

TSan is not a per-commit gate. It is a phase-boundary gate. TSan and ASan
are mutually exclusive - they run as separate targets.

TSan is particularly important for:
- Phase 4 (Layer 4): inject queue acquire/release ordering
- Phase 5 (Layer 5): scope atomic field access patterns
- Phase 7: integration tests with real multi-worker scenarios

### 3.4 Memory Discipline Check

Run the static check after every phase:

```sh
grep -rn "malloc\|calloc\|realloc\|free" src/ | grep -v "stack_alloc\|stack_free"
```

Permitted `malloc`/`free` calls in `src/`:
- Fiber descriptor allocation and dead pool management (`strand_fiber.c`)
- Scheduler and runtime control structure allocation (`strand_sched.c`,
  `strand_runtime.c`, `strand_offload.c`, `strand_scope.c`)

**Forbidden:** `malloc` for stack memory anywhere in `src/`. Stacks must use
`mmap`/`munmap` for guard page support. Any `malloc` call on a code path that
allocates a fiber stack is a bug.

---

## 4. Sanitizer Integration Verification

The sanitizer hooks must be verified explicitly in Phase 2 before proceeding
to Phase 3. Incorrect hooks produce confusing false positives that obscure real
bugs in higher layers.

### 4.1 ASan Hook Verification

Build with `make dev`. Run the Layer 1 context switch tests. Verify:
1. No ASan `stack-buffer-overflow` or `heap-use-after-poison` reports during
   context switches between test fibers
2. Confirm by temporarily removing the `__sanitizer_start_switch_fiber` call
   and verifying that ASan then produces false positives - this confirms the
   hook is actually suppressing them, not that the issue simply does not exist

### 4.2 TSan Hook Verification

Build with `make test-tsan`. Run Layer 1 context switch tests. Verify:
1. No TSan `data race` reports during context switches
2. TSan reports must be from actual races, not from the context switch
   mechanism itself

### 4.3 Valgrind Stack Registration Verification

Build with `make valgrind`. Run Layer 1 tests. Verify:
1. No `Invalid read/write` reports on fiber stack memory
2. No `Address ... is on thread N's stack` reports from Valgrind for fiber
   stacks - these indicate unregistered stacks

---

## 5. Platform Testing

### 5.1 Platform Matrix

Every phase must pass on all four targets before it is complete:

| Platform | Architecture | I/O multiplexing | Wakeup mechanism | CPU affinity |
|---|---|---|---|---|
| Linux | x86_64 | epoll | eventfd | pthread_setaffinity_np |
| Linux | ARM64 | epoll | eventfd | pthread_setaffinity_np |
| OpenBSD | amd64 | kqueue | pipe2 | Not available |
| OpenBSD | arm64 | kqueue | pipe2 | Not available |

### 5.2 Platform-Specific Test Cases

Some tests are platform-specific. These must be conditionally compiled:

**Linux-only tests:**
- `test_edge_triggered_oneshot_linux` - EPOLLET behaviour
- `test_post_rearm_readiness_check_linux` - zero-timeout epoll_wait after MOD
- `test_all_events_processed_in_rearm_check` - must process all returned events
- `test_epollerr_wakes_waiter` - EPOLLERR delivery
- `test_epollhup_wakes_waiter` - EPOLLHUP delivery
- `test_eventfd_wakeup` - eventfd as cross-worker wakeup mechanism

**OpenBSD-only tests:**
- `test_ev_dispatch_fires_on_reenable` - EV_DISPATCH level-triggered re-fire
- `test_ev_eof_wakes_reader` - EV_EOF on kqueue EVFILT_READ
- `test_pipe_wakeup` - pipe2 as cross-worker wakeup mechanism (no eventfd)
- `test_no_post_rearm_check_openbsd` - verify re-arm works without zero-timeout poll

**Both platforms - epoll/kqueue equivalence tests:**
These are structurally identical but use different fd types and trigger modes:
- `test_wait_readable_wakes`
- `test_simultaneous_read_write_waiters`
- `test_cancel_one_direction_leaves_other`
- `test_cross_worker_cancel_wins`

Use `#ifdef STRAND_LINUX` / `#ifdef STRAND_OPENBSD` guards for platform-
specific tests. The `make test` target compiles and runs the correct set for
the current platform.

### 5.3 OpenBSD-Specific Concerns

**No Valgrind on OpenBSD.** ASan/UBSan (`make dev`) is the memory safety gate
there.

**No CPU affinity.** The `test_worker_affinity` test (if any) must be guarded
`#ifdef STRAND_LINUX`.

**No eventfd.** Cross-worker wakeup tests on OpenBSD use the pipe2 path.
Verify the pipe-based wakeup works correctly - it is the more complex path
(two fds, drain both read end bytes).

**pipe2 wakeup drain:** The wakeup pipe on OpenBSD must be drained correctly
in scheduler advance Step 3. Verify in a dedicated test that writing multiple
bytes to the pipe (from multiple `strand_scheduler_stop` calls or multiple
cross-worker signals) drains all bytes, not just one.

---

## 6. Correctness Properties Requiring Explicit Tests

These are the most subtle correctness requirements in the architecture. Each
must have at least one dedicated test that specifically targets the property -
not just tests where the property happens to hold.

### 6.1 Post-Re-Arm Readiness Check (Linux)

**Property:** When one direction of an fd fires and the other direction still
has a waiter, re-arming with MOD followed by EPOLLET does not guarantee a new
edge if the condition was already true. A zero-timeout `epoll_wait` immediately
after MOD is required.

**Dedicated test:** `test_post_rearm_readiness_check_linux`

Setup: park two fibers on the same fd - one on read, one on write. Make both
directions ready simultaneously. Fire the event. Verify both fibers wake,
specifically that the second fiber wakes in the same `strand_scheduler_advance`
call (not in a subsequent one). The test is wrong if it passes only because the
second direction generates a new edge later.

**Second dedicated test:** `test_all_events_processed_in_rearm_check`

Setup: two different fds, both with waiters. One fd fires and triggers a re-arm
check. Verify the other fd's waiter also wakes in the same advance call. This
confirms the zero-timeout poll processes all returned events, not only the
rearmed fd.

### 6.2 Cancellation Race Rule

**Property:** Same-worker cancellation - readiness wins. Cross-worker
cancellation - cancel wins.

**Dedicated tests:**

`test_same_worker_readiness_wins`: arrange for both an fd readiness event and a
cancellation to be processed in the same `strand_scheduler_advance` call. The
fd readiness arrives in Step 4 before Step 5 runs. The cancel is enqueued but
the fiber is woken by readiness first. Fiber must receive the ready result, not
the cancel result.

`test_cross_worker_cancel_wins`: from a second worker, inject a cancel for a
fiber parked on an fd. The inject is drained in Step 1 before the I/O poll in
Step 4. Fiber must receive the cancel result even if the fd also becomes ready
in Step 4.

### 6.3 Offload CAS Both Outcomes

**Property:** Both the RESULT_CLAIMED and CANCELLED CAS outcomes must be
reachable and correct. The refcount must be released exactly once in each case.

**Dedicated tests:**

`test_offload_result_claimed_wins`: use synchronisation primitives to
guarantee RESULT_CLAIMED wins the CAS before the cancel attempt. Verify fiber
resumes with the offload result. Verify no refcount double-release (Valgrind
confirms).

`test_offload_cancelled_wins`: use synchronisation to guarantee CANCELLED wins.
Verify fiber resumes with cancel result. Verify `result_slot` was not written.
Verify no refcount double-release.

Both tests must be exercised in the same test run. A test that only covers one
CAS outcome is insufficient.

### 6.4 Scope Walk Reference Rule

**Property:** The scope control block must not be freed while a cancellation
walk is executing, even if the last child finishes during the walk.

**Dedicated test:** `test_scope_walk_ref_prevents_free`

Setup: open a scope with two children. Arrange for one child to finish (and
trigger `OWNER_RUNTIME` free conditions) while the cancellation walk is
mid-iteration. The walk holds `walk_ref_count > 0`. Verify the control block
is not freed until the walk completes and decrements `walk_ref_count`. Use
Valgrind or ASan to confirm no use-after-free during the walk.

This test requires injecting a delay or using synchronisation inside the walk
to create the window - it cannot be reliably tested without explicit control
of timing.

### 6.5 CANCELLING to COMPLETED Shortcut

**Property:** If the last child finishes before the cancellation walk completes,
the scope transitions directly to COMPLETED from CANCELLING. The walk must
continue safely - remaining stale handles return STRAND_HANDLE_STALE, not
crashing.

**Dedicated test:** `test_scope_cancelling_to_completed_shortcut`

Setup: spawn two very short-lived fibers. Initiate cancellation. Arrange for
both fibers to complete before the walk reaches them. Verify scope reaches
COMPLETED. Verify no assertion failure or crash from the walk encountering
stale handles.

### 6.6 Stack Cache Overflow Policy

**Property:** When the cache is at cap, the incoming stack is freed immediately
(not evicted from cache). The cache retains its existing contents.

**Dedicated test:** `test_stack_cache_cap`

Setup: configure cache cap to 4. Spawn and complete 5 fibers of the same stack
size. Verify exactly 4 stacks are cached (not 5). Verify the fifth stack was
unmapped (munmap call count matches). Verify cache contents are from the first
4 completions, not overwritten by the fifth.

### 6.7 Wakeup Fd Drain as Control Event

**Property:** Bytes written to the scheduler wakeup fd by `strand_scheduler_stop`
or cross-worker signals must be drained as control bytes in Step 3 and must not
be interpreted as fiber waiter events.

**Dedicated test:** `test_wakeup_fd_drained_as_control`

Setup: write a byte to the scheduler wakeup fd manually (simulating a stop or
cross-worker signal). Call `strand_scheduler_advance`. Verify no fiber is
woken. Verify no error result is delivered to any fiber. Verify the byte was
consumed (the fd is readable before advance, not readable after).

---

## 7. Integration Tests

Integration tests live in `tests/test_integration.c` and are built and run as
part of `make test` in Phase 7. They exercise realistic usage scenarios across
multiple layers simultaneously.

### 7.1 Echo Server

Verify that a complete connection lifecycle works: accept a connection, spawn a
per-connection fiber, read/write in a loop using `strand_fiber_wait_readable`
and `strand_fiber_wait_writable`, close cleanly. Verify multiple concurrent
connections (spawn one fiber per connection from the host thread via
round-robin) work correctly without interference.

### 7.2 Producer-Consumer Pipeline

A chain of fibers where each stage reads from one pipe and writes to the next,
passing work downstream. Verify correct ordering, that back-pressure is
respected (a full pipe parks the writing fiber until the reader drains), and
that the pipeline shuts down cleanly when the source fiber finishes. No items
lost, no deadlock.

### 7.3 Fan-Out Scope with Error Propagation

Spawn N child fibers under a scope. Arrange for one to fail partway through.
Verify:
- First error propagates to `strand_scope_wait`
- Siblings are cancelled in reverse spawn order
- `strand_scope_wait` does not return until all children have finished
- No fiber descriptor or stack memory leaks after scope completes

### 7.4 Timeout then Abandon

Open a scope, spawn long-running children. Call `strand_scope_wait_timeout`
with a short deadline. Verify timeout return. Verify scope is still in its
prior lifecycle state (not COMPLETED). Call `strand_scope_abandon`. Verify
children continue running and control block is freed when they finish. Verify
no use-after-free (Valgrind / ASan).

### 7.5 Offload with Cancellation Race

Call `strand_fiber_offload` with a blocking function. Cancel the waiting fiber
before the offload completes. Exercise both outcomes (RESULT_CLAIMED wins,
CANCELLED wins) in separate sub-tests. Verify correctness of each outcome per
ARCHITECTURE.md §6.6. Verify arg remains valid until `blocking_fn` returns in
all cases.

### 7.6 Offload EAGAIN and Retry Pattern

Fill the offload pool to capacity. Call `strand_fiber_offload`. Verify
`STRAND_EAGAIN` is returned immediately - the fiber is not parked. Call
`strand_fiber_yield()` and retry. Verify the fiber eventually succeeds when
a pool slot becomes available. This test confirms the mandatory yield-before-
retry pattern works correctly and that the pool does not deadlock under
saturation.

### 7.7 Subprocess via Offload

From a fiber, spawn a subprocess via `fork`/`exec`/`waitpid` through the
offload pool. Verify the subprocess runs and its exit status is delivered to
the originating fiber. Verify the worker thread is not blocked during the
subprocess wait.

### 7.8 Multi-Worker Accept Distribution

Register two workers. From the host thread, spawn 100 fibers with round-robin
selection. Verify fibers are distributed approximately evenly across both
workers. Verify no fiber is assigned to a stopped worker.

### 7.9 Simultaneous Read/Write on Same fd

Park two fibers on the same fd - one waiting for read readiness, one waiting
for write readiness. Make both directions ready simultaneously. Call
`strand_scheduler_advance`. Verify both fibers wake in that single advance
call - specifically that the second waiter wakes via the post-re-arm zero-
timeout readiness check, not in a subsequent advance call. This is the primary
integration-level verification of the Linux post-re-arm protocol from
ARCHITECTURE.md §5.5.

### 7.10 Guest Mode Host Loop

Drive a libstrand scheduler from an external event loop using
`strand_scheduler_advance`. Verify the host loop integration pattern from
ARCHITECTURE.md §4.3:
- The unconditional trailing `strand_scheduler_advance` call correctly
  processes timer expiry even when no scheduler fd event fired
- The host loop does not block when the scheduler has work

### 7.11 Scheduler Stop Interrupts Blocked Worker

Start a worker with no fibers scheduled. Verify `strand_scheduler_run` blocks
(worker thread is parked in `epoll_wait`/`kevent`). Call `strand_scheduler_stop`
from the host thread. Verify `strand_scheduler_run` returns within 100ms. This
test confirms both that the atomic flag is set AND that the wakeup fd write
interrupts the blocked wait - both steps of `strand_scheduler_stop` are
verified by this test.

---

## 8. Performance Benchmarks

Benchmarks live in `bench/` and are built and run by `make bench` under release
flags. They are not part of `make test` - they do not have pass/fail criteria
at this stage. Their purpose is to establish baselines and detect regressions
between versions.

### 8.1 What Is Measured

| Benchmark | Metric | Notes |
|---|---|---|
| `bench_context_switch` | Round-trip ns per switch | Two-fiber ping-pong; also measures with errno/FP save |
| `bench_fiber_spawn` | Fibers created per second | Cache-warm and cache-cold paths separately |
| `bench_io_roundtrip` | ns from write to fiber wake | pipe-based; epoll and kqueue separately |
| `bench_scheduler` | Fibers run per second | Budget sensitivity: 8, 16, 32, 64, 128 |
| `bench_multiworker` | Throughput vs worker count | 1, 2, 4, 8 workers |
| `bench_offload` | Offload submissions per second | Pool size sensitivity |
| `bench_cross_worker` | ns from inject to fiber resume | Cross-worker wakeup latency |

### 8.2 Baseline Recording Format

Each benchmark run prints full hardware context before results:

```
libstrand benchmark - context_switch
Platform : Linux x86_64
CPU      : Intel Core i9-13900K @ 5.8 GHz
Cores    : 24 (8P + 16E)
Build    : release (-O2)
Date     : 2026-03-28

two-fiber ping-pong (no errno/FP save):   142 ns/switch
two-fiber ping-pong (with errno/FP save): 158 ns/switch
```

Results without hardware context are not comparable across machines and must
not be recorded as baselines.

### 8.3 Benchmark Targets (Indicative)

These are design targets derived from the architecture, not pass/fail gates.
Actual results on specific hardware will differ.

| Benchmark | Design target |
|---|---|
| Context switch round-trip | < 300 ns on modern x86_64 |
| Fiber creation (cache warm) | < 500 ns per fiber |
| I/O park and wake (loopback pipe) | < 2 µs round-trip |
| Scheduler throughput | > 1M fibers/second (single worker) |
| Cross-worker wakeup | < 5 µs |

---

## 9. Test Coverage Tracking

Track coverage manually. Update after each phase. A cell is marked done only
when the test passes cleanly with no Valgrind or ASan errors on both platforms.

### Unit Test Coverage

| Module | Test file | Written | Valgrind clean | ASan clean | TSan clean | OpenBSD |
|---|---|---|---|---|---|---|
| Context switch (x86_64) | test_layer1.c | - | - | - | - | - |
| Context switch (AArch64) | test_layer1.c | - | - | - | - | - |
| Stack alloc/guard pages | test_layer1.c | - | - | - | - | - |
| Fiber scheduler (advance) | test_layer2.c | - | - | - | - | - |
| Fiber scheduler (run/stop) | test_layer2.c | - | - | - | - | - |
| Timer heap | test_layer2.c | - | - | - | - | - |
| Stack cache | test_layer2.c | - | - | - | - | - |
| Fiber handle ABA | test_layer2.c | - | - | - | - | - |
| Fiber-local storage | test_layer2.c | - | - | - | - | - |
| I/O parking (epoll/kqueue) | test_layer3.c | - | - | - | - | - |
| Re-arm readiness check | test_layer3.c | - | - | - | - | - |
| I/O cancellation | test_layer3.c | - | - | - | - | - |
| Error/hangup/EOF delivery | test_layer3.c | - | - | - | - | - |
| Inject queue | test_layer4.c | - | - | - | - | - |
| Worker registry/round-robin | test_layer4.c | - | - | - | - | - |
| Offload pool | test_layer4.c | - | - | - | - | - |
| Offload CAS outcomes | test_layer4.c | - | - | - | - | - |
| Scope lifecycle | test_layer5.c | - | - | - | - | - |
| Walk reference rule | test_layer5.c | - | - | - | - | - |
| Error propagation | test_layer5.c | - | - | - | - | - |
| Integration tests | test_integration.c | - | - | - | - | - |

### Key Correctness Test Cases

| Test case | File | Reference |
|---|---|---|
| GPRs preserved across context switch | test_layer1.c | ARCHITECTURE.md §3.2, §3.3 |
| errno saved and restored | test_layer1.c | ARCHITECTURE.md §3.4 |
| MXCSR saved and restored (x86_64) | test_layer1.c | ARCHITECTURE.md §3.4 |
| FPCR/FPSR saved and restored (AArch64) | test_layer1.c | ARCHITECTURE.md §3.4 |
| x87 FP environment unsupported warning documented in public API | test_layer1.c | ARCHITECTURE.md §3.4 |
| Guard page triggers SIGSEGV on overflow | test_layer1.c | ARCHITECTURE.md §3.5 |
| strand_context_init arg delivered correctly | test_layer1.c | ARCHITECTURE.md §3 |
| strand_scheduler_advance is nonblocking | test_layer2.c | ARCHITECTURE.md §4.2 |
| FIFO run queue order preserved | test_layer2.c | ARCHITECTURE.md §4.4 |
| Budget limiting (at most N fibers per advance) | test_layer2.c | ARCHITECTURE.md §4.4 |
| strand_scheduler_stop writes wakeup fd | test_layer2.c | ARCHITECTURE.md §4.2 |
| Wakeup fd bytes drained as control, not fiber events | test_layer2.c | ARCHITECTURE.md §4.2 |
| strand_scheduler_stop interrupts blocked worker | test_layer2.c | ARCHITECTURE.md §4.2 |
| Timer fires at correct deadline | test_layer2.c | ARCHITECTURE.md §4.2 |
| Timer cancel wakes fiber before deadline | test_layer2.c | ARCHITECTURE.md §8.1 |
| Stale handle returns STRAND_HANDLE_STALE | test_layer2.c | ARCHITECTURE.md §4.6 |
| Generation increments on descriptor reuse | test_layer2.c | ARCHITECTURE.md §4.6 |
| Stack cache reuse (no mmap on cache hit) | test_layer2.c | ARCHITECTURE.md §13 |
| Stack cache cap (overflow freed immediately) | test_layer2.c | ARCHITECTURE.md §13.3 |
| Idle reclamation shrinks cache to floor | test_layer2.c | ARCHITECTURE.md §13.4 |
| fiber_spawn returns STRAND_ERR_SHUTDOWN after stop | test_layer2.c | ARCHITECTURE.md §6.2 |
| fd parking wakes on readiness | test_layer3.c | ARCHITECTURE.md §5 |
| Post-re-arm readiness check - second waiter wakes immediately | test_layer3.c | ARCHITECTURE.md §5.5 |
| All events from zero-timeout poll processed | test_layer3.c | ARCHITECTURE.md §5.5 |
| EV_DISPATCH fires on re-enable (OpenBSD) | test_layer3.c | ARCHITECTURE.md §5.7 |
| Same-worker: readiness wins over cancel | test_layer3.c | ARCHITECTURE.md §5.3 |
| Cross-worker: cancel wins over readiness | test_layer3.c | ARCHITECTURE.md §5.3 |
| EPOLLERR/EPOLLHUP wakes waiter with error | test_layer3.c | ARCHITECTURE.md §5.6 |
| EV_EOF wakes reader waiter (OpenBSD) | test_layer3.c | ARCHITECTURE.md §5.7 |
| Cancel one direction: other direction remains | test_layer3.c | ARCHITECTURE.md §5.8 |
| O_NONBLOCK assertion fires in debug build | test_layer3.c | ARCHITECTURE.md §5.1 |
| Inject queue delivers to target worker | test_layer4.c | ARCHITECTURE.md §6.3 |
| Round-robin distributes across workers | test_layer4.c | ARCHITECTURE.md §6.4 |
| Stopped worker excluded from round-robin | test_layer4.c | ARCHITECTURE.md §6.2 |
| No workers registered returns error | test_layer4.c | ARCHITECTURE.md §6.4 |
| Offload RESULT_CLAIMED wins: fiber gets result | test_layer4.c | ARCHITECTURE.md §6.6 |
| Offload CANCELLED wins: result_slot not written | test_layer4.c | ARCHITECTURE.md §6.6 |
| Offload refcount released exactly once (both outcomes) | test_layer4.c | ARCHITECTURE.md §6.6 |
| Offload EAGAIN when pool full | test_layer4.c | ARCHITECTURE.md §6.5 |
| Inject queue never drops on overflow (backoff) | test_layer4.c | ARCHITECTURE.md §6.3 |
| Scope ACTIVE → COMPLETED direct (no failure) | test_layer5.c | ARCHITECTURE.md §7.3 |
| Scope CANCELLING → COMPLETED shortcut | test_layer5.c | ARCHITECTURE.md §7.3 |
| Walk reference prevents premature free | test_layer5.c | ARCHITECTURE.md §7.3 |
| First error wins; second discarded | test_layer5.c | ARCHITECTURE.md §7.5 |
| Reverse spawn-order cancellation | test_layer5.c | ARCHITECTURE.md §7.6 |
| scope_wait is terminal | test_layer5.c | ARCHITECTURE.md §7.4 |
| scope_wait_timeout: scope state unchanged on timeout | test_layer5.c | ARCHITECTURE.md §7.4 |
| scope_abandon: OWNER_RUNTIME frees on completion | test_layer5.c | ARCHITECTURE.md §7.4 |
| scope_abandon debug check: stack-allocated scope asserts | test_layer5.c | ARCHITECTURE.md §7.4 |
| scope_open from host thread returns error | test_layer5.c | ARCHITECTURE.md §7.1 |
| All OWNER × lifecycle combinations reachable | test_layer5.c | ARCHITECTURE.md §7.3 |
| Producer-consumer pipeline: ordering and back-pressure | test_integration.c | ARCHITECTURE.md §4 |
| Offload EAGAIN + yield + retry: successful under pool saturation | test_integration.c | ARCHITECTURE.md §6.5 |
| Simultaneous read/write on same fd: both waiters wake in same advance call | test_integration.c | ARCHITECTURE.md §5.5 |
| Echo server: per-connection fiber lifecycle end-to-end | test_integration.c | ARCHITECTURE.md §4.3 |
| Scheduler stop interrupts blocked worker within bounded time | test_integration.c | ARCHITECTURE.md §4.2 |

### Quality Milestone Gate

| Milestone | Confirmed by |
|---|---|
| M9 - context switch preserves all registers | test_layer1.c GPR + FP tests |
| M10 - advance is nonblocking | test_layer2.c advance test |
| M11 - stop interrupts blocked worker | test_layer2.c + integration §7.11 |
| M12 - post-re-arm readiness check correct | test_layer3.c dedicated tests |
| M13 - offload CAS both outcomes | test_layer4.c both CAS tests |
| M14 - scope lifecycle all transitions | test_layer5.c lifecycle tests |
| M15 - integration test suite passes | test_integration.c all tests |

---

## 10. Cross-Reference

| Topic | Reference |
|---|---|
| Test harness structure and RUN macro | TECH_STACK.md §6 |
| ASan fiber hook macros | TECH_STACK.md §7.2 |
| TSan fiber hook macros | TECH_STACK.md §7.3 |
| Valgrind stack registration macros | TECH_STACK.md §7.1 |
| Per-phase test tasks and named cases | DEVELOPMENT.md (each phase) |
| Context switch register set (x86_64) | ARCHITECTURE.md §3.2 |
| Context switch register set (AArch64) | ARCHITECTURE.md §3.3 |
| errno and FP control save/restore | ARCHITECTURE.md §3.4 |
| Guard page installation | ARCHITECTURE.md §3.5 |
| Scheduler five-step advance | ARCHITECTURE.md §4.2 |
| Fiber state machine | ARCHITECTURE.md §4.5 |
| Fiber handle ABA protection | ARCHITECTURE.md §4.6 |
| Stack cache policy | ARCHITECTURE.md §13 |
| fd lifecycle contract | ARCHITECTURE.md §5.2 |
| Cancellation race rule (same vs cross-worker) | ARCHITECTURE.md §5.3 |
| Post-re-arm readiness check | ARCHITECTURE.md §5.5 |
| EPOLLERR/EPOLLHUP handling | ARCHITECTURE.md §5.6 |
| OpenBSD EV_DISPATCH and EV_EOF | ARCHITECTURE.md §5.7 |
| Inject queue memory ordering | ARCHITECTURE.md §6.3 |
| Offload work item lifecycle and CAS | ARCHITECTURE.md §6.6 |
| Offload memory ordering chain | ARCHITECTURE.md §6.7 |
| Scope lifecycle state machine | ARCHITECTURE.md §7.3 |
| Walk reference rule | ARCHITECTURE.md §7.3 |
| scope_abandon allocation requirement | ARCHITECTURE.md §7.4 |
| Structured concurrency guarantee (honest) | ARCHITECTURE.md §7.6 |
| Thread safety matrix | ARCHITECTURE.md §12 |
| SAFETY comment convention | CODING_STANDARDS.md §6.2 |
| ATOMIC comment convention | CODING_STANDARDS.md §4.2 |
| TLS footguns | CODING_STANDARDS.md §6.3 |
| Source file purposes | REPOSITORY_STRUCTURE.md §3 |
| Integration test descriptions | REPOSITORY_STRUCTURE.md §4 |
| Benchmark descriptions | REPOSITORY_STRUCTURE.md §5 |

---

**Document Version**: 1.1
**Last Updated**: 2026-04-18
**See Also**: PROJECT.md, ARCHITECTURE.md, TECH_STACK.md, CODING_STANDARDS.md,
DEVELOPMENT.md, REPOSITORY_STRUCTURE.md
