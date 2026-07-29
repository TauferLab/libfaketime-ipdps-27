# Shared virtual-clock microbenchmark

## Method

`clock_benchmark.c` uses the same optimized C loop and
`clock_gettime(CLOCK_REALTIME)` call in all modes. It performs 100,000 warm-up
calls followed by 1,000,000 timed calls. Elapsed time comes from a direct
`SYS_clock_gettime(CLOCK_MONOTONIC_RAW)` syscall so libfaketime cannot alter the
measurement clock. A volatile accumulator prevents dead-call elimination.

`run_benchmark.sh` ran nine independent repetitions in a randomized mode order,
pinned to CPU 0. It compiled with GCC 8.5.0 and
`-O2 -std=gnu11 -Wall -Wextra -Werror`. Legacy mode used an atomic temporary
file plus rename on `/dev/shm` and `FAKETIME_NO_CACHE=1`. Shared mode set an
initial target, then called `faketime_set_realtime_ns()`. Both fake modes
verified that a dynamic target update was observed before timing reads.

Update latency is the per-call average of 1,000 consecutive updates in each
repetition. This excludes one-time symbol resolution and shared-page
attachment, matching Flux Fiction's cached `ctypes` binding during repeated
simulation advances.

Exact command:

```bash
CC=/usr/bin/gcc CALLS=1000000 REPETITIONS=9 UPDATE_CALLS=1000 \
  benchmarks/shared-clock/run_benchmark.sh
```

Machine details are in `machine_metadata.txt`; raw rows, including randomized
order and every repetition, are in `raw_results.csv`.

## Realtime-read results

| Mode | Median latency | IQR | Min–max | Relative to native |
|---|---:|---:|---:|---:|
| Native | 23.76 ns | 0.22 ns | 23.55–24.13 ns | 1.00× |
| Legacy timestamp file | 22,733.17 ns | 276.70 ns | 22,528.98–23,068.33 ns | 956.72× |
| Shared-memory clock | 52.92 ns | 1.02 ns | 51.60–53.54 ns | 2.23× |

The shared clock was **429.54× faster than legacy timestamp-file reads** on
this node and remained within 29.16 ns/call of native. This result measures
only intercepted reads; it does not place an update in the timed read loop.

## Dynamic-update results

| Mode | Median update | IQR | Min–max |
|---|---:|---:|---:|
| Legacy atomic timestamp-file replacement | 57.78 µs | 0.71 µs | 55.93–59.62 µs |
| Shared `faketime_set_realtime_ns()` | 0.129 µs | 0.016 µs | 0.099–0.152 µs |

## Archived mmap experiment diagnostic

The archived `experiment/timestamp-mmap` commit `d1af031` was rebuilt with the
same host compiler and sampled with the same million-call benchmark loop. Its
three raw latencies were 1,063.64, 1,065.95, and 1,064.04 ns/call (median
1,064.04 ns/call), about 20× slower than the final shared clock.

Source inspection explains that gap. The experiment avoids timestamp-file I/O
on unchanged reads, but still enters the complete legacy `fake_clock_gettime()`
path: the wrapper obtains the requested real time, the legacy core obtains
realtime again for cache bookkeeping, the multithreaded library locks its time
mutex, and the code performs cache/mode checks and updates process-local time
caches. Its public update function also opens, `fstat`s, maps, unmaps, and closes
the control-page path on every update; text-mode updates additionally parse a
timestamp specification. The final implementation bypasses that machinery for
supported realtime clocks and updates an already-mapped page.

## Hot-path syscall audit

Exact command:

```bash
benchmarks/shared-clock/verify_no_hotpath_files.sh
```

The script traces initialization, warm-up, and 10,000 optimized reads, then
examines only calls between explicit hot-path begin/end markers. It found no
`open`, `read`, `stat`, `access`, or path lookup. The complete trace and the
extracted marked region are saved as `shared_clock_strace.log` and
`shared_clock_hot_path.strace`.

## Plots

The log-scaled box-and-point plot preserves all nine repetitions and is saved
as:

- `shared_clock_latency.png`
- `shared_clock_latency.pdf`
- `shared_clock_latency.svg`

These benchmark assets live with libfaketime because they directly exercise
its C ABI and hot path independently of Flux Fiction or Python. The Flux
Fiction repository contains the separate end-to-end smoke comparison.
