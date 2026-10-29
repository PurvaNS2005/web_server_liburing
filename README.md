# ZeroHTTPd

A C++17 HTTP/1.1 static-file server for Linux, built around liburing. One event
loop handles asynchronous socket accept/receive/send and file open/stat/read.
The project studies correctness, resource use, and measured optimization of the
complete request path.

## Build and run

Requires Linux with the required io_uring operations enabled (Linux 5.6+),
liburing development headers, a C++17 compiler, CMake 3.15+, and Python 3.8+ for
tests. The server probes operations instead of comparing kernel version strings.
Containers and sandboxes can deny io_uring even on a recent kernel.

```bash
# Ubuntu/Debian
sudo apt install build-essential cmake liburing-dev python3

# Use a fresh build directory to avoid reusing an older CMake cache.
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
./build-release/webserver_liburing --root public --port 8000
```

Open http://127.0.0.1:8000/ or try:

```bash
curl -i http://127.0.0.1:8000/
curl -I http://127.0.0.1:8000/tux.png
./build-release/http_client 127.0.0.1 8000 /
./build-release/webserver_liburing --help
```

The default listener is loopback. Use `--bind 0.0.0.0` to accept external IPv4
connections. The server serves only the supplied document root; its working
directory need not contain public/ when an absolute root is supplied. `--port 0`
selects an available port and prints it after successful initialization.

## Implemented scope

- HTTP/1.1 GET and HEAD, HTTP/1.0 compatibility, Content-Length, Date, MIME types,
  persistent connections, and ordered processing of pipelined requests.
- Incremental request parsing; bounded headers; strict Host and framing checks.
  Request bodies, transfer-encoded requests, and Expect are rejected explicitly
  and the connection is closed. Unsupported methods receive 405 with Allow.
- io_uring ACCEPT, RECV, SEND, OPENAT2, STATX, READ, and ASYNC_CANCEL. File bodies
  stream in bounded chunks; short reads and partial sends advance offsets.
- Document-root confinement using descriptor-relative openat2 with RESOLVE_BENEATH
  and RESOLVE_NO_MAGICLINKS. Percent decoding precedes traversal checks. Relative
  symlinks staying beneath the root are supported; escaping/absolute symlinks
  are refused. Nonregular files, including FIFOs, are not served.
- Idle/I/O timeout, absolute incomplete-header deadline, connection/request
  limits, and SIGINT/SIGTERM shutdown that drains responses up to a grace period.
- Reusable file buffers, batched submissions/completions, configurable queue depth.
- Payload-bounded LRU cache. Each request asynchronously opens/stats the file;
  inode/device/size/nanosecond mtime/ctime changes invalidate cached content.
  Files are checked again before a newly collected body enters the cache.
- Optional access logs and JSON-lines metrics: requests, errors, active
  connections, bytes, latency percentiles, cache use, CPU, RSS, buffer reuse,
  partial sends, and submission counts.
- Socket regression tests, parser/cache tests, sanitizers, CI configuration,
  a repeatable load generator, raw results, and standalone benchmark figures.

Paths ending in `/` resolve to `index.html`. Query strings do not affect file
lookup. Directory listings, request bodies, ranges, compression, TLS, and HTTP/2
are outside this implementation. The server does not implement every HTTP/1.1
feature; it implements the documented static-serving subset.

## Configuration

All byte settings use integer bytes, and all timeout settings use milliseconds.

| Option | Default | Purpose |
|---|---:|---|
| `--bind` | `127.0.0.1` | IPv4 listener |
| `--port` | `8000` | TCP port; 0 chooses an available port |
| `--root` | `public` | Document root |
| `--queue-depth` | `256` | Submission queue entries, 8–4096 |
| `--batch-size` | `64` | CQ processing/submission batch; 1 disables batching |
| `--max-connections` | `1024` | Accepted connections; excess wait in the kernel backlog |
| `--timeout-ms` | `10000` | Maximum inactivity during socket/file operations |
| `--header-timeout-ms` | `10000` | Absolute deadline from first header bytes |
| `--grace-ms` | `3000` | Time to drain responses during shutdown |
| `--max-requests` | `1000` | Requests allowed per connection |
| `--header-limit` | `16384` | Maximum request header bytes |
| `--buffer-size` | `65536` | Reusable file chunk bytes |
| `--cache-bytes` | `16777216` | Resident LRU payload budget; 0 disables |
| `--cache-file-bytes` | `262144` | Maximum file eligible for caching |
| `--metrics-file` | disabled | JSON-lines output file, truncated on startup |
| `--metrics-interval-ms` | `1000` | Metrics snapshot interval |
| `--access-log` | disabled | Completed request log to stderr |

```bash
./build-release/webserver_liburing --root public --port 8000 \
  --queue-depth 256 --batch-size 64 --max-connections 512 \
  --cache-bytes 16777216 --metrics-file /tmp/zerohttpd-metrics.jsonl --access-log
```

Final metrics are also emitted to stderr. Counters are cumulative. `io_errors`
counts failed original I/O completions, including peer resets; it is separate
from completed HTTP error responses. Timeout cancellations have their own count.
Server latencies use fixed logarithmic histograms with approximately 10% bucket
resolution and measure header assembly through response completion; benchmark
latencies are measured separately at the client. Peak RSS comes from
`/proc/self/status` VmHWM, with getrusage as a fallback. CPU seconds come from
getrusage. Metrics output files must be outside the document root if they should
not be publicly served.

## Test

```bash
ctest --test-dir build-release --output-on-failure
# Compatibility wrapper, or run tests/integration.py directly:
SERVER_BIN="$PWD/build-release/webserver_liburing" bash tests/smoke.sh

cmake -S . -B build-asan -DENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

Tests create temporary document roots, bind ephemeral loopback ports, and fail
when io_uring is unavailable. There are no expected-failure markers hiding
known server defects. Tests cover real fragmented traffic, pipelining, HEAD,
large-file integrity, slow clients, cache changes, traversal, resets, resource
limits, small rings, and shutdown. The pure parser/cache suite also runs 20,000
seeded arbitrary-byte inputs and can run without io_uring:

```bash
ctest --test-dir build-asan -R '^core$' --output-on-failure
```

## Performance experiments

```bash
python3 benchmarks/run.py --server build-release/webserver_liburing \
  --output benchmark-results --duration 5 --warmup 1 --repeats 3 \
  --concurrency 1,16,64

# Optional plotting dependency:
# sudo apt install python3-matplotlib
python3 benchmarks/plot.py benchmark-results
```

The matrix compares the same implementation with batch size 1/cache off,
batching enabled, a smaller queue, and caching enabled. It varies file size,
concurrency, and keep-alive, shuffles run order, and saves every measured run,
server metrics, environment metadata, a CSV, and a Markdown report. It does not
claim an io_uring-versus-epoll result. Use a new output directory for each run.

[Recorded local results](docs/benchmark-results/report.md) include three repeats
of short exploratory runs. Read their limitations before interpreting them.
See [architecture](docs/architecture.md), [evaluation procedure](docs/evaluation.md),
[findings](docs/findings.md), and [validation record](docs/validation.md) for the
submission material. Run `bash examples/demo.sh build-release/webserver_liburing`
for a short curl demonstration.

## Resource and operational limits

The implementation is single-threaded; memory is bounded by configuration,
not just the cache budget. Each active connection owns an 8 KiB receive buffer,
bounded input, and at most one file chunk plus a cache candidate up to the
eligible-file size. Evicted cached bodies remain alive while clients send them.
The buffer pool retains at most the peak number of concurrent file buffers.
Account for these costs when choosing connection/cache limits. Cache accounting
covers payload bytes; keys, LRU nodes, and shared ownership add metadata overhead.

A request opens the file before checking the cache, so cache hits avoid body
reads rather than all filesystem operations. Replacing a file atomically is the
recommended update method. Concurrent in-place changes can interrupt a transfer
or produce a mixed-version response; truncation closes the connection rather
than sending uninitialized bytes. There is no filesystem snapshot guarantee.

Shutdown stops accepts, closes idle readers, drains active responses, then
cancels outstanding work after the grace period. Kernel-referenced memory and
file descriptors remain alive until completion; an uncancellable kernel/disk
operation can extend shutdown beyond the configured grace period.

## Layout and future work

`src/` contains the maintained C++ server. `examples/legacy_webserver.c` preserves
the old C prototype for historical comparison and is not a build target.
`examples/http_client.c` is the companion client. The root-level C examples are
legacy learning programs, excluded from the default build; opt in with
`-DBUILD_EXAMPLES=ON`. They are not part of the validated server feature set.

Deferred scope: multiple workers/CPU affinity, registered buffers, zero-copy
transfer, reverse proxy/load balancing/health checks, eBPF, and TLS. These should
be separate experiments after the current baseline is understood.

Generated build files are excluded from version control. Older local `build/`
directories can remain on disk; use a fresh build directory to avoid reusing
their CMake caches.

MIT license; see LICENSE. The original prototype and examples retain their
existing attribution. The server was adapted from the ZeroHTTPd/io_uring learning
project; this C++ implementation replaces its request and I/O lifecycle.
