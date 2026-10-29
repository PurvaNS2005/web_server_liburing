#!/usr/bin/env python3
"""Render standalone benchmark figures. Requires matplotlib (optional)."""
import argparse
import csv
import os
from pathlib import Path
import statistics
import tempfile

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "zerohttpd-matplotlib"))
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("results", type=Path, help="directory containing results.csv")
args = parser.parse_args()
with (args.results / "results.csv").open() as file:
    rows = list(csv.DictReader(file))
sizes = sorted({int(r["file_bytes"]) for r in rows})
concurrencies = sorted({int(r["concurrency"]) for r in rows})
profiles = [p for p in ["baseline","batched","queue64","cached"] if any(r["profile"] == p for r in rows)]
for field,ylabel,name in [("requests_per_second","Requests / second","throughput"),
                           ("p99_ms","p99 response latency (ms)","latency")]:
    figure,axes = plt.subplots(len(sizes),2,figsize=(11,3.7*len(sizes)),squeeze=False)
    for i,size in enumerate(sizes):
        for j,keep in enumerate(["True","False"]):
            ax = axes[i][j]
            for profile in profiles:
                x,y,low,high = [],[],[],[]
                for concurrency in concurrencies:
                    values = [float(r[field]) for r in rows if int(r["file_bytes"]) == size and
                              int(r["concurrency"]) == concurrency and r["keep_alive"] == keep and r["profile"] == profile]
                    if values:
                        median = statistics.median(values)
                        x.append(concurrency); y.append(median)
                        low.append(median-min(values)); high.append(max(values)-median)
                if x:
                    ax.errorbar(x,y,yerr=[low,high],marker="o",capsize=3,label=profile)
            ax.set_title(f"{size:,} bytes; keep-alive {'on' if keep == 'True' else 'off'}")
            ax.set_xlabel("Concurrent connections")
            ax.set_ylabel(ylabel)
            ax.grid(alpha=.25)
            ax.legend()
    figure.suptitle("Local loopback experiment — medians and min/max across repeats")
    figure.tight_layout()
    figure.savefig(args.results / f"{name}.png",dpi=180)
    figure.savefig(args.results / f"{name}.svg")
    plt.close(figure)
print(f"Figures written to {args.results}")
