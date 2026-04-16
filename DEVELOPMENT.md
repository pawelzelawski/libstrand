# Development Plan

## Status Overview

**Last Updated**: 2026-04-16
**Current Phase**: Phase 3 — Layer 2: Fiber Scheduler (in progress)
**Next Task**: Phase 3 — Task 3.10: Fiber-local storage

### Phase Summary

| Phase | Name | Status | Tests | Notes |
|---|---|---|---|---|
| 1 | Foundation | DONE | 0/0 | Build system, test harness, skeleton |
| 2 | Layer 1: Execution Contexts | DONE | 10/10 | Linux/OpenBSD on x86_64/arm64 green in CI; Phase 2 stabilization closed |
| 3 | Layer 2: Fiber Scheduler | IN PROGRESS | 28/28 | Tasks 3.1–3.9 done; 3.10 remaining |
| 4 | Layer 3: I/O Integration | NOT STARTED | — | epoll/kqueue, fd parking, re-arm protocol |
| 5 | Layer 4: Multi-Worker Runtime | NOT STARTED | — | Workers, inject queue, offload pool |
| 6 | Layer 5: Scopes and Coordination | NOT STARTED | — | Structured concurrency, fiber-local storage |
| 7 | Hardening, Benchmarks, and Release | NOT STARTED | — | Integration tests, benchmarks, documentation |

### Quality Milestones

| ID | Milestone | Status |
|---|---|---|
| M1 | Build system works on Linux and OpenBSD, both architectures | DONE |
| M2 | All unit tests pass on Linux | NOT STARTED |
| M3 | All unit tests pass on OpenBSD | NOT STARTED |
| M4 | Valgrind clean on Linux | DONE (10/10 tests pass under Valgrind) |
| M5 | ASan/UBSan clean on both platforms | IN PROGRESS (Linux x86_64 clean; OpenBSD ASan availability and parity under review) |
| M6 | TSan clean on Linux (Clang only) | DONE (Phase 2: 10/10 tests pass under TSan) |
| M7 | clang-format clean | DONE |
| M8 | clang-tidy zero warnings | DONE |
| M9 | Context switch preserves all registers — verified by test | DONE (test_context_gpr_preserved passes) |
| M10 | scheduler_advance is nonblocking — verified by test | DONE (test_advance_nonblocking passes) |
| M11 | scheduler_stop interrupts indefinitely blocked worker — verified by test | DONE (test_scheduler_run_blocks passes) |
| M12 | Post-re-arm readiness check correct — verified by test | NOT STARTED |
| M13 | Offload CAS both outcomes exercised — verified by test | NOT STARTED |
| M14 | Scope lifecycle all state transitions verified by test | NOT STARTED |
| M15 | Integration test suite passes: echo server, fan-out scope, offload cancel | NOT STARTED |

---

## Development Principles

Before starting any phase, read and follow these documents:

- **CODING_STANDARDS.md** — KNF style, assembly conventions, atomic operations,
  safety patterns, pre-commit checklist. Every function written must comply.
- **ARCHITECTURE.md** — The specification. Every implementation decision must
  match what is documented there. If there is a conflict, ARCHITECTURE.md wins
  — raise it before deviating.
- **TECH_STACK.md** — Compiler flags, build system, sanitizer integration,
  assembly conventions.

**Bottom-up approach**: Each phase builds on the previous. Do not start a
phase until the prior phase is complete and all its tests pass on both
platforms.

**Test as you go**: Write tests for each component before moving to the next.
A component without passing tests is not done.

**Platform parity**: Test on both Linux and OpenBSD at each phase. Platform
divergence caught late is much harder to fix than divergence caught at the
phase boundary. The epoll/kqueue split is most visible in Phase 4 — do not
defer OpenBSD testing to after Phase 4 is "working" on Linux.

**Assembly first in Phase 2**: The context switch is the foundation of
everything. Do not start the scheduler (Phase 3) until the context switch
is verified correct by test on both architectures. A broken context switch
produces extremely subtle and confusing failures in higher layers.

**Phase ordering rationale**: Layer 1 (Phase 2) has no dependencies. Layer 2
(Phase 3) depends on Layer 1. Layer 3 (Phase 4) depends on Layer 2 — it needs
a working scheduler to park and wake fibers. Layer 4 (Phase 5) extends Layer 2
and Layer 3 with multi-worker plumbing. Layer 5 (Phase 6) depends on all lower
layers. Phases cannot be reordered.

---

## Phase 1 — Foundation

**Goal**: Build system works on both platforms and both architectures. Repository
structure is in place. Test harness compiles and runs. Skeleton headers and source
files are in place with correct include structure. `_Static_assert` placeholders
are in place. Code compiles clean with zero warnings.

**Reference documents**:
- TECH_STACK.md §5 — Makefile structure, compiler flags, build targets
- TECH_STACK.md §6 — test harness structure
- CODING_STANDARDS.md §1.2 — file organisation and include order
- CODING_STANDARDS.md §1.3 — `_Static_assert` placement
- REPOSITORY_STRUCTURE.md §1 — top-level directory layout
- REPOSITORY_STRUCTURE.md §3 — source file list

**Prerequisite**: Phase 0 complete.

### Tasks

**1.1 — Repository skeleton** ✓ DONE
- Create directory structure per REPOSITORY_STRUCTURE.md §1:
  `src/`, `src/arch/x86_64/`, `src/arch/arm64/`, `include/`, `tests/`,
  `bench/`, `tools/`
- Create `include/strand.h` with skeleton: include guards, `<stddef.h>` and
  `<stdint.h>` includes, empty forward typedefs for all public types
  (no function declarations yet)
- Create `src/strand_internal.h` with skeleton: include guards, internal
  forward declarations, `STRAND_DEBUG_ASSERT` macro definition,
  `STRAND_CONTEXT_SIZE` placeholder constant
- Create stub `.c` files for all modules listed in REPOSITORY_STRUCTURE.md §3:
  each file includes its own header and compiles to an empty object
- Create top-level `Makefile` with working platform detection
  (`$(shell uname)`) and architecture detection (`$(shell uname -m)`),
  stubs for all targets, correct `ASM_SRC` selection per arch
- Create `.clang-format` per TECH_STACK.md §7.6 (KNF-based)
- Create `.clang-tidy` per TECH_STACK.md §7.4
- Verify `make dev` and `make release` compile all stubs with zero warnings
  on Linux and OpenBSD, x86_64 and ARM64

**1.2 — Test harness** ✓ DONE
- Create `tests/test_harness.h` with the `RUN(name, fn)` macro per
  TECH_STACK.md §6.1
- Create `tests/run_tests.c` as the test binary entry point
- `make test` must run the empty test suite and print `0/0 tests passed`
  with exit code 0
- Confirm `make valgrind` runs the test binary under Valgrind and exits clean
  on Linux

**1.3 — _Static_assert placeholders** ✓ DONE
- Add placeholder `_Static_assert(1 == 1, "placeholder — replaced in Phase 2")`
  entries in `src/strand_internal.h` for every assertion listed in
  CODING_STANDARDS.md §1.3. They will be replaced with real checks as
  struct definitions land in Phase 2 onward.

### Phase 1 Completion Criteria

- [x] `make dev` succeeds with zero warnings on Linux x86_64
- [x] `make dev` succeeds with zero warnings on Linux ARM64 (CI)
- [x] `make dev` succeeds with zero warnings on OpenBSD amd64
- [x] `make dev` succeeds with zero warnings on OpenBSD arm64 (CI)
- [x] `make test` runs and prints `0/0 tests passed` on Linux x86_64
- [x] `make test` runs and prints `0/0 tests passed` on OpenBSD amd64
- [x] `make valgrind` exits clean on Linux
- [x] `make lint` produces zero warnings on all C stubs
- [x] Quality milestone M1 confirmed

---

## Phase 2 — Layer 1: Execution Contexts

**Goal**: Context switching between two execution contexts works correctly on
all four targets (Linux/OpenBSD × x86_64/ARM64). All callee-saved registers
are preserved. errno is saved and restored. Floating-point control registers
(MXCSR on x86_64, FPCR/FPSR on AArch64) are saved and restored. Stack
alignment is maintained. Guard pages are installed and catch overflows. All
sanitizer hooks are in place and verified. The `strand_context_init` function
correctly fabricates an initial context so that the first switch to a new
stack begins execution at the fiber entry function.

No scheduler, no fibers, no run queue — this phase tests the raw context
switch in isolation.

**Reference documents**:
- ARCHITECTURE.md §3 — full Layer 1 specification
- ARCHITECTURE.md §3.2 — x86_64 register set and ABI rationale
- ARCHITECTURE.md §3.3 — AArch64 register set
- ARCHITECTURE.md §3.4 — errno and FP control registers
- ARCHITECTURE.md §3.5 — guard pages
- ARCHITECTURE.md §3.6 — CFI annotation requirements
- ARCHITECTURE.md §3.7 — sanitizer hooks (ASan, TSan, Valgrind)
- CODING_STANDARDS.md §2 — assembly conventions and CFI rules
- TECH_STACK.md §7.2 — ASan fiber hook macros
- TECH_STACK.md §7.3 — TSan fiber hook macros
- TECH_STACK.md §7.1 — Valgrind stack registration macros

**Prerequisite**: Phase 1 complete.

### Tasks

**2.1 — strand_context_t and strand_context.h** ✓ DONE
- Define `strand_context_t` in `src/strand_context.h`: register save area
  sized to hold all callee-saved registers for the current architecture,
  plus the stack pointer
- Define `STRAND_CONTEXT_SIZE` constant equal to `sizeof(strand_context_t)`
- Replace placeholder `_Static_assert` with the real check:
  `_Static_assert(sizeof(strand_context_t) == STRAND_CONTEXT_SIZE, ...)`
- Add `_Static_assert(offsetof(strand_fiber_t, context) == 0, ...)` —
  placeholder until `strand_fiber_t` is defined in Phase 3; note the
  dependency in a comment

**2.2 — x86_64 assembly: strand_context.S** ✓ DONE
- Implement `strand_context_swap(strand_context_t *old, strand_context_t *new)`
  in `src/arch/x86_64/strand_context.S`:
  - Save callee-saved GPRs to `old`: rbx, rbp, r12, r13, r14, r15
  - Save rsp to `old`
  - Load rsp from `new`
  - Restore callee-saved GPRs from `new`
  - Return (ret uses the restored rsp stack — jumps to wherever `new` was
    suspended or to the fabricated entry point)
- Emit valid unwind info for the switch stub. For stubs that save registers
  into a context struct (not onto the current stack), `.cfi_startproc` and
  `.cfi_endproc` are required and `.cfi_offset`/`.cfi_restore` are used only
  where CFA-relative stack saves actually exist
- No XMM saves — correct under SysV AMD64 ABI. Add a comment confirming
  this explicitly per ARCHITECTURE.md §3.2

**2.3 — AArch64 assembly: strand_context.S** ✓ DONE
- Implement `strand_context_swap` in `src/arch/arm64/strand_context.S`:
  - Save callee-saved GPRs: x19–x28, x29 (fp), x30 (lr), sp
  - Save callee-saved FP/SIMD lower 64-bit halves: d8–d15
  - Load sp, restore GPRs and d8–d15 from `new`
  - Return via restored lr
- Emit valid unwind info for the switch stub as above

**2.4 — C wrapper: strand_context.c** ✓ DONE
- Implement `strand_context_switch(strand_fiber_t *from, strand_fiber_t *to)`:
  - Save errno before switch: `int saved_errno = errno`
  - Save MXCSR (x86_64) via `_mm_getcsr()` or `stmxcsr` inline asm; or
    save FPCR/FPSR (AArch64) via `mrs` inline asm — into `from->fp_ctrl`
  - Call ASan hook before switch:
    `__sanitizer_start_switch_fiber(NULL, to->stack_base, to->stack_size)`
  - Call `strand_context_swap(&from->context, &to->context)`
  - Call ASan hook after switch:
    `__sanitizer_finish_switch_fiber(NULL, NULL, NULL)`
  - Call TSan hook (on outgoing side before swap):
    `__tsan_switch_to_fiber(to->tsan_fiber, 0)`
  - Restore MXCSR or FPCR/FPSR from `to->fp_ctrl`
  - Restore errno: `errno = saved_errno`
  - All sanitizer hooks wrapped in `#ifdef` guards — no-ops in non-sanitizer
    builds per TECH_STACK.md §7.2 and §7.3

**2.5 — strand_context_init** ✓ DONE
- Implement `strand_context_init(strand_context_t *ctx, void *stack_top,
  strand_fiber_fn_t entry, void *arg)`:
  - Fabricate the initial saved-register state so that the first
    `strand_context_swap` to this context begins execution at `entry(arg)`
  - On x86_64: write `entry` as the return address at `stack_top - 8`
    (below 16-byte alignment), set rsp to `stack_top - 8`, clear all
    other callee-saved registers
  - On AArch64: set x30 (lr) to `entry`, x0 to `arg`, sp to `stack_top`
    aligned to 16 bytes
  - Correct alignment is critical — the first instruction of `entry` must
    observe a properly aligned stack

**2.6 — Stack allocation with guard pages** ✓ DONE
- Implement `stack_alloc(size_t stack_size)` in `src/strand_fiber.c`:
  - `mmap(NULL, stack_size + PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)`
  - Check for `MAP_FAILED`
  - `mprotect(base, PAGE_SIZE, PROT_NONE)` — guard page at low end
  - On failure: `munmap` and return NULL
  - Register with Valgrind: `VALGRIND_STACK_REGISTER(base + PAGE_SIZE, base + PAGE_SIZE + stack_size)`
- Implement `stack_free(void *base, size_t stack_size)`:
  - `VALGRIND_STACK_DEREGISTER(valgrind_stack_id)`
  - `munmap(base, stack_size + PAGE_SIZE)`

**2.7 — TSan fiber handle lifecycle** ✓ DONE
- In `strand_context.c` or `strand_fiber.c`, on fiber creation:
  `fiber->tsan_fiber = __tsan_create_fiber(0)` (TSan builds only)
- On fiber destruction: `__tsan_destroy_fiber(fiber->tsan_fiber)`
- Wrapped in `#ifdef __SANITIZE_THREAD__` guards

### Tests for Phase 2

File: `tests/test_layer1.c`

- `test_context_switch_returns`: switch from A to B; B switches back to A;
  verify control returns to A after the round trip
- `test_context_gpr_preserved`: before switching, load known values into all
  callee-saved GPRs via inline asm; switch to B and back; verify GPRs are
  unchanged in A
- `test_context_errno_preserved`: set errno to a known value; switch to B
  (which modifies errno); switch back; verify errno restored in A
- `test_context_mxcsr_preserved` (x86_64): set a non-default MXCSR value;
  switch to B (which sets a different MXCSR); switch back; verify MXCSR
  restored in A
- `test_context_fpcr_preserved` (AArch64): equivalent test for FPCR
- `test_context_stack_alignment`: verify the stack pointer is 16-byte aligned
  on entry to the switched-to function
- `test_context_init_entry_fires`: fabricate a context with `strand_context_init`
  for a known function; switch to it; verify the function executes and sets a
  flag; switch back; verify flag was set
- `test_context_init_arg_delivered`: verify the `arg` pointer is correctly
  delivered as the first argument to `entry`
- `test_guard_page_present`: in a subprocess (to catch SIGSEGV safely),
  allocate a stack and deliberately write below the guard boundary; verify
  SIGSEGV is raised. Test passes if subprocess exits by signal, not normally.
- `test_stack_alloc_free_roundtrip`: allocate and free a stack; verify no
  Valgrind errors and no memory leak
- `test_x87_unsupported_documented`: verify that the documentation for
  `strand_context_switch` (in `strand_context.h` or `strand.h`) contains an
  explicit warning that x87 FP state (including `long double`) is not saved
  across context switches and that programs using `long double` across yield
  points may see corrupted FP behaviour. This is a documentation correctness
  test — it confirms the warning is present, not merely assumed.

### Phase 2 Completion Criteria

- [x] All Layer 1 tests pass on Linux x86_64
- [x] All Layer 1 tests pass on Linux ARM64
- [x] All Layer 1 tests pass on OpenBSD amd64
- [x] All Layer 1 tests pass on OpenBSD arm64
- [x] Valgrind clean on Linux (stack registration/deregistration correct)
- [x] ASan clean — start/finish_switch_fiber hooks suppress false positives
- [x] TSan clean — switch_to_fiber hooks in place; 10/10 tests pass under TSan
- [x] Assembly stubs emit valid CFI/FDE unwind info — verified by
  `readelf --debug-dump=frames` for `strand_context_swap`
- [x] Quality milestones M4, M5, M9 confirmed

### Phase 2 Closeout Changelog (2026-04-06)

- Sanitizer integration hardened: robust feature detection and corrected
  ASan/TSan switch hooks usage in context-switch paths.
- TSan lifecycle completed end-to-end: fiber create/destroy and current-fiber
  binding wired through runtime and test fixtures.
- Stack and platform hardening landed: guard-page allocation safety checks,
  optional `MAP_STACK`, and arm64/OpenBSD context-entry stack setup fixes.
- Assembly stability improvements on arm64: replaced paired register memory
  accesses with scalar loads/stores in context save/restore.
- Documentation/spec alignment completed: CFI expectations clarified to require
  valid unwind metadata/FDE, with plan/docs status synchronized to CI results.

---

## Phase 3 — Layer 2: Fiber Scheduler

**Goal**: A single-threaded scheduler correctly manages multiple fibers in
guest mode and worker mode. Fibers run cooperatively, FIFO order is maintained,
budget limiting works, and timers fire at the correct deadline. The stack cache
correctly reuses stacks and applies the cap and idle reclamation policy.
`strand_fiber_cancel` transitions fibers out of all parked states. The fiber
state machine transitions are correct throughout. This phase uses no real I/O —
the poller is stubbed out for scheduler-only testing.

**Reference documents**:
- ARCHITECTURE.md §4.2 — scheduler operating modes (advance, run, stop)
- ARCHITECTURE.md §4.3 — host loop integration pattern
- ARCHITECTURE.md §4.4 — fairness and budget
- ARCHITECTURE.md §4.5 — fiber state machine and transitions
- ARCHITECTURE.md §4.6 — fiber handle ABA protection
- ARCHITECTURE.md §4.7 — TLS safety
- ARCHITECTURE.md §13 — stack cache policy (64 stacks, idle reclamation)
- CODING_STANDARDS.md §4 — atomic operations (generation counter)
- CODING_STANDARDS.md §6.2 — SAFETY comments

**Prerequisite**: Phase 2 complete.

### Tasks

**3.1 — strand_fiber_t descriptor** ✓ DONE
- Define `strand_fiber_t` in `src/strand_internal.h`:
  - `strand_context_t context` — must be first field (offset 0)
  - `fiber_state_t state` — `_Atomic fiber_state_t`
  - `uint64_t generation`
  - `void *stack_base`, `size_t stack_size`
  - `void *local_ptr` — fiber-local storage slot
  - `strand_destructor_t local_dtor`
  - `strand_scope_t *scope` — owning scope, NULL if detached
  - `struct strand_fiber *next` — intrusive list for run queue and scope lists
  - TSan handle: `void *tsan_fiber` (in TSan builds)
  - Valgrind stack id: `unsigned long valgrind_stack_id`
- Activate `_Static_assert(offsetof(strand_fiber_t, context) == 0, ...)`
- Add `_Static_assert` for `strand_fiber_handle_t` size (16 bytes: ptr + generation)
- Implement `fiber_alloc()` / `fiber_free()` — dead pool recycling with
  generation counter increment on reuse
- Implement `fiber_handle_validate()` inline: null check + generation match

**3.2 — strand_scheduler_t and run queue** ✓ DONE
- Define `strand_scheduler_t` in `src/strand_internal.h`:
  - Run queue: pointer to head and tail of intrusive linked list
  - `run_queue_len`: current length
  - `budget`: configurable max fibers per advance call (default 64)
  - Timer heap: binary min-heap of `(deadline_ns, fiber*)` pairs
  - Wakeup fd: eventfd (Linux) or pipe[2] (OpenBSD), `O_CLOEXEC | O_NONBLOCK`
  - `_Atomic int stop_flag`
  - Inject queue: `strand_inject_queue_t` (stubbed in Phase 3, real in Phase 5)
  - Poller: pointer to `strand_poller_t` (stubbed in Phase 3, real in Phase 4)
  - Stack cache: array of `void *` pointers, `cache_len`, `cache_cap` (default 64),
    idle reclamation state: last_idle_ns timestamp, floor (default 8)
  - Dead pool: linked list of reusable `strand_fiber_t` descriptors
  - `strand_fiber_t *current_fiber` — currently running fiber, NULL if in scheduler
- Implement `run_queue_push(sched, f)` — append to tail
- Implement `run_queue_pop(sched)` — remove from head, return NULL if empty

**3.3 — strand_scheduler_advance** ✓ DONE
- Implement the five-step advance per ARCHITECTURE.md §4.2:
  - Step 1: inject queue drain (no-op stub in Phase 3 — real in Phase 5)
  - Step 2: timer heap — expire all fibers whose `deadline_ns <= now_ns`,
    move to run queue tail
  - Step 3: wakeup fd drain — `read()` all available bytes, discard;
    add `SAFETY:` comment: these bytes are control signals, not fd events
  - Step 4: poll I/O with zero timeout (no-op stub — real in Phase 4)
  - Step 5: run up to `sched->budget` fibers: for each, call
    `strand_context_switch(sched->scheduler_ctx, fiber)`, set
    `sched->current_fiber = f` before switch, clear after return
- Return `SCHED_PROGRESS` if any fibers ran or any timer fired; else `SCHED_IDLE`
  with next timer deadline from heap peek, or `UINT64_MAX` if no timers

**3.4 — strand_scheduler_run and strand_scheduler_stop** ✓ DONE
- Implement `strand_scheduler_run`: loop calling `strand_scheduler_advance`;
  when `SCHED_IDLE`, call `epoll_wait`/`kevent` on the wakeup fd only
  (poller stub) with the next deadline timeout; check stop flag after each
  iteration and return when set
- Implement `strand_scheduler_stop`: `atomic_store(&sched->stop_flag, 1)`;
  write 1 byte to wakeup fd — both steps required per ARCHITECTURE.md §4.2.
  Add `SAFETY:` comment on both steps.
- Implement `strand_scheduler_next_deadline` and `strand_scheduler_get_fd`

**3.5 — strand_fiber_spawn** ✓ DONE
- Implement the within-worker path of `strand_fiber_spawn`:
  - Check stop flag — return `STRAND_ERR_SHUTDOWN` if set
  - Check `sched->current_fiber != NULL` — return error if called from host
    thread (Phase 3 only supports same-worker spawn; host-thread path in Phase 5)
  - `fiber_alloc()` from dead pool or malloc
  - `stack_alloc()` from cache or mmap
  - `strand_context_init()` to fabricate initial context
  - Set `f->state = FIBER_NEW`, then `FIBER_RUNNABLE`
  - `run_queue_push(sched, f)`
  - Return handle: `{f, f->generation}`

**3.6 — strand_fiber_yield** ✓ DONE
- Implement `strand_fiber_yield()`:
  - Assert called from a running fiber (`sched->current_fiber != NULL`)
  - Transition `f->state = FIBER_RUNNABLE`
  - Append fiber to run queue tail
  - `strand_context_switch(f, sched->scheduler_ctx)` — switch back to scheduler

**3.7 — strand_fiber_sleep_until** ✓ DONE
- Implement `strand_fiber_sleep_until(uint64_t deadline_ns)`:
  - Transition `f->state = FIBER_PARKED_TIMER`
  - Insert `(deadline_ns, f)` into timer heap
  - `strand_context_switch(f, sched->scheduler_ctx)`
  - On return: check for cancellation flag and return appropriate result

**3.8 — strand_fiber_cancel (partial)** ✓ DONE
- Implement `strand_fiber_cancel(strand_fiber_handle_t handle)` for the states
  available in Phase 3:
  - Validate handle: null check + generation check
  - `FIBER_PARKED_TIMER`: remove from timer heap, transition to
    `FIBER_RUNNABLE` with cancellation result, push to run queue
  - `FIBER_RUNNABLE` or `FIBER_RUNNING`: set `FIBER_CANCELLATION_PENDING` flag
  - `FIBER_FINISHED`: no-op
  - Stale handle: return `STRAND_HANDLE_STALE`
  - I/O and offload parked states: placeholder — implemented in Phases 4 and 5

**3.9 — Stack cache** ✓ DONE
- Implemented `sched_stack_alloc` / `sched_stack_free` in `src/strand_fiber.c`
- Updated `strand_fiber_spawn` to use `sched_stack_alloc`
- Added idle reclamation block to `strand_scheduler_advance` (Step 5)
- Added `pending_free_base/size/vg_id` to `strand_scheduler_t`; scheduler
  processes deferred stack free after each context switch returns (safe:
  executing on scheduler stack, not fiber stack — ARCHITECTURE.md §13, §4.2)
- Updated `t35_switch_back` to store stack in `pending_free_*` instead of
  calling `sched_stack_free` directly (prevents munmap of live stack)
- Updated `t35_push_fiber` and Task 3.8 helpers to use `sched_stack_alloc`
- Tests: `test_stack_cache_reuse`, `test_stack_cache_cap`,
  `test_idle_reclamation` — 28/28 passed Linux (ASan/UBSan, TSan, Valgrind)
- Stack size per cache slot must match — only cache stacks of matching size.
  Different sizes are not interchangeable.

**3.10 — Fiber-local storage**
- Implement `strand_fiber_local_set(void *ptr, strand_destructor_t dtor)`:
  set `current_fiber->local_ptr = ptr`, `current_fiber->local_dtor = dtor`
- Implement `strand_fiber_local_get()`: return `current_fiber->local_ptr`
- Destructor called by scheduler when `FIBER_FINISHED` is processed

### Tests for Phase 3

File: `tests/test_layer2.c`

- `test_advance_nonblocking`: call `strand_scheduler_advance` with no fibers;
  verify it returns without blocking (returns within a bounded time)
- `test_fiber_runs`: spawn one fiber that sets a flag; call advance; verify
  flag is set
- `test_fifo_order`: spawn fibers A, B, C; verify they run in that order
- `test_budget_limiting`: spawn 200 fibers; set budget to 64; call advance
  once; verify exactly 64 ran, not 200
- `test_yield_requeues`: fiber yields explicitly; verify it runs again on
  the next advance pass
- `test_scheduler_run_blocks`: start `strand_scheduler_run` on a thread;
  verify the thread is blocked; call `strand_scheduler_stop`; verify the
  thread returns
- `test_stop_writes_wakeup_fd`: verify that `strand_scheduler_stop` writes to
  the wakeup fd (read the fd after stop; verify bytes available)
- `test_wakeup_fd_drained_as_control`: write a byte to the wakeup fd manually;
  call advance; verify no fiber is woken (bytes discarded in Step 3, not
  treated as fiber events)
- `test_timer_fires`: sleep_until a deadline 10ms in the future; advance with
  a clock tick past the deadline; verify fiber wakes
- `test_timer_order`: two fibers with different deadlines; verify they wake in
  deadline order, not spawn order
- `test_cancel_timer`: spawn a sleeping fiber; cancel it; verify it wakes with
  cancellation result before deadline
- `test_cancel_runnable`: cancel a runnable fiber; verify cancellation flag set
- `test_stale_handle_noop`: cancel a handle after fiber finishes; verify
  `STRAND_HANDLE_STALE` returned, no crash
- `test_generation_increments_on_reuse`: spawn and finish a fiber; spawn
  another (same descriptor reused from dead pool); verify generation increased
- `test_stack_cache_reuse`: spawn a fiber, let it finish; spawn another of the
  same stack size; verify mmap is not called the second time (cache hit)
- `test_stack_cache_cap`: fill cache to cap; finish one more fiber; verify
  the excess stack is unmapped (munmap called), not cached
- `test_idle_reclamation`: fill cache past floor; idle for 5s; verify cache
  shrinks to floor
- `test_fiber_local_set_get`: set local ptr in fiber; yield; get ptr; verify
  same value across yield
- `test_fiber_local_destructor`: set local ptr with destructor; let fiber
  finish; verify destructor called with correct ptr
- `test_spawn_returns_error_after_stop`: call stop; attempt spawn; verify
  `STRAND_ERR_SHUTDOWN` returned

### Phase 3 Completion Criteria

- [ ] All Layer 2 tests pass on Linux and OpenBSD, both architectures
- [ ] `strand_scheduler_advance` verified nonblocking — quality milestone M10
- [ ] `strand_scheduler_stop` interrupts blocked worker — quality milestone M11
- [ ] Stack cache reuse verified (mmap not called on cache hit)
- [ ] Generation counter increments on descriptor reuse
- [ ] Fiber-local destructor called on completion
- [ ] Valgrind clean; ASan/UBSan clean on both platforms
- [ ] TSan clean — context switch hooks in place

---

## Phase 4 — Layer 3: I/O Integration

**Goal**: Fibers can park on fd readiness and wake correctly when the fd
becomes ready or is cancelled. The full epoll (Linux) and kqueue (OpenBSD)
integration is implemented including edge-triggered one-shot semantics,
simultaneous read/write waiters, the post-re-arm readiness check (Linux only),
error/hangup event delivery, and the two-part cancellation race rule. The
wakeup fd is correctly integrated into the scheduler's poller. All
platform-specific code is guarded by `STRAND_LINUX` / `STRAND_OPENBSD`.

**Reference documents**:
- ARCHITECTURE.md §5 — full Layer 3 specification
- ARCHITECTURE.md §5.2 — fd lifecycle contract
- ARCHITECTURE.md §5.3 — waiter model and cancellation race rule
- ARCHITECTURE.md §5.4 — Linux EPOLLET | EPOLLONESHOT
- ARCHITECTURE.md §5.5 — post-re-arm readiness check (Linux)
- ARCHITECTURE.md §5.6 — EPOLLERR/EPOLLHUP handling (Linux)
- ARCHITECTURE.md §5.7 — OpenBSD EV_DISPATCH
- ARCHITECTURE.md §5.8 — cancellation of I/O waiter
- CODING_STANDARDS.md §6.1 — fd lifecycle safety rules and O_NONBLOCK assertion
- CODING_STANDARDS.md §6.2 — SAFETY comments

**Prerequisite**: Phase 3 complete.

### Tasks

**4.1 — strand_poller_t and fd waiter table**
- Define `strand_poller_t` containing:
  - epoll fd (Linux) or kqueue fd (OpenBSD)
  - fd waiter hash table: open-addressed, sized at scheduler init
  - Per-entry: fd key, read waiter pointer, write waiter pointer,
    current event mask, registration state
    (`REGISTERED_ACTIVE` / `REGISTERED_DISABLED` / `NOT_REGISTERED`),
    per-registration token, debug generation counter
- Implement `poller_create()` / `poller_destroy()`
- Implement `fd_table_lookup(poller, fd)` — returns entry pointer or NULL
- Implement `fd_table_insert(poller, fd)` — add new entry
- Implement `fd_table_remove(poller, fd)` — remove entry (both waiters gone)

**4.2 — strand_fiber_wait_readable and strand_fiber_wait_writable**
- Validate O_NONBLOCK in debug builds: `fcntl(F_GETFL)` and assert;
  add `SAFETY:` comment
- Validate no existing waiter in same direction on this fd: error if violated
- Register fd with epoll/kqueue:
  - Linux: `epoll_ctl ADD` or `MOD` depending on registration state;
    mask is `EPOLLIN|EPOLLET|EPOLLONESHOT` or `EPOLLIN|EPOLLOUT|...` if both
    directions active
  - OpenBSD: `kevent ADD | EV_DISPATCH` for the appropriate filter
- Store waiter pointer in fd table entry
- Transition fiber to `FIBER_PARKED_IO_READ` or `FIBER_PARKED_IO_WRITE`
- `strand_context_switch(f, sched->scheduler_ctx)`
- On return: return ready/cancelled result to caller

**4.3 — poller_poll and event delivery**
- Implement `poller_poll(poller, timeout_ms)`: call `epoll_wait`/`kevent`
  with the given timeout; process all returned events via `poller_deliver_event`
- Implement `poller_deliver_event(sched, event)`:
  - Linux: check `EPOLLERR` / `EPOLLHUP` first — wake any waiter with error
    regardless of interest mask. Add `SAFETY:` comment.
  - Check returned flags for `EPOLLIN` / `EPOLLOUT` — wake corresponding waiter
  - After waking one direction, if other direction still has a waiter:
    re-arm with MOD to remaining direction; **immediately call**
    `epoll_wait(poller->epfd, events, MAX_EVENTS, 0)` (zero timeout);
    process ALL returned events via `poller_deliver_event`. Add `SAFETY:`
    comment: must process all returned events, not only the rearmed fd.
  - OpenBSD: check `EV_EOF` — wake waiter with EOF result. No post-re-arm
    check needed on OpenBSD.
- Wire `poller_poll(poller, timeout_ms)` into scheduler advance Step 4
  (replacing the stub from Phase 3). Wire the blocking poll into
  `strand_scheduler_run` idle wait (poller fd replaces wakeup-fd-only wait).

**4.4 — strand_fiber_cancel for I/O waiters**
- Extend `strand_fiber_cancel` with:
  - `FIBER_PARKED_IO_READ`: call `poller_cancel(poller, fd, DIRECTION_READ)`:
    - If other direction has no waiter: `epoll_ctl DEL`
    - Else: `epoll_ctl MOD` to remaining direction only
    - Transition fiber to `FIBER_RUNNABLE` with cancellation result
  - `FIBER_PARKED_IO_WRITE`: symmetric
  - Cross-worker path: enqueue to inject queue (inject queue real in Phase 5;
    cross-worker cancel tested fully in Phase 5)

**4.5 — Wakeup fd integration into poller**
- Register the scheduler wakeup fd with the epoll/kqueue instance so that
  `strand_scheduler_stop`'s write wakes a blocked `epoll_wait`/`kevent`
- Drain wakeup fd bytes in scheduler advance Step 3 (already implemented
  in Phase 3 stub; now wired to the real poller fd)

### Tests for Phase 4

File: `tests/test_layer3.c`

- `test_wait_readable_wakes`: write to pipe after parking fiber on read end;
  verify fiber wakes and returns ready result
- `test_wait_writable_wakes`: drain write end of pipe; park fiber on write
  readiness; fill buffer then drain; verify fiber wakes
- `test_edge_triggered_oneshot_linux`: verify fd does not re-fire on advance
  without explicit re-arm; feed data twice without re-arming; verify only one
  wake occurs
- `test_post_rearm_readiness_check_linux`: set up two waiters on same fd
  (read and write); both directions become ready simultaneously; fire event;
  verify both waiters wake — specifically that the second waiter wakes
  immediately via the post-re-arm zero-timeout check, not on a subsequent advance
- `test_all_events_processed_in_rearm_check`: two different fds both ready;
  one fires and triggers re-arm check; verify the other fd's waiter also wakes
  (zero-timeout poll must process all returned events, not only rearmed fd)
- `test_ev_dispatch_openbsd`: after EV_DISPATCH fires, verify filter does
  not re-fire without EV_ENABLE; after EV_ENABLE, if condition still true,
  verify filter fires automatically without a separate edge
- `test_cancel_read_waiter`: park fiber on read; cancel it; verify fiber
  wakes with cancellation result; verify fd is free for read direction
- `test_cancel_one_direction_leaves_other`: park two fibers on same fd (read
  and write); cancel read waiter; verify write waiter still registered and
  wakes correctly when fd becomes writable
- `test_cancel_both_directions`: cancel both waiters; verify fd fully free
  (DEL called, not MOD)
- `test_epollerr_wakes_waiter`: force EPOLLERR on fd (close write end of pipe
  with read waiter registered); verify waiter wakes with error result
- `test_epollhup_wakes_waiter`: equivalent for EPOLLHUP
- `test_ev_eof_openbsd`: close write end with kqueue read filter registered;
  verify EV_EOF wakes reader waiter
- `test_same_worker_readiness_wins`: set up cancellation and readiness in
  same advance call (Step 4 before Step 5); verify readiness result, not cancel
- `test_nonblock_assertion_debug`: in debug build, pass a blocking fd to
  `strand_fiber_wait_readable`; verify assertion fires
- `test_scheduler_stop_interrupts_io_wait`: park fiber on fd; call stop on
  worker; verify `strand_scheduler_run` returns within bounded time

### Phase 4 Completion Criteria

- [ ] All Layer 3 tests pass on Linux (epoll path)
- [ ] All Layer 3 tests pass on OpenBSD (kqueue path)
- [ ] Post-re-arm readiness check test passes — quality milestone M12
- [ ] "All events processed in re-arm check" test passes
- [ ] EPOLLERR/EPOLLHUP tests pass on Linux
- [ ] EV_EOF test passes on OpenBSD
- [ ] Valgrind clean; ASan/UBSan clean on both platforms

---

## Phase 5 — Layer 4: Multi-Worker Runtime

**Goal**: Multiple worker threads each run their own scheduler. Fibers pinned
to a worker never migrate. Host-thread `strand_fiber_spawn` distributes across
workers via round-robin. Cross-worker cancellation is delivered via the inject
queue. The offload pool runs blocking functions on plain OS threads and
delivers results back to the originating fiber. Both RESULT_CLAIMED and
CANCELLED CAS outcomes are exercised and correct. Inject queue overflow uses
exponential backoff and never drops items.

**Reference documents**:
- ARCHITECTURE.md §6 — full Layer 4 specification
- ARCHITECTURE.md §6.1 — per-worker pinned model
- ARCHITECTURE.md §6.2 — worker registration and shutdown
- ARCHITECTURE.md §6.3 — inject queue (capacity, overflow, memory ordering)
- ARCHITECTURE.md §6.4 — fiber_spawn worker selection
- ARCHITECTURE.md §6.5 — blocking syscall offload
- ARCHITECTURE.md §6.6 — offload work item lifecycle and CAS semantics
- ARCHITECTURE.md §6.7 — offload memory ordering chain
- CODING_STANDARDS.md §4 — atomic operations, CAS discipline, CONCURRENT comments
- CODING_STANDARDS.md §5.4 — cooperative scheduling contract

**Prerequisite**: Phase 4 complete.

### Tasks

**5.1 — strand_inject_queue_t**
- Implement bounded MPSC ring buffer in `src/strand_inject.c`:
  - Capacity configured at scheduler init (default: 4 × max fiber count)
  - `inject_queue_push_release`: enqueue with release memory ordering;
    exponential backoff spin on full — never drops. Add `ATOMIC:` comment.
  - `inject_queue_pop_acquire`: dequeue with acquire memory ordering.
    Add `ATOMIC:` comment.
  - `inject_queue_drain`: move all queued items to run queue — called in
    scheduler advance Step 1 (replace Phase 3 stub)

**5.2 — strand_runtime_t and worker registry**
- Implement `strand_runtime_init()` / `strand_runtime_destroy()`
- Worker list: fixed-size array of `strand_worker_t *`, protected by spinlock
- Round-robin counter: `_Atomic uint32_t` — accessed without the spinlock
- `_Atomic int shutdown_flag` — checked on all spawn paths

**5.3 — strand_worker_start**
- Allocate and initialise `strand_worker_t` (wrapper around scheduler)
- Create OS thread via `pthread_create`; thread calls `strand_scheduler_run`
- Register worker in runtime worker list
- Linux: apply `pthread_setaffinity_np` if `worker_config.cpu_affinity` set;
  document that OpenBSD does not support affinity — skip silently there

**5.4 — Host-thread strand_fiber_spawn**
- Implement the host-thread path:
  - Check shutdown flag — return `STRAND_ERR_SHUTDOWN` if set
  - Check worker list non-empty — return error if none registered
  - Select worker via round-robin (atomically increment counter, mod worker count,
    skip stopped workers)
  - Allocate fiber descriptor and stack (from target worker's cache —
    requires cross-thread-safe stack alloc path or inject-carried alloc)
  - Inject fiber to target worker's inject queue
  - Return handle

**5.5 — Cross-worker strand_fiber_cancel**
- Complete the cross-worker path in `strand_fiber_cancel`:
  - For all parked states on a different worker: enqueue cancel operation
    to target worker's inject queue
  - Document: fd freedom and state visibility guaranteed only for same-worker
    calls. Add `SAFETY:` comment.
  - Inject queue processes cancel in Step 1 before I/O poll in Step 4 —
    cross-worker cancel wins over readiness per ARCHITECTURE.md §5.3

**5.6 — strand_offload_pool_init and offload pool**
- Implement `strand_offload_pool_init(size_t thread_count)`:
  - Create N `pthread_t` offload threads, each blocking on a mutex-protected
    work queue
  - Work queue: linked list of `strand_offload_item_t *`
- Implement `strand_offload_pool_destroy()`: signal threads to exit, join all

**5.7 — strand_fiber_offload**
- Implement per ARCHITECTURE.md §6.6:
  - Validate called from a running fiber
  - Allocate work item: `refcount=2`, `state=OFFLOAD_PENDING`
  - Enqueue to offload pool work queue
  - Transition fiber to `FIBER_PARKED_OFFLOAD`
  - `strand_context_switch(f, sched->scheduler_ctx)`
  - On return: read result from `result_slot`; decrement fiber-side refcount
  - If pool full: return `STRAND_EAGAIN` immediately (do not park)
  - Add mandatory yield retry pattern comment per ARCHITECTURE.md §6.5

**5.8 — Offload thread completion and CAS**
- Offload thread on `fn(arg)` completion:
  - `CAS(state, PENDING -> RESULT_CLAIMED)` with **release** semantics.
    Add `ATOMIC:` comment with full ordering chain reference.
  - If CAS succeeds: write `result_slot`; inject completion to fiber's
    home worker via inject queue with **release** semantics
  - If CAS fails (CANCELLED already won): skip `result_slot` write entirely;
    decrement offload-side refcount; if zero: free work item
- `strand_fiber_cancel` on `FIBER_PARKED_OFFLOAD`:
  - `CAS(state, PENDING -> CANCELLED)`
  - If CAS succeeds: transition fiber to `FIBER_RUNNABLE` with cancel result;
    decrement fiber-side refcount
  - If CAS fails (RESULT_CLAIMED already won): do NOT touch refcount;
    fiber resumes normally. Add `ATOMIC:` comment: RESULT_CLAIMED wins,
    fiber-side reference still live.
  - Add `CONCURRENT:` comment on `state` field

### Tests for Phase 5

File: `tests/test_layer4.c`

- `test_inject_delivers_to_worker`: inject a fiber to a worker via host-thread
  spawn; verify it runs on the target worker's scheduler
- `test_cross_worker_wakeup`: inject an item to a sleeping worker; verify
  the worker wakes from `epoll_wait`/`kevent` and processes it
- `test_round_robin_selection`: spawn 10 fibers from host thread with 2
  workers; verify fibers spread across both workers
- `test_explicit_worker_override`: spawn with explicit worker parameter;
  verify fiber runs on specified worker
- `test_no_workers_registered_error`: call host-thread spawn before any
  `strand_worker_start`; verify error returned
- `test_spawn_during_shutdown_error`: call stop; attempt host-thread spawn;
  verify `STRAND_ERR_SHUTDOWN`
- `test_stopped_worker_excluded_roundrobin`: stop one of two workers; verify
  all subsequent round-robin spawns go to the live worker
- `test_offload_result_delivered`: offload a function that returns a known
  value; verify fiber receives correct result
- `test_offload_eagain_when_full`: fill offload pool; call offload; verify
  `STRAND_EAGAIN` returned immediately (fiber not parked)
- `test_offload_yield_retry_pattern`: fill pool; yield then retry; verify
  fiber eventually succeeds
- `test_offload_result_claimed_wins`: arrange RESULT_CLAIMED CAS to win before
  cancel arrives; verify fiber resumes with offload result, not cancel result
- `test_offload_cancelled_wins`: arrange CANCELLED CAS to win; verify fiber
  resumes with cancel result; verify `result_slot` not written
- `test_offload_refcount_released_exactly_once`: in both CAS outcomes, verify
  work item freed exactly once (use Valgrind to confirm no double-free or leak)
- `test_offload_arg_outlives_cancel`: cancel fiber while offload running;
  verify offload thread still accesses arg correctly after cancel
- `test_inject_queue_overflow_backoff`: fill inject queue; verify subsequent
  push blocks briefly then succeeds (does not drop item)

### Phase 5 Completion Criteria

- [ ] All Layer 4 tests pass on Linux and OpenBSD
- [ ] RESULT_CLAIMED and CANCELLED CAS outcomes both exercised — M13 confirmed
- [ ] Refcount freed exactly once in both CAS outcomes (Valgrind confirms)
- [ ] Round-robin and explicit override worker selection correct
- [ ] Inject queue never drops items — overflow backoff confirmed
- [ ] Valgrind clean; ASan/UBSan clean on both platforms
- [ ] TSan clean — inject queue acquire/release ordering verified

---

## Phase 6 — Layer 5: Scopes and Coordination Primitives

**Goal**: Structured concurrency scopes work correctly through all lifecycle
states and all exit variants. Error propagation, sibling cancellation, and the
walk reference rule are correct. Scope control block memory is freed at the
right time with the right owner. Fiber-local storage and its destructor work
correctly across all exit paths. This phase completes the first implementation
target: Layers 1–4 plus scopes.

**Reference documents**:
- ARCHITECTURE.md §7 — full Layer 5 scope specification
- ARCHITECTURE.md §7.1 — scope overview and operations
- ARCHITECTURE.md §7.2 — scope control block fields
- ARCHITECTURE.md §7.3 — lifecycle state machine and walk reference rule
- ARCHITECTURE.md §7.4 — scope exit variants and allocation requirement
- ARCHITECTURE.md §7.5 — error propagation
- ARCHITECTURE.md §7.6 — sibling cancellation and structured concurrency guarantee
- ARCHITECTURE.md §7.7 — detached fibers
- CODING_STANDARDS.md §4 — atomic operations on scope fields
- CODING_STANDARDS.md §6.2 — SAFETY comment on abandon stack check

**Prerequisite**: Phase 5 complete.

### Tasks

**6.1 — strand_scope_t control block**
- Define `strand_scope_t` in `src/strand_internal.h`:
  - `_Atomic scope_lifecycle_t lifecycle`
  - `_Atomic int owner_flag` (OWNER_CALLER / OWNER_RUNTIME)
  - `_Atomic int live_child_count`
  - `_Atomic int walk_ref_count`
  - `_Atomic int first_error`
  - Spawn-order intrusive list head: `strand_fiber_t *spawn_list_head`
  - `strand_fiber_handle_t parent_fiber`
  - `int cancellation_flag`
- Add `CONCURRENT:` comment on every `_Atomic` field per CODING_STANDARDS.md §4.5

**6.2 — strand_scope_open and strand_scope_spawn**
- `strand_scope_open(strand_scope_t *scope)`: memset to zero, set
  `lifecycle = SCOPE_ACTIVE`, `owner_flag = OWNER_CALLER`,
  `live_child_count = 0`, `parent_fiber = current_fiber_handle()`
- `strand_scope_spawn(strand_scope_t *scope, strand_fiber_fn_t fn, void *arg)`:
  - Spawn fiber (same worker) with `scope` pointer set
  - Append handle to spawn-order list
  - `atomic_fetch_add(&scope->live_child_count, 1)`
  - Return handle

**6.3 — scope_child_finish**
- Called by scheduler when a tracked fiber reaches `FIBER_FINISHED`:
  - If fiber returned an error: CAS `first_error` from 0 to error code;
    if CAS succeeds (first error): initiate cancellation walk
  - `atomic_fetch_sub(&scope->live_child_count, 1)` — if reaches zero:
    transition lifecycle to `SCOPE_COMPLETED` (if not already); if
    `OWNER_RUNTIME` and `walk_ref_count == 0`: free control block

**6.4 — Cancellation walk**
- `scope_walk_cancel(scope)`:
  - `atomic_fetch_add(&scope->walk_ref_count, 1)` — hold reference for walk
  - Walk spawn-order list in reverse; call `strand_fiber_cancel` on each handle
    (stale handles return `STRAND_HANDLE_STALE` — idempotent, continue)
  - Transition `SCOPE_CANCELLING -> SCOPE_DRAINING` when walk completes
  - `atomic_fetch_sub(&scope->walk_ref_count, 1)` — release reference
  - If `live_child_count == 0` and `OWNER_RUNTIME`: free control block

**6.5 — strand_scope_wait**
- Park calling fiber until `scope->lifecycle == SCOPE_COMPLETED`
- Terminal: return `scope->first_error` (0 if no error)
- After return: scope is `SCOPE_COMPLETED` and `OWNER_CALLER`

**6.6 — strand_scope_wait_timeout**
- Park with timer deadline; wake on `SCOPE_COMPLETED` or timeout
- If completed: terminal — same as `strand_scope_wait`
- If timeout: non-terminal — scope unchanged; return timeout indicator
  Caller must follow with `strand_scope_wait` or `strand_scope_abandon`

**6.7 — strand_scope_cancel**
- If `lifecycle == SCOPE_ACTIVE`: CAS to `SCOPE_CANCELLING`, initiate walk
- Non-terminal — return immediately; caller must follow with wait or abandon

**6.8 — strand_scope_abandon**
- Debug check: assert scope pointer not within current fiber's stack range.
  Add `SAFETY:` comment per CODING_STANDARDS.md §6.2.
- `atomic_store(&scope->owner_flag, OWNER_RUNTIME)`
- Set `parent_fiber` handle to null (null ptr, any generation)
- Scope pointer invalid for caller after this call

**6.9 — strand_fiber_spawn_detached**
- Spawn fiber with `scope = NULL`; not tracked; no error propagation
- Document strongly discouraged per ARCHITECTURE.md §7.7

### Tests for Phase 6

File: `tests/test_layer5.c`

- `test_scope_basic_wait`: open scope; spawn two fibers; wait; verify both
  ran before scope_wait returned
- `test_scope_active_to_completed_direct`: last child finishes with no failure
  and no cancellation while ACTIVE; verify SCOPE_COMPLETED transition
- `test_scope_error_propagates`: one child fails; verify `scope_wait` returns
  that error code
- `test_scope_first_error_wins`: two children fail; verify only first error
  code returned
- `test_scope_cancelling_to_draining`: call `scope_cancel`; verify walk runs
  and state transitions to DRAINING after walk completes
- `test_scope_cancelling_to_completed_shortcut`: last child finishes before
  cancellation walk completes; verify scope reaches COMPLETED; verify walk
  continues safely with stale handles returning STRAND_HANDLE_STALE
- `test_scope_walk_ref_prevents_free`: scope transitions to COMPLETED while
  walk is executing; verify control block not freed until walk releases
  walk_ref_count
- `test_scope_owner_runtime_frees_on_complete`: `scope_abandon`; let all
  children finish; verify control block freed (Valgrind confirms no leak)
- `test_scope_wait_timeout_scope_unchanged`: wait with short timeout before
  children finish; verify timeout return; verify scope still in previous state
- `test_scope_wait_timeout_then_abandon`: timeout; abandon; children finish;
  verify control block freed correctly
- `test_scope_owner_orthogonal_to_lifecycle`: verify all valid combinations:
  CANCELLING+OWNER_RUNTIME, DRAINING+OWNER_RUNTIME, COMPLETED+OWNER_RUNTIME,
  COMPLETED+OWNER_CALLER — each reachable by corresponding operation sequence
- `test_scope_open_from_host_thread_error`: call `scope_open` from host
  thread; verify error returned
- `test_scope_abandon_stack_alloc_debug_assert`: in debug build, pass
  stack-allocated scope to `scope_abandon`; verify assertion fires
- `test_reverse_spawn_order_cancellation`: spawn A, B, C in that order;
  trigger cancellation; verify C cancelled before B before A
- `test_stale_handles_noop`: cancel a scope whose children have already
  finished; verify walk completes without error (stale handles silently skipped)
- `test_fiber_local_destructor_on_scope_exit`: fiber sets local ptr with
  destructor; finishes; verify destructor called before `scope_wait` returns
- `test_spawn_detached_no_scope`: spawn detached fiber; verify it runs but
  is not tracked by any scope

### Phase 6 Completion Criteria

- [ ] All Layer 5 tests pass on Linux and OpenBSD — quality milestone M14
- [ ] Walk reference rule verified: control block not freed during active walk
- [ ] OWNER_RUNTIME free verified (Valgrind confirms no leak and no double-free)
- [ ] All four lifecycle state transitions exercised
- [ ] All valid OWNER × lifecycle combinations reached by tests
- [ ] Reverse spawn-order cancellation verified
- [ ] Valgrind clean; ASan/UBSan clean on both platforms
- [ ] TSan clean — all scope atomic operations verified

---

## Phase 7 — Hardening, Benchmarks, and Release

**Goal**: Full integration test suite passes. All benchmarks produce baseline
numbers. All quality milestones confirmed on all platforms. Documentation
complete. Library is ready for a v0.1.0 release tag.

**Prerequisite**: Phase 6 complete. All unit tests passing.

### Tasks

**7.1 — Integration test suite**
- Implement all integration tests in `tests/test_integration.c` per
  REPOSITORY_STRUCTURE.md §4 and TESTING.md:
  - Echo server: accept, per-connection fiber, read/write loop, scope
  - Producer-consumer pipeline: chain of fibers where each passes results
    to the next via fiber coordination; verify correct ordering and
    back-pressure behaviour across the pipeline
  - Fan-out scope: N child fibers, first error propagates, siblings cancelled
  - Timeout then abandon: `scope_wait_timeout` followed by `scope_abandon`
  - `strand_fiber_offload` with `getaddrinfo` including cancellation race
  - `strand_fiber_offload` EAGAIN and retry pattern: fill offload pool; verify
    EAGAIN returned; yield; retry; verify eventual success
  - Subprocess via fork-then-exec through offload pool
  - Multi-worker accept distribution
  - Simultaneous read/write on same fd with post-re-arm readiness check:
    verify both directions wake correctly via the zero-timeout check path
  - Guest mode: scheduler driven from host event loop
  - `strand_scheduler_stop` interrupts blocked worker within bounded time
- All integration tests pass on Linux and OpenBSD — quality milestone M15

**7.2 — Benchmark suite**
- Implement all benchmarks in `bench/` per REPOSITORY_STRUCTURE.md §5
- Run on Linux x86_64 and ARM64, OpenBSD amd64:
  - Context switch round-trip latency
  - Fiber creation throughput (cache warm and cold)
  - I/O park and wake latency
  - Scheduler throughput (fibers/second, budget sensitivity)
  - Multi-worker scaling (1..N workers)
  - Offload pool throughput
  - Cross-worker wakeup latency
- Record baseline numbers with full hardware context
- Numbers are baselines, not pass/fail gates at this stage

**7.3 — Debug watchdog**
- Implement scheduler watchdog in debug builds (`STRAND_DEBUG=1`):
  - Track wall time since a fiber started running
  - If a fiber runs for more than a configurable threshold (default 100ms)
    without yielding, emit a warning to stderr
  - Watchdog does not preempt — warning only
- Add test: spin loop fiber exceeding threshold; verify warning emitted

**7.4 — strand_inspect tool**
- Implement `tools/strand_inspect.c` per REPOSITORY_STRUCTURE.md §6:
  scheduler state dump — run queue depth, timer heap size, inject queue depth,
  fiber count by state, stack cache depth

**7.5 — Final quality pass**
- `make lint` → zero clang-tidy and cppcheck warnings on all platforms
- `make test` → all tests pass
- `make valgrind` → clean on Linux
- Full ASan/UBSan run on both platforms
- Full TSan run on Linux
- All quality milestone status cells updated

**7.6 — README.md**
- Written for an embedder coming to the project cold
- Contents: one-paragraph description, requirements (libc, pthreads),
  how to build (`make && make install`), minimal integration example
  (guest mode host loop pattern from ARCHITECTURE.md §4.3), known
  limitations (cooperative scheduling, no preemption, OpenBSD no CPU
  affinity), link to ARCHITECTURE.md for full design

### Phase 7 Completion Criteria

- [ ] Integration test suite passes on Linux and OpenBSD — M15 confirmed
- [ ] Benchmark baselines recorded on Linux x86_64 and ARM64, OpenBSD amd64
- [ ] `make lint` zero warnings on all platforms — M7, M8 confirmed
- [ ] All unit and integration tests pass on all platforms — M2, M3 confirmed
- [ ] Valgrind clean — M4 confirmed
- [ ] ASan/UBSan clean on all platforms — M5 confirmed
- [ ] TSan clean on Linux — M6 confirmed
- [ ] All quality milestone status cells updated to DONE
- [ ] README.md complete and integration example compiles and runs
- [ ] v0.1.0 release tag applied

---

## Cross-Reference Index

| Topic | Primary reference | Secondary reference |
|---|---|---|
| Context switch register set (x86_64) | ARCHITECTURE.md §3.2 | TECH_STACK.md §2.2 |
| Context switch register set (AArch64) | ARCHITECTURE.md §3.3 | TECH_STACK.md §2.3 |
| errno save/restore | ARCHITECTURE.md §3.4 | CODING_STANDARDS.md §2.4 |
| Guard page installation | ARCHITECTURE.md §3.5 | REPOSITORY_STRUCTURE.md §3 (strand_fiber.c) |
| CFI annotation rules | ARCHITECTURE.md §3.6 | CODING_STANDARDS.md §2.2 |
| ASan fiber hooks | ARCHITECTURE.md §3.7 | TECH_STACK.md §7.2 |
| TSan fiber hooks | ARCHITECTURE.md §3.7 | TECH_STACK.md §7.3 |
| Valgrind stack registration | ARCHITECTURE.md §3.7 | TECH_STACK.md §7.1 |
| Scheduler five-step advance | ARCHITECTURE.md §4.2 | REPOSITORY_STRUCTURE.md §3 (strand_sched.c) |
| Host loop integration pattern | ARCHITECTURE.md §4.3 | — |
| Fiber state machine | ARCHITECTURE.md §4.5 | REPOSITORY_STRUCTURE.md §3 (strand_fiber.c) |
| Fiber handle ABA protection | ARCHITECTURE.md §4.6 | CODING_STANDARDS.md §6.2 |
| Stack cache policy | ARCHITECTURE.md §13 | REPOSITORY_STRUCTURE.md §3 (strand_fiber.c) |
| TLS footguns | ARCHITECTURE.md §4.7 | CODING_STANDARDS.md §6.3 |
| fd lifecycle contract | ARCHITECTURE.md §5.2 | CODING_STANDARDS.md §6.1 |
| Cancellation race rule | ARCHITECTURE.md §5.3 | — |
| Post-re-arm readiness check | ARCHITECTURE.md §5.5 | CODING_STANDARDS.md §6.2 |
| EPOLLERR/EPOLLHUP handling | ARCHITECTURE.md §5.6 | — |
| OpenBSD EV_DISPATCH | ARCHITECTURE.md §5.7 | — |
| Per-worker pinned model | ARCHITECTURE.md §6.1 | REPOSITORY_STRUCTURE.md §3 (strand_runtime.c) |
| Inject queue memory ordering | ARCHITECTURE.md §6.3 | CODING_STANDARDS.md §4.2 |
| Offload work item lifecycle | ARCHITECTURE.md §6.6 | CODING_STANDARDS.md §4.3 |
| Offload memory ordering chain | ARCHITECTURE.md §6.7 | CODING_STANDARDS.md §4.2 |
| Scope lifecycle state machine | ARCHITECTURE.md §7.3 | REPOSITORY_STRUCTURE.md §3 (strand_scope.c) |
| scope_abandon allocation rule | ARCHITECTURE.md §7.4 | CODING_STANDARDS.md §6.2 |
| Walk reference rule | ARCHITECTURE.md §7.3 | — |
| Structured concurrency guarantee | ARCHITECTURE.md §7.6 | — |
| Thread safety matrix | ARCHITECTURE.md §12 | — |

---

**Document Version**: 1.0
**Last Updated**: 2026-03-28
**See Also**: PROJECT.md, ARCHITECTURE.md, TECH_STACK.md, CODING_STANDARDS.md,
REPOSITORY_STRUCTURE.md, TESTING.md
