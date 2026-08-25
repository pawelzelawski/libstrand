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
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

struct connection {
    int fd;
};

static void echo_fiber(void *arg) {
    struct connection *conn = arg;
    int fd = conn->fd;
    strand_scheduler_t *sched = strand_fiber_self_scheduler();
    char buf[256];
    ssize_t n;

    free(conn);       /* this fiber owns the heap-allocated argument */
    for (;;) {
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

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);

    return flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1
        ? -1 : 0;
}

/* Call after accept(). rt must outlive every spawned connection fiber. */
static int spawn_connection(strand_runtime_t *rt, int fd) {
    struct connection *conn = malloc(sizeof(*conn));
    int rc;

    if (conn == NULL || set_nonblocking(fd) != 0) {
        free(conn);
        close(fd);
        return -1;
    }
    conn->fd = fd;
    rc = strand_runtime_spawn(rt, echo_fiber, conn, 0, NULL, NULL);
    if (rc != STRAND_OK) {
        free(conn);
        close(fd);
    }
    return rc;
}
```

Set every fd passed to `strand_fiber_wait_readable()` or
`strand_fiber_wait_writable()` to `O_NONBLOCK`. A fiber argument must remain
valid until its entry function takes ownership; do not pass the address of a
short-lived stack variable to an asynchronous spawn.

## Guest Mode (Host Loop Integration)

For embedders who already own an event loop, create a guest scheduler with
`strand_scheduler_create()`, bootstrap roots with `strand_scheduler_spawn()`,
and register `strand_scheduler_get_fd()` with the host epoll/kqueue instance.
After every host poll return, call `strand_scheduler_advance()` repeatedly
until it returns `STRAND_SCHED_IDLE` before blocking again. This handles timer
expiry and ensures that a budget-limited pass cannot leave runnable fibers
stranded behind the host poll. See `examples/guest_mode.c` and
ARCHITECTURE.md §4.3.

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
