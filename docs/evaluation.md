# Evaluation procedure

## Research questions

1. Does submission/completion batching reduce submission calls per operation or
   server CPU cost for the same workload?
2. How does queue depth change throughput and tail latency as concurrency rises?
3. Does the validated LRU cache reduce file-read operations and CPU cost for
   repeated small files? How much memory does it retain?
4. How do persistent connections change throughput and latency compared with
   reconnecting for every request?

The benchmark baseline is the maintained C++ implementation with batch size 1
and the LRU disabled. This isolates optimizations while preserving HTTP behavior
and correctness. It is not the historical C server, which has different protocol
behavior and known correctness problems. An epoll comparison is future work.

## Build and regression gate

Use a fresh Release build for performance. Run both normal and ASan/UBSan tests
before collecting results. Do not benchmark sanitizer builds. Keep the compiler,
optimization flags, kernel, filesystem, machine, and power configuration constant.
Record git revision and uncommitted changes; commit the final submission before
its final evaluation. The runner records the binary SHA-256 as well.

## Workload matrix

The default runner uses four profiles, 1 KiB and 1 MiB files, concurrency 1/16/64,
keep-alive on/off, three repeats, five seconds per measured interval, and a
one-second warmup. Jobs are shuffled with a fixed seed. The cache profile has a
256 KiB eligibility limit, so the large file is a deliberate bypass control.

```bash
python3 benchmarks/run.py --server build-release/webserver_liburing \
  --output benchmark-results-full --duration 5 --warmup 1 --repeats 3
python3 benchmarks/plot.py benchmark-results-full
```

Use 15–30 second runs and at least five repeats for stronger conclusions.
Additional experiments can vary file sizes, per-file cache eligibility, and
concurrency through CLI arguments. Use a fresh output directory; the runner
refuses to overwrite an existing results.json.

## Measurement definitions

- Successful requests/elapsed seconds: application throughput. The measured
  duration includes finishing requests started before the deadline.
- Successful payload bytes/elapsed seconds: MiB/s, excluding response headers.
- Client p50/p95/p99: elapsed request time through the complete body; includes
  connection establishment when keep-alive is disabled or a socket is reopened.
- Server CPU: delta of /proc PID stat user+system CPU ticks across the measured
  interval. 100% corresponds to one full CPU core.
- Peak RSS: executable VmHWM from server metrics, including warmup.
- Cache hits, reads, submissions: final server counters, including warmup.
- Generator CPU: CPU time used by the Python load generator during measurement.

Every response must have status 200 and the expected Content-Length; the load
client drains that many bytes. Full body integrity belongs to regression tests,
which compare bytes/hashes and exercise large/slow transfers. Errors are counted
and cause the overall benchmark command to fail. Every run saves interval and
final metrics, server stderr, and results. CSV rows and raw JSON are authoritative;
the report presents medians and repeat ranges. Figures show median and min/max.

## Interpretation and limits

The bundled client is single-process Python asyncio, runs on the same host as the
server, and uses a closed-loop workload. It can limit observed throughput and
understate latency under independent offered load (coordinated omission). Avoid
claiming maximum capacity from these runs. Validate important conclusions using
an independent generator on a separate machine; record its version and exact
command. Report negative and inconclusive outcomes as well as improvements.

File data is warm in the OS page cache after warmup, including when the
application LRU is disabled. No system-wide page cache is dropped by the runner.
A cold-cache experiment requires a dedicated machine and careful isolation;
state those conditions separately. Short local runs fluctuate with scheduling,
thermal behavior, and unrelated activity. The checked-in run is exploratory.

## Optional Linux profiling

Profile the server process while a separate client generates load. For example,
after obtaining its PID:

```bash
perf stat -p SERVER_PID -e task-clock,cycles,instructions,context-switches,page-faults -- sleep 15
perf record -g -p SERVER_PID -- sleep 15
perf report
```

Record the workload active during the sample. Hardware counters can require
host perf permissions; do not silently substitute fabricated numbers when
unavailable. The bundled measurements already provide CPU, RSS, file-read and
submission counts without requiring privileged profiling.
