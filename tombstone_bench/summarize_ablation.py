#!/usr/bin/env python3
#  Copyright (c) Meta Platforms, Inc. and affiliates.
#  This source code is licensed under both the GPLv2 (found in the
#  COPYING file in the root directory) and Apache 2.0 License
#  (found in the LICENSE.Apache file in the root directory).
#
# Markdown tables from tombstone_bench/ablation.sh output: one row per run,
# then the mean per (workload, config).
# Usage: python3 tombstone_bench/summarize_ablation.py tombstone_bench/results/ablation
import collections
import glob
import os
import re
import sys

d = sys.argv[1]


def l0_files_per_compaction(t):
    # "L0 compactions:" line (absent in older runs): reason=n(avg_l0_files=x,...)
    if "L0 compactions:" not in t:
        return float("nan")
    line = t.split("L0 compactions:")[1].splitlines()[0]
    parts = re.findall(r"=(\d+)\(avg_l0_files=([\d.]+)", line)
    n = sum(int(a) for a, _ in parts)
    return sum(int(a) * float(b) for a, b in parts) / n if n else float("nan")


ORDER = ["base", "trig", "skip", "trig+skip", "trig+sav", "skip+sav", "fade", "fade+skip", "fade+p", "fade+skip+p"]
rows = []
for path in sorted(glob.glob(os.path.join(d, "*.txt"))):
    tag = os.path.basename(path)[:-4]
    m = re.match(r"(\w+)_([\w+]+)_r(\d+)$", tag)
    t = open(path).read()
    if not m or "verify:" not in t:
        continue

    def f(pat, cast=float, default=None):
        mm = re.search(pat, t)
        return cast(mm.group(1)) if mm else default

    skips = re.findall(r"(L\d)->(L\d)=(\d+)",
                       t.split("skip-level shapes:")[1].splitlines()[0])
    rows.append(dict(w=m.group(1), c=m.group(2), rep=int(m.group(3)),
                     skips=[(int(a[1]), int(b[1]), int(n)) for a, b, n in skips],
                     shapes=t.split("compactions by shape:")[1].splitlines()[0]))
    r = rows[-1]
    r.update(dict(
        load=f(r"load=\{ ([\d.]+)"),
        wa=f(r"write_amp=([\d.]+)"),
        comp=f(r"compaction ([\d.]+) MB"),
        tomb=f(r"avg_upper_tombstones=(\d+)", int),
        space=f(r"space_amp=([\d.]+)"),
        sl=f(r"SkipLevelTombstone=(\d+)", int, 0),
        l0f=l0_files_per_compaction(t),
        age=f(r"true_tomb_age: peak=([\d.]+)s", float, float("nan")),
        over=f(r"samples_over_\d+s=(\d+)/", int, -1),
        fade=f(r"FadeTtl=(\d+)", int, 0),
        ok="PASS" if "-> PASS" in t else "FAIL"))

# The normal L0 output level (base level) is taken from the base config's run
# of the same workload: the most common L0->Lx shape there. Skipping can empty
# the intermediate levels and move the base level, so the skip runs' own
# shapes are not a reliable reference.
base_of = {}
for r in rows:
    if r["c"] == "base":
        l0 = [(int(n), int(b)) for b, n in re.findall(r"L0->L(\d)=(\d+)", r["shapes"])]
        if l0:
            base_of[r["w"]] = max(l0)[1]
for r in rows:
    base = base_of.get(r["w"], 1)
    r["real"] = r["lvls"] = 0
    for a, b, n in r["skips"]:
        nxt = base if a == 0 else a + 1
        if b > nxt:
            r["real"] += n
            r["lvls"] += n * (b - nxt)

print("| workload | config | rep | load | write amp | compaction MB | avg upper tombstones | space amp | SkipLevel compactions | real skips | levels skipped | L0 files per L0 compaction | FadeTtl compactions | peak true tombstone age (s) | samples over D | verify |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
key = lambda r: (r["w"], ORDER.index(r["c"]) if r["c"] in ORDER else 99, r["rep"])
for r in sorted(rows, key=key):
    print("| {w} | {c} | {rep} | {load:.0f} | {wa:.2f} | {comp:.0f} | {tomb} | {space:.3f} | {sl} | {real} | {lvls} | {l0f:.2f} | {fade} | {age:.1f} | {over} | {ok} |".format(**r))

print()
print("| workload | config | runs | mean write amp | vs base | mean avg upper tombstones | vs base | mean real skips | mean peak true tombstone age (s) |")
print("|---|---|---|---|---|---|---|---|---|")
g = collections.defaultdict(list)
for r in rows:
    g[(r["w"], r["c"])].append(r)
mean = lambda xs: sum(xs) / len(xs)
for (w, c) in sorted(g, key=lambda k: (k[0], ORDER.index(k[1]) if k[1] in ORDER else 99)):
    rs = g[(w, c)]
    wa, tb = mean([r["wa"] for r in rs]), mean([r["tomb"] for r in rs])
    b = g.get((w, "base"))
    bwa = mean([r["wa"] for r in b]) if b else wa
    btb = mean([r["tomb"] for r in b]) if b else tb
    print("| %s | %s | %d | %.2f | %+.1f%% | %.0f | %+.1f%% | %.1f | %.1f |" % (
        w, c, len(rs), wa, 100 * (wa / bwa - 1), tb,
        100 * (tb / btb - 1) if btb else 0, mean([r["real"] for r in rs]),
        mean([r["age"] for r in rs])))
