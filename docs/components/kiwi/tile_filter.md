# KiWi Per-Tile XOR+Page Filter (`tile-filter`)

This note documents the per-tile XOR+page filter optimization implemented on
the `tile-filter` branch and benchmarked against both pre-XOR baselines and
eager filter reconstruction.

- Implementation:
  - [`XorFilterHash` / `XorFilterPolicy`](../../../table/block_based/filter_policy_internal.h)
  - [`TileXorFilterBuilder`, `TileXorFilterReader`, `Directory` v2](../../../table/block_based/kiwi_tile.h)
  - [`BlockBasedTableBuilder::FlushTile()`](../../../table/block_based/block_based_table_builder.cc)
  - [`BlockBasedTable::KiWiGet()`](../../../table/block_based/block_based_table_reader.cc)
  - [`examples/kiwi_tile_filter_bench.cc`](../../../examples/kiwi_tile_filter_bench.cc)
- Optimizations & augmentations (`tile-filter-opt`): [tile_filter_optimizations.md](tile_filter_optimizations.md)

## Design

When `BlockBasedTableOptions::kiwi_tile_pages >= 2` is paired with
`NewXorFilterPolicy(fingerprint_bytes)` (or `"xorfilter:<bytes>"`), KiWi writes
**Directory v2** (`DirectoryBuilder::kFormatVersionTileFilter = 2`): instead of
`h` separate Bloom filters per tile, each tile carries one serialized
`TileXorFilter` in `rocksdb.kiwi.directory`.

### 1. Slot Encoding (`[fingerprint || page_index]`)

For a tile with `num_pages` pages (`1 <= num_pages <= kiwi_tile_pages`), let:
- `page_bits = ceil(log2(num_pages))` (`0` when `num_pages == 1`)
- `fp_bits = min(8 * fingerprint_bytes, 64 - page_bits)`
- `slot_bits = fp_bits + page_bits` (`1 <= slot_bits <= 64`)

Every key added to the tile is mapped via `XorFilterHash::SeededDerive(hash,
seg_len, seed)` to three distinct segment slots `idx[0], idx[1], idx[2]` and a
`fp_bits`-bit fingerprint `fp`. During hypergraph peeling and assignment, the
target value solved for each key `(user_key, page_index)` is:

```
target = (fp << page_bits) | page_index
```

Slots are bit-packed little-endian into `(3 * seg_len * slot_bits + 7) / 8`
bytes after an 8-byte header (`version=1, seed, fp_bits, page_bits, seg_len`).

On lookup (`BlockBasedTable::KiWiGet` -> `Directory::TileMayMatch`):
1. One `GetSliceHash64(user_key)` and 3 XOR slot reads produce `combined`.
2. **True / false negative**: if `(combined >> page_bits) != fp` or
   `(combined & page_mask) >= num_pages`, the tile cannot contain `user_key`
   (`BLOOM_FILTER_USEFUL` incremented, `0` pages opened).
3. **True / false positive**: `BLOOM_FILTER_FULL_POSITIVE` is incremented and
   **only `tile.pages[page_index]`** is passed to
   `KiWiTableIterator::NewPageIterator()`.

### 2. Unique-Sort-Key Assumption

Because a 3-way XOR filter solves one fixed-width value per distinct key hash
(duplicate 3-edges make hypergraph peeling impossible), per-tile XOR+page
filters assume **sort keys (`user_key`) are unique within each tile** and **do
not span 2 or more pages in the same tile**. As a safety guard,
`TileXorFilterBuilder::Finish()` deduplicates equal 64-bit hashes keeping the
earliest insertion position (newest internal key), so peeling never fails if an
internal-key version ever collides, but multi-page version resolution within one
tile is not supported in v2 tile-filter mode. Use Bloom/Ribbon per-page filters
(v1) when multiple versions of the same user key can coexist across pages of one
tile.

### 3. Avoiding Filter Reconstruction on Page Drops

In a 3-way XOR filter, overwriting the peeled pivot slot `B[v]` of a key on a
dropped page **is unsafe**: during peeling, keys on surviving pages often
assigned their own pivot vertices after an edge on the dropped page was removed,
so their reverse-assignment values `B[pivot']` depend algebraically on `B[v]`.
Mutating `B[v]` in-place would corrupt surviving keys on other pages unless the
entire peel dependency DAG were retained and re-propagated.

Instead, KiWi avoids filter reconstruction completely with **zero cost and zero
false negatives**:
- `DB::SecondaryDeleteBelow(floor)` records `kiwi_delete_floor = floor` in the
  MANIFEST (`O(files)` metadata only) without touching the XOR filter.
- On a lookup, `TileMayMatch` returns the single `page_index` for any matching
  or false-positive key.
- `KiWiTableIterator::NewPageIterator()` immediately checks
  `tile.pages[page_index].max_delete_key < floor` in the pinned directory and
  returns `nullptr` without reading the page from disk.
- Physical shrinkage of both the SST pages and the tile XOR filters occurs when
  compaction rewrites the file.

---

## Benchmark Results

Measured in release mode (`DEBUG_LEVEL=0`, `-DNDEBUG -fno-rtti -O2`) on a
48-core Linux host with `examples/kiwi_tile_filter_bench` (4 KB data blocks,
uncompressed, 100-byte values carrying an 8-byte big-endian delete key `D`,
`write_buffer_size = 64 MB` for Suites 1--2 and `1 MB` / `L1 = 1 MB` /
multiplier `5` for Suite 3).

Pre-XOR baselines included in every benchmark:
- `plain_bloom10`: Stock RocksDB (`kiwi_tile_pages = 0`, `NewBloomFilterPolicy(10)`)
- `kiwi_no_filter`: KiWi without any filter (`filter_policy = nullptr`)
- `kiwi_page_bloom10`: Original KiWi per-page 10-bit Bloom filters (v1)

Tile-XOR configurations:
- `kiwi_tile_xor1`: Per-tile XOR+page filter with 1-byte fingerprint (`8 + ceil(log2(h))` bits/slot)
- `kiwi_tile_xor2`: Per-tile XOR+page filter with 2-byte fingerprint (`16 + ceil(log2(h))` bits/slot)

Query metrics are split into:
- **Memory-access time** (`mem_*`): 512 MB `LRUCache` pre-warmed by full scan,
  `ReadOptions::read_tier = kBlockCacheTier`, asserting
  `PerfContext::block_read_count == 0`.
- **I/O-inclusive time** (`io_*`): `no_block_cache = true`, measuring wall time
  and `PerfContext::block_read_count` per query.

### Suite 1: Query-Heavy Workload (200k unique keys, 300k queries each)

| `h` | Config | Flush (ms) | Mem Present (ns/op) | Mem Absent (ns/op) | Mem 95%Get/5%Put (ns/op) | IO Present (us/op) | IO Absent (us/op) | Absent FP (%) | Filter Checks/Get | Filter (bits/key) | Dir (bits/key) | TableReader Mem (KB) | SST (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `plain_bloom10` | 133.7 | 1850.0 | 444.5 | 1602.2 | 3.85 | 0.44 | 0.885% | 1.00 | 10.16 | 0.00 | 402.7 | 22.11 |
| 4 | `kiwi_no_filter` | 121.1 | 4718.4 | 4440.7 | 4579.3 | 13.35 | 12.54 | 0.000% | 0.00 | 0.00 | 5.53 | 889.1 | 21.94 |
| 4 | `kiwi_page_bloom10` | 125.9 | 2539.1 | 983.9 | 1748.7 | 4.22 | 0.99 | 0.069% | 4.00 | 16.48 | 22.01 | 1291.5 | 22.33 |
| 4 | `kiwi_tile_xor1` | 111.8 | 1654.5 | 915.4 | 1594.5 | 3.82 | 0.91 | 0.402% | 1.00 | 15.28 | 20.70 | 921.2 | 22.30 |
| 4 | `kiwi_tile_xor2` | 109.8 | 1660.7 | 871.9 | 1587.2 | 3.65 | 0.88 | 0.003% | 1.00 | 27.11 | 32.52 | 1209.8 | 22.58 |
| 8 | `kiwi_no_filter` | 106.0 | 5160.0 | 5267.7 | 6058.2 | 20.98 | 21.22 | 0.000% | 0.00 | 0.00 | 5.52 | 869.7 | 21.98 |
| 8 | `kiwi_page_bloom10` | 111.8 | 1917.2 | 1033.1 | 1735.3 | 3.94 | 1.03 | 0.072% | 8.00 | 16.54 | 22.06 | 1273.5 | 22.37 |
| 8 | `kiwi_tile_xor1` | 122.8 | 1642.7 | 848.4 | 1564.4 | 3.73 | 0.91 | 0.398% | 1.00 | 15.07 | 20.41 | 852.9 | 22.33 |
| 8 | `kiwi_tile_xor2` | 121.4 | 1698.4 | 847.6 | 1627.2 | 3.72 | 0.86 | 0.001% | 1.00 | 25.86 | 31.20 | 1116.3 | 22.59 |
| 16 | `kiwi_no_filter` | 99.2 | 9738.4 | 9309.7 | 9361.0 | 41.80 | 41.45 | 0.000% | 0.00 | 0.00 | 5.52 | 855.9 | 22.02 |
| 16 | `kiwi_page_bloom10` | 119.6 | 1893.3 | 1127.8 | 1835.7 | 4.21 | 1.17 | 0.073% | 15.99 | 16.57 | 22.09 | 1260.5 | 22.42 |
| 16 | `kiwi_tile_xor1` | 125.3 | 1531.3 | 799.2 | 1479.4 | 3.63 | 0.81 | 0.381% | 1.00 | 15.59 | 20.90 | 830.0 | 22.39 |
| 16 | `kiwi_tile_xor2` | 120.7 | 1512.6 | 799.5 | 1521.3 | 3.60 | 0.80 | 0.003% | 1.00 | 25.91 | 31.21 | 1081.7 | 22.63 |

Key observations:
- **1 filter probe vs `h` probes**: `kiwi_tile_xor1`/`xor2` stay at `1.00` filter
  check per lookup regardless of `h`, while `kiwi_page_bloom10` probes `4`, `8`,
  and `16` Bloom filters per lookup.
- **Query latency**: At `h = 16`, `kiwi_tile_xor1` improves memory-access query
  time by **19.1%** on present keys (`1531 vs 1893 ns`) and **29.1%** on absent
  keys (`799 vs 1128 ns`), and improves I/O-inclusive query time by **13.8%** on
  present keys (`3.63 vs 4.21 us`) and **30.8%** on absent keys (`0.81 vs 1.17 us`).
- **Space & pinned memory**: Because `FastLocalBloom` enforces a minimum of one
  64-byte (512-bit) cache line per page, `kiwi_page_bloom10` actually uses
  `16.5 bits/key` on 4 KB pages (~31 keys/page), plus `h` `FilterBitsReader`
  objects per tile (`1260--1292 KB` `TableReader` memory). `kiwi_tile_xor1`
  pools the entire tile into one filter (`15.1--15.6 bits/key`) and cuts
  `TableReader` memory by **28.7%--34.2%** (`830--921 KB`).

### Suite 2: Flush + Secondary Delete & Filter Reconstruction (`h = 8`, 200k keys)

Comparing `plain_bloom10` (Delete + Flush + `CompactRange(kForce)`),
`kiwi_no_filter`, `kiwi_page_bloom10`, **eager tile-XOR reconstruction**
(reading surviving pages of affected tiles and re-peeling `TileXorFilterBuilder`),
and **lazy `fp || page_idx` floor invalidation** (`DB::SecondaryDeleteBelow`):

| Drop | Config | Delete / Recon Time (ms) | Compact Reclaim (ms) | Deleted Key Q (mem ns / io us / io_blk) | Surviving Key Q (mem ns / io us) | Filter KB (Before -> Lazy -> Eager -> Comp) | TableReader KB (Before -> Comp) | SST MB (Before -> Lazy -> Comp) |
|---|---|---|---|---|---|---|---|---|
| 10% | `plain_bloom10` (full rewrite) | 258.78 | -- | 395.6 / 0.42 / 0.01 | 1536.2 / 3.89 | 247.9 -> 223.9 -> -- -> 223.9 | 402.7 -> 362.7 | 22.11 -> 19.91 -> 19.91 |
| 10% | `kiwi_no_filter` | 0.67 | 130.69 | 5299.6 / 21.37 / 7.92 | 5556.6 / 21.79 | 0.0 -> 0.0 -> -- -> 0.0 | 869.7 -> 781.4 | 21.98 -> 21.98 -> 19.79 |
| 10% | `kiwi_page_bloom10` | 0.93 | 132.51 | 1759.5 / 3.66 / 0.91 | 1864.0 / 4.01 | 403.8 -> 403.8 -> -- -> 363.5 | 1273.5 -> 1144.9 | 22.37 -> 22.37 -> 20.14 |
| 10% | `kiwi_tile_xor1` (**eager recon**) | **31.58** (11.73 io + 18.18 peel) | -- | -- | -- | 368.0 -> -- -> **336.2** -> -- | -- | 22.33 -> 22.33 -> -- |
| 10% | `kiwi_tile_xor1` (**lazy floor**) | **1.05** | 145.88 | 1617.3 / 3.48 / 0.90 | 1682.8 / 3.77 | 368.0 -> 368.0 -> -- -> 331.2 | 852.9 -> 766.3 | 22.33 -> 22.33 -> 20.11 |
| 10% | `kiwi_tile_xor2` (**lazy floor**) | **0.82** (eager: 32.62) | 144.39 | 1612.8 / 3.49 / 0.90 | 1689.2 / 3.70 | 631.4 -> 631.4 -> 576.2 -> 568.2 | 1116.3 -> 1003.3 | 22.59 -> 22.59 -> 20.34 |
| 50% | `plain_bloom10` (full rewrite) | 294.46 | -- | 398.9 / 0.43 / 0.01 | 1584.1 / 3.53 | 247.9 -> 119.9 -> -- -> 119.9 | 402.7 -> 202.7 | 22.11 -> 11.06 -> 11.06 |
| 50% | `kiwi_no_filter` | 0.52 | 77.73 | 3626.1 / 12.78 / 4.62 | 3641.3 / 12.99 | 0.0 -> 0.0 -> -- -> 0.0 | 869.7 -> 436.0 | 21.98 -> 21.98 -> 11.01 |
| 50% | `kiwi_page_bloom10` | 1.27 | 81.16 | 1167.4 / 1.51 / 0.14 | 1816.4 / 3.91 | 403.8 -> 403.8 -> -- -> 202.0 | 1273.5 -> 638.0 | 22.37 -> 22.37 -> 11.21 |
| 50% | `kiwi_tile_xor1` (**eager recon**) | **19.49** (7.08 io + 11.19 peel) | -- | -- | -- | 368.0 -> -- -> **197.7** -> -- | -- | 22.33 -> 22.33 -> -- |
| 50% | `kiwi_tile_xor1` (**lazy floor**) | **0.79** | 86.63 | 1016.1 / 1.34 / 0.14 | 1681.8 / 3.61 | 368.0 -> 368.0 -> -- -> 184.0 | 852.9 -> 427.6 | 22.33 -> 22.33 -> 11.19 |
| 50% | `kiwi_tile_xor2` (**lazy floor**) | **1.01** (eager: 18.66) | 87.57 | 1016.2 / 1.33 / 0.14 | 1839.3 / 3.69 | 631.4 -> 631.4 -> 341.5 -> 315.7 | 1116.3 -> 559.3 | 22.59 -> 22.59 -> 11.32 |
| 75% | `plain_bloom10` (full rewrite) | 321.51 | -- | 395.1 / 0.42 / 0.01 | 1417.2 / 3.48 | 247.9 -> 63.9 -> -- -> 63.9 | 402.7 -> 106.7 | 22.11 -> 5.54 -> 5.54 |
| 75% | `kiwi_page_bloom10` | 1.80 | 46.93 | 1146.8 / 1.39 / 0.10 | 1770.7 / 3.87 | 403.8 -> 403.8 -> -- -> 101.0 | 1273.5 -> 320.2 | 22.37 -> 22.37 -> 5.62 |
| 75% | `kiwi_tile_xor1` (**lazy floor**) | **0.87** (eager: 10.44) | 50.02 | 976.8 / 1.19 / 0.09 | 1599.7 / 3.60 | 368.0 -> 368.0 -> 107.7 -> 92.0 | 852.9 -> 215.0 | 22.33 -> 22.33 -> 5.61 |

Key observations:
- **Avoiding reconstruction cuts delete latency by 12x--30x**: Lazy
  `SecondaryDeleteBelow` finishes in **0.79--1.09 ms** across all drop
  fractions, versus **10.44--32.62 ms** for eager tile-XOR filter
  reconstruction (`3.91--11.73 ms` reading surviving pages + `5.50--19.45 ms`
  re-peeling `TileXorFilterBuilder`), and **258.8--321.5 ms** for `plain_bloom10`
  full-tree compaction.
- **Lazy deleted-key queries skip dropped pages with 0 I/O**: At 10% drop, edge
  pages still hold surviving keys so `io_blk = 0.90`; at 25% / 50% / 75% drop,
  `TileMayMatch` routes deleted keys to pages whose `max_delete_key < floor`,
  cutting `io_blk` to `0.26 -> 0.14 -> 0.09` (`977 ns` mem / `1.19 us` I/O at
  75%).

### Suite 3: Delete-Heavy LSM Workloads (`seqdel` and `churn`, `h = 8`)

Modeled on `tombstone_bench` (`1 MB` write buffer/target file/L1 base,
multiplier `5`, `200k` preloaded keys + `200k` ops, verified against an exact
in-memory model):
- `seqdel`: `30%` sequential point deletes sweeping the key space + `70%` puts.
- `churn`: `90%` delete/reinsert toggling of `20k` hot keys + `10%` puts.

| Workload | Config | Ops/s | Avg Op (us) | Write Amp | Mem Pres (ns) | Mem Abs (ns) | IO Pres (us) | IO Abs (us) | IO Blk/Abs | Filter KB (Before -> After) | TableReader KB (Before -> After) | SST MB (Before -> After) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `seqdel` | `plain_bloom10` | 591,253 | 1.69 | 5.13 | 1659.0 | 494.3 | 3.71 | 0.52 | 0.01 | 172 -> 186 | 341 -> 367 | 15.3 -> 16.4 |
| `seqdel` | `kiwi_no_filter` | 589,626 | 1.70 | 4.74 | 7540.2 | 6598.9 | 32.56 | 28.14 | 10.80 | 0 -> 0 | 636 -> 755 | 15.0 -> 17.7 |
| `seqdel` | `kiwi_page_bloom10` | 574,392 | 1.74 | 4.59 | 2430.4 | 1558.2 | 4.89 | 1.69 | 0.05 | 270 -> 309 | 885 -> 1022 | 15.0 -> 17.0 |
| `seqdel` | `kiwi_tile_xor1` | **509,503** | **1.96** | 5.35 | **1757.2** | **906.4** | **3.87** | **0.95** | **0.00** | 254 -> 269 | **623 -> 666** | 15.4 -> 16.4 |
| `seqdel` | `kiwi_tile_xor2` | 530,756 | 1.88 | 5.59 | 1678.2 | 1153.5 | 3.79 | 1.51 | 0.17 | 442 -> 506 | 824 -> 919 | 15.8 -> 17.1 |
| `churn` | `plain_bloom10` | 738,824 | 1.35 | 3.14 | 1809.2 | 847.3 | 3.92 | 1.17 | 0.15 | 167 -> 215 | 334 -> 403 | 15.0 -> 17.6 |
| `churn` | `kiwi_no_filter` | 718,712 | 1.39 | 2.59 | 15730.9 | 15374.5 | 67.11 | 67.52 | 26.13 | 0 -> 0 | 623 -> 774 | 14.8 -> 18.2 |
| `churn` | `kiwi_page_bloom10` | 655,965 | 1.52 | 2.73 | 2748.6 | 2096.8 | 4.92 | 2.43 | 0.16 | 274 -> 339 | 912 -> 1087 | 15.2 -> 17.9 |
| `churn` | `kiwi_tile_xor1` | **605,403** | **1.65** | 3.00 | **1930.5** | **1227.9** | **4.21** | **1.47** | **0.10** | 248 -> 290 | **616 -> 706** | 15.1 -> 16.8 |
| `churn` | `kiwi_tile_xor2` | 564,964 | 1.77 | 3.13 | 2011.1 | 1312.1 | 4.13 | 1.54 | 0.10 | 426 -> 507 | 791 -> 931 | 15.3 -> 17.2 |

**Does performance necessarily go up?** **No:**
1. On **write/delete-heavy LSM churn** (`seqdel` and `churn`), overall op
   throughput **decreases by 7.7%--13.9%** with `kiwi_tile_xor1`/`xor2` relative
   to `kiwi_page_bloom10` (`510k vs 574k ops/s` in `seqdel`; `605k vs 656k ops/s`
   in `churn`). Constructing an XOR filter requires building and peeling a
   3-uniform hypergraph (~3.6--4.1x slower filter build than Bloom in
   `examples/xor_filter_query_example`: `14.43--16.40 ms` vs `3.99 ms`), so when
   every `1 MB` flush and compaction (`w_amp = 2.6--5.6`) re-peels tile filters,
   build CPU dominates.
2. On **queries and `TableReader` memory**, `kiwi_tile_xor1` still wins even
   after heavy `seqdel`/`churn` fragmentation: present and absent queries are
   **28%--42% faster** in memory (`1757 vs 2430 ns` and `906 vs 1558 ns` in
   `seqdel`; `1931 vs 2749 ns` and `1228 vs 2097 ns` in `churn`) and **14%--44%
   faster** with I/O, while pinning **~35% less `TableReader` memory**
   (`666 vs 1022 KB` in `seqdel`, `706 vs 1087 KB` in `churn`).
