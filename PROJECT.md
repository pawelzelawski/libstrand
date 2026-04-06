# libstrand

## Overview

libstrand is a C11 library that brings fiber-based cooperative concurrency to
programs targeting Linux and OpenBSD on x86_64 and ARM64. It provides
stackful fibers, a cooperative scheduler, epoll/kqueue I/O integration,
multi-worker parallelism, and structured concurrency scopes — all as a guest
in the programmer's program. The library does not own the process, does not
take over the main loop, and does not impose a framework. The programmer
calls libstrand; libstrand does not call the programmer back.

The design philosophy is the same as the rest of this author's infrastructure
libraries: zero mandatory third-party dependencies, explicit ownership
contracts, first-class OpenBSD support, and code that can be audited,
understood, and trusted.

## Philosophy

### The Problem This Library Solves

The classical C concurrency architecture — a fixed thread pool with epoll or
kqueue for I/O multiplexing — handles connections with simple per-connection
logic elegantly. The pain appears when per-connection or per-session logic
grows in complexity. Consider a single logical operation that must authenticate
against a remote service, acquire a distributed lock, fan out requests to
multiple peers and wait for a quorum, stream a response back in chunks, handle
a timeout at any of those stages, and clean up correctly whether it succeeded,
failed, or was cancelled.

In the thread pool + epoll model, each wait point is a state transition. The
programmer must explicitly encode where the operation is in its lifecycle, save
all intermediate state, register the next event, and return. Logic that would
be ten lines of sequential code becomes a state machine with many states,
explicit intermediate storage, and transitions that are easy to get wrong —
especially around error paths and cancellation. The code no longer reads like
the problem it is solving.

This is the **state machine explosion problem**. It is a correctness and
maintainability problem, not a performance problem.

libstrand solves it by allowing the programmer to write concurrent logic as
sequential code. A fiber parks at each wait point and the scheduler handles
the rest. The code reads like the problem. Error handling lives in one place.
Cancellation propagates through the call stack rather than being threaded
through every state transition.

### Primary and Secondary Goals

The primary goal is **expressiveness** — eliminating the state machine
explosion problem for C programmers who want to write concurrent logic
without losing readability or correctness. Once the expressiveness problem is
solved with fibers, a secondary benefit follows: fibers are cheaper than OS
threads, so the same expressive programming model becomes affordable at large
scale. A kernel thread context switch costs roughly 1–10 µs. A userspace
fiber switch costs roughly 100–300 ns. At scale this difference compounds
into infrastructure cost, energy consumption, and user-visible latency. But
this is the secondary argument. The primary argument is that the code is
better.

### Library, Not Framework

libstrand is a library. The direction of control flow always remains with the
programmer's code. Layers 1 and 2 (execution contexts and the fiber scheduler)
are fully guest-capable — the programmer creates a scheduler object, calls
`strand_scheduler_advance()` from their own event loop, and the scheduler does
its work without taking ownership of any thread. Layers 4 and 5 (the
multi-worker runtime and coordination primitives) are more accurately described
as an embeddable runtime, but even there the programmer starts and stops
workers explicitly and owns the shutdown sequence. The library never calls
`main()`.

### Core Principles

1. **Guest not host**: The programmer drives the scheduler. The library does
   not own the process, the main loop, or any OS thread it was not explicitly
   given.

2. **Zero mandatory dependencies**: No external libraries. No allocator
   framework, no portability layer, no protocol helpers beyond what the OS
   provides. The zero-dependency requirement is what makes libstrand auditable,
   embeddable, and buildable on a fresh system with a C11 compiler and make.

3. **Explicit ownership contracts**: Every fiber has a known owner at every
   point in its lifecycle. Every scope has a defined cleanup guarantee. Every
   fd lifecycle rule is stated precisely and enforced in debug builds. The API
   surface makes ownership decisions visible rather than burying them in prose.

4. **Per-worker pinned model**: No fiber ever migrates between workers. Each
   worker thread owns its scheduler, I/O poller, timer heap, and all fibers
   assigned to it. Cross-worker interaction happens only through inject queues
   and wakeup signals. This eliminates migration races, simplifies the memory
   model, and matches the natural load-balancing point for connection-oriented
   workloads — connection acceptance time.

5. **Honest cooperative scheduling**: libstrand does not preempt fibers.
   Uncooperative fibers stall their worker. The documentation states this
   clearly and provides tools to detect it. A library that overpromises its
   scheduling guarantees is worse than one that understates them.

6. **Boring technology**: Open-addressed hash tables, intrusive lists, a
   binary heap for timers, and platform assembly for context switching.
   Everything is well-understood, correctly specified, and implementable in
   auditable C.

## The Five Layers

libstrand is organised as five separable layers. Lower layers can be used
independently. Each layer is tested before the next is built.

### Layer 1 — Execution Contexts

Raw save/restore of execution state. Implemented in x86_64 and ARM64 assembly.
Saves and restores callee-saved general-purpose registers, the stack pointer,
errno, and floating-point control registers (MXCSR on x86_64; FPCR and FPSR
on AArch64). Guard pages on fiber stacks. Correct CFI unwind annotations
throughout. No global state. This layer has no knowledge of scheduling — it
only knows how to switch from one execution context to another.

### Layer 2 — Fiber Scheduler

A cooperative scheduler built on Layer 1. One OS thread, many fibers. The
scheduler object is explicit and caller-owned. Two operating modes:

- **Guest mode** (`strand_scheduler_advance`): nonblocking, called from the
  programmer's own event loop. Returns immediately after processing available
  work — timers, injected fibers, I/O events, and up to a configurable budget
  of runnable fibers.
- **Worker mode** (`strand_scheduler_run`): blocking, owns the calling thread
  until `strand_scheduler_stop` is called. Used when the programmer hands a
  thread to libstrand to manage.

### Layer 3 — I/O Integration

Connects the scheduler to epoll (Linux) or kqueue (OpenBSD). Wraps I/O
parking — the fiber calls `strand_fiber_wait_readable` or
`strand_fiber_wait_writable`, parks, and resumes when the fd is ready or the
wait is cancelled. The library never modifies fd flags; O_NONBLOCK is always
the caller's responsibility. Edge-triggered one-shot semantics on Linux
(EPOLLET | EPOLLONESHOT). EV_DISPATCH on OpenBSD.

### Layer 4 — Multi-Worker Runtime

Multiple OS threads each running their own Layer 2 scheduler. Per-worker
pinned — fibers do not migrate. Fiber spawning from the host thread uses
round-robin worker selection with optional explicit override. Cross-worker
operations (cancellation, scope signalling, offload completion) use bounded
inject queues and wakeup signals. The optional offload pool handles blocking
syscalls (getaddrinfo, regular file I/O, fsync) by running them on plain OS
threads and injecting the result back to the originating fiber.

### Layer 5 — Coordination Primitives

Fiber-aware synchronisation and structured concurrency built on Layers 1–4:

- **Structured concurrency scopes** (`strand_scope_open`,
  `strand_scope_wait`, `strand_scope_abandon`): scoped fiber lifetimes with
  guaranteed cleanup, first-error propagation, reverse-spawn-order
  cancellation, and orthogonal owner/lifecycle state.
- **Fiber-local storage** (`strand_fiber_local_set`,
  `strand_fiber_local_get`): single void* slot per fiber with destructor.
- **Cancellation** (`strand_fiber_cancel`): cancels any parked state.
  Cooperative — fibers observe cancellation at park points.
- **Timers** (`strand_fiber_sleep_until`): deadline-based sleep, integrated
  with the per-worker timer heap.

  *Layer 5 full scope (channels, mutexes, and additional timer APIs) is
  deferred to a subsequent specification and implementation phase. The first
  implementation phase delivers Layers 1–4 and the scope primitive.*

## What libstrand Is For

libstrand is aimed at C programmers building:

- **Connection-oriented network daemons**: protocol servers, relay servers,
  proxies, load balancers — any program where the dominant bottleneck is
  per-connection logic complexity rather than raw throughput.
- **Distributed system components**: storage nodes, coordination daemons,
  anything with fan-out/fan-in patterns, distributed lock acquisition, or
  multi-step async protocols expressed in C.
- **Embedded event loop integration**: programs that already have a main
  event loop (epoll, kqueue, or custom) and want to add fiber concurrency
  without surrendering control. Guest mode supports exactly this.

libstrand is **not** aimed at:

- Programs that need preemptive scheduling or bounded worst-case scheduling
  latency. libstrand is cooperative. An uncooperative fiber stalls its worker.
- CPU-bound parallel workloads. For parallel computation on multiple cores,
  OS threads are the right primitive. libstrand's multi-worker model is
  designed for I/O-bound concurrency, not CPU parallelism.
- Programs targeting platforms other than Linux and OpenBSD on x86_64 or
  ARM64. Porting is possible but not a goal.

## What libstrand Explicitly Does Not Do

- Does not own an event loop or intercept the programmer's main loop
- Does not wrap, replace, or intercept standard library functions
- Does not intercept or handle signals
- Does not migrate fibers between workers
- Does not implement work stealing
- Does not preempt fibers
- Does not modify fd flags (O_NONBLOCK is always the caller's responsibility)
- Does not register `pthread_atfork` handlers
- Does not implement garbage collection
- Does not have mandatory third-party dependencies
- Does not require a non-standard build system

## Current Status

**Architecture phase complete.** The full five-layer architecture has been
designed and reviewed. No code has been written yet.

**In progress**: documentation phase - producing the specification documents 
that will drive implementation.


| Phase | Name | Status |
|---|---|---|
| — | Architecture | COMPLETE |
| — | Documentation | IN PROGRESS |
| 1 | Layer 1: Execution Contexts | NOT STARTED |
| 2 | Layer 2: Fiber Scheduler | NOT STARTED |
| 3 | Layer 3: I/O Integration | NOT STARTED |
| 4 | Layer 4: Multi-Worker Runtime | NOT STARTED |
| 5 | Layer 5: Scopes and Coordination Primitives | NOT STARTED |
| 6 | Hardening, Benchmarks, and Release | NOT STARTED |

## Relationship to Existing Libraries

### libmill / libdill

Martin Sustrik's libmill and libdill are the closest prior work. libmill
provides a serious 1:N fiber implementation with epoll/kqueue, channels,
timers, and stack caching. libdill adds structured concurrency. Both are
unmaintained. Both require surrendering the main loop. Both have macro-heavy
APIs. libstrand learns from their design while targeting a different
integration model (guest-capable, no main loop requirement) and a maintained
codebase.

### libco / minicoro / aco

These are bare context-switching primitives only — equivalent to Layer 1 of
libstrand. They provide no scheduler, no I/O integration, no multi-worker
support, and no structured concurrency. libstrand is a complete concurrency
library built on top of the same class of mechanism.

### libuv / libevent / libev

These are event loop libraries. They multiplex I/O events and timers but
provide no fiber scheduling. Programs using them still write state machines
for complex per-connection logic. libstrand can coexist with these libraries
in guest mode but addresses a different problem.

### Go runtime / goroutines

Go's goroutine scheduler is the clearest existence proof that the fiber model
is correct for the problem class. libstrand borrows the stackful fiber
primitive and I/O parking model from Go's design, adds structured concurrency
from the Trio/Swift Concurrency tradition, and delivers it in a library-mode
C11 form factor that does not require surrendering control to a language
runtime.

## License

ISC License. Simple, permissive, compatible with OpenBSD philosophy.

## Document Index

| Document | Audience | Purpose |
|---|---|---|
| PROJECT.md | Both | Overview, goals, scope, design philosophy (this file) |
| ARCHITECTURE.md | Implementer | Full technical architecture — layers, state machines, data structures, platform specifics, decision rationale |
| TECH_STACK.md | Implementer | Build system, compiler flags, assembly conventions, sanitizer integration, tooling |
| CODING_STANDARDS.md | Implementer | C11 style, assembly conventions, naming, error handling, atomic operations, documentation requirements |
| REPOSITORY_STRUCTURE.md | Implementer | Directory layout, file-by-file descriptions, layer-to-file mapping |
| DEVELOPMENT.md | Implementer | Phased build plan, milestones, per-layer tasks, test and benchmark requirements |
| TESTING.md | Implementer primary, reviewer secondary | Test strategy, unit and integration test catalogue, sanitizer testing, platform testing, CI approach |

---

**Document Version**: 1.0
**Last Updated**: 2026-03-28
**Status**: Documentation phase — no code written yet
