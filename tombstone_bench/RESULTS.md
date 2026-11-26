# Skip-level tombstone compaction: benchmark results (draft)

LLM-drafted prototype (CSC443, keep the chat log per the course policy).
Branch `skip-level-tombstone-draft`, base `main` (6eeb6d1). Run on 2026-11-23
on the MacBook (10 cores), Release static lib, `tombstone_bench` driver.

## Setup

- Tree: 1 MB write buffer and target file size, 4 MB level base, T=10,
  L0 trigger 4, 4 background jobs, no compression, 10-bit Bloom filter,
  WAL off, `level_compaction_dynamic_level_bytes=true` (default), so the
  levels in use are L0, L4, L5, L6.
- Preload 1M random keys (100 B values), then 60 s at 20k ops/s.
- Workloads (`run.sh`):
  - `uniform`: 10% point deletes of random keys.
  - `seqdel`: 30% point deletes that sweep the key space in order
    (expiry-style), so deletes cluster in key ranges and produce
    tombstone-dense files.
  - `rangedel`: 5% sequential point deletes + 1% range deletes of 200 keys.
- Modes: 0 = off (identical to stock RocksDB; the code path is gated on the
  option), 1 = always, 2 = cost-based. Density threshold 0.5, max overlap
  ratio 10 (defaults). 2 repeats, mode order rotated per repeat.
- Metrics: write amp = (compaction + flush bytes written) / user bytes in the
  timed phase; "upper-level tombstones" = num_deletions in files above the
  last non-empty level, sampled every second; tombstone age is an upper bound
  from `oldest_ancester_time`; space amp = SST bytes / live key+value bytes
  after flush + WaitForCompact. Every run ends with a full scan checked
  against an in-memory model.

## Results

| workload | mode | rep | load(1m) | write amp | avg upper-level tombstones | peak tombstone age UB (s) | upper tombstones at end | space amp (drained) | SkipLevelTombstone compactions | multi-level (skip >1 level) | verify |
|---|---|---|---|---|---|---|---|---|---|---|---|
| rangedel | 0 | 1 | 79 | 8.26 | 842 | 21 | 1755 | 1.045 | 0 | 0 | PASS |
| rangedel | 0 | 2 | 6 | 8.74 | 755 | 31 | 1755 | 1.059 | 0 | 0 | PASS |
| rangedel | 1 | 1 | 73 | 8.24 | 775 | 24 | 1376 | 1.132 | 0 | 0 | PASS |
| rangedel | 1 | 2 | 18 | 8.83 | 724 | 22 | 1376 | 1.138 | 0 | 0 | PASS |
| rangedel | 2 | 1 | 64 | 8.68 | 715 | 36 | 1376 | 1.056 | 0 | 0 | PASS |
| rangedel | 2 | 2 | 9 | 8.27 | 753 | 38 | 1377 | 1.085 | 0 | 0 | PASS |
| seqdel | 0 | 1 | 132 | 8.68 | 9803 | 36 | 2826 | 1.122 | 0 | 0 | PASS |
| seqdel | 0 | 2 | 27 | 8.58 | 9697 | 14 | 2826 | 1.134 | 0 | 0 | PASS |
| seqdel | 1 | 1 | 134 | 10.15 | 5630 | 30 | 2826 | 1.142 | 30 | 28 | PASS |
| seqdel | 1 | 2 | 32 | 10.84 | 6042 | 7 | 2826 | 1.136 | 32 | 31 | PASS |
| seqdel | 2 | 1 | 104 | 10.49 | 5802 | 11 | 6616 | 1.147 | 32 | 0 | PASS |
| seqdel | 2 | 2 | 25 | 11.05 | 5433 | 25 | 2826 | 1.147 | 28 | 0 | PASS |
| uniform | 0 | 1 | 250 | 8.46 | 11507 | 41 | 14271 | 1.125 | 0 | 0 | PASS |
| uniform | 0 | 2 | 26 | 8.27 | 11579 | 35 | 14047 | 1.132 | 0 | 0 | PASS |
| uniform | 1 | 1 | 216 | 8.27 | 11661 | 31 | 11966 | 1.137 | 0 | 0 | PASS |
| uniform | 1 | 2 | 53 | 8.34 | 11492 | 43 | 13338 | 1.151 | 1 | 1 | PASS |
| uniform | 2 | 1 | 175 | 8.18 | 11683 | 37 | 11085 | 1.128 | 0 | 0 | PASS |
| uniform | 2 | 2 | 40 | 8.38 | 11684 | 46 | 12184 | 1.139 | 0 | 0 | PASS |

Means over the 2 repeats:

| workload | mode | write amp | avg upper-level tombstones |
|---|---|---|---|
| uniform | 0 / 1 / 2 | 8.36 / 8.30 / 8.28 | 11543 / 11576 / 11684 |
| seqdel | 0 / 1 / 2 | 8.63 / 10.50 / 10.77 | 9750 / 5836 / 5618 |
| rangedel | 0 / 1 / 2 | 8.50 / 8.53 / 8.47 | 798 / 750 / 734 |

## Reading

- All 18 runs PASS the model check (no lost or resurrected keys).
- `uniform` and `rangedel`: files almost never reach 50% tombstones, so the
  feature barely fires (one skip compaction in 12 runs). Differences there are
  noise. Range tombstones count as one entry each, so the density metric
  under-rates range-delete files; weighting by covered keys (or by
  `compensated_file_size`) is an obvious next step.
- `seqdel`: with the feature on, the average number of tombstones held above
  the last level drops by about 40% (9.75k -> 5.6-5.8k), at the cost of about
  22-25% more write amplification (8.6 -> 10.5-10.8).
  - Mode 1 (always) ran about 30 real multi-level compactions per run
    (L4 -> L6, skipping L5).
  - Mode 2 (cost-based) never chose to skip: with T=10, the L6 overlap of an
    L4 key range is about 10x the L5 overlap, so score(L6) < score(L5). Its
    ~30 kSkipLevelTombstone compactions are the density trigger falling back
    to a normal one-level compaction. So most of the tombstone reduction (and
    the extra write amp) in this setup comes from the new density trigger,
    not from skipping levels. Mode 1's skips did not cost more write amp than
    mode 2's one-level compactions.
- Hypothesis for the report: skipping pays off when deeper overlap is small
  relative to the dense file (narrow key ranges, sparse deep levels, or
  bottom-heavy deletes); this workload does not show a write-amp win.

## Caveats

- The laptop was busy: 1-minute load average ranged from 6 to 250 during the
  matrix (first repeat was the worst). Timing-derived numbers (peak tombstone
  age) are noisy; byte counts (write amp, tombstone counts) are much less
  sensitive to load but still depend on compaction scheduling.
- Only 2 repeats, 60 s each, one tree shape. Not enough for a claim; use the
  Linux server with sbatch for the report.
- "multi-level" in the table counts compactions whose output level is more
  than one below a non-L0 start level, read from the listener's
  base_input_level -> output_level.

## Reproduce

```bash
JOBS=4 BUILD=/path/to/build-rel tombstone_bench/build.sh
BIN=/path/to/build-rel/tombstone_bench DB=/tmp/tombstone_bench_db RUN_S=60 REPEATS=2 tombstone_bench/run.sh
python3 tombstone_bench/summarize.py tombstone_bench/results
```

The same driver builds against `main`; there the mode 1/2 runs fail fast with
"Extra option not recognized" because the options do not exist.

Unit test results: see `TEST_RESULTS.txt`.

## v2: cascade-savings mode 2 and the trigger/skip ablation (2026-11-24)

### What changed

- Mode 2 (`skip_level_tombstone_compaction=2`) no longer uses
  `score(t) = (t - start) / (B(t) / S)`. That score can never favour a deeper
  level in a leveled tree: B grows about T-fold per level while the numerator
  grows by 1, so it always fell back to a one-level compaction (v1 mode 2
  never skipped).
- New mode 2: B(t) = bytes of the compaction from the start level straight to
  level t, n = normal output level. A one-level-at-a-time cascade of the same
  key range writes about B(n) + ... + B(t); the skip writes B(t). The
  estimated saving savings(t) = B(n) + ... + B(t-1) grows with t, so mode 2
  takes the deepest feasible t with B(t) / B(n) <= `skip_level_max_overlap_ratio`
  (default now 20 = 2T: one extra level, not two). The picker logs B(n) and
  the estimated saving for every skip.
- New option `skip_level_tombstone_density_trigger` (default true). False
  disables the density trigger, so only dense size-triggered compactions are
  extended. This plus ratio 1 (no deeper level with data qualifies) separates
  "trigger" from "skip" for the ablation.
- New `DELPAT=churn` workload in the driver, plus `MULT` / `LBASE_MB` to make
  the tree deeper, and a per-shape tally of SkipLevelTombstone compactions.

### Setup

`ablation.sh`: same driver settings as above except T = 5 and a 1 MB level
base, so L0, L3, L4, L5 and L6 are in use (a start file can skip up to two
levels). Density threshold 0.3 for every non-base config (churn files are
about 45% tombstones, so the 0.5 default never fires on churn). 60 s at 20k
ops/s after a 1M-key preload, 2 repeats, config order rotated per repeat.

- `seqdel`: 30% point deletes sweeping the key space in order.
- `churn`: 90% of ops toggle one of 20,000 hot keys spread evenly over the
  key space (delete it if present, else re-insert it); 10% uniform puts.
  Every hot key is deleted and re-inserted about 50 times per run.

| config | options (plus density 0.3) | meaning |
|---|---|---|
| base | none | stock behaviour |
| trig | mode 2, ratio 1 | density trigger, one level at a time |
| skip | mode 1, trigger off | skip on dense size picks only, deepest level |
| trig+skip | mode 1 | both, deepest level |
| trig+sav | mode 2, ratio 20 | both, cascade-savings bound |
| skip+sav | mode 2, ratio 20, trigger off | skip on size picks, bounded |

"real skips" counts SkipLevelTombstone compactions whose output level is
deeper than the normal one (for L0, the base level seen in the base run).
The few "real skips" in `trig` are artifacts: L0 -> L4 after the trigger
emptied L3 and the dynamic base level moved, and L3 -> L5 where L4 had no
overlapping data (growth 1.0 passes ratio 1).

| workload | config | rep | load | write amp | compaction MB | avg upper tombstones | space amp | SkipLevel compactions | real skips | levels skipped | verify |
|---|---|---|---|---|---|---|---|---|---|---|---|
| churn | base | 1 | 4 | 4.04 | 255 | 20993 | 1.161 | 0 | 0 | 0 | PASS |
| churn | base | 2 | 1 | 4.24 | 270 | 21343 | 1.161 | 0 | 0 | 0 | PASS |
| churn | trig | 1 | 20 | 20.30 | 1555 | 10772 | 1.132 | 238 | 103 | 103 | PASS |
| churn | trig | 2 | 1 | 20.01 | 1531 | 10971 | 1.145 | 457 | 0 | 0 | PASS |
| churn | skip | 1 | 8 | 10.72 | 788 | 12394 | 1.126 | 57 | 57 | 114 | PASS |
| churn | skip | 2 | 2 | 10.64 | 782 | 12601 | 1.139 | 56 | 56 | 112 | PASS |
| churn | trig+skip | 1 | 4 | 19.91 | 1523 | 10665 | 1.130 | 116 | 113 | 226 | PASS |
| churn | trig+skip | 2 | 3 | 19.79 | 1514 | 10794 | 1.132 | 113 | 113 | 226 | PASS |
| churn | trig+sav | 1 | 3 | 20.95 | 1606 | 10827 | 1.130 | 235 | 113 | 114 | PASS |
| churn | trig+sav | 2 | 2 | 21.59 | 1658 | 10258 | 1.137 | 256 | 114 | 114 | PASS |
| churn | skip+sav | 1 | 2 | 10.49 | 770 | 12487 | 1.135 | 56 | 56 | 112 | PASS |
| churn | skip+sav | 2 | 2 | 10.52 | 773 | 12608 | 1.136 | 56 | 56 | 112 | PASS |
| seqdel | base | 1 | 54 | 9.15 | 791 | 11930 | 1.176 | 0 | 0 | 0 | PASS |
| seqdel | base | 2 | 2 | 9.23 | 798 | 11107 | 1.193 | 0 | 0 | 0 | PASS |
| seqdel | trig | 1 | 30 | 30.55 | 2871 | 3306 | 1.162 | 207 | 7 | 7 | PASS |
| seqdel | trig | 2 | 3 | 23.39 | 2175 | 3459 | 1.162 | 172 | 7 | 7 | PASS |
| seqdel | skip | 1 | 16 | 10.86 | 957 | 11103 | 1.164 | 47 | 47 | 78 | PASS |
| seqdel | skip | 2 | 3 | 10.93 | 964 | 10076 | 1.167 | 46 | 46 | 76 | PASS |
| seqdel | trig+skip | 1 | 7 | 29.48 | 2767 | 3027 | 1.148 | 146 | 83 | 141 | PASS |
| seqdel | trig+skip | 2 | 3 | 31.91 | 3004 | 2752 | 1.143 | 157 | 79 | 136 | PASS |
| seqdel | trig+sav | 1 | 4 | 31.32 | 2946 | 3123 | 1.153 | 167 | 67 | 123 | PASS |
| seqdel | trig+sav | 2 | 3 | 30.91 | 2906 | 2438 | 1.151 | 169 | 68 | 125 | PASS |
| seqdel | skip+sav | 1 | 3 | 11.04 | 520 | 9864 | 1.137 | 25 | 25 | 40 | PASS |
| seqdel | skip+sav | 2 | 3 | 10.96 | 967 | 9948 | 1.179 | 48 | 48 | 78 | PASS |

| workload | config | runs | mean write amp | vs base | mean avg upper tombstones | vs base | mean real skips |
|---|---|---|---|---|---|---|---|
| churn | base | 2 | 4.14 | +0.0% | 21168 | +0.0% | 0.0 |
| churn | trig | 2 | 20.15 | +386.6% | 10872 | -48.6% | 51.5 |
| churn | skip | 2 | 10.68 | +157.8% | 12498 | -41.0% | 56.5 |
| churn | trig+skip | 2 | 19.85 | +379.2% | 10730 | -49.3% | 113.0 |
| churn | trig+sav | 2 | 21.27 | +413.5% | 10542 | -50.2% | 113.5 |
| churn | skip+sav | 2 | 10.51 | +153.7% | 12548 | -40.7% | 56.0 |
| seqdel | base | 2 | 9.19 | +0.0% | 11518 | +0.0% | 0.0 |
| seqdel | trig | 2 | 26.97 | +193.4% | 3382 | -70.6% | 7.0 |
| seqdel | skip | 2 | 10.89 | +18.5% | 10590 | -8.1% | 46.5 |
| seqdel | trig+skip | 2 | 30.70 | +234.0% | 2890 | -74.9% | 81.0 |
| seqdel | trig+sav | 2 | 31.11 | +238.5% | 2780 | -75.9% | 67.5 |
| seqdel | skip+sav | 2 | 11.00 | +19.7% | 9906 | -14.0% | 36.5 |

### Findings

1. **Skipping did not reduce write amp in either workload.** With the trigger
   off, skipping cost +18.5% (`skip`) / +19.7% (`skip+sav`) on seqdel and
   +158% / +154% on churn, for 8-14% (seqdel) and 41% (churn) fewer
   tombstones above the last level. The two bounded/unbounded variants are
   within noise of each other.
2. **The density trigger is the dominant cost.** At threshold 0.3 it raises
   write amp 2.9x (seqdel) and 4.9x (churn) on its own (`trig`). Its outputs
   stay dense (churn keeps re-deleting the same keys), so the same key ranges
   are picked again and again: in churn `trig` wrote 1.4 GB in L4 -> L5
   compactions versus 0.3 GB in base. It needs hysteresis or a rate limit.
3. **The cascade-savings model is too optimistic.** It charges the cascade the
   full B(j) at every level for this key range alone. In a real leveled tree
   the deep overlap is shared: when level j-1 later compacts into j, its file
   carries data from many upper compactions, so the per-byte cost of a level
   crossing is about T (amortized), not B(j)/S. With O_j ~ S T^(j-i) a skip of
   k levels writes about S T^k now, versus about S T k amortized for the
   cascade, so a skip only pays when the deep overlap is unusually small. In
   churn there is a second effect: a tombstone is usually superseded by the
   key's re-insert near the top of the tree, so in the cascade most
   tombstones never travel far, while a skip pays the full deep overlap at
   once.
4. **Mode 2's bound rarely bound.** `trig+sav` skipped fewer levels than
   `trig+skip` on seqdel (about 124 vs 139 levels per run) with no write-amp
   difference; on churn the two were identical in skips.

Finding (3) led to v3 below.

## v3: amortized-cascade mode 2 (2026-11-24)

### Rule

S = start-file bytes, n = normal output level, B(t) = input bytes of the
compaction from the start level straight to t, T =
`max_bytes_for_level_multiplier`. The first hop costs B(n) either way. Each
later one-level hop of S bytes costs about (1 + T) * S in steady state,
because the overlap it meets is shared with data arriving from other
compactions. The skip pays the measured extra overlap B(t) - B(n) now:

    net(t) = (t - n) * (1 + T) * S - (B(t) - B(n))

Mode 2 takes the t with the largest positive net(t) (ties deeper), still
capped by `skip_level_max_overlap_ratio` * B(n). For a one-level skip this is
simply "skip past level n+1 only if the overlap there is at most (1 + T) * S",
against the T^2 * S a uniformly spread key range would meet. v2's rule
compared against B(n) + ... + B(t-1), which charged the cascade the whole deep
overlap for this key range alone.

### Results

Same setup as the v2 ablation (`ONLY="base trig+sav skip+sav"
OUT=.../ablation_v3 ablation.sh`); base was rerun for a same-session
reference. v2 rows are from the table above.

| workload | config | rep | load | write amp | compaction MB | avg upper tombstones | space amp | SkipLevel compactions | real skips | levels skipped | verify |
|---|---|---|---|---|---|---|---|---|---|---|---|
| churn | base | 1 | 33 | 4.13 | 262 | 20666 | 1.175 | 0 | 0 | 0 | PASS |
| churn | base | 2 | 2 | 4.14 | 263 | 21772 | 1.170 | 0 | 0 | 0 | PASS |
| churn | trig+sav | 1 | 16 | 20.49 | 1570 | 10815 | 1.131 | 226 | 113 | 113 | PASS |
| churn | trig+sav | 2 | 2 | 20.37 | 1560 | 10924 | 1.131 | 224 | 113 | 113 | PASS |
| churn | skip+sav | 1 | 7 | 4.07 | 257 | 19119 | 1.164 | 56 | 56 | 56 | PASS |
| churn | skip+sav | 2 | 3 | 4.03 | 254 | 18704 | 1.166 | 56 | 56 | 56 | PASS |
| seqdel | base | 1 | 296 | 9.29 | 804 | 10926 | 1.194 | 0 | 0 | 0 | PASS |
| seqdel | base | 2 | 4 | 9.30 | 805 | 10428 | 1.164 | 0 | 0 | 0 | PASS |
| seqdel | trig+sav | 1 | 137 | 19.32 | 1779 | 4063 | 1.172 | 149 | 81 | 81 | PASS |
| seqdel | trig+sav | 2 | 4 | 20.11 | 1857 | 3131 | 1.173 | 144 | 82 | 82 | PASS |
| seqdel | skip+sav | 1 | 62 | 9.15 | 790 | 9682 | 1.172 | 48 | 48 | 48 | PASS |
| seqdel | skip+sav | 2 | 3 | 9.06 | 782 | 11399 | 1.197 | 44 | 44 | 44 | PASS |

| workload | config | runs | mean write amp | vs base | mean avg upper tombstones | vs base | mean real skips |
|---|---|---|---|---|---|---|---|
| churn | base | 2 | 4.13 | +0.0% | 21219 | +0.0% | 0.0 |
| churn | trig+sav | 2 | 20.43 | +394.1% | 10870 | -48.8% | 113.0 |
| churn | skip+sav | 2 | 4.05 | -1.9% | 18912 | -10.9% | 56.0 |
| seqdel | base | 2 | 9.29 | +0.0% | 10677 | +0.0% | 0.0 |
| seqdel | trig+sav | 2 | 19.72 | +112.2% | 3597 | -66.3% | 81.5 |
| seqdel | skip+sav | 2 | 9.10 | -2.0% | 10540 | -1.3% | 46.0 |

| config | v2 write amp vs base | v3 write amp vs base | v3 upper tombstones vs base |
|---|---|---|---|
| seqdel skip+sav | +19.7% | -2.0% | -1.3% |
| churn skip+sav | +153.7% | -1.9% | -10.9% |
| seqdel trig+sav | +238.5% | +112.2% | -66.3% |
| churn trig+sav | +413.5% | +394.1% | -48.8% |

### Findings

1. **With the amortized rule, skipping no longer costs write amp.** Without
   the density trigger, mode 2 wrote about 2% less than base on both
   workloads (both repeats agree; base repeats were within 0.2% of each
   other) and held 11% fewer upper-level tombstones on churn. The skips it
   still takes are L0 -> L4 (past L3) and L3 -> L5, i.e. one level where the
   overlap was small. The gain is small and from 2 repeats on a laptop with
   very uneven load (1-minute load 3 to 296), so treat it as "no longer worse,
   possibly slightly better", not as a measured win.
2. **The density trigger is still the problem** (addressed in v4 below). `trig+sav` is +112% (seqdel)
   and +394% (churn). Almost all of it is one-level kSkipLevelTombstone
   compactions the trigger picks over and over: L5 -> L6 (1.2 GB per seqdel
   run) and L4 -> L5 (1.4 GB per churn run). The outputs stay above the 0.3
   density threshold, so they are picked again. The trigger needs hysteresis
   (for example, do not re-pick a file produced by a kSkipLevelTombstone
   compaction until it has been rewritten for another reason), a budget, or a
   much higher threshold before it is useful.
3. The rule uses the configured T. With dynamic level bytes the real ratio
   between adjacent levels can differ, so a better estimate would use the
   actual level sizes.

## v4: trigger gate + hysteresis, 5 repeats (2026-11-24)

### Change

The density trigger used to compact a dense file one level down whenever no
deeper target qualified, and its outputs were usually still dense, so it
picked the same tombstones again at every level (v3 `trig+sav`: +112% /
+394% write amp). Two changes:

1. **Gate:** the trigger tries the dense files densest first and runs the
   first one for which the mode-2 rule finds a profitable deeper target. A
   dense file that cannot skip is left to size-based compaction; the trigger
   never runs a one-level compaction.
2. **Hysteresis:** files written by a kSkipLevelTombstone compaction carry an
   in-memory flag (`FileMetaData::from_skip_level_compaction`, false after
   reopen) and are not trigger candidates until another compaction rewrites
   them. Size-based compactions may still pick and extend them.

Size-triggered compactions are unchanged (dense ones are still extended when
the rule finds a profitable target), so `skip+sav` behaves as in v3.

### Results

`REPEATS=5 ONLY="base trig+sav skip+sav" OUT=.../ablation_v4 ablation.sh`,
same tree and workloads as v2/v3. 30 runs, all PASS.

| workload | config | rep | load | write amp | compaction MB | avg upper tombstones | space amp | SkipLevel compactions | real skips | levels skipped | verify |
|---|---|---|---|---|---|---|---|---|---|---|---|
| churn | base | 1 | 32 | 4.24 | 271 | 21077 | 1.167 | 0 | 0 | 0 | PASS |
| churn | base | 2 | 5 | 4.23 | 270 | 21503 | 1.167 | 0 | 0 | 0 | PASS |
| churn | base | 3 | 2 | 4.25 | 272 | 21344 | 1.160 | 0 | 0 | 0 | PASS |
| churn | base | 4 | 2 | 4.24 | 271 | 21746 | 1.149 | 0 | 0 | 0 | PASS |
| churn | base | 5 | 3 | 4.28 | 274 | 21893 | 1.165 | 0 | 0 | 0 | PASS |
| churn | trig+sav | 1 | 26 | 5.87 | 401 | 17670 | 1.157 | 113 | 113 | 113 | PASS |
| churn | trig+sav | 2 | 4 | 5.92 | 405 | 17237 | 1.154 | 113 | 113 | 113 | PASS |
| churn | trig+sav | 3 | 2 | 5.80 | 395 | 16946 | 1.151 | 113 | 113 | 113 | PASS |
| churn | trig+sav | 4 | 2 | 5.80 | 395 | 17325 | 1.154 | 113 | 113 | 113 | PASS |
| churn | trig+sav | 5 | 1 | 5.93 | 405 | 17093 | 1.151 | 113 | 113 | 113 | PASS |
| churn | skip+sav | 1 | 16 | 4.02 | 253 | 19283 | 1.164 | 57 | 57 | 57 | PASS |
| churn | skip+sav | 2 | 5 | 4.24 | 270 | 19395 | 1.145 | 56 | 56 | 56 | PASS |
| churn | skip+sav | 3 | 2 | 4.06 | 256 | 18732 | 1.159 | 56 | 56 | 56 | PASS |
| churn | skip+sav | 4 | 4 | 4.15 | 263 | 19446 | 1.153 | 56 | 56 | 56 | PASS |
| churn | skip+sav | 5 | 2 | 4.27 | 273 | 18904 | 1.151 | 57 | 57 | 57 | PASS |
| seqdel | base | 1 | 162 | 9.14 | 789 | 11224 | 1.187 | 0 | 0 | 0 | PASS |
| seqdel | base | 2 | 4 | 9.15 | 791 | 11936 | 1.166 | 0 | 0 | 0 | PASS |
| seqdel | base | 3 | 3 | 9.26 | 801 | 13206 | 1.183 | 0 | 0 | 0 | PASS |
| seqdel | base | 4 | 2 | 9.38 | 813 | 11516 | 1.176 | 0 | 0 | 0 | PASS |
| seqdel | base | 5 | 2 | 9.26 | 801 | 9856 | 1.170 | 0 | 0 | 0 | PASS |
| seqdel | trig+sav | 1 | 76 | 9.64 | 838 | 8499 | 1.185 | 62 | 62 | 62 | PASS |
| seqdel | trig+sav | 2 | 9 | 9.63 | 837 | 10589 | 1.177 | 67 | 67 | 67 | PASS |
| seqdel | trig+sav | 3 | 3 | 9.72 | 846 | 11196 | 1.168 | 61 | 61 | 61 | PASS |
| seqdel | trig+sav | 4 | 2 | 9.55 | 829 | 10683 | 1.178 | 65 | 65 | 65 | PASS |
| seqdel | trig+sav | 5 | 2 | 9.77 | 851 | 10078 | 1.172 | 67 | 67 | 67 | PASS |
| seqdel | skip+sav | 1 | 41 | 8.99 | 775 | 11220 | 1.187 | 48 | 48 | 48 | PASS |
| seqdel | skip+sav | 2 | 5 | 9.17 | 792 | 11061 | 1.171 | 40 | 40 | 40 | PASS |
| seqdel | skip+sav | 3 | 4 | 8.87 | 763 | 11466 | 1.188 | 44 | 44 | 44 | PASS |
| seqdel | skip+sav | 4 | 1 | 8.97 | 773 | 11308 | 1.184 | 40 | 40 | 40 | PASS |
| seqdel | skip+sav | 5 | 3 | 8.91 | 767 | 9050 | 1.194 | 43 | 43 | 43 | PASS |

| workload | config | runs | mean write amp | vs base | mean avg upper tombstones | vs base | mean real skips |
|---|---|---|---|---|---|---|---|
| churn | base | 5 | 4.25 | +0.0% | 21513 | +0.0% | 0.0 |
| churn | trig+sav | 5 | 5.86 | +38.0% | 17254 | -19.8% | 113.0 |
| churn | skip+sav | 5 | 4.15 | -2.4% | 19152 | -11.0% | 56.4 |
| seqdel | base | 5 | 9.24 | +0.0% | 11548 | +0.0% | 0.0 |
| seqdel | trig+sav | 5 | 9.66 | +4.6% | 10209 | -11.6% | 64.4 |
| seqdel | skip+sav | 5 | 8.98 | -2.8% | 10821 | -6.3% | 43.0 |

Write amp per run, sorted (mean, sample sd; t = Welch t vs base):

| workload | config | runs | mean | sd | t vs base |
|---|---|---|---|---|---|
| seqdel | base | 9.14 9.16 9.26 9.26 9.38 | 9.24 | 0.10 | |
| seqdel | skip+sav | 8.87 8.91 8.97 8.99 9.17 | 8.98 (-2.8%) | 0.12 | -3.8 |
| seqdel | trig+sav | 9.55 9.63 9.64 9.72 9.77 | 9.66 (+4.6%) | 0.09 | +7.3 |
| churn | base | 4.23 4.24 4.24 4.26 4.28 | 4.25 | 0.02 | |
| churn | skip+sav | 4.02 4.06 4.15 4.24 4.27 | 4.15 (-2.4%) | 0.11 | -2.0 |
| churn | trig+sav | 5.80 5.80 5.87 5.92 5.93 | 5.86 (+38.0%) | 0.06 | +55 |

### Findings

1. **The gate and hysteresis fixed the runaway trigger** (+112% -> +4.6% on
   seqdel, +394% -> +38% on churn), and every trigger compaction now skips a
   level. They did not push write amp below base: the trigger still runs
   compactions size-based compaction would not have run yet, and on churn it
   fires on almost every L0 flush (113 L0 -> L4 per run). It buys 12% (seqdel)
   to 20% (churn) fewer upper-level tombstones for that cost.
2. **Skip-only mode 2 (no trigger) is the write-amp configuration.** seqdel:
   -2.8%, 5 of 5 runs below the base mean, t = -3.8 (about p < 0.01 with ~8
   degrees of freedom). churn: -2.4%, 4 of 5 below the base mean but with a
   much larger spread than base (sd 0.11 vs 0.02), t = -2.0 (about p ~ 0.1),
   so on churn the improvement is suggestive, not established.

## v5: density trigger ignores L0, 3 repeats (2026-11-25)

### Diagnosis

`tombstone_bench` now prints, for every compaction that starts at L0, how many
L0 files it took ("L0 compactions:" line). On the v4 code
(`results/diag_v4_l0`, one run each):

| v4 | L0 compactions | L0 files each | MB read per compaction | MB read per L0 file |
|---|---|---|---|---|
| churn base | 56 | 2.00 | 1.7 | 0.85 |
| churn skip+sav | 56 | 2.00 | 3.4 | 1.7 |
| churn trig+sav | 113 | 1.00 | 2.8 | 2.8 |
| seqdel trig+sav (trigger picks only) | 56 | 1.25 | 3.3 | 2.6 |

The trigger took each dense L0 file as soon as it was flushed. L0 files span
about the whole key range, so the overlap below is about the same for 1 file
or 2, and the trigger paid it once per file. The amortized rule did not object:
it compares "skip now" with "cascade now", not with "wait and batch". Charging
the trigger for its whole compaction was also considered and rejected. Churn's
real per-level cost (about 1.2 × S) is so far below the (1 + T) · S estimate
that such a rule would still approve these compactions.

### Change

`ComputeTombstoneDenseFiles()` starts at L1: dense L0 files are no longer
trigger candidates. They wait for the L0 file-count compaction, which still
skips levels when the amortized rule says so. The skip rule itself, the
trigger's "skip at least one level" gate and the hysteresis flag are
unchanged.

### Results

Same setup as v4 (MULT=5, LBASE_MB=1, θ = 0.3, mode 2, R = 20), 3 repeats,
rotated config order. `skip` = skip+sav (trigger off), `trig+skip` = trig+sav.

| workload | config | write amp per run | mean | vs base | Welch t vs base | upper tombstones vs base | L0 files per L0 compaction |
|---|---|---|---|---|---|---|---|
| churn | base | 4.26, 4.20, 4.24 | 4.23 | | | | 2.00 |
| churn | skip | 4.09, 4.14, 4.12 | 4.11 | −2.8% | −5.1 | −11.4% | 2.00 |
| churn | trig+skip | 4.05, 4.15, 4.13 | 4.11 | −2.8% | −3.5 | −10.8% | 2.00 |
| seqdel | base | 9.41*, 9.25, 9.28 | 9.31 | | | | 2.00 |
| seqdel | skip | 8.77, 9.01, 9.02 | 8.93 | −4.1% | −4.0 | −1.3% | 1.99 |
| seqdel | trig+skip | 8.81, 9.17, 9.15 | 9.04 | −2.9% | −2.1 | −0.9% | 1.97 |

\* Repeat 1 ran under a load average of about 100 (other work on the laptop);
its seqdel runs all wrote less, base included. Without repeat 1, seqdel is
−2.7% (skip) and −1.1% (trig+skip).

Full per-run table: `python3 tombstone_bench/summarize_ablation.py
tombstone_bench/results/ablation_v5`.

### Findings

- **The trigger no longer costs write amp.** v4 trig+skip was +38% (churn) and
  +4.6% (seqdel); v5 is −2.8% and −2.9%. L0 compactions take 2 files again in
  every config.
- **Trigger + skip is now about equal to skip-only.** The difference is within
  noise: Welch t = −0.2 on churn and +0.8 on seqdel. On churn the trigger finds
  no L1+ candidates and the runs are the same compactions (56 L0 -> L4 skips).
  On seqdel it adds a few L3 -> L5 skips that save about as much as they cost.
- **The trigger now adds nothing measurable** on these workloads, for write amp
  or for upper-level tombstones. Its remaining use is for trees where dense
  files sit in L1+ while no size-based compaction reaches them, for example
  after deletes stop. These benchmarks do not exercise that case.
- Skip-only is still the setting that saves write amp: −2.8% (churn) and −2.7%
  to −4.1% (seqdel).

## fade-plus-skip-level: FADE + skip-level (2026-11-25)

Branch `fade-plus-skip-level` = `fade-oneshot-claude` (FADE) merged with
`skip-level-tombstone-draft` v5. Skip-level runs without the density trigger
(`skip_level_tombstone_density_trigger=false`, the best config from v5).

### What the combination does

- FADE decides **when** a file's tombstones must move: its TTL for that level
  has expired (`CompactionReason::kFadeTtl`).
- Skip-level decides **how far** they go. Dense FADE picks, like dense
  size-triggered picks, go through the mode 2 amortized rule and can cross
  several levels in one rewrite. Those compactions are reported as
  `kSkipLevelTombstone`, so the FadeTtl counts below are FADE picks that
  stayed one level.

### Checks before benchmarking

- Clean build with warnings as errors.
- Tests:

  | suite | result |
  |---|---|
  | `compaction_picker_test` | 181/181 |
  | `db_skip_level_compaction_test` | 8/8 |
  | `db_compaction_test` | 390/390, including the 6 FADE tests |
  | `options_test` | 83/83 |
  | `version_edit_test` | 35/35 |

  `options_settable_test` compiles its tests only on Linux with GCC, so it
  ran 0 tests on macOS, the same as on the parent branches.
- New test `FadePickSkipsLevelsWhenCheap`: an expired L1 tombstone file goes
  L1 -> L2 with skip off, and L1 -> L3 (dropping its tombstones) with mode 2.
- `RandomizedAgainstModel` now preloads L2 and L3. Before, its tree never got
  past L1, so neither feature ever ran in it. It runs every skip mode with
  FADE off and on, against an in-memory model.
- Smoke run: 15 s per config and workload; all eight passed.

### Setup

Same tree as v5 (MULT=5, LBASE_MB=1, θ = 0.3, mode 2, R = 20). D_th = 10 s,
3 repeats, rotated order, 60 s per run.

"True tombstone age" is new: the oldest tombstone still above the last level.
It is measured from a table-properties collector (smallest tombstone seqno per
SST) and a seqno -> time map, the same method as fade_bench, so it does not
depend on FADE's own bookkeeping. It is sampled every second.

D_th = 10 s was chosen because a 15 s smoke run with D_th = 30 s never
reached the bound: churn's natural peak age is about 30-40 s over 60 s, and
seqdel's is about 6 s.

### Results

| workload | config | write amp (3 runs) | mean | vs base | vs FADE | Welch t | peak true tombstone age | samples over 10 s | upper tombstones vs base |
|---|---|---|---|---|---|---|---|---|---|
| seqdel | base | 9.37, 9.22, 9.24 | 9.28 | | | | 6.3-7.7 s | 0/59 | |
| seqdel | skip | 8.93, 9.13, 8.90 | 8.99 | -3.1% | | -3.5 vs base | 4.8-5.7 s | 0/59 | -14% |
| seqdel | FADE | 10.36, 10.04, 10.50 | 10.30 | +11.0% | | | 5.9-6.3 s | 0/59 | -6% |
| seqdel | FADE + skip | 10.03, 10.09, 9.70 | 9.94 | +7.1% | -3.5% | -2.0 vs FADE | 6.1-6.4 s | 0/59 | -12% |
| churn | base | 4.29, 4.28, 4.30 | 4.29 | | | | 29.6-40.9 s | 49/59 | |
| churn | skip | 4.17, 4.11, 4.11 | 4.13 | -3.7% | | -7.7 vs base | 24.4-37.1 s | 49/59 | -12% |
| churn | FADE | 8.96, 9.55, 8.29 | 8.93 | +108% | | | 11.0-11.3 s | 4-6/59 | -37% |
| churn | FADE + skip | 8.47, 9.34, 8.37 | 8.73 | +103% | -2.3% | -0.4 vs FADE | 10.8 s | 4-5/59 | -47% |

Compaction mix per run (FadeTtl / SkipLevelTombstone):

| workload | FADE | FADE + skip |
|---|---|---|
| seqdel | about 52 / 0 | about 40 / 38 |
| churn | about 145 / 0 | about 45 / 56 |

On churn the skips are mostly L0 -> L5 directly, past two levels.

### Findings

- **Skip-level's saving survives the merge:**
  - On its own it gives -3.1% (seqdel) and -3.7% (churn) vs base, in line with
    v5 on its own branch.
  - On top of FADE it gives -3.5% (seqdel, t = -2.0) and -2.3% (churn) vs FADE.
  - On churn, FADE's run-to-run spread (sd 0.63) is larger than the gap, so 3
    repeats don't make the churn number significant (t = -0.4). Paired by
    repeat, FADE + skip wrote less in 2 of 3 runs.
- **FADE's bound is kept, slightly tighter.**
  - Churn peak age: 10.8 s with FADE + skip vs 11.0-11.3 s with FADE alone.
  - Upper-level tombstones: -47% vs -37%.
  - A skip sends expired tombstones further, so later TTL expiries have less
    to do; FADE-only compactions drop from about 145 to about 45 on churn.
- **FADE is the expensive part:** +11% (seqdel) and +108% (churn) at
  D_th = 10 s.
  - With T = 5 and four stages, FADE's exponential split gives the top levels
    TTLs well under a second, so it fires constantly on churn.
  - On seqdel the natural peak age is already under D_th, so its cost there
    comes entirely from those short per-level TTLs.
  - Tuning D_th or the split is the obvious next lever. Skip-level recovers a
    few percent of FADE's cost, not most of it.

## fade-plus-skip-level: D_th sweep (2026-11-25)

The run at D_th = 10 s above left FADE + skip at +103% write amp vs base on
churn. Where that cost comes from (MB written by shape, churn, repeat 1):

| D_th | config | L0 -> base | L3 -> L4 | L4 -> L5 | **L5 -> L6** |
|---|---|---|---|---|---|
| 10 s | base | 74 | 80 | 83 | **37** |
| 10 s | FADE | 53 | 5 | 198 | **392** |
| 10 s | FADE + skip | 131 (L0 -> L4/L5) | | 81 | **398** |
| 20 s | FADE | 62 | 54 | 130 | **260** |
| 40 s | FADE | 61 | 95 | 95 | **93** |

What the breakdown shows:

- Almost all of FADE's extra cost is in its last stage: tombstones in L5 must
  reach L6 (about 70 MB) within D_th.
- The upper levels' sub-second TTLs cost little (5 MB at D_th = 10 s).
- Skip-level lowers bytes everywhere except that last stage, so turning it off
  for the top levels would not help.
- D_th scales every stage's deadline, so it is the lever. The sweep reruns
  base, FADE and FADE + skip at 20 s and 40 s, 3 repeats each.

Two background jobs (a Time Machine backup and indexing) pushed the load
average to about 35 during the first attempt. The sweep was restarted with
`MAX_LOAD=8`: before each run, `ablation.sh` waits up to 10 min for the
1-minute load to drop below 8. All runs then started at load 3-8.

### Results (write amp, 3 runs each; Welch t in brackets)

| workload | D_th | base | FADE (vs base) | FADE + skip (vs base) | FADE + skip vs FADE | FADE + skip peak true tombstone age | samples over D_th |
|---|---|---|---|---|---|---|---|
| seqdel | 10 s | 9.28 | +11.0% | +7.1% | **-3.5%** (t = -2.0) | 6.1-6.4 s | 0/59 |
| seqdel | 20 s | 9.40 | -0.2% | **-3.7%** (t = -2.8) | **-3.4%** (t = -2.6) | 4.9-8.8 s | 0/59 |
| seqdel | 40 s | 9.31 | +1.3% | **-4.2%** (t = -4.3) | **-5.4%** (t = -5.8) | 5.6-6.4 s | 0/59 |
| churn | 10 s | 4.29 | +108% | +103% | -2.3% (t = -0.4) | 10.8 s | 4-5/59 |
| churn | 20 s | 4.09 | +66% | +57% | -5.3% (t = -0.6) | 16.5-19.5 s | 0/59 |
| churn | 40 s | 4.33 | +20% | +22% | +1.2% (t = 0.9) | 19.6-20.5 s | 0/59 |

For reference, base's peak true tombstone age is 5.6-7.9 s on seqdel and 29-44
s on churn.

### Findings

- **Raising D_th removes most of the penalty:**

  | FADE + skip vs base | churn | seqdel |
  |---|---|---|
  | D_th = 10 s | +103% | +7.1% |
  | D_th = 20 s | +57% | -3.7% |
  | D_th = 40 s | +22% | -4.2% |

  On seqdel at 20 s and above, FADE + skip writes *less* than base while
  bounding tombstone age.
- **Skip-level keeps saving on top of FADE on seqdel at every D_th:** -3.4% to
  -5.4%, significant at 20 s and 40 s.
- **On churn the saving over FADE is not significant at any D_th:**
  - At 10-20 s it is -2% to -5%, but with t > -1: FADE's runs vary by up to
    20%.
  - At 40 s it is a tie (+1.2%). The skips replace FADE's own L3 -> L4 work
    byte for byte (346 vs 344 MB in repeat 1), and both take 2.00 L0 files per
    compaction, so there is no batching regression.
  - FADE + skip still has about 3x fewer upper-level tombstones on churn
    (-23% vs -8% at 40 s).
- **The remaining churn cost at 40 s (+20-22%) is FADE's.**
  - At 40 s, L3's and L4's cumulative deadlines are only about 1.5 s and 8 s,
    so FADE still runs about 88 compactions per run.
  - Two FADE-side options remain untested here: a minimum per-level TTL, or
    leaving the stages above the base level to size compaction and giving
    their budget to deeper stages.

## fade-kiwi-plus-skip-level: KiWi + FADE + skip-level (2026-11-25)

Branch `fade-kiwi-plus-skip-level` is `fade-plus-skip-level` with `kiwi` (KiWi
delete tiles and `DB::SecondaryDeleteBelow`) merged in. KiWi is our
colleagues' branch; it is merged here only to check that the features combine.

### Merge

- The only conflict was in VersionEdit's NewFile4 custom fields. FADE's
  `kOldestTombstoneTime` is tag 20, safe for old readers to ignore. KiWi's
  `kKiWiDeleteFloor` is unignorable and sits after `kPathId`. The tags do not
  collide, and both are encoded and decoded.
- KiWi's input-record check in CompactionJob already loops over every input
  level, so multi-level skip compactions are covered.

### Tests

| suite | result |
|---|---|
| `compaction_picker_test` | 181/181 |
| `db_skip_level_compaction_test` | 8/8 |
| `db_compaction_test` | 390/390 |
| `options_test` | 84/84 |
| `version_edit_test` | 36/36 |
| `kiwi_test` | 42/42 |
| `table_test` | 5631/5631 |
| `db_etc3_test` | 25/25 |

New: `DBKiWiSecondaryDeleteTest.SecondaryDeleteWithFadeAndSkipLevel`.

- Skip-level compactions read several levels' files at once, each through its
  own delete floor. The test checks that entries hidden by
  `SecondaryDeleteBelow` stay hidden through those rewrites.
- It uses a preloaded 4-level tree, skip modes 1 and 2, FADE on, and a model
  check after the delete, after further compactions and after a reopen.

### Benchmark

Same setup as `fade-plus-skip-level` at D_th = 10 s, 3 repeats. The `kiwi`
configs set `KIWI_TILE_PAGES=4`; values then carry their version as the KiWi
delete key. All 36 runs pass the model check. The third repeat ran after a
pause; one `churn base` run had a load average of 23 and was about 3% low.

| workload | config | write amp (3 runs) | mean | vs base | vs FADE in the same layout (Welch t) |
|---|---|---|---|---|---|
| seqdel | base | 9.35, 9.14, 9.39 | 9.29 | | |
| seqdel | FADE | 10.39, 10.15, 10.20 | 10.25 | +10.3% | |
| seqdel | FADE + skip | 9.79, 9.93, 9.73 | 9.82 | +5.6% | **-4.2%** (t = -4.6) |
| seqdel | KiWi | 9.24, 9.39, 9.33 | 9.32 | +0.3% | |
| seqdel | KiWi + FADE | 10.60, 10.73, 10.66 | 10.66 | +14.8% | |
| seqdel | KiWi + FADE + skip | 10.06, 9.56, 10.01 | 9.88 | +6.3% | **-7.3%** (t = -4.8) |
| churn | base | 4.26, 4.25, 4.13 | 4.22 | | |
| churn | FADE | 8.42, 9.48, 8.38 | 8.76 | +108% | |
| churn | FADE + skip | 8.45, 9.76, 8.44 | 8.88 | +111% | +1.5% (t = 0.2) |
| churn | KiWi | 4.38, 4.26, 4.38 | 4.34 | +3.0% | |
| churn | KiWi + FADE | 8.85, 9.10, 8.58 | 8.84 | +110% | |
| churn | KiWi + FADE + skip | 8.71, 8.87, 8.93 | 8.84 | +110% | -0.1% (t = 0.0) |

- **KiWi's layout alone is about write-amp neutral:** +0.3% (seqdel), +3.0%
  (churn).
- **Skip-level's saving over FADE survives with KiWi on, and is larger on
  seqdel** (-7.3%).
- **On churn at D_th = 10 s both layouts tie,** as on `fade-plus-skip-level`.
  FADE's last-stage rewrites dominate there; see the D_th sweep above for the
  settings where FADE + skip is cheap.
