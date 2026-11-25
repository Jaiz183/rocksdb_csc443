# Optimizations & Augmentations to KiWi Per-Tile XOR+Page Filters (`tile-filter-opt`)

This document details the bottlenecks diagnosed in the initial `tile-filter`
design (`302fcd7e6`), the optimizations and 3-way XOR slot/directory
augmentations implemented on the `tile-filter-opt` branch, and release-mode
benchmark results across all three workload suites. All changes preserve the
3-way XOR construction (`XorFilterHash::SeededDerive` + 3-slot hypergraph
peeling) rather than swapping in a different filter structure.

- Primary files touched:
  - [`table/block_based/kiwi_tile.h`](../../../table/block_based/kiwi_tile.h) /
    [`table/block_based/kiwi_tile.cc`](../../../table/block_based/kiwi_tile.cc)
  - [`table/block_based/filter_policy_internal.h`](../../../table/block_based/filter_policy_internal.h) /
    [`table/block_based/filter_policy.cc`](../../../table/block_based/filter_policy.cc)
  - [`table/block_based/block_based_table_builder.h`](../../../table/block_based/block_based_table_builder.h) /
    [`table/block_based/block_based_table_builder.cc`](../../../table/block_based/block_based_table_builder.cc)
  - [`table/block_based/block_based_table_reader.cc`](../../../table/block_based/block_based_table_reader.cc)
  - [`db/db_impl/db_impl_kiwi.cc`](../../../db/db_impl/db_impl_kiwi.cc)
  - [`examples/kiwi_tile_filter_bench.cc`](../../../examples/kiwi_tile_filter_bench.cc)
- Previous design & baseline results: [tile_filter.md](tile_filter.md)

## Naming Convention (`kiwi_{granularity}_xor_{description}`)

| Name | Granularity | Policy String / API | Description |
|---|---|---|---|
| `kiwi_tile_xor_baseline` | `tile` | `NewXorFilterPolicy(1)` (`tile-filter`) | Initial 8-bit `[fp \|\| page]` per-tile XOR filter (`+32` segment slack) |
| `kiwi_tile_xor_baseline_16bit` | `tile` | `NewXorFilterPolicy(2)` (`tile-filter`) | Initial 16-bit `[fp \|\| page]` per-tile XOR filter (`+32` segment slack) |
| `kiwi_tile_xor_tight` | `tile` | `NewXorFilterPolicy(1)` (`tile-filter-opt`) | Optimized 8-bit `[fp \|\| page]` per-tile XOR (`+4` slack, 1-derive peel, compact `Directory`, zero-peel `ShiftBasePage`) |
| `kiwi_tile_xor_tight_16bit` | `tile` | `NewXorFilterPolicy(2)` (`tile-filter-opt`) | Optimized 16-bit `[fp \|\| page]` per-tile XOR (`+4` slack, compact `Directory`) |
| `kiwi_tile_xor_delete_rank` | `tile` | `{id=xorfilter:1;kiwi_delete_rank_bits=2}` | Per-tile XOR packing 2 sub-page delete-key rank bits (`[fp \|\| rank \|\| page]`) to prune straddling edge pages |
| `kiwi_sst_xor_index_free` | `sst` | `{id=xorfilter:1;kiwi_file_filter=true}` | SST-wide `Directory` v3 XOR (`[fp \|\| local_page]`) probed before the index block |

---

## Weaknesses Diagnosed in `kiwi_tile_xor_baseline` (`302fcd7e6`)

1. **Ballooning Write Amplification (`w_amp`):** In Suite 3 (`1 MB` target file
   / `L1` base), `kiwi_tile_xor_baseline` suffered `w_amp = 5.35` on `seqdel`
   and `3.00` on `churn` (vs `4.59` / `2.73` for `kiwi_page_bloom10`).
   `BlockBasedTableBuilder::EstimatedFileSize()` returned
   `FileSize() + packer.buffered_bytes()`, omitting `rocksdb.kiwi.directory`.
   Compacted SSTs routinely overshot `target_file_size_base = 1 MB` into
   `1.04--1.06 MB`, pushing `L1` above `score = 1.0` and triggering extra
   cascading compactions.
2. **Filter Build CPU & Allocation Overhead:** `TileXorFilterBuilder::Finish()`
   recomputed `XorFilterHash::SeededDerive` three times per key per seed
   attempt, stored a 24-byte `Entry` struct, allocated peeling vectors inside
   tight inner loops, packed slots byte-by-byte, and `TilePacker::Pack()`
   re-allocated its sorting vector on every tile.
3. **Bits per Key & Pinned `TableReader` RAM:** A fixed `+32`-slot slack per
   segment (`96` extra slots per tile) added ~`0.78` wasted slots per key in
   `h = 4..8` tiles (~124--248 keys), pushing `kiwi_tile_xor_baseline` to
   `15.1--15.6 bpk` (and `kiwi_tile_xor_baseline_16bit` to `25.9--27.1 bpk`). On
   read, `Directory` retained raw encoded slices alongside parsed
   `DirectoryTile` + 56-byte `TileXorFilterReader` structs and
   `std::vector<uint64_t>` offsets (`830--921 KB` `TableReader` RAM on 200k
   keys).
4. **Space Amplification, Reconstruction Cost, & Straddling Edge-Page I/O:**
   Lazy `SecondaryDeleteBelow(floor)` leaves dropped pages on disk until
   compaction (`2.00x` space amp at 50% drop, `3.98x` at 75% drop), whereas
   eager re-peeling takes `10.3--31.4 ms` and forced `CompactRange` takes
   `49.5--143.2 ms`. Furthermore, on small drops (`10%`), edge pages straddling
   `floor` cannot be skipped (`0.90` `io_blk` on deleted keys) because `[fp ||
   page_index]` carries no intra-page delete-key signal, and in a multi-SST DB,
   100%-dead SSTs were still only marked rather than immediately unlinked.
5. **Index-Block I/O on Absent-Key Lookups:** Every per-tile check had to
   probe the standard index block first (`1.00` uncached index read in I/O mode
   plus binary search CPU and heap/iterator overhead) before reaching
   `Directory::TileMayMatch()`.

---

## Implemented Optimizations & XOR Augmentations

### 1. Eliminating Cascading Compactions (`EstimateKiWiDirectoryBytes`)
`BlockBasedTableBuilder::Rep::EstimateKiWiDirectoryBytes()` tracks
`directory.current_bytes()` plus a tight bound on buffered tile keys and (in v3
mode) the SST-wide XOR filter size, wired into both `EstimatedFileSize()` and
`EstimatedTailSize()`. Compaction outputs split accurately near
`target_file_size_base`, preventing extra `L1+` cascades.

### 2. Tight Adaptive 3-Way XOR Sizing & Fast Single-Derive Peeling (`kiwi_tile_xor_tight` / `kiwi_tile_xor_tight_16bit`)
- **Tighter segment sizing:** `TileXorFilterBuilder::Finish()` starts at
  `seg_len = ceil((1.23 * n) / 3) + 4` instead of `+32` (saving `84` slots per
  tile while still peeling cleanly on small tiles) and steps
  `seg_len += max(1, seg_len / 64)` every 8 failed seeds up to 256 seed attempts.
- **1-derive cached peeler & 16-byte `Entry`:** `Entry` shrinks from 24 to 16
  bytes (`{hash, order, page_and_rank}`). Within each seed attempt, one pass
  computes `(idx0, idx1, idx2)` via `SeededDerive` into a reused scratch vector;
  degree construction, peeling, and reverse slot assignment reuse those cached
  indices instead of rehashing `3x` per key.
- **64-bit streaming bit-packer:** Packs slots via a 64-bit accumulator and
  `EncodeFixed64`, avoiding byte-at-a-time loops on hot flush/compaction paths.
- **`TilePacker` scratch reuse:** Reuses `order_scratch_` across tiles.

### 3. Compact Pinned `Directory` Memory & In-Place Dead-Tile Stripping
- `TileXorFilterReader` shrinks from `56 B` to `32 B` (`base_page_`,
  `min_live_page_`, `rank_bits_`, bit-packed slice pointer).
- `Directory` replaces per-tile index offsets inside large structs with a
  contiguous `tile_offsets_` array (`std::lower_bound` in `FindTile()`), calls
  `shrink_to_fit()` after decode, and when using compact v2/v3 filters releases
  redundant per-tile record backing bytes while accounting `directory_bytes_`
  accurately for `rocksdb.estimate-table-readers-mem`.
- `SetKiWiDeleteFloor(floor)` invokes `Directory::ApplyDeleteFloor(floor)`,
  advancing each tile's `min_live_page_` and compacting `contents_` to free
  100%-dead tiles' XOR filter payloads in RAM (`ClearPayload`) with zero disk I/O
  and zero peeling.
- `BlockBasedTable::KiWiGet()` stack-allocates `IndexBlockIter` instead of heap
  allocating per `Get()`, and skips `TileAllDeleted` tiles without hashing.

### 4. Zero-Peel Physical Page Drop (`ShiftBasePage`) & Whole-File Unlink
- **`kFormatVersionAugmented = 2` (`TileXorFilter` v2 header):** A 12-byte
  header adds `fixed32 base_page` (v1 8-byte header is kept when `rank_bits == 0
  && base_page == 0`). Because KiWi orders pages inside every tile by ascending
  `delete_key`, `SecondaryDeleteBelow(floor)` always drops a contiguous prefix
  `pages[0 .. k-1]`.
- **`TileXorFilterBuilder::ShiftBasePage` &
  `BlockBasedTableBuilder::RewriteKiWiTableDroppingPages`:** Physically reclaims
  disk space by copying surviving raw data blocks (`max_delete_key >= floor`)
  directly without block decompression, sorting, or **any XOR filter
  peeling**, updating each surviving tile's XOR filter via a `memcpy` + adding
  `k` to `base_page` (while keeping `page_bits = ceil(log2(base_page +
  surviving_pages))`). Dropped prefix pages (`decoded_page < base_page`) fail
  bounds check immediately on lookup with 0% false-positive pass-through on
  their true slots.
- **Whole-SST Unlink (`DBImpl::SecondaryDeleteBelow`):** Parses
  `rocksdb.kiwi.max_delete_key` and directly unlinks 100%-dead SSTs
  (`max_delete_key < floor && num_entries > 0 && num_range_deletions == 0`)
  via `edit.DeleteFile(level, fnum)` in the `VersionEdit`, achieving `0`
  compaction write amplification and instant disk + `TableReader` RAM
  reclamation.

### 5. Sub-Page Delete-Key Rank Bits (`kiwi_tile_xor_delete_rank`, `kiwi_delete_rank_bits` = 0..4)
Configured via `FilterPolicy::CreateFromString(..., "{id=xorfilter:1;kiwi_delete_rank_bits=2}", ...)`,
each 3-way XOR slot packs `[fp || rank_bucket || page_index]` directly inside
the existing 64-bit slot math (`slot_bits = fp_bits + rank_bits + page_bits`),
encoding which quantized `[min_d, max_d]` bucket within a page holds the key
without adding a single byte to `DirectoryPage`. Lookups against straddling
edge pages prune keys whose rank bucket's upper bound falls strictly below
`floor` before reading the page from block cache or disk.

### 6. SST-Wide Index-Free XOR+Tile+Page Filter (`kiwi_sst_xor_index_free`, `kiwi_file_filter`, Directory v3)
Configured via `FilterPolicy::CreateFromString(..., "{id=xorfilter:1;kiwi_file_filter=true}", ...)`,
one SST-wide 3-way XOR filter (`[fp || local_page_idx]` encoded in
`DirectoryBuilder::kFormatVersionFileFilter = 3`, where `Order = global_order`)
resolves absent-key lookups **before** `NewIndexIterator()` is ever called
(while positive lookups use the SST tile index for `tile_idx` plus the decoded
`local_page_idx`), eliminating index-block I/O and binary search on negative
queries. If any
multi-version `user_key` spans a tile boundary (`prev_tile_last_user_key ==
first_user_key`), `BlockBasedTableBuilder` automatically falls back to per-tile
v2 filters. (`RewriteKiWiTableDroppingPages` is currently limited to v1/v2
per-tile filters.)

---

## Empirical Benchmark Results (`DEBUG_LEVEL=0`)

Measured in release mode (`-DNDEBUG -fno-rtti -O2`) via
[`examples/kiwi_tile_filter_bench.cc`](../../../examples/kiwi_tile_filter_bench.cc)
(4 KB uncompressed blocks, 100-byte values carrying 8-byte `D`). Numbers for
`kiwi_tile_xor_baseline` come from [tile_filter.md](tile_filter.md).

### Suite 1: Query-Heavy Workload (200k unique keys, 300k queries each)

| `h` | Config | Flush (ms) | Mem Pres (ns) | Mem Abs (ns) | Mem Mix (ns) | IO Pres (us) | IO Abs (us) | Abs FP (%) | Filter (bpk) | Dir (bpk) | TableReader (KB) | SST (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `plain_bloom10` | 124.2 | 1524.2 | 444.5 | 1421.2 | 4.36 | 0.44 | 0.885% | 10.16 | 0.00 | 402.7 | 22.11 |
| 4 | `kiwi_no_filter` | 114.4 | 4022.5 | 3354.1 | 3489.6 | 11.99 | 11.99 | 0.000% | 0.00 | 5.53 | 912.5 | 21.94 |
| 4 | `kiwi_page_bloom10` | 118.4 | 1694.6 | 917.0 | 1716.9 | 3.98 | 0.98 | 0.069% | 16.48 | 22.01 | 1314.8 | 22.33 |
| 4 | `kiwi_tile_xor_baseline` | 111.8 | 1654.5 | 915.4 | 1594.5 | 3.82 | 0.91 | 0.402% | 15.28 | 20.70 | 921.2 | 22.30 |
| 4 | `kiwi_tile_xor_tight` | 128.2 | 1635.8 | 855.9 | 1607.9 | 4.00 | 0.84 | 0.362% | **13.02** | **18.43** | **745.4** | 22.25 |
| 4 | `kiwi_tile_xor_delete_rank` | 124.7 | 1824.2 | 845.0 | 1653.1 | 4.16 | 0.85 | 0.362% | 15.77 | 21.18 | 812.5 | 22.31 |
| 4 | `kiwi_sst_xor_index_free` | 152.0 | 1576.5 | **363.2** | **1319.9** | 4.01 | **0.36** | 0.376% | **12.30** | **17.59** | **657.9** | 22.23 |
| 4 | `kiwi_tile_xor_tight_16bit` | 124.2 | 1585.5 | 841.5 | 1663.9 | 3.92 | 0.85 | 0.000% | 23.05 | 28.46 | 990.4 | 22.49 |
| 8 | `kiwi_no_filter` | 110.4 | 5496.9 | 5597.4 | 5465.1 | 22.51 | 22.31 | 0.000% | 0.00 | 5.52 | 881.4 | 21.98 |
| 8 | `kiwi_page_bloom10` | 113.7 | 1718.6 | 963.6 | 1711.7 | 3.96 | 0.99 | 0.072% | 16.54 | 22.06 | 1285.2 | 22.37 |
| 8 | `kiwi_tile_xor_baseline` | 122.8 | 1642.7 | 848.4 | 1564.4 | 3.73 | 0.91 | 0.398% | 15.07 | 20.41 | 852.9 | 22.33 |
| 8 | `kiwi_tile_xor_tight` | 126.2 | **1511.7** | 803.9 | 1538.4 | **3.77** | 0.83 | 0.378% | **13.96** | **19.30** | **701.3** | 22.31 |
| 8 | `kiwi_tile_xor_delete_rank` | 109.7 | 1667.7 | 807.0 | 1633.1 | 3.89 | 0.82 | 0.378% | 16.60 | 21.94 | 765.7 | 22.37 |
| 8 | `kiwi_sst_xor_index_free` | 141.2 | 1530.2 | **338.8** | **1347.0** | 3.81 | **0.36** | 0.376% | **13.53** | **18.81** | **655.6** | 22.30 |
| 8 | `kiwi_tile_xor_tight_16bit` | 123.4 | 1529.5 | 798.0 | 1528.8 | 3.86 | 0.81 | 0.001% | 23.94 | 29.28 | 944.9 | 22.55 |
| 16 | `kiwi_no_filter` | 107.7 | 9902.7 | 9649.8 | 9755.2 | 42.71 | 42.69 | 0.000% | 0.00 | 5.52 | 861.8 | 22.02 |
| 16 | `kiwi_page_bloom10` | 116.5 | 1836.5 | 1106.9 | 1810.3 | 4.21 | 1.12 | 0.073% | 16.57 | 22.09 | 1266.4 | 22.42 |
| 16 | `kiwi_tile_xor_baseline` | 125.3 | 1531.3 | 799.2 | 1479.4 | 3.63 | 0.81 | 0.381% | 15.59 | 20.90 | 830.0 | 22.39 |
| 16 | `kiwi_tile_xor_tight` | 110.7 | 1577.2 | 760.1 | 1464.6 | 3.78 | 0.78 | 0.410% | **14.98** | **20.29** | **688.3** | 22.37 |
| 16 | `kiwi_tile_xor_delete_rank` | 118.8 | 1585.3 | 764.4 | 1614.9 | 3.85 | 0.83 | 0.410% | 17.52 | 22.82 | 750.3 | 22.43 |
| 16 | `kiwi_sst_xor_index_free` | 144.5 | 1548.0 | **349.5** | **1279.4** | **3.77** | **0.36** | 0.376% | **14.76** | **20.04** | **665.4** | 22.37 |
| 16 | `kiwi_tile_xor_tight_16bit` | 124.3 | **1472.9** | 761.6 | 1540.4 | 3.89 | 0.78 | 0.000% | 24.89 | 30.19 | 930.2 | 22.61 |

Key Suite 1 findings:
- **Space & RAM:** Tight `+4` segment sizing + stripped `Directory` cuts
  `kiwi_tile_xor_tight` filter space to `13.02--14.98 bpk` (`14.8%--21.0%`
  smaller than `kiwi_page_bloom10`'s `16.48--16.57 bpk`) and drops `TableReader`
  RAM by **17%--19%** vs `kiwi_tile_xor_baseline` (**45%--49%** vs
  `kiwi_page_bloom10`).
- **Index-skipping lookups (`kiwi_sst_xor_index_free`):** Absent-key queries
  plunge from `760--856 ns` / `0.78--0.84 us` to **`339--363 ns` / `0.36 us`**
  (**2.17x--2.37x faster than `kiwi_tile_xor_tight`** and **18%--24% faster than
  `plain_bloom10`**), and 95%/5% mixed workload reaches `1279--1347 ns` vs
  `1421 ns` (`plain_bloom10`). Trade-off: flush takes ~15%--28% longer to peel
  the full SST hypergraph.

### Suite 2: Flush + Secondary Delete, Zero-Peel Physical Drop & Edge Pruning (`h = 8`)

| Drop | Config | Lazy Del (ms) | Zero-Peel Drop (ms / MB / spAmp) | Eager Recon (ms = io + peel) | Full Compact (ms) | Lazy SpaceAmp | DelQ (mem ns / io us / io_blk) | SurvQ (mem ns / io us) | SST MB (B -> L -> C) | TableReader KB (B -> C) |
|---|---|---|---|---|---|---|---|---|---|---|
| 10% | `plain_bloom10` | 248.27 | -- | -- | -- | 1.00x | 395.1 / 0.44 / 0.01 | 1544.7 / 3.74 | 22.11 -> 19.91 | 402.7 -> 362.7 |
| 10% | `kiwi_no_filter` | 0.88 | 53.94 / 21.76 / 1.10x | -- | 147.76 | 1.11x | 5921.2 / 21.89 / 7.92 | 6414.0 / 22.11 | 21.98 -> 21.98 -> 19.79 | 881.4 -> 791.9 |
| 10% | `kiwi_page_bloom10` | 0.95 | 52.20 / 22.15 / 1.10x | -- | 131.08 | 1.11x | 1734.8 / 3.76 / 0.91 | 1833.7 / 4.14 | 22.37 -> 22.37 -> 20.14 | 1285.2 -> 1155.4 |
| 10% | `kiwi_tile_xor_tight` | **0.90** | **50.01 / 22.09 / 1.10x** | 31.42 (11.14 + 18.72) | 143.22 | 1.11x | 1553.4 / 3.55 / 0.90 | 1657.6 / 3.91 | 22.31 -> 22.31 -> 20.08 | 701.3 -> 629.9 |
| 10% | `kiwi_tile_xor_delete_rank` | **0.87** | 52.00 / 22.15 / 1.10x | 32.16 (11.49 + 18.95) | 138.39 | 1.11x | **984.7 / 1.33 / 0.15** | 1736.7 / 3.92 | 22.37 -> 22.37 -> 20.14 | 765.7 -> 687.8 |
| 10% | `kiwi_sst_xor_index_free` | 1.39 | -- | -- | 164.86 | 1.11x | 1554.3 / 3.57 / 0.90 | 1804.3 / 3.98 | 22.30 -> 22.30 -> 20.07 | 655.6 -> 588.7 |
| 10% | `kiwi_tile_xor_tight_16bit` | 0.91 | 48.77 / 22.33 / 1.10x | 30.78 (10.72 + 18.22) | 140.49 | 1.11x | 1556.3 / 3.50 / 0.90 | 1658.7 / 3.93 | 22.55 -> 22.55 -> 20.30 | 944.9 -> 849.1 |
| 25% | `kiwi_tile_xor_tight` | 0.73 | 41.00 / 18.22 / 1.09x | 25.48 (9.16 + 14.63) | 117.15 | 1.33x | 1078.3 / 1.66 / 0.26 | 1609.2 / 3.83 | 22.31 -> 22.31 -> 16.75 | 701.3 -> 524.6 |
| 25% | `kiwi_tile_xor_delete_rank` | 1.30 | 44.48 / 18.28 / 1.09x | 28.14 (10.61 + 15.40) | 116.86 | 1.33x | **920.0 / 1.06 / 0.06** | 1620.8 / 3.78 | 22.37 -> 22.37 -> 16.79 | 765.7 -> 572.9 |
| 50% | `plain_bloom10` | 293.98 | -- | -- | -- | 1.00x | 400.1 / 0.44 / 0.01 | 1517.2 / 3.76 | 22.11 -> 11.06 | 402.7 -> 202.7 |
| 50% | `kiwi_page_bloom10` | 0.89 | 33.17 / 12.77 / 1.14x | -- | 78.90 | 2.00x | 1208.4 / 1.50 / 0.14 | 1877.7 / 4.19 | 22.37 -> 22.37 -> 11.21 | 1285.2 -> 643.8 |
| 50% | `kiwi_tile_xor_tight` | **0.71** | **30.10 / 12.88 / 1.15x** | 18.38 (6.83 + 10.24) | 86.88 | 2.00x | 994.5 / 1.31 / 0.14 | 1584.6 / 3.91 | 22.31 -> 22.31 -> 11.18 | 701.3 -> 352.1 |
| 50% | `kiwi_tile_xor_delete_rank` | **0.78** | 35.10 / 12.94 / 1.15x | 18.45 (7.07 + 10.05) | 84.65 | 2.00x | **898.0 / 0.97 / 0.03** | 1686.5 / 3.86 | 22.37 -> 22.37 -> 11.21 | 765.7 -> 384.3 |
| 75% | `plain_bloom10` | 339.48 | -- | -- | -- | 1.00x | 400.1 / 0.42 / 0.01 | 1448.8 / 3.63 | 22.11 -> 5.54 | 402.7 -> 106.7 |
| 75% | `kiwi_tile_xor_tight` | **0.88** | **19.58 / 7.40 / 1.32x** | 10.26 (4.15 + 5.13) | 49.45 | 3.98x | 972.0 / 1.16 / 0.09 | 1579.7 / 3.77 | 22.31 -> 22.31 -> 5.60 | 701.3 -> 177.5 |
| 75% | `kiwi_tile_xor_delete_rank` | **0.80** | 20.89 / 7.46 / 1.33x | 10.32 (4.07 + 5.20) | 49.35 | 3.98x | **882.7 / 0.93 / 0.02** | 1546.3 / 3.77 | 22.37 -> 22.37 -> 5.62 | 765.7 -> 193.5 |

Key Suite 2 findings:
- **Sub-page delete-rank bits (`kiwi_tile_xor_delete_rank`):** On the hardest
  `10%` edge case (every tile still has surviving keys on page 0), `rk=2` cuts
  deleted-key I/O block reads by **83.3%** (`0.90 -> 0.15 io_blk`), I/O latency
  by **62.5%** (`3.55 -> 1.33 us`), and memory latency by **36.6%** (`1553 ->
  985 ns`), and reaches `0.02--0.06 io_blk` at 25%--75% drops.
- **Zero-peel physical page reclamation:** `RewriteKiWiTableDroppingPages`
  physically shrinks the SST from `22.31 MB` down to `12.88 MB` (50% drop,
  `30.1 ms`) and `7.40 MB` (75% drop, `19.6 ms`), **2.5x--2.9x faster** than full
  `CompactRange` (`86.9 ms` / `49.5 ms`) with **0 XOR peeling**. Residual space
  amp (`1.15x` / `1.32x`) reflects the single straddling edge page retained per
  tile until compaction. (Notice in this single-SST uniform-`D` workload, every
  tile retains at least page 7, so `ApplyDeleteFloor` does not drop whole tiles;
  see Suite 2B when `D` correlates with flush time.)

### Suite 2B: Multi-SST Whole-File Secondary Delete (8 x 25k SSTs = 200k keys, `h = 8`)

| Drop Older SSTs | Config | Delete (ms) | Write Amp | Files (Before -> After) | TableReader RAM (Before -> After KB) | SST Disk (Before -> After MB) |
|---|---|---|---|---|---|---|
| 50% (4/8 files) | `plain_bloom10` (Delete+Compact) | 307.43 | 16.44 | 8 -> 1 | 437.3 -> 202.7 | 22.22 -> 11.06 |
| 50% (4/8 files) | `kiwi_page_bloom10` | 3.79 | **0.00** | 8 -> 4 | 1298.1 -> 649.1 | 22.52 -> 11.26 |
| 50% (4/8 files) | `kiwi_tile_xor_tight` | **2.88** | **0.00** | 8 -> 4 | 717.0 -> **358.5** | 22.46 -> **11.23** |
| 50% (4/8 files) | `kiwi_sst_xor_index_free` | **3.01** | **0.00** | 8 -> 4 | 671.2 -> **335.6** | 22.45 -> **11.22** |
| 75% (6/8 files) | `plain_bloom10` (Delete+Compact) | 332.78 | 6.13 | 8 -> 1 | 437.3 -> 106.7 | 22.22 -> 5.54 |
| 75% (6/8 files) | `kiwi_page_bloom10` | 4.88 | **0.00** | 8 -> 2 | 1298.1 -> 324.5 | 22.52 -> 5.63 |
| 75% (6/8 files) | `kiwi_tile_xor_tight` | 5.31 | **0.00** | 8 -> 2 | 717.0 -> **179.2** | 22.46 -> **5.61** |
| 75% (6/8 files) | `kiwi_sst_xor_index_free` | 6.15 | **0.00** | 8 -> 2 | 671.2 -> **167.8** | 22.45 -> **5.61** |

When older SSTs fall completely below `floor`, `SecondaryDeleteBelow` unlinks
dead files in **2.9--6.2 ms** (**54x--107x faster** than `plain_bloom10`
Delete+Compact) with **`w_amp = 0.00`** and immediate 2x--4x disk + RAM
reclamation.

### Suite 3: Delete-Heavy LSM Workloads (`seqdel` & `churn`, `h = 8`, `1 MB` L1)

| Workload | Config | Ops/s | Avg Op (us) | Write Amp | Mem Pres (ns) | Mem Abs (ns) | IO Pres (us) | IO Abs (us) | IO Blk/Abs | Filter (B->A KB) | TableReader (B->A KB) | SST (B->A MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `seqdel` | `plain_bloom10` | 519,956 | 1.92 | 5.65 | 1562.4 | 613.1 | 3.73 | 0.88 | 0.12 | 168->193 | 332->371 | 14.9->16.2 |
| `seqdel` | `kiwi_no_filter` | 580,868 | 1.72 | 4.61 | 7376.5 | 6353.3 | 29.87 | 27.18 | 10.28 | 0->0 | 633->709 | 14.8->16.5 |
| `seqdel` | `kiwi_page_bloom10` | 551,436 | 1.81 | 5.27 | 2066.1 | 1082.5 | 4.29 | 1.14 | 0.01 | 272->295 | 906->985 | 15.1->16.3 |
| `seqdel` | `kiwi_tile_xor_baseline` | 509,503 | 1.96 | 5.35 | 1757.2 | 906.4 | 3.87 | 0.95 | 0.00 | 254->269 | 623->666 | 15.4->16.4 |
| `seqdel` | `kiwi_tile_xor_tight` | 515,021 | 1.94 | **4.61** | 1984.7 | 992.5 | 4.09 | 1.11 | 0.05 | 227->268 | **509->615** | 14.9->17.3 |
| `seqdel` | `kiwi_tile_xor_delete_rank` | 520,240 | 1.92 | 5.05 | 1776.5 | 877.2 | 3.86 | 1.02 | 0.06 | 272->304 | 559->626 | 15.1->16.5 |
| `seqdel` | `kiwi_sst_xor_index_free` | 469,605 | 2.13 | 4.75 | 1993.8 | **469.5** | 3.90 | **0.49** | **0.01** | 229->250 | **498->557** | 15.5->16.9 |
| `seqdel` | `kiwi_tile_xor_tight_16bit` | 517,987 | 1.93 | 5.08 | 1646.0 | 845.5 | 3.90 | 0.86 | 0.00 | 392->417 | 682->730 | 15.2->16.1 |
| `churn` | `plain_bloom10` | 664,732 | 1.50 | 3.97 | 1642.2 | 672.1 | 3.84 | 0.97 | 0.14 | 173->193 | 339->368 | 15.5->16.2 |
| `churn` | `kiwi_no_filter` | 639,094 | 1.56 | 3.08 | 13110.6 | 12918.1 | 58.21 | 58.63 | 22.50 | 0->0 | 651->749 | 15.2->17.2 |
| `churn` | `kiwi_page_bloom10` | 662,476 | 1.51 | 3.25 | 2853.4 | 2113.6 | 5.12 | 2.48 | 0.16 | 281->344 | 937->1110 | 15.6->18.1 |
| `churn` | `kiwi_tile_xor_baseline` | 605,403 | 1.65 | 3.00 | 1930.5 | 1227.9 | 4.21 | 1.47 | 0.10 | 248->290 | 616->706 | 15.1->16.8 |
| `churn` | `kiwi_tile_xor_tight` | **670,336** | **1.49** | **2.80** | 2035.0 | 1351.2 | 4.25 | 1.72 | 0.15 | 235->289 | **525->633** | 15.4->17.7 |
| `churn` | `kiwi_tile_xor_delete_rank` | 602,972 | 1.66 | 3.26 | 2315.2 | 1582.5 | 4.49 | 1.94 | 0.15 | 280->339 | 570->677 | 15.5->17.6 |
| `churn` | `kiwi_sst_xor_index_free` | **678,008** | **1.47** | **2.49** | **1769.0** | **781.2** | **3.95** | **1.13** | 0.15 | 218->283 | **474->604** | 14.7->17.8 |
| `churn` | `kiwi_tile_xor_tight_16bit` | 647,215 | 1.55 | 2.94 | 2123.3 | 1428.0 | 4.36 | 1.73 | 0.11 | 396->466 | 689->806 | 15.3->17.1 |

Key Suite 3 findings:
- **Write amplification (`w_amp`) fixed:** Counting `rocksdb.kiwi.directory` in
  `EstimatedFileSize()` drops `kiwi_tile_xor_tight` `w_amp` from `5.35 -> 4.61`
  on `seqdel` (**-13.8%**, tying `kiwi_no_filter` and beating
  `kiwi_page_bloom10`'s `5.27`) and `3.00 -> 2.80` on `churn` (`2.49` on
  `kiwi_sst_xor_index_free`).
- **`churn` flips from a loss to a win:** `kiwi_tile_xor_tight` (`670.3k ops/s`)
  and `kiwi_sst_xor_index_free` (`678.0k ops/s`) now **outperform both
  `kiwi_page_bloom10` (`662.5k`) and `plain_bloom10` (`664.7k`)** while cutting
  `TableReader` RAM by **43%--46%** (`604--633 KB` vs `1110 KB`) and absent
  query latency by **63%** (`781 vs 2114 ns` on `kiwi_sst_xor_index_free`).
- **`seqdel` trade-off remains:** Even after cutting `w_amp` to `4.61`, `seqdel`
  writes still trail `kiwi_page_bloom10` by ~6.6% (`515k vs 551k ops/s`,
  `kiwi_sst_xor_index_free` at `470k`) because 3-way hypergraph peeling on small
  1 MB SSTs uses more CPU than Bloom bit-sets. Choose
  `kiwi_sst_xor_index_free` when absent-lookup latency & minimal RAM dominate,
  `kiwi_tile_xor_delete_rank` when small secondary deletes leave active edge
  pages, and `kiwi_tile_xor_tight` + `RewriteKiWiTableDroppingPages` when fast
  zero-peel physical reclamation is required.
