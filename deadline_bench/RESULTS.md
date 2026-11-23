# Tombstone deadline + deadline coalescing: first benchmark

LLM-drafted prototype (Claude, 2026-11-23). Numbers come from a noisy laptop and
are a smoke-level comparison, not a result for the report.

## Setup

- Driver: `deadline_bench/deadline_bench.cc`, public API only. Run with
  `REL=<release build dir> OUT=deadline_bench/results RUN_S=60 REPEATS=2 DEADLINE=30 PRELOAD=500000 IDLE_S=40 OPS=15000 ./deadline_bench/run.sh`.
- Tree: 1 MB write buffer and target file size, 4 MB level base, T=10, L0
  trigger 4, 4 background jobs, no compression, WAL off, 10-bit Bloom filter,
  7 levels with dynamic level bytes.
- Workload: preload 500k keys (100 B values), then 60 s at 15k ops/s with
  uniform keys, 90% puts and 10% point deletes, then 40 s idle while sampling.
- Configs (all with this branch's binary):
  - `base`: feature off. This is the same code path as `main`, because all new
    work is gated on `tombstone_deadline_seconds > 0`.
  - `deadline`: `tombstone_deadline_seconds=30`.
  - `coalesce`: `tombstone_deadline_seconds=30;deadline_coalescing_window_seconds=10`.
- Tombstone age is an upper bound: `now - oldest_ancester_time` over non-last
  level files with `num_deletions > 0`, sampled once per second.
- Machine load was very high because another RocksDB build was running at the
  same time. 1-minute load average before each run: 324, 250, 191, 128, 112, 78.
- Every run passed the full-scan check against an in-memory model.

## Results (r1 / r2)

| Config | Write amp | Compaction MB written | Peak tombstone age (s) | Seconds over 30 s | Avg tombstones in non-last levels | Final SST MB | Deadline compactions (files in) |
|---|---|---|---|---|---|---|---|
| base | 7.11 / 7.15 | 549 / 553 | 61 / 62 | 33 / 32 | 8982 / 9001 | 60.5 / 60.5 | - |
| deadline | 7.76 / 7.98 | 608 / 628 | 31 / 31 | 2 / 3 | 6644 / 6796 | 50.7 / 50.7 | 12 (66) / 13 (70) |
| coalesce | 8.14 / 8.28 | 643 / 655 | 30 / 30 | 0 / 0 | 6414 / 6625 | 50.7 / 50.7 | 5 (59) / 8 (100) |

No run stalled writes.

## Reading

- The deadline holds. Peak tombstone age drops from about 2x Dth to Dth. The
  old FADE bug, where deadlines went unenforced on an idle DB, does not appear:
  the idle-phase peak equals the run-phase peak.
- The cost is about 10% more compaction writes. In return, space is 16% lower
  at the end (50.7 vs 60.5 MB) and about 25% fewer tombstones sit in upper
  levels.
- Coalescing does what it is built to do: 2-2.5x fewer deadline compactions,
  each one bigger, and zero seconds over the deadline (deadlines are met early
  instead of 1 s late).
- Coalescing did not reduce total bytes written in this workload. It wrote
  about 5% more than `deadline`. Uniform keys spread tombstones over every
  file, so pulling a neighbour in early mostly moves work forward rather than
  removing a repeated rewrite of a shared next-level file. The expected win
  needs repeated rewrites to dominate: skewed or clustered deletes, a larger
  window relative to Dth, or a tree with more files per level. That is the
  next experiment.
- The `coalesce` r2 run did 8 deadline compactions with 100 input files,
  versus 5 with 59 in r1. Run-to-run variance is large under this load.

## Next steps

- Re-run on a quiet machine with 3+ repeats.
- Sweep the window (0, Dth/10, Dth/3, Dth) and add a skewed or range-clustered
  delete workload.
- Count bytes rewritten from the *next* level per deadline compaction (the
  overlap that coalescing is meant to share) instead of only total bytes.
