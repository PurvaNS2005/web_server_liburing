# Validation record

The maintained target is `src/server.cpp` plus the parser, cache and configuration
components. Historical examples are excluded from these server correctness claims.

## Automated checks

- Normal C++17 build with `-Wall -Wextra -Wpedantic`.
- HTTP parser and LRU behavior tests, including framing, duplicate length/Host,
  encoded traversal, arbitrary control bytes, and 20,000 deterministic arbitrary-byte inputs.
- Real io_uring integration tests with temporary roots and ephemeral ports.
- The same suite built with AddressSanitizer and UndefinedBehaviorSanitizer.
- Python syntax compilation for regression and benchmark tooling.

The socket suite checks static MIME/body/header correctness, empty files, HEAD,
query strings, percent decoding, regular-file checks/FIFOs, escaping symlinks,
fragmentation, persistent/pipelined requests, strict body/framing rejection,
oversized headers, concurrent transfers, connection resets and descriptor cleanup,
cache invalidation, timeouts, connection/request limits, small rings, slow readers,
file truncation during transfer, cached large transfers under backpressure,
SIGINT/SIGTERM, and graceful shutdown deadlines.

The test harness treats server crashes and sanitizer reports as failures. The
integration test does not silently skip when io_uring is denied. On restricted
hosts, run the core tests separately and run the integration suite on an allowed
Linux host. CI configuration is included; hosted CI execution is not claimed by
this local validation record.

## Performance evidence

`benchmark-results/report.md` and its CSV/JSON describe the actual local matrix.
The matrix contains baseline, batching, smaller queue and cache profiles, with
small/large files, two concurrency levels, persistent/nonpersistent connections,
and repeated measurements. Figures are generated from those recorded rows.

See `evaluation.md` for definitions and limitations. These results validate the
experiment pipeline and show local behavior. They do not establish superiority
over epoll, production capacity, cold-storage performance, or multicore scaling.

## Recorded local outcome

- Normal RelWithDebInfo build: core suite and all 25 integration tests passed.
- ASan/UBSan Debug build: core suite and all 25 integration tests passed, with no
  sanitizer findings.
- Benchmark: 96 runs, three repeats per configuration, zero measured errors.
- Curl demonstration: GET, HEAD, 404, connection reuse (one initial connection,
  zero new connections for the second URL), and signal shutdown succeeded.
- Shell syntax, Python syntax and git whitespace checks passed.

Normal and sanitizer tests ran outside the execution sandbox because its
security policy denies io_uring. The actual Linux host permits the required
operations. No sandbox fallback backend is substituted for io_uring.
