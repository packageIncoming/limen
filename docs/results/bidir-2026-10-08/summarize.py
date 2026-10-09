#!/usr/bin/env python3
"""summarize.py - medians and ranges from one run.sh output folder.

Usage: summarize.py <mtu folder>

ib_send_bw prints MiB/s under the heading MB/sec. Everything printed here is Gbit/s
with 10^9 bits to the Gbit, the unit limen_bench reports.
"""
import json
import re
import statistics
import sys
from pathlib import Path

SIZE, ITERS, RUNS = 65536, 100000, 9
GBIT = 8 * 2**20 / 1e9

d = Path(sys.argv[1])
sets = sorted(int(p.stem.rsplit("set", 1)[1]) for p in d.glob("limen-bw-set*.json"))


def perftest(tag):
    """One list per run: the BW average of every result line, in Gbit/s."""
    runs = []
    for i in range(1, RUNS + 1):
        lines = (d / f"{tag}-run{i}.cli").read_text().splitlines()
        runs.append([float(l.split()[3]) * GBIT for l in lines
                     if re.match(rf"\s*{SIZE}\s+{ITERS}\s", l)])
    return runs


def limen(tag):
    runs = json.loads((d / f"{tag}.json").read_text())["runs"]
    return [r["bytes"] * 8 / r["elapsed_ns"] for r in runs]


def row(name, v):
    spread = 100 * (max(v) - min(v)) / statistics.mean(v)
    print(f"  {name:<33} median {statistics.median(v):5.2f}   {min(v):5.2f} to {max(v):5.2f}"
          f"   spread {spread:5.2f}%")
    return statistics.median(v)


both = {}
for s in sets:
    # with --report-both the client prints the sum, then its own direction, then the server's
    bidir = perftest(f"perftest-bidir-set{s}")
    cells = {
        "perftest -b, client to server": [r[1] for r in bidir],
        "perftest -b, server to client": [r[2] for r in bidir],
        "perftest -b, sum as printed":   [r[0] for r in bidir],
        "limen, per direction of echo":  limen(f"limen-bw-set{s}"),
        "perftest one way":              [r[0] for r in perftest(f"perftest-oneway-set{s}")],
    }
    print(f"set {s}, Gbit/s over {RUNS} runs")
    med = {name: row(name, v) for name, v in cells.items()}
    for name, v in cells.items():
        both.setdefault(name, []).extend(v)
    gap = med["limen, per direction of echo"] / med["perftest -b, client to server"] - 1
    print(f"  limen against perftest -b         {100 * gap:+.1f}%\n")

print(f"all sets, Gbit/s over {RUNS * len(sets)} runs")
for name, v in both.items():
    row(name, v)
