#!/usr/bin/env python3
"""Closed-loop, loopback experiments. No third-party packages or root required."""
import argparse
import asyncio
import csv
import datetime
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import random
import resource
import select
import signal
import statistics
import subprocess
import tempfile
import time

PROFILES = {
    "baseline": ["--batch-size", "1", "--cache-bytes", "0", "--queue-depth", "256"],
    "batched": ["--batch-size", "64", "--cache-bytes", "0", "--queue-depth", "256"],
    "queue64": ["--batch-size", "64", "--cache-bytes", "0", "--queue-depth", "64"],
    "cached": ["--batch-size", "64", "--cache-bytes", "16777216", "--queue-depth", "256"],
}


def percentile(values, fraction):
    if not values:
        return 0
    return values[min(len(values)-1, max(0, math.ceil(len(values)*fraction)-1))]


async def load(port, path, size, concurrency, seconds, keep_alive):
    deadline = time.perf_counter() + seconds
    latency = []
    completed = errors = 0
    request = (f"GET /{path} HTTP/1.1\r\nHost: localhost\r\nConnection: " +
               ("keep-alive" if keep_alive else "close") + "\r\n\r\n").encode()

    async def worker():
        nonlocal completed, errors
        reader = writer = None
        try:
            while time.perf_counter() < deadline:
                start = time.perf_counter()
                try:
                    if writer is None:
                        reader, writer = await asyncio.wait_for(asyncio.open_connection("127.0.0.1", port), 5)
                    writer.write(request)
                    await asyncio.wait_for(writer.drain(), 5)
                    block = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 5)
                    lines = block.split(b"\r\n")
                    status = int(lines[0].split()[1])
                    headers = dict(line.lower().split(b":",1) for line in lines[1:] if b":" in line)
                    length = int(headers[b"content-length"].strip())
                    if status != 200 or length != size:
                        raise ValueError(f"unexpected response: status={status}, length={length}")
                    # Drain in bounded chunks so large-file runs do not inflate client memory.
                    remaining = length
                    while remaining:
                        chunk = await asyncio.wait_for(reader.readexactly(min(remaining,65536)), 5)
                        remaining -= len(chunk)
                    completed += 1
                    latency.append((time.perf_counter()-start)*1000)
                    if not keep_alive or headers.get(b"connection",b"").strip() == b"close":
                        writer.close()
                        await writer.wait_closed()
                        reader = writer = None
                except (OSError, asyncio.TimeoutError, ValueError, asyncio.IncompleteReadError) as error:
                    errors += 1
                    if writer is not None:
                        writer.close()
                        try:
                            await writer.wait_closed()
                        except OSError:
                            pass
                    reader = writer = None
                    if errors > 100:
                        raise RuntimeError(f"too many load-test errors: {error}")
        finally:
            if writer is not None:
                writer.close()
                try:
                    await writer.wait_closed()
                except OSError:
                    pass

    start = time.perf_counter()
    client_cpu = time.process_time()
    await asyncio.gather(*(worker() for _ in range(concurrency)))
    elapsed = time.perf_counter() - start
    latency.sort()
    return {"requests": completed, "errors": errors, "elapsed_seconds": elapsed,
            "requests_per_second": completed / elapsed, "throughput_mib_s": completed*size/elapsed/1048576,
            "p50_ms": percentile(latency,.50), "p95_ms": percentile(latency,.95),
            "p99_ms": percentile(latency,.99), "generator_cpu_seconds": time.process_time()-client_cpu}


def process_cpu(pid):
    # /proc stat fields 14 and 15; split after comm, which may contain spaces.
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")",1)[1].split()
    return (int(fields[11])+int(fields[12])) / os.sysconf("SC_CLK_TCK")


def build_metadata(binary):
    """Read actual CMake build flags when available; do not guess optimization."""
    info = {}
    cache = binary.parent / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:")):
                key, value = line.split("=", 1)
                info[key.split(":",1)[0]] = value
    flags = binary.parent / "CMakeFiles" / "webserver_liburing.dir" / "flags.make"
    if flags.exists():
        info["compile_flags"] = flags.read_text()
    compiler = info.get("CMAKE_CXX_COMPILER")
    if compiler:
        info["compiler_version"] = subprocess.check_output([compiler,"--version"],text=True).splitlines()[0]
    return info


def experiment(args, root, profile, size, concurrency, keep_alive, repeat, index):
    metrics_path = args.output / f"metrics-{index:04d}.jsonl"
    stderr_path = args.output / f"server-{index:04d}.log"
    command = [str(args.server), "--port", "0", "--root", str(root), "--max-requests", "1000000",
               "--metrics-file", str(metrics_path), "--metrics-interval-ms", "100",
               *PROFILES[profile]]
    with stderr_path.open("w") as stderr:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=stderr, text=True)
        try:
            if not select.select([process.stdout], [], [], 8)[0]:
                raise RuntimeError("server did not announce readiness")
            ready = process.stdout.readline()
            if "listening on" not in ready:
                raise RuntimeError("server startup failed: " + stderr_path.read_text())
            port = int(ready.strip().rsplit(":",1)[1])
            asyncio.run(load(port, f"file-{size}.bin", size, concurrency, args.warmup, keep_alive))
            cpu_before = process_cpu(process.pid)
            result = asyncio.run(load(port, f"file-{size}.bin", size, concurrency, args.duration, keep_alive))
            result["server_cpu_seconds"] = process_cpu(process.pid) - cpu_before
            result["server_cpu_percent_one_core"] = 100 * result["server_cpu_seconds"] / result["elapsed_seconds"]
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise RuntimeError("server shutdown timed out")
            process.stdout.close()
        if process.returncode != 0:
            raise RuntimeError(stderr_path.read_text())
    final = json.loads(metrics_path.read_text().splitlines()[-1])
    result.update({"profile": profile, "file_bytes": size, "concurrency": concurrency,
                   "keep_alive": keep_alive, "repeat": repeat,
                   "server_peak_rss_kib": final["peak_rss_kib"],
                   "cache_hits_including_warmup": final["cache_hits"],
                   "operations_per_submit_including_warmup": final["operations_submitted"] / max(1,final["submit_calls"]),
                   "metrics_file": metrics_path.name})
    return result


def report(args, rows, metadata):
    with (args.output / "results.csv").open("w",newline="") as output:
        writer = csv.DictWriter(output,fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (args.output / "results.json").write_text(json.dumps({"metadata":metadata,"results":rows},indent=2)+"\n")
    groups = {}
    for row in rows:
        key = (row["file_bytes"],row["concurrency"],row["keep_alive"],row["profile"])
        groups.setdefault(key,[]).append(row)
    lines = ["# Local benchmark results", "", "Generated from actual loopback runs; these are exploratory measurements, not server capacity claims.", "",
             f"Kernel: `{metadata['platform']}`. Python: `{metadata['python']}`. CPU: {metadata['cpu']}.",
             f"Duration: {args.duration}s per run after {args.warmup}s warmup; repeats: {args.repeats}.", "",
             "The baseline is the same C++ implementation with cache disabled and batch size 1. "
             "Batched uses batch 64; queue64 changes only the batched queue depth; cached adds the LRU. "
             "Files over 256 KiB bypass the LRU. File data is warm in the OS page cache. "
             "All profiles still use userspace buffers; no zero-copy or epoll comparison is claimed.", "",
             "Values below are medians across repeats. The RPS range shows minimum–maximum; p99 is the median of run p99 values.", "",
             "| Bytes | Connections | Keep-alive | Profile | Requests/s | RPS range | p99 ms | Server CPU % | Peak RSS KiB |",
             "|---:|---:|:---:|---|---:|---:|---:|---:|---:|"]
    for key,group in sorted(groups.items()):
        size,concurrency,keep,profile = key
        median = lambda field: statistics.median(row[field] for row in group)
        rates = [r["requests_per_second"] for r in group]
        lines.append(f"| {size} | {concurrency} | {'yes' if keep else 'no'} | {profile} | {median('requests_per_second'):.1f} | {min(rates):.1f}–{max(rates):.1f} | {median('p99_ms'):.3f} | {median('server_cpu_percent_one_core'):.1f} | {median('server_peak_rss_kib'):.0f} |")
    lines += ["", f"HTTP/transport failures across measured runs: **{sum(r['errors'] for r in rows)}**.", "",
              "Limitations: a single Python asyncio generator shares the host with the server and can become the bottleneck. "
              "This is a closed-loop test, so latency omits queueing from an independent arrival stream (coordinated omission). "
              "Short runs are sensitive to scheduling and background load. CPU readings use /proc tick granularity. "
              "Peak RSS and server operation counters include warmup. No privileged cold-cache experiment was performed. "
              "Use longer runs, a dedicated load-generator machine, and an independent tool before drawing performance conclusions.", "",
              "See `results.json` for environment metadata, `results.csv` for every run, and `metrics-*.jsonl` for server snapshots.", ""]
    (args.output / "report.md").write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("benchmark-results"))
    parser.add_argument("--duration", type=float, default=5)
    parser.add_argument("--warmup", type=float, default=1)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--concurrency", default="1,16,64")
    parser.add_argument("--sizes", default="1024,1048576")
    parser.add_argument("--profiles", default=",".join(PROFILES))
    parser.add_argument("--keep-alive", choices=["both","yes","no"], default="both")
    args = parser.parse_args()
    if args.duration <= 0 or args.warmup <= 0 or args.repeats <= 0:
        parser.error("duration, warmup and repeats must be positive")
    sizes = [int(x) for x in args.sizes.split(",")]
    concurrencies = [int(x) for x in args.concurrency.split(",")]
    profiles = args.profiles.split(",")
    if not all(0 < x <= 64*1024*1024 for x in sizes) or not all(0 < x <= 1024 for x in concurrencies):
        parser.error("sizes must be 1..64MiB and concurrency 1..1024")
    if any(p not in PROFILES for p in profiles):
        parser.error("unknown profile")
    args.server = args.server.resolve()
    args.output.mkdir(parents=True,exist_ok=True)
    if (args.output / "results.json").exists():
        parser.error("output already contains results; choose a new directory")
    keep = [True,False] if args.keep_alive == "both" else [args.keep_alive == "yes"]
    jobs = list(itertools.product(profiles,sizes,concurrencies,keep,range(1,args.repeats+1)))
    random.Random(20261028).shuffle(jobs)  # Reduce systematic profile/order bias.
    try:
        revision = subprocess.check_output(["git","rev-parse","HEAD"],text=True).strip()
        dirty = bool(subprocess.check_output(["git","status","--porcelain"],text=True).strip())
    except (OSError,subprocess.CalledProcessError):
        revision,dirty = "unknown",True
    cpu = next((line.split(":",1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")),"unknown")
    metadata = {"platform":platform.platform(),"python":platform.python_version(),"cpu":cpu,
                "logical_cpus":os.cpu_count(),"affinity":sorted(os.sched_getaffinity(0)),
                "git_revision":revision,"working_tree_dirty":dirty,"server":str(args.server),
                "duration":args.duration,"warmup":args.warmup,"repeats":args.repeats,
                "generator":"stdlib asyncio, loopback, closed-loop","page_cache":"warm",
                "timestamp_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "server_sha256":hashlib.sha256(args.server.read_bytes()).hexdigest(),
                "build":build_metadata(args.server)}
    rows = []
    with tempfile.TemporaryDirectory(prefix="zerohttpd-bench-") as temp:
        root = Path(temp)
        for size in sizes:
            pattern = bytes(range(256))
            (root / f"file-{size}.bin").write_bytes((pattern * ((size+255)//256))[:size])
        for index,job in enumerate(jobs):
            row = experiment(args,root,*job,index)
            rows.append(row)
            print(f"[{index+1}/{len(jobs)}] {row['profile']} {row['file_bytes']}B c={row['concurrency']} keep={row['keep_alive']}: {row['requests_per_second']:.0f} req/s, errors={row['errors']}",flush=True)
            report(args,rows,metadata)  # Preserve completed experiments if interrupted.
    if any(r["errors"] for r in rows):
        raise SystemExit("load errors occurred; inspect results")
    print(f"Report: {args.output / 'report.md'}")


if __name__ == "__main__":
    main()
