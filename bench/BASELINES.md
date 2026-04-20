# libstrand Benchmark Baselines

Baseline numbers recorded per DEVELOPMENT.md Task 7.2.
Numbers are reference points, not pass/fail gates.
All benchmarks built with `-O2 -DNDEBUG` (release flags, no sanitizers).

---

## Linux x86_64

**Hardware**: AMD Ryzen 7 4800H with Radeon Graphics  
**CPUs**: 16 online (8 cores × 2 SMT threads)  
**OS**: Linux (kernel 6.x)  
**Compiler**: Clang  
**Date**: 2026-04-20

### bench_context_switch

```
  single-fiber yield (ns/switch)                    26 ns/op      37964 Kop/s
  two-fiber ping-pong (ns/switch)                   52 ns/op      18986 Kop/s
  switch with errno save/restore (ns/switch)        27 ns/op      36282 Kop/s
  switch with MXCSR save/restore (ns/switch)        34 ns/op      29126 Kop/s
```

Notes:
- Single yield = 26 ns/switch (2 context switches per `strand_fiber_yield`).
- Ping-pong doubles latency (two fibers interleaved through scheduler).
- MXCSR save adds ~8 ns vs baseline (SSE control register read/write overhead).

### bench_fiber_spawn

```
  fiber spawn cold (stack mmap, ns/spawn)         6483 ns/op        154 Kop/s
  fiber spawn warm (cache hit, ns/spawn)          6286 ns/op        159 Kop/s
```

Notes:
- Warm path (stack cache hit) is ~3% faster than cold (mmap).
- Dominant cost is `mprotect` for the guard page, even on cache hits.

### bench_io_roundtrip

```
  I/O park+wake pre-ready (ns/cycle)              4430 ns/op        225 Kop/s
```

Notes:
- Measures full park→epoll_wait→deliver cycle through a pre-written pipe.
- ~170× slower than a raw context switch due to syscall overhead.

### bench_scheduler

64 fibers × 1000 yields each = 64000 total fiber-run events.

```
  scheduler budget=8   (ns/fiber-run)               243 ns/op       4099 Kop/s
  scheduler budget=16  (ns/fiber-run)               139 ns/op       7155 Kop/s
  scheduler budget=32  (ns/fiber-run)                87 ns/op      11374 Kop/s
  scheduler budget=64  (ns/fiber-run)                63 ns/op      15828 Kop/s
  scheduler budget=128 (ns/fiber-run)                50 ns/op      19856 Kop/s
  scheduler budget=256 (ns/fiber-run)                43 ns/op      23118 Kop/s
```

Notes:
- Throughput roughly doubles from budget=8 to budget=32 (amortised advance overhead).
- Diminishing returns above budget=64; default budget=64 is a good balance.

### bench_multiworker

10000 fibers × 10 yields each.

```
  multi-worker 1 worker(s) (ns/fiber)            11748 ns/op         85 Kop/s
  multi-worker 2 worker(s) (ns/fiber)            11437 ns/op         87 Kop/s
  multi-worker 3 worker(s) (ns/fiber)            11313 ns/op         88 Kop/s
  multi-worker 4 worker(s) (ns/fiber)            13838 ns/op         72 Kop/s
  multi-worker 5 worker(s) (ns/fiber)            12089 ns/op         82 Kop/s
  multi-worker 6 worker(s) (ns/fiber)            12590 ns/op         79 Kop/s
  multi-worker 7 worker(s) (ns/fiber)            11559 ns/op         86 Kop/s
  multi-worker 8 worker(s) (ns/fiber)            13237 ns/op         75 Kop/s
```

Notes:
- Throughput is roughly flat across worker counts because work is embarrassingly
  parallel and the bottleneck is fiber spawn cost, not scheduler contention.
- Variance at 4 and 8 workers reflects NUMA/SMT effects on the 4800H.

### bench_offload

5000 sequential `noop_fn` offload calls per pool size.

```
  offload pool=1  thread(s) (ns/offload)         18554 ns/op         53 Kop/s
  offload pool=2  thread(s) (ns/offload)         18205 ns/op         54 Kop/s
  offload pool=4  thread(s) (ns/offload)         18055 ns/op         55 Kop/s
  offload pool=8  thread(s) (ns/offload)         19150 ns/op         52 Kop/s
  offload pool=16 thread(s) (ns/offload)         17821 ns/op         56 Kop/s
```

Notes:
- Sequential single-fiber offload: pool size has minimal effect since only
  one item is in-flight at a time.
- ~18 µs per call = fiber park + offload thread wake + inject + fiber resume.
- Pool creation overhead excluded; measured only the park→inject→resume path.

### bench_cross_worker

10000 cancel→resume round-trips across two workers.

```
  cross-worker cancel->resume (ns/wakeup)         7897 ns/op        126 Kop/s
```

Notes:
- ~8 µs cross-worker wakeup latency on shared-memory SMT system.
- Dominated by the eventfd write + epoll_wait wake latency between cores.

---

## Linux ARM64

*Pending - to be recorded on ARM64 hardware.*

---

## OpenBSD amd64

*Pending - to be recorded on OpenBSD hardware.*

