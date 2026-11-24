# fade_bench

Workload driver for checking baseline RocksDB behaviour and FADE (Lethe).
Public API only; tombstone ages come from a TablePropertiesCollector
(smallest tombstone seqno per SST) plus a seqno -> wall-time map, so they
are independent of FADE's own `oldest_tombstone_time` bookkeeping. Every run
checks the DB against an exact in-memory model (full scan, 200k Gets, 2k
Seeks) before and after reopen.

The benchmark reports space amplification (final SST bytes / logical live
bytes), write amplification, point-Get block accesses and physical reads, and
write CPU time per operation. `b_threshold` sweeps delete-heavy FADE candidates;
`filter_probe=true` enables filter-based output-level selection. Compare probe
on/off with identical seeds and workload settings to estimate its CPU and
compaction-read overhead. The picker scans distinct point keys in FADE inputs
and probes target SST filters only; `fade_output_levels` records the selected
levels. It does not read target data blocks. `tombstone_route=true` enables the
experimental per-point-tombstone routing mode. `fade_avg_level_skips` averages
the deepest actual output-level jump beyond the normal FADE output level for
each successful FADE compaction; it is a per-job depth measure, not a
per-tombstone average.

## FADE implementation

FADE is an opt-in leveled-compaction policy. `delete_persistence_threshold`
(`dth`) sets the maximum tombstone persistence time in seconds; zero disables
the age-based policy. The database divides this threshold cumulatively across
stages from L0 through the level before the last non-empty level. With level
size multiplier `T`, `n` stages, and zero-based stage `k`, the cumulative
deadline is

    D_k = dth * (T^(k+1) - 1) / (T^n - 1)

The final governed stage has deadline `dth`. A file is age-eligible when its
oldest point tombstone is older than its level deadline. The independent
`fade_b_compaction_threshold` can also select a file when its estimated
invalidated-entry count (currently the file's point-deletion count) reaches the
positive threshold. A zero `b_threshold` disables that trigger. Candidates are
ordered by level, oldest tombstone, and then estimated invalidated entries.
When the tree is saturated, ordinary size compactions take priority; FADE can
choose a tombstone-bearing file within the size-selected level.

Two filter modes are available:

* `fade_filter_probe_enabled=true` chooses one output level for the whole
  compaction. It probes distinct point keys from the compaction input and skips
  a level only when all relevant SST filters say the keys are absent.
* `fade_tombstone_level_routing_enabled=true` instead probes point tombstones
  individually. Live values from the source level still go to the normal next
  level; each point tombstone goes to the first deeper level whose overlapping
  SST filter may contain its key. Matching destination files are included in
  the same compaction and their records are rewritten at their original level.
  If all deeper probes are negative, the tombstone remains subject to the
  ordinary snapshot-aware compaction deletion rules.

A filter positive is only 'may match'; it is never evidence that a key exists.
Unavailable or failed probes conservatively retain the default destination.
Per-tombstone routing falls back to ordinary FADE output when range tombstones,
timestamps, merge operators, compaction filters, blob files, proximal output,
or a conflicting/oversized destination input make the multi-level operation
unsupported. The modes are separate; per-tombstone routing takes precedence
when both options are enabled. Routing jobs run locally because remote
compaction results do not represent arbitrary output levels; they do not use
the secondary worker's resumable progress writer.

## Measuring skipped levels

For each successful FADE compaction, the benchmark takes the deepest level
among its actual output files and subtracts the compaction's normal output
level and one. Negative results are clamped to zero. The reported average is
the mean of that job-level depth measure across successful FADE compactions.
For example, normal output at L1 plus output at L3 counts one skipped level
(L2). This captures the maximum level jump in a job, not how many tombstones
took that path. `fade_output_levels` lists actual FADE output SST counts by
level.

To compare the new routing mode, use identical workload parameters and seeds:

    fade_bench ... label=route_off dth=20 b_threshold=50 \
        tombstone_route=false dynamic_level_bytes=false
    fade_bench ... label=route_on dth=20 b_threshold=50 \
        tombstone_route=true dynamic_level_bytes=false

Correctness is checked against an in-memory model before and after reopening
the database. WA, SA, point-read amplification, compaction read/write bytes,
and CPU time can be compared alongside `fade_avg_level_skips`. Configure the
workload and level-size targets so live SSTs occupy levels below the normal
FADE destination; otherwise there are no deeper levels to skip.

## Routing benchmark results

Measured on Linux x86_64 with an optimized static RocksDB build. Workload:
50,000 preloaded keys, 112-byte values, 25% point deletes, 10,000 write
operations/s, 256 KiB write buffers and target files, 256 KiB L1 base, level
multiplier 10, dynamic level bytes disabled, `dth=5`, and `seed=42`. Every run
passed model verification before and after reopen.

The short `b_threshold` sweep used 4 seconds of writes and 2 seconds of reads.
Results are exploratory, single-seed measurements:

| b threshold | Avg. skipped levels/job, routing on | WA off/on | SA off/on |
|---:|---:|---:|---:|
| 0 | n/a (no FADE jobs in the routing-on run) | 5.72 / 6.00 | 1.573 / 1.591 |
| 20 | 0.304 | 16.22 / 17.49 | 1.008 / 1.010 |
| 50 | 0.044 | 16.22 / 16.15 | 1.009 / 1.008 |
| 100 | 0.231 | 16.28 / 16.83 | 1.007 / 1.008 |

For `b_threshold=20`, two longer seeds (12 seconds of writes and 4 seconds of
reads) give these means:

| Metric | Normal FADE | Per-tombstone routing |
|---|---:|---:|
| Average skipped levels/job | 0 | 0.321 |
| FADE compactions | 158.5 | 67 |
| Write amplification | 16.58 | 17.73 |
| Space amplification | 1.004 | 1.005 |
| Compaction bytes written | 187.3 MB | 201.2 MB |
| Compaction bytes read | 200.0 MB | 196.5 MB |
| Write CPU | 36.1 us/op | 39.5 us/op |
| Point-Get block accesses/Get | 0.753 | 0.760 |
| Physical block reads/Get | 0.0028 | 0.0030 |

In this workload the routing mode skipped levels and reduced the number of
FADE jobs, but it increased compaction output bytes and CPU. The short
threshold sweep is noisy and does not show a monotonic relationship between
`b_threshold` and skipped levels. These results are workload-specific and do
not establish a performance improvement; larger and repeated workloads are
needed before drawing that conclusion.

Build (release RocksDB is built with -fno-rtti, so the driver must be too):

    cmake -S .. -B ../build-rel -DCMAKE_BUILD_TYPE=Release -DWITH_GFLAGS=OFF
    cmake --build ../build-rel -j10 --target rocksdb
    clang++ -std=c++20 -O2 -DNDEBUG -fno-rtti -I../include fade_bench.cc \
        ../build-rel/librocksdb.a -lpthread -o fade_bench
    # against a tree without FADE (e.g. main), add -DFADE_BENCH_NO_FADE

Run the matrix: `./run_matrix.sh <fade_bench> <fade_bench_main> <db dir> <out dir>`

`results/` holds the 2026-11-20 run on an M-series MacBook (10 cores, APFS,
not direct I/O): `summary.csv` (one row per run), `*_ts.csv` (tombstone age
per level every 250 ms), `log.txt`, and `unpaced_repeat_summary.csv`.
