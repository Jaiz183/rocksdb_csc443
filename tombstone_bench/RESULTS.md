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

Next step suggested by (3): replace the saving estimate with the amortized
one, i.e. skip to t only if B(t) <= S * (1 + T * (t - start)), which accepts a
skip exactly when the measured deep overlap is well below the tree's average
(for example sequential deletes over a range that has little old data).
