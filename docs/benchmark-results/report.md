# Local benchmark results

Generated from actual loopback runs; these are exploratory measurements, not server capacity claims.

Kernel: `Linux-7.0.0-38-generic-x86_64-with-glibc2.39`. Python: `3.12.3`. CPU: 13th Gen Intel(R) Core(TM) i7-13650HX.
Duration: 1.0s per run after 0.2s warmup; repeats: 3.

The baseline is the same C++ implementation with cache disabled and batch size 1. Batched uses batch 64; queue64 changes only the batched queue depth; cached adds the LRU. Files over 256 KiB bypass the LRU. File data is warm in the OS page cache. All profiles still use userspace buffers; no zero-copy or epoll comparison is claimed.

Values below are medians across repeats. The RPS range shows minimum–maximum; p99 is the median of run p99 values.

| Bytes | Connections | Keep-alive | Profile | Requests/s | RPS range | p99 ms | Server CPU % | Peak RSS KiB |
|---:|---:|:---:|---|---:|---:|---:|---:|---:|
| 1024 | 8 | no | baseline | 9012.0 | 8977.2–9033.5 | 0.924 | 32.0 | 4704 |
| 1024 | 8 | no | batched | 9222.1 | 9144.1–9374.9 | 0.891 | 32.0 | 4520 |
| 1024 | 8 | no | cached | 8993.5 | 8925.1–9018.4 | 0.845 | 30.0 | 4596 |
| 1024 | 8 | no | queue64 | 8897.2 | 8356.7–9211.9 | 1.045 | 35.0 | 4620 |
| 1024 | 8 | yes | baseline | 39779.2 | 39555.9–41568.2 | 0.432 | 74.0 | 4696 |
| 1024 | 8 | yes | batched | 41905.3 | 39917.0–42002.7 | 0.357 | 75.0 | 4744 |
| 1024 | 8 | yes | cached | 41552.2 | 40619.4–41679.1 | 0.421 | 72.0 | 4584 |
| 1024 | 8 | yes | queue64 | 40368.2 | 39608.9–42275.1 | 0.433 | 75.0 | 4636 |
| 1024 | 32 | no | baseline | 9646.4 | 9480.9–10235.3 | 3.002 | 36.9 | 5940 |
| 1024 | 32 | no | batched | 10766.9 | 10678.8–10949.1 | 2.621 | 32.0 | 5696 |
| 1024 | 32 | no | cached | 10243.3 | 10149.0–10450.2 | 3.170 | 34.0 | 5972 |
| 1024 | 32 | no | queue64 | 10132.3 | 9717.4–10201.2 | 3.161 | 32.9 | 5536 |
| 1024 | 32 | yes | baseline | 44052.9 | 38070.6–44446.5 | 0.911 | 78.9 | 6380 |
| 1024 | 32 | yes | batched | 43636.0 | 43576.8–44717.5 | 0.837 | 79.9 | 6460 |
| 1024 | 32 | yes | cached | 44352.3 | 43879.6–44768.3 | 0.903 | 75.9 | 5844 |
| 1024 | 32 | yes | queue64 | 43368.1 | 42468.2–44017.2 | 0.910 | 79.9 | 6376 |
| 1048576 | 8 | no | baseline | 2088.5 | 1984.3–2107.6 | 4.478 | 42.9 | 4700 |
| 1048576 | 8 | no | batched | 2061.3 | 1973.5–2131.7 | 4.210 | 42.9 | 4636 |
| 1048576 | 8 | no | cached | 2110.6 | 2102.2–2144.9 | 4.187 | 41.9 | 4696 |
| 1048576 | 8 | no | queue64 | 2064.2 | 1885.8–2073.5 | 7.170 | 43.9 | 4672 |
| 1048576 | 8 | yes | baseline | 3253.4 | 2865.9–3485.3 | 4.967 | 67.9 | 4712 |
| 1048576 | 8 | yes | batched | 3374.6 | 3292.8–3389.7 | 4.953 | 67.9 | 4704 |
| 1048576 | 8 | yes | cached | 3009.2 | 2816.1–3435.6 | 6.118 | 67.9 | 4704 |
| 1048576 | 8 | yes | queue64 | 3192.0 | 2981.9–3454.3 | 5.569 | 67.9 | 4684 |
| 1048576 | 32 | no | baseline | 1980.4 | 1731.4–2031.3 | 24.889 | 45.6 | 6460 |
| 1048576 | 32 | no | batched | 1975.0 | 1954.9–2041.8 | 16.723 | 45.7 | 6460 |
| 1048576 | 32 | no | cached | 2025.3 | 1977.0–2029.4 | 17.088 | 46.7 | 6420 |
| 1048576 | 32 | no | queue64 | 2033.0 | 1943.9–2068.4 | 16.841 | 46.6 | 6472 |
| 1048576 | 32 | yes | baseline | 2137.8 | 2132.0–2450.3 | 22.091 | 62.6 | 6360 |
| 1048576 | 32 | yes | batched | 1790.8 | 1554.5–1960.7 | 25.749 | 52.6 | 6464 |
| 1048576 | 32 | yes | cached | 1955.2 | 1738.0–2021.7 | 20.352 | 50.5 | 6500 |
| 1048576 | 32 | yes | queue64 | 1810.3 | 1621.0–2131.8 | 20.882 | 48.6 | 6448 |

HTTP/transport failures across measured runs: **0**.

Limitations: a single Python asyncio generator shares the host with the server and can become the bottleneck. This is a closed-loop test, so latency omits queueing from an independent arrival stream (coordinated omission). Short runs are sensitive to scheduling and background load. CPU readings use /proc tick granularity. Peak RSS and server operation counters include warmup. No privileged cold-cache experiment was performed. Use longer runs, a dedicated load-generator machine, and an independent tool before drawing performance conclusions.

See `results.json` for environment metadata, `results.csv` for every run, and `metrics-*.jsonl` for server snapshots.
