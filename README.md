# libstrand

A stackful fiber concurrency library for C.  Designed as an embeddable
runtime for connection-oriented network daemons on Linux and OpenBSD.

C programs handling many concurrent connections must either use callbacks
(fragmenting logic into state machines) or spawn one thread per connection
(consuming kernel resources at scale).  libstrand provides stackful fibers
that preserve sequential control flow without per-connection thread overhead.

## Requirements

- C11 compiler (GCC or Clang)
- POSIX libc with pthreads
- Linux (x86_64, AArch64) or OpenBSD (amd64, arm64)
- GNU Make

No third-party dependencies.

## Building

```sh
make release    # optimised build (-O2, no sanitizers)
make dev        # debug build (ASan + UBSan)
make test       # build and run test suite
make valgrind   # run tests under Valgrind (Linux)
make test-tsan  # run tests under TSan (Linux, Clang only)
make lint       # clang-tidy + cppcheck
make install    # install to /usr/local (or PREFIX=...)
```

The build produces `build/libstrand.a` and the public header is
`include/strand.h`.

## Quick Start - Worker Mode

The primary usage pattern: the runtime owns worker threads; the host
thread spawns fibers and shuts down when done.

```c
#include <strand.h>
#include <unistd.h>

/*
 * Per-connection fiber: parks on I/O, echoes data back.
 * Each connection runs in its own fiber on the assigned worker.
 */
static void echo_fiber(void *arg) {
    int fd = *(int *)arg;
    strand_scheduler_t *sched = strand_fiber_self_scheduler();
    char buf[256];
    ssize_t n;

    for (;;) {
        /* Park until data arrives - releases the worker. */
        if (strand_fiber_wait_readable(sched, fd) != STRAND_OK)
            break;
        n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        /* Park until the socket is writable. */
        if (strand_fiber_wait_writable(sched, fd) != STRAND_OK)
            break;
        write(fd, buf, (size_t)n);
    }
    close(fd);
}

int main(void) {
    strand_runtime_t *rt = strand_runtime_init(NULL);
    strand_worker_start(rt, NULL);   /* one worker thread */

    /* For each accepted connection, spawn a fiber: */
    int conn_fd = accepted_fd;
    strand_runtime_spawn(rt, echo_fiber, &conn_fd, 0, NULL, NULL);

    /* ... run until shutdown ... */
    strand_runtime_destroy(rt);      /* stops workers, joins, frees */
    return 0;
}
```

## Guest Mode (Host Loop Integration)

For embedders who already own an event loop: get the scheduler fd with
`strand_worker_get_scheduler()` + `strand_scheduler_get_fd()`, register
it with your epoll/kqueue instance, and call `strand_scheduler_advance()`
when it fires.  An unconditional trailing advance after every poll return
is required for correct timer expiry.  See ARCHITECTURE.md §4.3 for the
full host loop pattern.

## Performance

Measured on AMD Ryzen 7 4800H (Linux x86_64), release build (`-O2 -DNDEBUG`):

| Operation | Latency |
|---|---|
| Context switch (single-fiber yield) | 26 ns |
| Fiber spawn (warm, stack cache hit) | 117 ns |
| Scheduler throughput (budget=64) | 63 ns/fiber-run (~16M ops/s) |

Numbers vary by platform and hardware.  See `bench/BASELINES.md` for full
results on Linux x86_64, Linux AArch64, and OpenBSD amd64.

## Known Limitations

- **Cooperative scheduling only.**  No preemption.  Fibers that do not
  yield or park starve the worker.
- **No work stealing.**  Fibers are pinned to their assigned worker and
  never migrate.
- **No CPU affinity on OpenBSD.**  The `cpu_affinity` config field is
  silently ignored (no platform API available).
- **x87 FP state not saved.**  Code using `long double` across yield
  points may see corrupted results.  SSE/NEON state is saved.
- **No fiber APIs from signal handlers.**  All fiber and scheduler APIs
  are unsafe to call from signal handler context.
- **fork() after runtime init without exec() is undefined behaviour.**
- **Cross-worker wakeup latency on Linux is ~7 µs** (vs ~3 µs on
  OpenBSD) due to CFS scheduler wakeup cost.  See ARCHITECTURE.md §6.3.1.

## Documentation

- [`include/strand.h`](include/strand.h) - public API with inline
  documentation; the primary reference for embedders
- [ARCHITECTURE.md](ARCHITECTURE.md) - full five-layer design document
- [CODING_STANDARDS.md](CODING_STANDARDS.md) - code style and conventions
- [DEVELOPMENT.md](DEVELOPMENT.md) - phased implementation plan
- [TESTING.md](TESTING.md) - test strategy and quality milestones

## License

[ISC](LICENSE)
