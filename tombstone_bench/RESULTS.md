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
