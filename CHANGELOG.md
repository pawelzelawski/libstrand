# Changelog

## v1.2.1

Released 2026-11-05 as `v1.2.1`.

- Fix cross-thread fiber cancellation racing with descriptor reuse: generation
  checks are atomic and scheduler ownership remains immutable across reuse.
- Snapshot runtime-spawn handles before enqueueing so immediate completion and
  descriptor reuse cannot change the returned generation.
- Add concurrent cancellation/reuse and queued stale-cancellation regressions.
- Owner-run OpenBSD amd64 validation passed: dev, test (109/109), release,
  lint, real-clock, release-profile (108/108), and examples. Linux validation
  is recorded in TESTING.md.
- GitHub CI passed on Linux and OpenBSD, both amd64 and arm64.

## v1.2.0

- Make `make dev` produce a real-clock debug archive; deterministic time is
  now confined to the test-linked archive.
- Add public opaque-scope allocation through `strand_scope_create()` and
  `strand_scope_destroy()`, plus public-header-only scope and worker examples.
- OpenBSD amd64 validation passed: dev, test (107/107), release, lint,
  real-clock, release-profile (106/106), and examples.

## v1.1.0

Released 2026-08-25 as `v1.1.0`.

### Fixed

- OpenBSD kqueue registrations no longer retain pointers into a movable fd
  table, and bytes buffered before a peer close are delivered to readers.
- Fiber startup preserves the required stack alignment for arbitrary public
  stack sizes, real-clock sub-millisecond waits do not busy-spin, and the
  active scheduler accessor works while guest fibers run.
- Scope membership and ownership survive descriptor reuse and concurrent
  abandonment/completion.  Runtime, inject, and offload shutdown paths now
  protect scheduler lifetime and discard queued spawn stacks correctly.

### Added

- `strand_scheduler_spawn()` bootstraps guest-mode fibers through the public
  header.  Guest hosts must drain `strand_scheduler_advance()` until
  `STRAND_SCHED_IDLE` before blocking again.
- Release-profile and real-clock regression targets, public guest-mode
  example, and explicit Linux/OpenBSD quality-gate documentation.

### Release evidence

- Linux x86_64 local evidence and the benchmark comparison are recorded in
  [TESTING.md](TESTING.md) and [bench/BASELINES.md](bench/BASELINES.md).
- OpenBSD amd64 owner-run evidence: `make dev`, `make test` (107/107),
  `make release`, `make lint`, `make test-real-clock`, `make test-release`
  (106/106), and `make examples` all passed on 2026-08-25.  OpenBSD sanitizer
  support is not claimed.
