# Tombstone deadline + deadline coalescing: benchmarks

LLM-drafted prototype (Claude, 2026-11-23/24). Laptop numbers: good enough to
compare designs, not final report numbers.

## What changed in v2

v1 (commit d117ac7fb) measured tombstone age from a file's
`oldest_ancestor_time`, which is the age of the file's oldest *data*. v2 adds a
per-file `oldest_tombstone_time`, borrowed from the FADE prototype:

- The memtable records when its first tombstone was written.
- Flush stores the oldest such time over the flushed memtables.
- A compaction output takes the oldest tombstone time of the overlapping
  inputs that hold tombstones, or none if no tombstone survived.
- The value is saved in the MANIFEST under custom tag 20, which old readers can
  safely ignore.

Deadlines now start when a tombstone is written. Files written before this
existed fall back to the ancestor time.

## Setup

- Driver: `deadline_bench/deadline_bench.cc`. Matrix:
  `REL=<release build> OUT=deadline_bench/results RUN_S=60 REPEATS=3 DEADLINE=30 PRELOAD=500000 IDLE_S=40 OPS=15000 ./deadline_bench/run.sh`.
- Tree: 1 MB write buffer and target file size, 4 MB level base, T=10, L0
  trigger 4, 4 background jobs, no compression, WAL off, 7 levels with dynamic
  level bytes.
- Workload: preload 500k keys (100 B values), then 60 s at 15k ops/s with
  uniform keys, 90% puts and 10% point deletes, then an idle phase that is
  still sampled.
- Configs, all using this branch's binary:
  - `base`: feature off. All new picking work is gated on the option; the only
    always-on cost is recording one timestamp per memtable and one varint per
    file with tombstones.
  - `deadline`: `tombstone_deadline_seconds=30`.
  - `coalesce`: adds `deadline_coalescing_window_seconds=10`.
- Tombstone age is now the true age (`now - oldest_tombstone_time`), read from
  internal file metadata every second. `peak_proxy_age` is the old
  ancestor-time measure, kept for comparison.
- Load average was 2-48 during the v2 runs, much quieter than v1's 78-324.
  Runs were nearly deterministic: `base` and `coalesce` gave identical numbers
  on all 3 repeats.
- Every run passed the full-scan check against an in-memory model.

## 1. Uniform workload (3 repeats; raw lines in `results/runs_full.log`)

| Config | Write amp | Compaction MB written | Peak true tombstone age (s) | Seconds over 30 s | Avg tombstones in non-last levels | Final SST MB | Deadline compactions (MB) |
|---|---|---|---|---|---|---|---|
| base | 6.95 (all 3) | 535 | 64 | 39 | 8545 | 59.3 | - |
| deadline | 7.95 / 7.99 / 7.78 | 626 / 629 / 610 | 29-30 | 0 | 6520 | 50.7 | 14 / 16 / 11 (79 / 91 / 59) |
| coalesce | 7.87 (all 3) | 619 | 30 | 0 | 6264 | 50.7 | 4 (35) |

- The deadline holds: peak age goes from 64 s to 30 s, including while idle.
  This costs about 14% more writes, and ends with 15% less space and about 25%
  fewer tombstones in upper levels.
- In this workload, true age and proxy age are the same. Deletes start at
  once and nearly every file holds one, so the oldest tombstone is about as old
  as the file. The v1 numbers here were therefore not wrong.
- Coalescing: 3-4x fewer deadline compactions, and about half the bytes in
  them. Total writes are about the same as `deadline` (7.87 vs a mean of
  7.91), because the size-triggered compactions that absorb near-deadline
  neighbours write more (425 vs about 380 MB).

## 2. Aged-data workload: v1 (ancestor clock) vs v2 (tombstone clock)

Same as above, but `AGE_S=60`: after the preload the DB waits 60 s before the
deletes start, so the old data gets its first tombstones late. `IDLE_S=20`,
1 repeat. Files are in `results/aged/`. The v1 rows use v1's own driver, so
their age column is the proxy.

| Version / config | Write amp | Compaction MB | Deadline compactions (MB, input files) | Peak true age (s) | Peak proxy age (s) | Final SST MB |
|---|---|---|---|---|---|---|
| base | 6.95 | 535 | - | 44 | 95 | 59.3 |
| v1 deadline | 8.17 | 646 | 20 (124 MB, 154) | <= 29 | 29 | 50.7 |
| **v2 deadline** | **7.42** | **577** | **7 (26 MB, 30)** | 30 | 90 | 54.9 |
| v1 coalesce | 7.97 | 628 | 9 (81 MB, 102) | <= 30 | 30 | 52.9 |
| v2 coalesce | 7.76 | 608 | 3 (25 MB, 33) | 30 | 81 | 52.8 |

- This is the case the fix targets. v1 sees old data and treats a fresh
  tombstone as already expired. It runs 3x more deadline compactions and
  writes 5x more bytes in them, to meet a guarantee that was already met.
- With the true clock, the deadline costs +6.8% write amp over base instead
  of +17.6%, with the same 30 s bound.
- `base` shows the proxy is misleading as a metric: it reports 95 s while no
  tombstone is older than 44 s.
- v2 keeps less space back than v1 (54.9 vs 50.7 MB). That is expected: v1
  was also cleaning up early.
- Coalescing still adds writes on top of `deadline` with the true clock
  (7.76 vs 7.42). Batching into size-triggered compactions pulls files in
  early, and here that costs more than the deadline compactions it saves.

## Open questions

- Coalescing has not shown a write-amp win yet. Next steps: a window sweep
  (0, 3, 10, 30 s), turning off the size-compaction piggyback separately from
  deadline-only coalescing, and a skewed or clustered delete workload.
- Single repeat for the aged workload. Rerun on the Linux server with 3+
  repeats.
