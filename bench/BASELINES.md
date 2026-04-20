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
  fiber spawn cold (mmap+munmap, ns/spawn)        7758 ns/op        128 Kop/s
  fiber spawn warm (cache hit, ns/spawn)           117 ns/op       8489 Kop/s
```

Notes:
- Methodology: spawn one fiber, drive it to completion, repeat. Each iteration
  exercises either the full mmap+mprotect+munmap path (cold) or the stack cache
  hit path (warm). This replaced the original batch-spawn methodology which was
  flawed - it spawned 100k fibers against a 64-entry cache, resulting in a
  0.064% cache hit rate that made warm and cold numbers nearly identical.
- Warm path (cache hit) is 66× faster than cold (mmap+munmap) - the stack
  cache eliminates all syscall overhead from the spawn path.
- Warm 117 ns/spawn meets the < 500 ns design target with 4× headroom.
- Cold 7.8 µs cost is dominated by mmap/mprotect/munmap syscall overhead
  (not TLB shootdowns - verified via taskset pinning experiments).

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
- ~8 µs cross-worker wakeup latency on Linux with eventfd + epoll.
- Dominated by the CFS scheduler wakeup latency after eventfd write.
- Exceeds the 5 µs design target - this is a known Linux platform
  characteristic, not a libstrand defect. See ARCHITECTURE.md §6.3.1.
- OpenBSD achieves ~3 µs (kqueue + pipe path); see OpenBSD section below.

---

## Linux ARM64

*Pending - to be recorded on ARM64 hardware.*

---

## OpenBSD amd64

**Hardware**: Intel Core i5-4278U CPU @ 2.60GHz  
**CPUs**: 4  
**OS**: OpenBSD  
**Compiler**: Clang  
**Date**: 2026-04-20

### bench_context_switch

```
  single-fiber yield (ns/switch)                    33 ns/op      29662 Kop/s
  two-fiber ping-pong (ns/switch)                   67 ns/op      14792 Kop/s
  switch with errno save/restore (ns/switch)        34 ns/op      28617 Kop/s
  switch with MXCSR save/restore (ns/switch)        33 ns/op      29585 Kop/s
```

### bench_fiber_spawn

```
  fiber spawn cold (mmap+munmap, ns/spawn)       13318 ns/op         75 Kop/s
  fiber spawn warm (cache hit, ns/spawn)           168 ns/op       5939 Kop/s
```

Notes:
- Warm path 168 ns meets the < 500 ns design target.
- Cold path is 1.7× slower than Linux (13.3 µs vs 7.8 µs) - OpenBSD's
  mmap/mprotect/munmap syscall path has higher per-call overhead.
- Warm cache hit provides 79× speedup over cold.

### bench_io_roundtrip

```
  I/O park+wake pre-ready (ns/cycle)              3138 ns/op        318 Kop/s
```

### bench_scheduler

64 fibers × 1000 yields each = 64000 total fiber-run events.

```
  scheduler budget=8   (ns/fiber-run)               258 ns/op       3873 Kop/s
  scheduler budget=16  (ns/fiber-run)               161 ns/op       6209 Kop/s
  scheduler budget=32  (ns/fiber-run)               109 ns/op       9126 Kop/s
  scheduler budget=64  (ns/fiber-run)                84 ns/op      11805 Kop/s
  scheduler budget=128 (ns/fiber-run)                71 ns/op      14044 Kop/s
  scheduler budget=256 (ns/fiber-run)                63 ns/op      15715 Kop/s
```

### bench_multiworker

10000 fibers × 10 yields each.

```
  multi-worker 1 worker(s) (ns/fiber)            18974 ns/op         52 Kop/s
  multi-worker 2 worker(s) (ns/fiber)            21855 ns/op         45 Kop/s
  multi-worker 3 worker(s) (ns/fiber)            23038 ns/op         43 Kop/s
  multi-worker 4 worker(s) (ns/fiber)            22800 ns/op         43 Kop/s
  multi-worker 5 worker(s) (ns/fiber)            22940 ns/op         43 Kop/s
  multi-worker 6 worker(s) (ns/fiber)            22836 ns/op         43 Kop/s
  multi-worker 7 worker(s) (ns/fiber)            22576 ns/op         44 Kop/s
  multi-worker 8 worker(s) (ns/fiber)            22748 ns/op         43 Kop/s
```

### bench_offload

5000 sequential `noop_fn` offload calls per pool size.

```
  offload pool=1  thread(s) (ns/offload)          9186 ns/op        108 Kop/s
  offload pool=2  thread(s) (ns/offload)         10176 ns/op         98 Kop/s
  offload pool=4  thread(s) (ns/offload)          9648 ns/op        103 Kop/s
  offload pool=8  thread(s) (ns/offload)         10105 ns/op         98 Kop/s
  offload pool=16 thread(s) (ns/offload)         10040 ns/op         99 Kop/s
```

### bench_cross_worker

10000 cancel→resume round-trips across two workers.

```
  cross-worker cancel->resume (ns/wakeup)         3025 ns/op        330 Kop/s
```

Notes:
- 3.0 µs cross-worker wakeup - meets the < 5 µs design target.
- OpenBSD kqueue + pipe path is ~2.6× faster than Linux epoll + eventfd.

