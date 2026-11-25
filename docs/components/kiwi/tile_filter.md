# KiWi Tile- and SST-Granular XOR Filters

This note documents the KiWi `Directory` filter formats in
`rocksdb.kiwi.directory`:
- **Directory v1**: Per-page Bloom/Ribbon filters (`BloomFilterPolicy` /
  `RibbonFilterPolicy`).
- **Directory v2 (`kiwi_tile_xor_baseline`)**: Tile-granular 3-way XOR+page
  filters (`NewXorFilterPolicy(fingerprint_bytes)` or `"xorfilter:<bytes>"`).
- **Directory v3 (`kiwi_sst_xor_baseline`)**: SST-granular 3-way XOR+page
  filters (`{id=xorfilter:<bytes>;kiwi_file_filter=true}`), with automatic
  fallback to Directory v2 if a multi-version user key crosses a tile boundary.

Implementation paths:
- [`XorFilterHash` / `XorFilterPolicy`](../../../table/block_based/filter_policy_internal.h)
- [`TileXorFilterBuilder`, `TileXorFilterReader`, `DirectoryBuilder`, `Directory`](../../../table/block_based/kiwi_tile.h)
- [`BlockBasedTableBuilder::FlushTile()` & `WriteKiWiDirectoryBlock()`](../../../table/block_based/block_based_table_builder.cc)
- [`BlockBasedTable::KiWiGet()`](../../../table/block_based/block_based_table_reader.cc)

## 1. `TileXorFilter` Slot Encoding and Construction

Both Directory v2 (per-tile) and Directory v3 (per-SST) use
`TileXorFilterBuilder` and `TileXorFilterReader`.

### Slot Layout (`[fingerprint || page_index]`)

Given a page bound `num_pages` (`1 <= num_pages <= kiwi_tile_pages`), let:
- `page_bits = ceil(log2(num_pages))` (`0` when `num_pages == 1`)
- `fp_bits = min(8 * fingerprint_bytes, 64 - page_bits)`
- `slot_bits = fp_bits + page_bits` (`1 <= slot_bits <= 64`)

Each key hash is mapped via `XorFilterHash::SeededDerive(hash, seg_len, seed)` to
three distinct segment indices `idx[0], idx[1], idx[2]` across `3 * seg_len`
slots and an `fp_bits`-bit fingerprint `fp`. During hypergraph peeling and
reverse assignment, the target value solved for `(user_key, page_index)` is:

```
target = page_bits == 0 ? fp : ((fp << page_bits) | page_index)
```

### Tight Sizing, Peeling, and Bit-Packing

- **Tight segment sizing**: `TileXorFilterBuilder::Finish()` starts at
  `base_unrounded = floor(1.23 * num_keys) + 4` (`seg_len = max(2,
  ceil(base_unrounded / 3))`). If 8 consecutive seeds fail to peel, `seg_len`
  steps upward by `max(1, seg_len / 64)` until peeling succeeds (up to 256
  attempts).
- **Single-derive degree/XOR peeler**: Per seed attempt, slot indices and
  fingerprints are derived once into `derived_` while maintaining flat
  `degree_[]` and `xor_edge_[]` arrays, avoiding pointer-based sets or repeated
  hash derivations during peeling and reverse assignment.
- **Serialized format**: An 8-byte header
  `[version=1, seed, fp_bits, page_bits, fixed32(seg_len)]` followed by
  `ceil(3 * seg_len * slot_bits / 8)` bytes packed via a 64-bit little-endian
  streaming accumulator (`TileXorFilterReader::ReadSlot` decodes each slot with
  an unaligned 64-bit load and shift on the fast path).

### Unique-Sort-Key Assumption

Because a 3-way XOR filter maps each distinct key hash to a single fixed-width
value, `TileXorFilter` assumes a `user_key` does not need to resolve versions
across multiple pages of the same tile. As a safety guard against hash or
same-tile version collisions, `TileXorFilterBuilder::Finish()` sorts entries by
`(hash, order)` and deduplicates equal hashes keeping the earliest insertion
order (the newest internal key), so hypergraph peeling never fails on duplicate
hashes. Workloads that require merging multiple versions of the same user key
across pages within a single tile should use Directory v1 per-page Bloom/Ribbon
filters.

## 2. Directory Formats (`v1`, `v2`, `v3`)

`DirectoryBuilder` encodes `rocksdb.kiwi.directory` in one of three versions:

```
v1: 1(1B) varint32(num_tiles)
      [varint64(num_pages) (BlockHandle fixed64(min_d) fixed64(max_d) lp-bloom)+]*

v2: 2(1B) varint32(num_tiles)
      [varint64(num_pages) lp-tile_xor_filter (BlockHandle fixed64(min_d) fixed64(max_d))+]*

v3: 3(1B) varint32(num_tiles) lp-file_xor_filter
      [varint64(num_pages) (BlockHandle fixed64(min_d) fixed64(max_d))+]*
```

On table open, `Directory::Decode()`:
1. Validates all tile counts, page counts, monotonic tile start offsets,
   non-inverted `[min_delete_key, max_delete_key]` ranges, and `TileXorFilter`
   headers/payload lengths.
2. Stores contiguous `tile_offsets_` (`uint64_t` per tile) for cache-friendly
   binary search in `Directory::FindTile()` and `tile_max_delete_key_` for
   O(1) `TileAllDeleted(tile_index, floor)` checks.
3. In v2 and v3 modes, discards the already-decoded page records and filter
   headers from `contents_`, shrinking `contents_` to hold only the raw packed
   XOR slot payloads referenced by `TileXorFilterReader`.

`BlockBasedTableBuilder::EstimatedFileSize()` and `EstimatedTailSize()` include
`EstimateKiWiDirectoryBytes()` (accounting for buffered `TilePacker` entries and
the growing KiWi directory / pending v3 file filter) so compaction file cuts
respect `target_file_size_base`.

## 3. Directory v2: Tile-Granular XOR (`kiwi_tile_xor_baseline`)

Configured via `NewXorFilterPolicy(fingerprint_bytes)` or `"xorfilter:<bytes>"`
with `kiwi_tile_pages >= 2`:
- `BlockBasedTableBuilder::FlushTile()` builds one `TileXorFilter` per tile over
  that tile's `pages.size()` pages.
- On `BlockBasedTable::KiWiGet()`, the SST index locates the candidate tile via
  a stack-allocated `IndexBlockIter`. `Directory::TileMayMatch()` performs one
  hash and three slot reads:
  - **Negative** (fingerprint mismatch, `page_index >= num_pages`, or
    `page_index < min_live_page_`): increments `BLOOM_FILTER_USEFUL` and opens 0
    data blocks.
  - **Positive**: increments `BLOOM_FILTER_FULL_POSITIVE` and opens only
    `tile.pages[page_index]` via `KiWiTableIterator::NewPageIterator()`.

### Secondary Deletes Without Filter Reconstruction

Mutating peeled XOR filter slots in-place when pages are dropped is
algebraically unsafe because surviving keys whose pivot slots were assigned
later in reverse peeling order depend on the slot values of earlier keys.
Instead, KiWi handles secondary deletes without touching or re-peeling any XOR
filter:
- `DB::SecondaryDeleteBelow(floor)` unlinks whole SSTs whose
  `max_delete_key < floor` directly in the `VersionEdit` and records
  `kiwi_delete_floor = floor` on partially expired SSTs.
- When opening or updating a floored reader, `Directory::ApplyDeleteFloor(floor)`
  computes each tile's dead prefix `min_live_page_` (`pages[p].max_delete_key <
  floor`) so `TileXorFilterReader::MayMatchPage()` rejects dropped prefix pages
  immediately, and compacts `contents_` to free the XOR filter payloads of any
  100%-deleted tiles from memory.
- Physical reclamation of partially deleted tiles happens during normal
  compaction.

## 4. Directory v3: SST-Granular XOR (`kiwi_sst_xor_baseline`)

Configured via `{id=xorfilter:<bytes>;kiwi_file_filter=true}` with
`kiwi_tile_pages >= 2`:
- Because tiles partition the key space into disjoint sort-key intervals, the
  SST index already identifies which tile holds a present key; the filter only
  needs to reject absent keys and identify the **tile-local** `page_index` in
  `[0, max_tile_pages)` (`ceil(log2(max_tile_pages))` bits per slot, independent
  of the number of tiles in the SST).
- On `BlockBasedTable::KiWiGet()`, `Directory::FileMayMatch(user_key,
  &local_page_index)` runs **before** `NewIndexIterator()`:
  - **Negative**: increments `BLOOM_FILTER_USEFUL` and returns immediately with
    0 index-block and 0 data-block lookups.
  - **Positive**: increments `BLOOM_FILTER_FULL_POSITIVE`, seeks the SST index
    to locate the tile, and opens only `tile.pages[local_page_index]` (skipping
    it without I/O if `max_delete_key < floor`).
- **Automatic cross-tile fallback**: If a multi-version user key spans a tile
  boundary (`prev_tile_last_user_key == first_ukey` in `FlushTile()`), a single
  SST-wide XOR entry cannot represent two different tile-local page indices for
  the same user key. `BlockBasedTableBuilder::WriteKiWiDirectoryBlock()` detects
  this during build and automatically emits Directory v2 per-tile XOR filters
  for that SST so multi-tile version resolution remains exact.

## 5. Mixed Secondary-Delete + Point-Query Benchmark Results

Release-mode (`DEBUG_LEVEL=0`, `-O2 -DNDEBUG -fno-rtti`) raw measurements from
[`examples/kiwi_tile_filter_bench.cc`](../../../examples/kiwi_tile_filter_bench.cc)
(percentage improvements summarized in
[`benchmark_summary.md`](benchmark_summary.md)):

- **Dataset / LSM**: `N = 120,000` shuffled keys (`15 B` keys, `100 B` values
  with 8-byte big-endian `D in [1 .. 120,000]` at offset `0`), `4 KB`
  uncompressed blocks, `write_buffer_size = 2 MB`,
  `target_file_size_base = 2 MB`, `max_bytes_for_level_base = 4 MB`,
  `max_bytes_for_level_multiplier = 4.0` (~6--8 multi-level SSTs, `~13.4 MB`
  after `Flush` + `WaitForCompact`).
- **Mixed workload (`100,000` key-equivalent ops per split)**: 5 splits
  (`0-100`, `25-75`, `50-50`, `75-25`, `100-0`) interleaving `del_pct`
  secondary range deletes (`1,000` keys per delete, advancing `floor` up to
  `1 + 1000 * del_pct`, leaving `>= 20,000` live keys at `100-0`) with
  `1000 * query_pct` point `Get` queries (50% existing keys across `[0 .. N-1]`
  including deleted and surviving keys, 50% absent keys, all verified against
  `D >= current_floor`).
- **Candidates**:
  - `plain_bloom10`: non-KiWi (`h = 0`) + `NewBloomFilterPolicy(10)` (executes
    secondary deletes via iterator scan + `WriteBatch` `Delete` + `Flush`;
    independent of `h`, run once per split and repeated for reference).
  - `kiwi_page_bloom10`: KiWi (`kiwi_tile_pages = h`) +
    `NewBloomFilterPolicy(10)` (Directory v1).
  - `kiwi_tile_xor_baseline`: KiWi (`kiwi_tile_pages = h`) +
    `NewXorFilterPolicy(1)` (8-bit tile-granular XOR, Directory v2).
  - `kiwi_sst_xor_baseline`: KiWi (`kiwi_tile_pages = h`) +
    `{id=xorfilter:1;kiwi_file_filter=true}` (8-bit SST-granular XOR, Directory
    v3).
- **Columns**:
  - `Wall (ms)`, `Total (kops/s)` (`(deleted_keys + num_queries) / wall_s`),
    `Raw (kops/s)` (`(num_range_dels + num_queries) / wall_s`),
    `Del (k keys/s)`, `Range Del (/s, ms)`, and `IO Get (kqps, us, blk)` come
    from the uncached I/O-inclusive pass (`no_block_cache = true`).
  - `Mem Get (kqps, ns)` comes from the warmed `512 MB LRUCache` pass
    (`ReadTier::kBlockCacheTier`, asserting `PerfContext::block_read_count == 0`).
  - `w_amp (wl_MB)` reports total `(FLUSH_WRITE_BYTES + COMPACT_WRITE_BYTES) /
    (preload_user_bytes + workload_tombstone_user_bytes)` and workload-only
    write MB after `WaitForCompact`.
  - `sp_amp` reports `live_sst_bytes_after_workload /
    live_sst_bytes_after_forced_bottommost_CompactRange`.
  - `flt_bpk / dir_bpk`, `tr_KB (B->A)` (pinned `TableReader` RAM before ->
    after workload), and `sst_MB (B->A / C)` (live SST MB before -> after
    workload, plus compacted MB `C`) capture memory and disk footprint.

### Tile Size `h = 4`

| `h` | Split | Candidate | Wall (ms) | Total (kops/s) | Raw (kops/s) | Del (k keys/s) | Range Del (/s, ms) | IO Get (kqps, us, blk) | Mem Get (kqps, ns) | `w_amp` (`wl_MB`) | `sp_amp` | `flt_bpk` / `dir_bpk` | `tr_KB` (`B->A`) | `sst_MB` (`B->A` / `C`) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `0-100` | `plain_bloom10` | 212.8 | 469.8 | 469.8 | N/A | N/A | 469.8 (2.13, 0.51) | 730.2 (1369.5) | 3.10 (0.00) | 1.00x | 10.08 / 0.00 | 266.6->266.6 | 13.28->13.28 (13.27) |
| 4 | `0-100` | `kiwi_page_bloom10` | 258.8 | 386.4 | 386.4 | N/A | N/A | 386.4 (2.59, 0.50) | 686.5 (1456.6) | 2.99 (0.00) | 1.00x | 16.49 / 21.84 | 802.9->802.9 | 13.42->13.42 (13.41) |
| 4 | `0-100` | `kiwi_tile_xor_baseline` | 244.2 | 409.5 | 409.5 | N/A | N/A | 409.5 (2.44, 0.50) | 756.8 (1321.3) | 3.13 (0.00) | 1.00x | 13.02 / 18.22 | 439.2->439.2 | 13.36->13.36 (13.35) |
| 4 | `0-100` | `kiwi_sst_xor_baseline` | 213.7 | 468.0 | 468.0 | N/A | N/A | 468.0 (2.14, 0.50) | 951.1 (1051.4) | 2.98 (0.00) | 1.00x | 12.31 / 17.41 | 411.2->411.2 | 13.35->13.35 (13.34) |
| 0 | `25-75` | `plain_bloom10` | 892.4 | 112.1 | 84.1 | 37.7 | 37.7 (26.539) | 327.6 (3.05, 0.53) | 800.2 (1249.7) | 4.13 (29.13) | 1.18x | 10.43 / 0.00 | 278.1->279.7 | 13.31->12.44 (10.51) |
| 4 | `25-75` | `kiwi_page_bloom10` | 216.4 | 462.1 | 346.7 | 1360.9 | 1360.9 (0.735) | 378.7 (2.64, 0.49) | 685.0 (1459.9) | 2.99 (0.00) | 1.26x | 16.49 / 21.84 | 802.9->802.9 | 13.42->13.42 (10.62) |
| 4 | `25-75` | `kiwi_tile_xor_baseline` | 209.6 | 477.1 | 357.9 | 1323.0 | 1323.0 (0.756) | 393.3 (2.54, 0.50) | 740.6 (1350.2) | 3.13 (0.00) | 1.26x | 13.02 / 18.22 | 439.0->439.0 | 13.36->13.36 (10.58) |
| 4 | `25-75` | `kiwi_sst_xor_baseline` | 200.2 | 499.4 | 374.7 | 1267.8 | 1267.8 (0.789) | 415.5 (2.41, 0.50) | 1000.8 (999.2) | 3.12 (0.00) | 1.26x | 12.31 / 17.39 | 414.4->414.4 | 13.35->13.35 (10.57) |
| 0 | `50-50` | `plain_bloom10` | 1506.6 | 66.4 | 33.2 | 38.0 | 38.0 (26.347) | 264.3 (3.78, 0.48) | 804.8 (1242.5) | 4.85 (26.55) | 1.29x | 10.08 / 0.00 | 266.6->229.5 | 13.28->10.00 (7.75) |
| 4 | `50-50` | `kiwi_page_bloom10` | 170.0 | 588.4 | 294.5 | 1302.1 | 1302.1 (0.768) | 380.1 (2.63, 0.44) | 711.2 (1406.0) | 3.14 (0.00) | 1.71x | 16.50 / 21.82 | 806.1->806.1 | 13.42->13.42 (7.83) |
| 4 | `50-50` | `kiwi_tile_xor_baseline` | 157.5 | 634.8 | 317.7 | 1566.3 | 1566.3 (0.638) | 398.1 (2.51, 0.45) | 757.6 (1319.9) | 3.13 (0.00) | 1.71x | 13.02 / 18.22 | 439.1->439.1 | 13.36->13.36 (7.80) |
| 4 | `50-50` | `kiwi_sst_xor_baseline` | 134.8 | 741.7 | 371.2 | 1606.7 | 1606.7 (0.622) | 482.2 (2.07, 0.44) | 1014.9 (985.3) | 2.98 (0.00) | 1.46x | 12.31 / 17.41 | 411.2->349.4 | 13.35->11.35 (7.79) |
| 0 | `75-25` | `plain_bloom10` | 1775.8 | 56.3 | 14.1 | 44.5 | 44.5 (22.454) | 272.6 (3.67, 0.43) | 813.7 (1229.0) | 5.32 (49.01) | 1.45x | 9.98 / 0.00 | 271.6->173.9 | 13.31->7.24 (4.98) |
| 4 | `75-25` | `kiwi_page_bloom10` | 131.7 | 759.6 | 190.5 | 1257.9 | 1257.9 (0.795) | 347.2 (2.88, 0.39) | 695.9 (1437.1) | 2.99 (0.00) | 2.26x | 16.49 / 21.83 | 802.7->682.6 | 13.41->11.41 (5.04) |
| 4 | `75-25` | `kiwi_tile_xor_baseline` | 117.6 | 850.6 | 213.3 | 1421.8 | 1421.8 (0.703) | 385.8 (2.59, 0.39) | 762.2 (1312.0) | 3.13 (0.00) | 2.66x | 13.02 / 18.22 | 439.1->439.1 | 13.36->13.36 (5.02) |
| 4 | `75-25` | `kiwi_sst_xor_baseline` | 112.7 | 887.1 | 222.4 | 1372.5 | 1372.5 (0.729) | 430.5 (2.32, 0.39) | 1019.2 (981.2) | 3.12 (0.00) | 2.66x | 12.31 / 17.39 | 414.5->414.5 | 13.35->13.35 (5.02) |
| 0 | `100-0` | `plain_bloom10` | 2088.8 | 47.9 | 0.05 | 47.9 | 47.9 (20.888) | N/A | N/A | 5.60 (54.98) | 2.07x | 10.22 / 0.00 | 275.1->128.7 | 13.31->4.61 (2.22) |
| 4 | `100-0` | `kiwi_page_bloom10` | 63.9 | 1565.2 | 1.57 | 1565.3 | 1565.3 (0.639) | N/A | N/A | 3.14 (0.00) | 4.19x | 16.50 / 21.82 | 806.1->565.2 | 13.42->9.41 (2.25) |
| 4 | `100-0` | `kiwi_tile_xor_baseline` | 59.1 | 1692.3 | 1.69 | 1692.6 | 1692.6 (0.591) | N/A | N/A | 3.13 (0.00) | 4.18x | 13.02 / 18.22 | 439.1->307.5 | 13.36->9.36 (2.24) |
| 4 | `100-0` | `kiwi_sst_xor_baseline` | 57.0 | 1753.8 | 1.75 | 1754.1 | 1754.1 (0.570) | N/A | N/A | 3.12 (0.00) | 4.18x | 12.31 / 17.39 | 414.4->290.5 | 13.35->9.36 (2.24) |

### Tile Size `h = 8`

| `h` | Split | Candidate | Wall (ms) | Total (kops/s) | Raw (kops/s) | Del (k keys/s) | Range Del (/s, ms) | IO Get (kqps, us, blk) | Mem Get (kqps, ns) | `w_amp` (`wl_MB`) | `sp_amp` | `flt_bpk` / `dir_bpk` | `tr_KB` (`B->A`) | `sst_MB` (`B->A` / `C`) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `0-100` | `plain_bloom10` | 212.8 | 469.8 | 469.8 | N/A | N/A | 469.8 (2.13, 0.51) | 730.2 (1369.5) | 3.10 (0.00) | 1.00x | 10.08 / 0.00 | 266.6->266.6 | 13.28->13.28 (13.27) |
| 8 | `0-100` | `kiwi_page_bloom10` | 264.1 | 378.7 | 378.7 | N/A | N/A | 378.7 (2.64, 0.51) | 661.7 (1511.2) | 3.15 (0.00) | 1.00x | 16.56 / 21.86 | 787.7->787.7 | 13.44->13.44 (13.43) |
| 8 | `0-100` | `kiwi_tile_xor_baseline` | 241.0 | 414.9 | 414.9 | N/A | N/A | 414.9 (2.41, 0.50) | 754.0 (1326.2) | 3.14 (0.00) | 1.00x | 13.96 / 19.09 | 423.3->423.3 | 13.40->13.40 (13.39) |
| 8 | `0-100` | `kiwi_sst_xor_baseline` | 206.8 | 483.6 | 483.6 | N/A | N/A | 483.6 (2.07, 0.50) | 1013.8 (986.4) | 3.14 (0.00) | 1.00x | 13.54 / 18.61 | 412.8->412.8 | 13.40->13.40 (13.38) |
| 0 | `25-75` | `plain_bloom10` | 892.4 | 112.1 | 84.1 | 37.7 | 37.7 (26.539) | 327.6 (3.05, 0.53) | 800.2 (1249.7) | 4.13 (29.13) | 1.18x | 10.43 / 0.00 | 278.1->279.7 | 13.31->12.44 (10.51) |
| 8 | `25-75` | `kiwi_page_bloom10` | 213.3 | 468.7 | 351.7 | 1344.9 | 1344.9 (0.744) | 385.1 (2.60, 0.47) | 666.0 (1501.6) | 3.00 (0.00) | 1.26x | 16.55 / 21.88 | 785.7->785.7 | 13.44->13.44 (10.64) |
| 8 | `25-75` | `kiwi_tile_xor_baseline` | 201.3 | 496.8 | 372.7 | 1333.8 | 1333.8 (0.750) | 410.8 (2.43, 0.47) | 794.1 (1259.2) | 2.99 (0.00) | 1.26x | 13.96 / 19.12 | 421.0->421.0 | 13.40->13.40 (10.61) |
| 8 | `25-75` | `kiwi_sst_xor_baseline` | 181.6 | 550.7 | 413.2 | 1335.7 | 1335.7 (0.749) | 460.5 (2.17, 0.47) | 1013.7 (986.4) | 3.14 (0.00) | 1.26x | 13.54 / 18.61 | 412.7->412.7 | 13.40->13.40 (10.60) |
| 0 | `50-50` | `plain_bloom10` | 1506.6 | 66.4 | 33.2 | 38.0 | 38.0 (26.347) | 264.3 (3.78, 0.48) | 804.8 (1242.5) | 4.85 (26.55) | 1.29x | 10.08 / 0.00 | 266.6->229.5 | 13.28->10.00 (7.75) |
| 8 | `50-50` | `kiwi_page_bloom10` | 169.0 | 591.8 | 296.2 | 1413.6 | 1413.6 (0.707) | 374.3 (2.67, 0.42) | 644.5 (1551.7) | 3.00 (0.00) | 1.46x | 16.55 / 21.88 | 785.7->669.1 | 13.44->11.44 (7.85) |
| 8 | `50-50` | `kiwi_tile_xor_baseline` | 162.5 | 615.5 | 308.0 | 1367.8 | 1367.8 (0.731) | 397.1 (2.52, 0.42) | 814.1 (1228.4) | 3.14 (0.00) | 1.71x | 13.96 / 19.09 | 423.4->423.4 | 13.40->13.40 (7.82) |
| 8 | `50-50` | `kiwi_sst_xor_baseline` | 140.2 | 713.0 | 356.9 | 1377.4 | 1377.3 (0.726) | 481.1 (2.08, 0.42) | 1047.5 (954.7) | 2.99 (0.00) | 1.46x | 13.54 / 18.63 | 410.5->349.6 | 13.40->11.40 (7.82) |
| 0 | `75-25` | `plain_bloom10` | 1775.8 | 56.3 | 14.1 | 44.5 | 44.5 (22.454) | 272.6 (3.67, 0.43) | 813.7 (1229.0) | 5.32 (49.01) | 1.45x | 9.98 / 0.00 | 271.6->173.9 | 13.31->7.24 (4.98) |
| 8 | `75-25` | `kiwi_page_bloom10` | 131.0 | 763.1 | 191.3 | 1292.3 | 1292.3 (0.774) | 342.5 (2.92, 0.37) | 726.0 (1377.4) | 3.15 (0.00) | 2.66x | 16.56 / 21.86 | 787.7->787.7 | 13.44->13.44 (5.05) |
| 8 | `75-25` | `kiwi_tile_xor_baseline` | 109.8 | 910.9 | 228.4 | 1574.5 | 1574.5 (0.635) | 402.4 (2.49, 0.37) | 802.7 (1245.9) | 2.99 (0.00) | 2.26x | 13.96 / 19.12 | 420.7->358.0 | 13.40->11.40 (5.04) |
| 8 | `75-25` | `kiwi_sst_xor_baseline` | 117.4 | 851.9 | 213.6 | 1245.0 | 1245.0 (0.803) | 437.7 (2.28, 0.37) | 1059.9 (943.5) | 3.14 (0.00) | 2.66x | 13.54 / 18.61 | 412.7->412.7 | 13.40->13.40 (5.03) |
| 0 | `100-0` | `plain_bloom10` | 2088.8 | 47.9 | 0.05 | 47.9 | 47.9 (20.888) | N/A | N/A | 5.60 (54.98) | 2.07x | 10.22 / 0.00 | 275.1->128.7 | 13.31->4.61 (2.22) |
| 8 | `100-0` | `kiwi_page_bloom10` | 71.0 | 1407.5 | 1.41 | 1407.7 | 1407.7 (0.710) | N/A | N/A | 3.00 (0.00) | 5.08x | 16.55 / 21.88 | 785.7->669.1 | 13.44->11.44 (2.25) |
| 8 | `100-0` | `kiwi_tile_xor_baseline` | 69.6 | 1437.4 | 1.44 | 1437.5 | 1437.5 (0.696) | N/A | N/A | 3.14 (0.00) | 4.18x | 13.96 / 19.09 | 423.4->297.0 | 13.40->9.40 (2.25) |
| 8 | `100-0` | `kiwi_sst_xor_baseline` | 71.8 | 1393.0 | 1.39 | 1393.1 | 1393.2 (0.718) | N/A | N/A | 2.99 (0.00) | 5.08x | 13.54 / 18.63 | 410.5->349.6 | 13.40->11.40 (2.24) |

### Tile Size `h = 16`

| `h` | Split | Candidate | Wall (ms) | Total (kops/s) | Raw (kops/s) | Del (k keys/s) | Range Del (/s, ms) | IO Get (kqps, us, blk) | Mem Get (kqps, ns) | `w_amp` (`wl_MB`) | `sp_amp` | `flt_bpk` / `dir_bpk` | `tr_KB` (`B->A`) | `sst_MB` (`B->A` / `C`) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `0-100` | `plain_bloom10` | 212.8 | 469.8 | 469.8 | N/A | N/A | 469.8 (2.13, 0.51) | 730.2 (1369.5) | 3.10 (0.00) | 1.00x | 10.08 / 0.00 | 266.6->266.6 | 13.28->13.28 (13.27) |
| 16 | `0-100` | `kiwi_page_bloom10` | 282.4 | 354.1 | 354.1 | N/A | N/A | 354.1 (2.82, 0.52) | 579.1 (1726.7) | 3.15 (0.00) | 1.00x | 16.59 / 21.89 | 777.1->777.1 | 13.47->13.47 (13.46) |
| 16 | `0-100` | `kiwi_tile_xor_baseline` | 234.3 | 426.7 | 426.7 | N/A | N/A | 426.7 (2.34, 0.50) | 813.0 (1230.0) | 3.00 (0.00) | 1.00x | 14.98 / 20.10 | 423.2->423.2 | 13.44->13.44 (13.43) |
| 16 | `0-100` | `kiwi_sst_xor_baseline` | 210.2 | 475.8 | 475.8 | N/A | N/A | 475.8 (2.10, 0.50) | 1021.3 (979.2) | 3.15 (0.00) | 1.00x | 14.77 / 19.83 | 419.2->419.2 | 13.44->13.44 (13.43) |
| 0 | `25-75` | `plain_bloom10` | 892.4 | 112.1 | 84.1 | 37.7 | 37.7 (26.539) | 327.6 (3.05, 0.53) | 800.2 (1249.7) | 4.13 (29.13) | 1.18x | 10.43 / 0.00 | 278.1->279.7 | 13.31->12.44 (10.51) |
| 16 | `25-75` | `kiwi_page_bloom10` | 229.8 | 435.2 | 326.5 | 1059.3 | 1059.3 (0.944) | 363.8 (2.75, 0.47) | 607.7 (1645.5) | 3.15 (0.00) | 1.26x | 16.59 / 21.90 | 777.3->777.3 | 13.47->13.47 (10.66) |
| 16 | `25-75` | `kiwi_tile_xor_baseline` | 192.4 | 519.9 | 390.0 | 1446.5 | 1446.5 (0.691) | 428.4 (2.33, 0.46) | 822.1 (1216.4) | 3.15 (0.00) | 1.26x | 14.98 / 20.07 | 425.9->425.9 | 13.44->13.44 (10.64) |
| 16 | `25-75` | `kiwi_sst_xor_baseline` | 176.3 | 567.4 | 425.7 | 1501.1 | 1501.1 (0.666) | 469.9 (2.13, 0.46) | 1050.1 (952.3) | 3.15 (0.00) | 1.26x | 14.77 / 19.83 | 419.2->419.2 | 13.44->13.44 (10.64) |
| 0 | `50-50` | `plain_bloom10` | 1506.6 | 66.4 | 33.2 | 38.0 | 38.0 (26.347) | 264.3 (3.78, 0.48) | 804.8 (1242.5) | 4.85 (26.55) | 1.29x | 10.08 / 0.00 | 266.6->229.5 | 13.28->10.00 (7.75) |
| 16 | `50-50` | `kiwi_page_bloom10` | 182.9 | 546.9 | 273.7 | 1442.0 | 1442.0 (0.694) | 337.4 (2.96, 0.42) | 617.0 (1620.7) | 3.15 (0.00) | 1.71x | 16.60 / 21.90 | 777.5->777.5 | 13.47->13.47 (7.86) |
| 16 | `50-50` | `kiwi_tile_xor_baseline` | 152.7 | 655.0 | 327.8 | 1430.6 | 1430.5 (0.699) | 424.8 (2.35, 0.41) | 857.1 (1166.7) | 3.00 (0.00) | 1.46x | 14.98 / 20.10 | 423.1->360.4 | 13.44->11.44 (7.85) |
| 16 | `50-50` | `kiwi_sst_xor_baseline` | 133.1 | 751.5 | 376.1 | 1618.4 | 1618.4 (0.618) | 489.4 (2.04, 0.41) | 1074.1 (931.0) | 3.00 (0.00) | 1.46x | 14.77 / 19.86 | 416.5->354.8 | 13.44->11.44 (7.84) |
| 0 | `75-25` | `plain_bloom10` | 1775.8 | 56.3 | 14.1 | 44.5 | 44.5 (22.454) | 272.6 (3.67, 0.43) | 813.7 (1229.0) | 5.32 (49.01) | 1.45x | 9.98 / 0.00 | 271.6->173.9 | 13.31->7.24 (4.98) |
| 16 | `75-25` | `kiwi_page_bloom10` | 119.7 | 835.4 | 209.5 | 1497.9 | 1497.9 (0.668) | 359.1 (2.78, 0.36) | 604.2 (1655.1) | 3.01 (0.00) | 2.27x | 16.59 / 21.92 | 774.8->660.2 | 13.47->11.47 (5.06) |
| 16 | `75-25` | `kiwi_tile_xor_baseline` | 108.6 | 920.8 | 230.9 | 1535.2 | 1535.2 (0.651) | 418.6 (2.39, 0.36) | 841.9 (1187.8) | 3.00 (0.00) | 2.26x | 14.98 / 20.10 | 423.1->360.4 | 13.44->11.44 (5.05) |
| 16 | `75-25` | `kiwi_sst_xor_baseline` | 104.8 | 954.5 | 239.3 | 1500.6 | 1500.6 (0.666) | 456.5 (2.19, 0.35) | 1142.5 (875.3) | 3.00 (0.00) | 2.26x | 14.77 / 19.86 | 416.5->354.8 | 13.44->11.44 (5.05) |
| 0 | `100-0` | `plain_bloom10` | 2088.8 | 47.9 | 0.05 | 47.9 | 47.9 (20.888) | N/A | N/A | 5.60 (54.98) | 2.07x | 10.22 / 0.00 | 275.1->128.7 | 13.31->4.61 (2.22) |
| 16 | `100-0` | `kiwi_page_bloom10` | 66.4 | 1506.9 | 1.51 | 1507.1 | 1507.0 (0.663) | N/A | N/A | 3.15 (0.00) | 4.19x | 16.59 / 21.90 | 777.3->547.3 | 13.47->9.47 (2.26) |
| 16 | `100-0` | `kiwi_tile_xor_baseline` | 61.1 | 1635.4 | 1.64 | 1635.6 | 1635.6 (0.611) | N/A | N/A | 3.15 (0.00) | 4.19x | 14.98 / 20.07 | 425.9->300.2 | 13.44->9.44 (2.25) |
| 16 | `100-0` | `kiwi_sst_xor_baseline` | 69.4 | 1440.4 | 1.44 | 1440.6 | 1440.6 (0.694) | N/A | N/A | 3.15 (0.00) | 4.19x | 14.77 / 19.83 | 419.2->295.6 | 13.44->9.44 (2.25) |
