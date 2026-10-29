# Six-week scope delivered

The original proposal describes a broader high-performance C++ server. This
implementation follows the reduced six-week scope agreed for this repository.

| Target | Implementation/evidence |
|---|---|
| C++ foundation and ownership | `src/server.cpp`: RAII descriptors, owned operations and connections |
| Robust HTTP subset | `src/http.cpp`: incremental GET/HEAD, HTTP/1.1 persistence and framing checks |
| Asynchronous file lifecycle | OPENAT2, STATX, READ completions in the same ring as socket operations |
| Bounded streaming and partial writes | Offset-based reads/sends with reusable per-transfer chunks |
| Limits, timeouts and shutdown | Configurable connections/headers/requests, idle/header deadlines, signal drain |
| Configuration and logs | Validated CLI, access log, interval/final metrics |
| Batching/queue tuning | Baseline/batched/queue64 benchmark profiles |
| LRU cache | Payload budget, per-file eligibility, metadata validation, eviction tests |
| Regression tests and sanitizers | Parser/cache tests plus real-socket suite in normal and sanitized builds |
| Benchmark study and figures | Runner, environment metadata, raw rows, repeated local experiments, plots |
| Demonstration and report material | `examples/demo.sh`, README, architecture and evaluation documents |

Deferred from the larger PDF proposal: multiple workers, CPU-affinity scheduling,
registered/fixed-buffer integration, zero-copy transfer, reverse proxy, load
balancing, backend health checks, and eBPF. TLS, compression, uploads, and dynamic
application features are also outside this scope.

The implemented HTTP subset deliberately rejects request bodies and transfer
encoding. Sequential HTTP/1.1 requests and ordered pipelining are supported;
full general-purpose HTTP protocol compliance is not claimed.

The local experiment is not an academic evaluation substitute: review the code,
understand ownership/cancellation and framing, reproduce the results, and explain
limitations and optimization tradeoffs in the final presentation. Hosted CI and
dedicated-machine capacity experiments require their respective environments.
