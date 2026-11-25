#!/usr/bin/env python3
#  Copyright (c) Meta Platforms, Inc. and affiliates.
#  This source code is licensed under both the GPLv2 (found in the
#  COPYING file in the root directory) and Apache 2.0 License
#  (found in the LICENSE.Apache file in the root directory).
#
# Print a markdown table from tombstone_bench/run.sh output files.
# Usage: python3 tombstone_bench/summarize.py tombstone_bench/results
import glob
import os
import re
import sys

d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "results")
rows = []
for path in sorted(glob.glob(os.path.join(d, "*.txt"))):
    tag = os.path.basename(path)[:-4]
    t = open(path).read()

    def f(pat, cast=float, default=None):
        m = re.search(pat, t)
        return cast(m.group(1)) if m else default

    m = re.match(r"(\w+)_m(\d)_r(\d)", tag)
    if not m or "verify:" not in t:
        continue
    skips = re.findall(r"(L\d->L\d)=(\d+)", t.split("compactions by shape:")[-1].splitlines()[0]) if "compactions by shape:" in t else []
    multi = sum(int(n) for s, n in skips if int(s[-1]) - int(s[1]) > 1 and s[1] != "0")
    rows.append(dict(
        workload=m.group(1), mode=int(m.group(2)), rep=int(m.group(3)),
        load=f(r"load=\{ ([\d.]+)"),
        wa=f(r"write_amp=([\d.]+)"),
        avg_tomb=f(r"avg_upper_tombstones=(\d+)", int),
        peak_age=f(r"peak_tomb_age_ub=(\d+)", int),
        end_tomb=f(r"end_of_run: .*?upper_tombs=(\d+)", int),
        space=f(r"space_amp=([\d.]+)"),
        skip_n=f(r"SkipLevelTombstone=(\d+)", int, 0),
        multi=multi,
        verify="PASS" if "-> PASS" in t else "FAIL",
    ))

print("| workload | mode | rep | load(1m) | write amp | avg upper-level tombstones | peak tombstone age UB (s) | upper tombstones at end | space amp (drained) | SkipLevelTombstone compactions | multi-level (skip >1 level) | verify |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
for r in sorted(rows, key=lambda r: (r["workload"], r["mode"], r["rep"])):
    print("| {workload} | {mode} | {rep} | {load:.0f} | {wa:.2f} | {avg_tomb} | {peak_age} | {end_tomb} | {space:.3f} | {skip_n} | {multi} | {verify} |".format(**r))
