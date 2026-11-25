# KiWi 3-Way XOR Filter Benchmark Summary

This document synthesizes the release-mode (`DEBUG_LEVEL=0`, `-DNDEBUG -fno-rtti -O2`) empirical evaluation of all KiWi 3-way XOR filter designs against both Stock RocksDB with Bloom filters (`plain_bloom10`) and KiWi with per-page Bloom filters (`kiwi_page_bloom10`).

- **Related component docs:**
  - Architecture & `DB::SecondaryDeleteBelow`: [index.md](index.md)
  - Initial per-tile XOR design (`tile-filter`): [tile_filter.md](tile_filter.md)
  - Tight sizing, compact directory, zero-peel drop, delete-rank bits, & SST-wide XOR (`tile-filter-opt`): [tile_filter_optimizations.md](tile_filter_optimizations.md)
- **Benchmark harness:** [`examples/kiwi_tile_filter_bench.cc`](../../../examples/kiwi_tile_filter_bench.cc) (48-core Linux host, 4 KB uncompressed data blocks, 100-byte values with an 8-byte big-endian delete key `D` at offset `0`, WAL disabled).
  - **Memory-only queries (`mem_*`)**: 512 MB `LRUCache` pre-warmed via full scan with `ReadOptions::read_tier = kBlockCacheTier`, asserting `PerfContext::block_read_count == 0`.
  - **Uncached I/O queries (`io_*`)**: `no_block_cache = true`, measuring end-to-end wall latency, throughput, and `PerfContext::block_read_count` (`io_blk`) per `Get()`.
  - **Percentage convention**: Signed deltas report `(candidate - baseline) / baseline * 100%`. Because `QPS = 1 / latency`, negative latency deltas (`-X%`) correspond to positive throughput deltas (`+Y%`); both are stated explicitly where latency and QPS appear together. Primary comparisons use the contemporaneous `tile-filter-opt` run ([tile_filter_optimizations.md](tile_filter_optimizations.md)); historical `kiwi_tile_xor_baseline` / `kiwi_tile_xor_baseline_16bit` rows (and the 75%-drop `kiwi_page_bloom10` reference row, marked `[tf]`) come from [tile_filter.md](tile_filter.md).

## Standardized Filter Configurations

| Configuration | Granularity | Policy / Option | Slot Encoding & Mechanism |
|---|---|---|---|
| `plain_bloom10` | Whole SST (non-KiWi) | `kiwi_tile_pages=0`, `NewBloomFilterPolicy(10)` | Stock RocksDB `FastLocalBloom` + standard per-block index; secondary delete requires point `Delete` + `Flush` + `CompactRange(kForce)` |
| `kiwi_no_filter` | KiWi (`h` pages/tile) | `filter_policy=nullptr` | Directory v1 delete fences only; every point lookup reads and merges up to `h` surviving pages in the target tile |
| `kiwi_page_bloom10` | Page (KiWi v1) | `NewBloomFilterPolicy(10)` | `h` independent `FastLocalBloom` filters per tile (`>= 64 B` cache line each); `h` hashes/probes per `Get()` |
| `kiwi_tile_xor_baseline` | Tile (KiWi v2) | `NewXorFilterPolicy(1)` (`tile-filter`) | Initial 8-bit `[fp \|\| page_index]` 3-way XOR per tile (`+32` segment slack, uncounted directory in `EstimatedFileSize`) |
| `kiwi_tile_xor_baseline_16bit` | Tile (KiWi v2) | `NewXorFilterPolicy(2)` (`tile-filter`) | Initial 16-bit `[fp \|\| page_index]` 3-way XOR per tile (`+32` segment slack) |
| `kiwi_tile_xor_tight` | Tile (KiWi v2) | `NewXorFilterPolicy(1)` (`tile-filter-opt`) | Tight `+4` segment slack, 1-derive cached peeler, 32 B `TileXorFilterReader`, directory size accounting, zero-peel `ShiftBasePage` |
| `kiwi_tile_xor_tight_16bit` | Tile (KiWi v2) | `NewXorFilterPolicy(2)` (`tile-filter-opt`) | 16-bit `[fp \|\| page_index]` with tight `+4` slack, compact `Directory`, and zero-peel `ShiftBasePage` (`<= 0.001%` FPR) |
| `kiwi_tile_xor_delete_rank` | Tile (KiWi v2) | `{id=xorfilter:1;kiwi_delete_rank_bits=2}` | Packs 2 sub-page delete-key rank bits (`[fp \|\| rank_bucket \|\| page_index]`) to skip straddling edge pages when bucket `< floor` |
| `kiwi_sst_xor_index_free` | SST (KiWi v3) | `{id=xorfilter:1;kiwi_file_filter=true}` | Single SST-wide 3-way XOR (`[fp \|\| local_page_index]`) probed before `NewIndexIterator()`, skipping index I/O on negatives |

---

## 1. Executive Summary & Best Filter Design Matrix

### Best XOR Design by Evaluation Dimension

| Dimension | Best XOR Design(s) | Headline Metric | vs `plain_bloom10` (non-KiWi Bloom) | vs `kiwi_page_bloom10` (KiWi Bloom) | Why It Wins |
|---|---|---|---|---|---|
| **Write Amp (`seqdel`, 1 MB L1)** | `kiwi_tile_xor_tight` | `w_amp = 4.61` | **-18.4%** (`5.65`) | **-12.5%** (`5.27`) | `EstimateKiWiDirectoryBytes()` prevents L1 file-size overshoot; tight `+4` slack trims directory tail (`-13.8%` vs `kiwi_tile_xor_baseline` `5.35`) |
| **Write Amp (`churn`, 1 MB L1)** | `kiwi_sst_xor_index_free` (`kiwi_tile_xor_tight` 2nd) | `w_amp = 2.49` (`2.80`) | **-37.3%** (`-29.5%` vs `3.97`) | **-23.4%** (`-13.8%` vs `3.25`) | Smallest directory tail + accurate SST split estimation eliminates cascading L1->L2 compactions |
| **Write Amp (Multi-SST Secondary Delete)** | `kiwi_tile_xor_tight` / `kiwi_sst_xor_index_free` | `w_amp = 0.00` | **-100.0%** (`16.44` at 50%, `6.13` at 75%) | Ties `0.00` | `SecondaryDeleteBelow` unlinks SSTs with `max_delete_key < floor` via MANIFEST `DeleteFile` with zero data rewrite |
| **Filter Space (`flt_bpk`, `h = 4..16`)** | `kiwi_sst_xor_index_free` (`kiwi_tile_xor_tight` 2nd) | `12.30--14.76 bpk` (`13.02--14.98`) | `+21.1%` to `+45.3%` (`10.16 bpk` has no page/tile route) | **-25.4% to -10.9%** (`16.48--16.57 bpk`) | Pools keys into one bit-packed `1.23n + 12`-slot array, avoiding `FastLocalBloom`'s 64 B (512-bit) per-page cache-line floor |
| **Pinned `TableReader` RAM (`h = 4..16`)** | `kiwi_sst_xor_index_free` (`kiwi_tile_xor_tight` 2nd) | `655.6--665.4 KB` (`688.3--745.4 KB`) | `+62.8%` to `+65.2%` (`402.7 KB`, no KiWi dir) | **-50.0% to -47.5%** (`1266.4--1314.8 KB`) | One SST/tile filter reader + stripped `Directory` replaces `h` per-page Bloom readers and raw slice retention |
| **Absent Point-Query QPS (Suite 1, `h = 4..16`)** | `kiwi_sst_xor_index_free` | `2.75--2.95 Mops/s` mem (`338.8--363.2 ns`); `2.78 Mops/s` I/O (`0.36 us`) | **+22.4% to +31.2% mem QPS** (`-18.3%` to `-23.8%` lat); **+22.2% I/O QPS** (`-18.2%` lat) | **+152.5% to +216.7% mem QPS** (`-60.4%` to `-68.4%` lat); **+172.2% to +211.1% I/O QPS** (`-63.3%` to `-67.9%` lat) | Rejects 99.62% of absent keys before opening or binary-searching the SST index block (`0` index/data reads) |
| **Present Point-Query QPS (Suite 1, `h = 8 / 16`)** | `kiwi_tile_xor_tight` (`h=8`) / `kiwi_tile_xor_tight_16bit` (`h=16`) | `661.5 kops/s` (`1511.7 ns`, `h=8`); `678.9 kops/s` (`1472.9 ns`, `h=16`) | **+0.8% / +3.5% mem QPS**; **+15.6% / +12.1% I/O QPS** | **+13.7% / +24.7% mem QPS**; **+5.0% / +8.2% I/O QPS** | Single XOR probe pinpoints the exact `page_index` directly without `h` Bloom hashes or multi-page merge iterators |
| **95% Get / 5% Put Mix QPS (Suite 1, `h = 4..16`)** | `kiwi_sst_xor_index_free` | `742.4--781.6 kops/s` (`1279.4--1347.0 ns`) | **+5.5% to +11.1% QPS** (`-5.2%` to `-10.0%` lat) | **+27.1% to +41.5% QPS** (`-21.3%` to `-29.3%` lat) | Combines 1-probe present routing with index-free absent rejection on the 47.5% negative-Get fraction |
| **Post-Delete Edge-Page `delQ` (Suite 2, 10%--75% Drop)** | `kiwi_tile_xor_delete_rank` | `751.9--1075.3 kops/s` I/O (`0.93--1.33 us`, `0.15->0.02 io_blk`) | Lazy `0.78--0.87 ms` delete vs `248--339 ms` rewrite (`delQ` `+120.5%` to `+202.3%` I/O lat vs compacted `plain_bloom10`) | **+182.7% I/O QPS** (`-64.6%` lat, **-83.5%** `io_blk`) at 10% drop; **+49.5% to +54.6% I/O QPS** at 50%--75% | 2-bit intra-page delete-key rank bucket prunes straddling edge-page reads in RAM when bucket upper bound `< floor` |
| **Physical Page Reclaim Speed (Suite 2, 10%--75% Drop)** | `kiwi_tile_xor_tight` (`ShiftBasePage`) | `19.58--50.01 ms` (`1.10x--1.32x` spAmp) | **-79.9% to -94.2% time** (`4.96x--17.34x` faster than `248.27--339.48 ms` Delete+Compact) | **-4.2% to -9.3% faster** than `kiwi_page_bloom10` zero-peel; **2.53x--2.89x faster** than `CompactRange` | Copies raw surviving data blocks and increments `TileXorFilter` v2 `base_page` via `memcpy` with **0 hypergraph peeling** |
| **LSM `churn` Ingestion + Query QPS (Suite 3)** | `kiwi_sst_xor_index_free` (`kiwi_tile_xor_tight` 2nd) | `678.0 kops/s` write; `1.28 Mops/s` absent mem (`781.2 ns`) | **+2.0% write QPS**, **-37.3% `w_amp`** (`-14.0%` absent mem QPS vs `plain_bloom10`) | **+2.3% write QPS**, **+170.6% absent mem QPS** (`-63.0%` lat), **+119.5% absent I/O QPS** | Lower `w_amp` (`2.49` vs `3.25`) offsets XOR peel CPU on writes while skipping L0/L1/L2 index blocks on tombstone-heavy reads |
| **Ultra-Low FPR (`<= 0.001%`)** | `kiwi_tile_xor_tight_16bit` | `0.000%--0.001%` FPR (`678.9 kops/s` pres at `h=16`) | **-99.9% to -100% FPR** (`0.885%`), `+131.0%` to `+145.9%` RAM | **-98.6% to -100% FPR** (`0.069%--0.073%`), **-24.7% to -26.5% RAM** | 16-bit XOR fingerprint drives false positives near zero while still using less RAM than `h` 64 B Bloom cache lines |

---

### Architectural Summary Across Core Dimensions

#### 1. Write Amplification (`w_amp`)
- **Root cause fixed in `tile-filter-opt`**: In the initial `tile-filter` prototype, `kiwi_tile_xor_baseline` omitted `rocksdb.kiwi.directory` from `BlockBasedTableBuilder::EstimatedFileSize()`, causing `1 MB` target SSTs in Suite 3 to overshoot to `1.04--1.06 MB` and trigger cascading L1 compactions (`w_amp = 5.35` on `seqdel`, `3.00` on `churn`). Wiring `EstimateKiWiDirectoryBytes()` into `EstimatedFileSize()` and `EstimatedTailSize()` plus shrinking segment slack from `+32` to `+4` slots eliminated this overshoot.
- **`seqdel` (`1 MB` L1)**: `kiwi_tile_xor_tight` achieves the lowest `w_amp` of any filtered configuration at **`4.61`** (**`-18.4%`** vs `plain_bloom10`'s `5.65`, **`-12.5%`** vs `kiwi_page_bloom10`'s `5.27`, **`-13.8%`** vs `kiwi_tile_xor_baseline`'s `5.35`, and matching `kiwi_no_filter`'s `4.61`). `kiwi_sst_xor_index_free` follows closely at **`4.75`** (**`-15.9%`** vs `plain_bloom10`, **`-9.9%`** vs `kiwi_page_bloom10`). Notice KiWi's tile layout inherently exhibits lower `w_amp` than `plain_bloom10` here because coarse tile-level index blocks (`1/8` as many index entries) leave slightly more data capacity per SST before hitting the target file size.
- **`churn` (`1 MB` L1)**: `kiwi_sst_xor_index_free` achieves **`w_amp = 2.49`** (**`-37.3%`** vs `plain_bloom10`'s `3.97`, **`-23.4%`** vs `kiwi_page_bloom10`'s `3.25`, **`-17.0%`** vs `kiwi_tile_xor_baseline`'s `3.00`), while `kiwi_tile_xor_tight` reaches **`2.80`** (**`-29.5%`** vs `plain_bloom10`, **`-13.8%`** vs `kiwi_page_bloom10`).
- **Multi-SST secondary deletes (Suite 2B)**: When `D` correlates with ingestion order (such as write timestamps), `DB::SecondaryDeleteBelow(floor)` unlinks 100%-dead SSTs directly in the `VersionEdit` with **`w_amp = 0.00`** (**`-100.0%`** vs `plain_bloom10`'s **`16.44`** at 50% drop and **`6.13`** at 75% drop).

#### 2. Space Complexity (Memory, Disk, & Post-Delete Space Amplification)
- **Filter & Directory Bits/Key (`flt_bpk`, `dir_bpk`)**: On 4 KB blocks (~31 keys/page), `FastLocalBloom` rounds every per-page filter up to at least one 64-byte (512-bit) cache line, inflating `kiwi_page_bloom10` to `16.48--16.57 flt_bpk` (`22.01--22.09 dir_bpk`). Pooling keys into one 3-way XOR array with `+4` segment slack cuts `kiwi_sst_xor_index_free` to **`12.30 / 13.53 / 14.76 flt_bpk`** (**`-25.4% / -18.2% / -10.9%`** vs `kiwi_page_bloom10`; **`-19.5% / -10.2% / -5.3%`** vs `kiwi_tile_xor_baseline`) and `17.59 / 18.81 / 20.04 dir_bpk` (**`-20.1% / -14.7% / -9.3%`** vs `kiwi_page_bloom10`), while `kiwi_tile_xor_tight` reaches **`13.02 / 13.96 / 14.98 flt_bpk`** (**`-21.0% / -15.6% / -9.6%`** vs `kiwi_page_bloom10`). Relative to `plain_bloom10` (`10.16 flt_bpk`, `0.00 dir_bpk`), KiWi 8-bit XOR filters carry `+21.1%` to `+47.4%` more filter bits/key because each `1.23`-overhead XOR slot embeds `page_index` (`ceil(log2(h))`) routing bits alongside the 8-bit fingerprint.
- **Pinned `TableReader` RAM (`KB`)**: Shrinking `TileXorFilterReader` from `56 B` to `32 B`, storing contiguous `tile_offsets_`, and releasing redundant raw tile buffers cuts `TableReader` RAM in `kiwi_sst_xor_index_free` to **`655.6--665.4 KB`** (**`-47.5%` to `-50.0%`** vs `kiwi_page_bloom10`'s `1266.4--1314.8 KB`; **`-19.8%` to `-28.6%`** vs `kiwi_tile_xor_baseline`'s `830.0--921.2 KB`) and in `kiwi_tile_xor_tight` to **`688.3--745.4 KB`** (**`-43.3%` to `-45.6%`** vs `kiwi_page_bloom10`). Under LSM `churn`, post-workload `TableReader` RAM drops from `1110 KB` (`kiwi_page_bloom10`) to **`604 KB`** (`kiwi_sst_xor_index_free`, **`-45.6%`**) and **`633 KB`** (`kiwi_tile_xor_tight`, **`-43.0%`**). When secondary deletes are not needed, `plain_bloom10` remains smallest (`402.7 KB` in Suite 1; `368--371 KB` in Suite 3) because it pins no KiWi directory.
- **SST File Size (`MB`)**: Across all workloads, 8-bit XOR SSTs (`22.23--22.37 MB` in Suite 1) are **`-0.22%` to `-0.45%`** smaller than `kiwi_page_bloom10` (`22.33--22.42 MB`) and within **`+0.54%` to `+1.18%`** of `plain_bloom10` (`22.11 MB`).
- **Post-Secondary-Delete Space Amplification**: KiWi provides three complementary points on the latency-vs-space reclamation curve:
  1. **Lazy MANIFEST floor (`DB::SecondaryDeleteBelow`)**: Completes in **`0.71--1.39 ms`** (`O(files)` metadata, zero disk I/O), deferring physical page removal (`1.11x / 1.33x / 2.00x / 3.98x` space amp at `10% / 25% / 50% / 75%` single-SST uniform-`D` drops; `ApplyDeleteFloor` strips 100%-dead tiles' XOR payloads in RAM immediately).
  2. **Zero-peel physical page drop (`RewriteKiWiTableDroppingPages` + `ShiftBasePage`)**: Streams surviving raw data blocks (`max_delete_key >= floor`) and shifts `base_page` in the 12-byte `TileXorFilter` v2 header via `memcpy` with **zero decompression, zero sorting, and zero XOR peeling**, shrinking `kiwi_tile_xor_tight` from `22.31 MB` to **`22.09 / 18.22 / 12.88 / 7.40 MB`** (**`1.10x / 1.09x / 1.15x / 1.32x`** space amp -- retaining only the single straddling edge page per tile) in **`50.01 / 41.00 / 30.10 / 19.58 ms`** (**`2.53x--2.89x` faster** than `CompactRange`).
  3. **Whole-SST unlink (Suite 2B)**: Immediately reclaims **`1.00x`** disk and `TableReader` RAM (`22.46 -> 11.23 / 5.61 MB`, `717.0 -> 358.5 / 179.2 KB` on `kiwi_tile_xor_tight`) in **`2.88--5.31 ms`** with `w_amp = 0.00`.

---

## 2. Quantitative Results & Percentage Deltas by Workload Suite

### Suite 1: Query-Heavy Workload (200k Unique Keys, 300k Queries, `64 MB` Flush)

#### Full Latency, Explicit Throughput, & Space Table

Rows marked `[tf]` are historical baselines from [tile_filter.md](tile_filter.md); all other rows are from the contemporaneous `tile-filter-opt` suite ([tile_filter_optimizations.md](tile_filter_optimizations.md)). Flush throughput is computed over the `200,000`-entry memtable flush (`200k / flush_s`).

| `h` | Config | Flush (`ms` / `Mops/s`) | Mem Pres (`ns` / `kops/s`) | Mem Abs (`ns` / `kops/s`) | Mem 95/5 Mix (`ns` / `kops/s`) | IO Pres (`us` / `kops/s`) | IO Abs (`us` / `kops/s`) | Abs FP (%) | `flt_bpk` | `dir_bpk` | `TableReader` (KB) | SST (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `plain_bloom10` | 124.2 / 1.61 | 1524.2 / 656.1 | 444.5 / 2249.7 | 1421.2 / 703.6 | 4.36 / 229.4 | 0.44 / 2272.7 | 0.885% | 10.16 | 0.00 | 402.7 | 22.11 |
| 4 | `kiwi_no_filter` | 114.4 / 1.75 | 4022.5 / 248.6 | 3354.1 / 298.1 | 3489.6 / 286.6 | 11.99 / 83.4 | 11.99 / 83.4 | 0.000% | 0.00 | 5.53 | 912.5 | 21.94 |
| 4 | `kiwi_page_bloom10` | 118.4 / 1.69 | 1694.6 / 590.1 | 917.0 / 1090.5 | 1716.9 / 582.4 | 3.98 / 251.3 | 0.98 / 1020.4 | 0.069% | 16.48 | 22.01 | 1314.8 | 22.33 |
| 4 | `kiwi_tile_xor_baseline` `[tf]` | 111.8 / 1.79 | 1654.5 / 604.4 | 915.4 / 1092.4 | 1594.5 / 627.2 | 3.82 / 261.8 | 0.91 / 1098.9 | 0.402% | 15.28 | 20.70 | 921.2 | 22.30 |
| 4 | `kiwi_tile_xor_baseline_16bit` `[tf]` | 109.8 / 1.82 | 1660.7 / 602.2 | 871.9 / 1146.9 | 1587.2 / 630.0 | 3.65 / 274.0 | 0.88 / 1136.4 | 0.003% | 27.11 | 32.52 | 1209.8 | 22.58 |
| 4 | `kiwi_tile_xor_tight` | 128.2 / 1.56 | 1635.8 / 611.3 | 855.9 / 1168.4 | 1607.9 / 621.9 | 4.00 / 250.0 | 0.84 / 1190.5 | 0.362% | **13.02** | **18.43** | **745.4** | 22.25 |
| 4 | `kiwi_tile_xor_delete_rank` | 124.7 / 1.60 | 1824.2 / 548.2 | 845.0 / 1183.4 | 1653.1 / 604.9 | 4.16 / 240.4 | 0.85 / 1176.5 | 0.362% | 15.77 | 21.18 | 812.5 | 22.31 |
| 4 | `kiwi_sst_xor_index_free` | 152.0 / 1.32 | **1576.5 / 634.3** | **363.2 / 2753.3** | **1319.9 / 757.6** | 4.01 / 249.4 | **0.36 / 2777.8** | 0.376% | **12.30** | **17.59** | **657.9** | **22.23** |
| 4 | `kiwi_tile_xor_tight_16bit` | 124.2 / 1.61 | 1585.5 / 630.7 | 841.5 / 1188.4 | 1663.9 / 601.0 | 3.92 / 255.1 | 0.85 / 1176.5 | **0.000%** | 23.05 | 28.46 | 990.4 | 22.49 |
| 8 | `kiwi_no_filter` | 110.4 / 1.81 | 5496.9 / 181.9 | 5597.4 / 178.7 | 5465.1 / 183.0 | 22.51 / 44.4 | 22.31 / 44.8 | 0.000% | 0.00 | 5.52 | 881.4 | 21.98 |
| 8 | `kiwi_page_bloom10` | 113.7 / 1.76 | 1718.6 / 581.9 | 963.6 / 1037.8 | 1711.7 / 584.2 | 3.96 / 252.5 | 0.99 / 1010.1 | 0.072% | 16.54 | 22.06 | 1285.2 | 22.37 |
| 8 | `kiwi_tile_xor_baseline` `[tf]` | 122.8 / 1.63 | 1642.7 / 608.8 | 848.4 / 1178.7 | 1564.4 / 639.2 | 3.73 / 268.1 | 0.91 / 1098.9 | 0.398% | 15.07 | 20.41 | 852.9 | 22.33 |
| 8 | `kiwi_tile_xor_baseline_16bit` `[tf]` | 121.4 / 1.65 | 1698.4 / 588.8 | 847.6 / 1179.8 | 1627.2 / 614.6 | 3.72 / 268.8 | 0.86 / 1162.8 | 0.001% | 25.86 | 31.20 | 1116.3 | 22.59 |
| 8 | `kiwi_tile_xor_tight` | 126.2 / 1.58 | **1511.7 / 661.5** | 803.9 / 1243.9 | 1538.4 / 650.0 | **3.77 / 265.3** | 0.83 / 1204.8 | 0.378% | **13.96** | **19.30** | **701.3** | 22.31 |
| 8 | `kiwi_tile_xor_delete_rank` | **109.7 / 1.82** | 1667.7 / 599.6 | 807.0 / 1239.2 | 1633.1 / 612.3 | 3.89 / 257.1 | 0.82 / 1219.5 | 0.378% | 16.60 | 21.94 | 765.7 | 22.37 |
| 8 | `kiwi_sst_xor_index_free` | 141.2 / 1.42 | 1530.2 / 653.5 | **338.8 / 2951.6** | **1347.0 / 742.4** | 3.81 / 262.5 | **0.36 / 2777.8** | 0.376% | **13.53** | **18.81** | **655.6** | **22.30** |
| 8 | `kiwi_tile_xor_tight_16bit` | 123.4 / 1.62 | 1529.5 / 653.8 | 798.0 / 1253.1 | 1528.8 / 654.1 | 3.86 / 259.1 | 0.81 / 1234.6 | **0.001%** | 23.94 | 29.28 | 944.9 | 22.55 |
| 16 | `kiwi_no_filter` | 107.7 / 1.86 | 9902.7 / 101.0 | 9649.8 / 103.6 | 9755.2 / 102.5 | 42.71 / 23.4 | 42.69 / 23.4 | 0.000% | 0.00 | 5.52 | 861.8 | 22.02 |
| 16 | `kiwi_page_bloom10` | 116.5 / 1.72 | 1836.5 / 544.5 | 1106.9 / 903.4 | 1810.3 / 552.4 | 4.21 / 237.5 | 1.12 / 892.9 | 0.073% | 16.57 | 22.09 | 1266.4 | 22.42 |
| 16 | `kiwi_tile_xor_baseline` `[tf]` | 125.3 / 1.60 | 1531.3 / 653.0 | 799.2 / 1251.3 | 1479.4 / 675.9 | 3.63 / 275.5 | 0.81 / 1234.6 | 0.381% | 15.59 | 20.90 | 830.0 | 22.39 |
| 16 | `kiwi_tile_xor_baseline_16bit` `[tf]` | 120.7 / 1.66 | 1512.6 / 661.1 | 799.5 / 1250.8 | 1521.3 / 657.3 | 3.60 / 277.8 | 0.80 / 1250.0 | 0.003% | 25.91 | 31.21 | 1081.7 | 22.63 |
| 16 | `kiwi_tile_xor_tight` | **110.7 / 1.81** | 1577.2 / 634.0 | 760.1 / 1315.6 | 1464.6 / 682.8 | 3.78 / 264.6 | 0.78 / 1282.1 | 0.410% | **14.98** | **20.29** | **688.3** | **22.37** |
| 16 | `kiwi_tile_xor_delete_rank` | 118.8 / 1.68 | 1585.3 / 630.8 | 764.4 / 1308.2 | 1614.9 / 619.2 | 3.85 / 259.7 | 0.83 / 1204.8 | 0.410% | 17.52 | 22.82 | 750.3 | 22.43 |
| 16 | `kiwi_sst_xor_index_free` | 144.5 / 1.38 | 1548.0 / 646.0 | **349.5 / 2861.2** | **1279.4 / 781.6** | **3.77 / 265.3** | **0.36 / 2777.8** | 0.376% | **14.76** | **20.04** | **665.4** | **22.37** |
| 16 | `kiwi_tile_xor_tight_16bit` | 124.3 / 1.61 | **1472.9 / 678.9** | 761.6 / 1313.0 | 1540.4 / 649.2 | 3.89 / 257.1 | 0.78 / 1282.1 | **0.000%** | 24.89 | 30.19 | 930.2 | 22.61 |

#### Suite 1 Exact Percentage Deltas (`kiwi_sst_xor_index_free` & `kiwi_tile_xor_tight`)

Cells show **`QPS delta (latency delta)`** for throughput metrics and **size delta** for space metrics:

| `h` | Candidate vs Baseline | Flush QPS (Lat) | Mem Pres QPS (Lat) | Mem Abs QPS (Lat) | Mem Mix QPS (Lat) | IO Pres QPS (Lat) | IO Abs QPS (Lat) | `flt_bpk` | `dir_bpk` | `TableReader` RAM | SST Size |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 4 | `kiwi_sst_xor_index_free` vs `plain_bloom10` | `-18.3%` (`+22.4%`) | `-3.3%` (`+3.4%`) | **+22.4% (-18.3%)** | **+7.7% (-7.1%)** | **+8.7% (-8.0%)** | **+22.2% (-18.2%)** | `+21.1%` | N/A | `+63.4%` | `+0.54%` |
| 4 | `kiwi_sst_xor_index_free` vs `kiwi_page_bloom10` | `-22.1%` (`+28.4%`) | **+7.5% (-7.0%)** | **+152.5% (-60.4%)** | **+30.1% (-23.1%)** | `-0.7%` (`+0.8%`) | **+172.2% (-63.3%)** | **-25.4%** | **-20.1%** | **-50.0%** | **-0.45%** |
| 4 | `kiwi_tile_xor_tight` vs `plain_bloom10` | `-3.1%` (`+3.2%`) | `-6.8%` (`+7.3%`) | `-48.1%` (`+92.6%`) | `-11.6%` (`+13.1%`) | **+9.0% (-8.3%)** | `-47.6%` (`+90.9%`) | `+28.1%` | N/A | `+85.1%` | `+0.63%` |
| 4 | `kiwi_tile_xor_tight` vs `kiwi_page_bloom10` | `-7.6%` (`+8.3%`) | **+3.6% (-3.5%)** | **+7.1% (-6.7%)** | **+6.8% (-6.3%)** | `-0.5%` (`+0.5%`) | **+16.7% (-14.3%)** | **-21.0%** | **-16.3%** | **-43.3%** | **-0.36%** |
| 8 | `kiwi_sst_xor_index_free` vs `plain_bloom10` | `-12.0%` (`+13.7%`) | `-0.4%` (`+0.4%`) | **+31.2% (-23.8%)** | **+5.5% (-5.2%)** | **+14.4% (-12.6%)** | **+22.2% (-18.2%)** | `+33.2%` | N/A | `+62.8%` | `+0.86%` |
| 8 | `kiwi_sst_xor_index_free` vs `kiwi_page_bloom10` | `-19.5%` (`+24.2%`) | **+12.3% (-11.0%)** | **+184.4% (-64.8%)** | **+27.1% (-21.3%)** | **+3.9% (-3.8%)** | **+175.0% (-63.6%)** | **-18.2%** | **-14.7%** | **-49.0%** | **-0.31%** |
| 8 | `kiwi_tile_xor_tight` vs `plain_bloom10` | `-1.6%` (`+1.6%`) | **+0.8% (-0.8%)** | `-44.7%` (`+80.9%`) | `-7.6%` (`+8.2%`) | **+15.6% (-13.5%)** | `-47.0%` (`+88.6%`) | `+37.4%` | N/A | `+74.1%` | `+0.90%` |
| 8 | `kiwi_tile_xor_tight` vs `kiwi_page_bloom10` | `-9.9%` (`+11.0%`) | **+13.7% (-12.0%)** | **+19.9% (-16.6%)** | **+11.3% (-10.1%)** | **+5.0% (-4.8%)** | **+19.3% (-16.2%)** | **-15.6%** | **-12.5%** | **-45.4%** | **-0.27%** |
| 16 | `kiwi_sst_xor_index_free` vs `plain_bloom10` | `-14.0%` (`+16.3%`) | `-1.5%` (`+1.6%`) | **+27.2% (-21.4%)** | **+11.1% (-10.0%)** | **+15.6% (-13.5%)** | **+22.2% (-18.2%)** | `+45.3%` | N/A | `+65.2%` | `+1.18%` |
| 16 | `kiwi_sst_xor_index_free` vs `kiwi_page_bloom10` | `-19.4%` (`+24.0%`) | **+18.6% (-15.7%)** | **+216.7% (-68.4%)** | **+41.5% (-29.3%)** | **+11.7% (-10.5%)** | **+211.1% (-67.9%)** | **-10.9%** | **-9.3%** | **-47.5%** | **-0.22%** |
| 16 | `kiwi_tile_xor_tight` vs `plain_bloom10` | **+12.2% (-10.9%)** | `-3.4%` (`+3.5%`) | `-41.5%` (`+71.0%`) | `-3.0%` (`+3.1%`) | **+15.3% (-13.3%)** | `-43.6%` (`+77.3%`) | `+47.4%` | N/A | `+70.9%` | `+1.18%` |
| 16 | `kiwi_tile_xor_tight` vs `kiwi_page_bloom10` | **+5.2% (-5.0%)** | **+16.4% (-14.1%)** | **+45.6% (-31.3%)** | **+23.6% (-19.1%)** | **+11.4% (-10.2%)** | **+43.6% (-30.4%)** | **-9.6%** | **-8.1%** | **-45.6%** | **-0.22%** |

Additional Suite 1 highlights:
- **Improvements over `kiwi_tile_xor_baseline`**: `kiwi_tile_xor_tight` cuts `flt_bpk` by **`-14.8% / -7.4% / -3.9%`**, `dir_bpk` by **`-11.0% / -5.4% / -2.9%`**, and `TableReader` RAM by **`-19.1% / -17.8% / -17.1%`** across `h = 4 / 8 / 16` while improving `h = 8` present mem QPS by **`+8.7%`** (`-8.0%` latency). Likewise, `kiwi_tile_xor_tight_16bit` cuts `flt_bpk` by **`-15.0% / -7.4% / -3.9%`** and `TableReader` RAM by **`-18.1% / -15.4% / -14.0%`** vs `kiwi_tile_xor_baseline_16bit`, while delivering the fastest `h = 16` warm-cache present Get at **`678.9 kops/s`** (`1472.9 ns`, **`+3.5%` QPS** vs `plain_bloom10`, **`+24.7%` QPS** vs `kiwi_page_bloom10`).
- **Why I/O present Get beats `plain_bloom10` across all `h`**: KiWi's tile-granularity index block is `4x--16x` smaller than `plain_bloom10`'s block-level index, reducing uncached index-read overhead so `kiwi_tile_xor_tight` and `kiwi_sst_xor_index_free` deliver **`+8.7%` to `+15.6%` higher I/O present QPS** (`3.77--4.01 us` vs `4.36 us`) than `plain_bloom10`.

---

### Suite 2 & Suite 2B: Secondary Delete, Zero-Peel Drop, Edge-Page Rank Pruning, & Multi-SST Unlink (`h = 8`)

#### Suite 2 Single-SST Uniform-`D` Secondary Delete & Reclamation Table (`200k` Keys, `100k` Queries)

In Suite 2, delete keys `D in [1, 200000]` are uniformly distributed across key order in one `22.3 MB` SST (`~248` keys/tile across `8` pages/tile), so at `10%` drop every tile's oldest page (`page 0`) straddles `floor`.

| Drop | Config | Lazy Del (`ms`) | Zero-Peel (`ms` / `MB` / `spAmp`) | Eager Recon (`ms`) | Full Compact (`ms`) | Lazy `spAmp` | `delQ` Mem (`ns` / `kops/s`) | `delQ` IO (`us` / `kops/s` / `io_blk`) | `survQ` Mem (`ns` / `kops/s`) | `survQ` IO (`us` / `kops/s`) | `TableReader` KB (`B -> C`) | SST MB (`B -> C`) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 10% | `plain_bloom10` (Del+Compact) | 248.27 | -- | -- | -- | 1.00x | 395.1 / 2531.0 | 0.44 / 2272.7 / 0.01 | 1544.7 / 647.4 | 3.74 / 267.4 | 402.7 -> 362.7 | 22.11 -> 19.91 |
| 10% | `kiwi_no_filter` | 0.88 | 53.94 / 21.76 / 1.10x | -- | 147.76 | 1.11x | 5921.2 / 168.9 | 21.89 / 45.7 / 7.92 | 6414.0 / 155.9 | 22.11 / 45.2 | 881.4 -> 791.9 | 21.98 -> 19.79 |
| 10% | `kiwi_page_bloom10` | 0.95 | 52.20 / 22.15 / 1.10x | -- | 131.08 | 1.11x | 1734.8 / 576.4 | 3.76 / 266.0 / 0.91 | 1833.7 / 545.3 | 4.14 / 241.5 | 1285.2 -> 1155.4 | 22.37 -> 20.14 |
| 10% | `kiwi_tile_xor_baseline` `[tf]` | 1.05 | -- | 31.58 | 145.88 | 1.11x | 1617.3 / 618.3 | 3.48 / 287.4 / 0.90 | 1682.8 / 594.2 | 3.77 / 265.3 | 852.9 -> 766.3 | 22.33 -> 20.11 |
| 10% | `kiwi_tile_xor_baseline_16bit` `[tf]` | 0.82 | -- | 32.62 | 144.39 | 1.11x | 1612.8 / 620.0 | 3.49 / 286.5 / 0.90 | 1689.2 / 592.0 | 3.70 / 270.3 | 1116.3 -> 1003.3 | 22.59 -> 20.34 |
| 10% | `kiwi_tile_xor_tight` | 0.90 | 50.01 / 22.09 / 1.10x | 31.42 | 143.22 | 1.11x | 1553.4 / 643.7 | 3.55 / 281.7 / 0.90 | **1657.6 / 603.3** | **3.91 / 255.8** | 701.3 -> 629.9 | 22.31 -> 20.08 |
| 10% | `kiwi_tile_xor_delete_rank` | **0.87** | 52.00 / 22.15 / 1.10x | 32.16 | 138.39 | 1.11x | **984.7 / 1015.5** | **1.33 / 751.9 / 0.15** | 1736.7 / 575.8 | 3.92 / 255.1 | 765.7 -> 687.8 | 22.37 -> 20.14 |
| 10% | `kiwi_sst_xor_index_free` | 1.39 | -- | -- | 164.86 | 1.11x | 1554.3 / 643.4 | 3.57 / 280.1 / 0.90 | 1804.3 / 554.2 | 3.98 / 251.3 | **655.6 -> 588.7** | **22.30 -> 20.07** |
| 10% | `kiwi_tile_xor_tight_16bit` | 0.91 | **48.77** / 22.33 / 1.10x | 30.78 | 140.49 | 1.11x | 1556.3 / 642.5 | 3.50 / 285.7 / 0.90 | 1658.7 / 602.9 | 3.93 / 254.5 | 944.9 -> 849.1 | 22.55 -> 20.30 |
| 25% | `kiwi_tile_xor_tight` | **0.73** | **41.00 / 18.22 / 1.09x** | 25.48 | 117.15 | 1.33x | 1078.3 / 927.4 | 1.66 / 602.4 / 0.26 | **1609.2 / 621.4** | 3.83 / 261.1 | **701.3 -> 524.6** | **22.31 -> 16.75** |
| 25% | `kiwi_tile_xor_delete_rank` | 1.30 | 44.48 / 18.28 / 1.09x | 28.14 | 116.86 | 1.33x | **920.0 / 1087.0** | **1.06 / 943.4 / 0.06** | 1620.8 / 617.0 | **3.78 / 264.6** | 765.7 -> 572.9 | 22.37 -> 16.79 |
| 50% | `plain_bloom10` (Del+Compact) | 293.98 | -- | -- | -- | 1.00x | 400.1 / 2499.4 | 0.44 / 2272.7 / 0.01 | 1517.2 / 659.1 | 3.76 / 266.0 | 402.7 -> 202.7 | 22.11 -> 11.06 |
| 50% | `kiwi_page_bloom10` | 0.89 | 33.17 / 12.77 / 1.14x | -- | 78.90 | 2.00x | 1208.4 / 827.5 | 1.50 / 666.7 / 0.14 | 1877.7 / 532.6 | 4.19 / 238.7 | 1285.2 -> 643.8 | 22.37 -> 11.21 |
| 50% | `kiwi_tile_xor_baseline` `[tf]` | 0.79 | -- | 19.49 | 86.63 | 2.00x | 1016.1 / 984.2 | 1.34 / 746.3 / 0.14 | 1681.8 / 594.6 | 3.61 / 277.0 | 852.9 -> 427.6 | 22.33 -> 11.19 |
| 50% | `kiwi_tile_xor_baseline_16bit` `[tf]` | 1.01 | -- | 18.66 | 87.57 | 2.00x | 1016.2 / 984.1 | 1.33 / 751.9 / 0.14 | 1839.3 / 543.7 | 3.69 / 271.0 | 1116.3 -> 559.3 | 22.59 -> 11.32 |
| 50% | `kiwi_tile_xor_tight` | **0.71** | **30.10 / 12.88 / 1.15x** | 18.38 | 86.88 | 2.00x | 994.5 / 1005.5 | 1.31 / 763.4 / 0.14 | **1584.6 / 631.1** | 3.91 / 255.8 | **701.3 -> 352.1** | **22.31 -> 11.18** |
| 50% | `kiwi_tile_xor_delete_rank` | 0.78 | 35.10 / 12.94 / 1.15x | 18.45 | 84.65 | 2.00x | **898.0 / 1113.6** | **0.97 / 1030.9 / 0.03** | 1686.5 / 592.9 | **3.86 / 259.1** | 765.7 -> 384.3 | 22.37 -> 11.21 |
| 75% | `plain_bloom10` (Del+Compact) | 339.48 | -- | -- | -- | 1.00x | 400.1 / 2499.4 | 0.42 / 2381.0 / 0.01 | 1448.8 / 690.2 | 3.63 / 275.5 | 402.7 -> 106.7 | 22.11 -> 5.54 |
| 75% | `kiwi_page_bloom10` `[tf]` | 1.80 | -- | -- | 46.93 | 3.98x | 1146.8 / 872.0 | 1.39 / 719.4 / 0.10 | 1770.7 / 564.7 | 3.87 / 258.4 | 1273.5 -> 320.2 | 22.37 -> 5.62 |
| 75% | `kiwi_tile_xor_baseline` `[tf]` | 0.87 | -- | 10.44 | 50.02 | 3.98x | 976.8 / 1023.8 | 1.19 / 840.3 / 0.09 | 1599.7 / 625.1 | 3.60 / 277.8 | 852.9 -> 215.0 | 22.33 -> 5.61 |
| 75% | `kiwi_tile_xor_tight` | 0.88 | **19.58 / 7.40 / 1.32x** | 10.26 | 49.45 | 3.98x | 972.0 / 1028.8 | 1.16 / 862.1 / 0.09 | 1579.7 / 633.0 | 3.77 / 265.3 | **701.3 -> 177.5** | **22.31 -> 5.60** |
| 75% | `kiwi_tile_xor_delete_rank` | **0.80** | 20.89 / 7.46 / 1.33x | 10.32 | 49.35 | 3.98x | **882.7 / 1132.9** | **0.93 / 1075.3 / 0.02** | **1546.3 / 646.7** | **3.77 / 265.3** | 765.7 -> 193.5 | 22.37 -> 5.62 |

#### Suite 2 Exact Percentage Deltas

| Drop | Comparison | Delete Time | Zero-Peel vs Baseline Reclaim | `delQ` Mem QPS (Lat) | `delQ` IO QPS (Lat / `io_blk`) | `survQ` Mem QPS (Lat) | `survQ` IO QPS (Lat) | Compacted `TableReader` KB |
|---|---|---|---|---|---|---|---|---|
| 10% | `kiwi_tile_xor_tight` vs `plain_bloom10` | **-99.6%** (`275.9x`) | **-79.9%** (`50.01` vs `248.27 ms`, `4.96x`) | `-74.6%` (`+293.2%`) | `-87.6%` (`+706.8%` / `+8900%`) | `-6.8%` (`+7.3%`) | `-4.3%` (`+4.5%`) | `+73.7%` |
| 10% | `kiwi_tile_xor_tight` vs `kiwi_page_bloom10` | **-5.3%** (`1.06x`) | **-4.2%** (`50.01` vs `52.20 ms`) | **+11.7% (-10.5%)** | **+5.9% (-5.6% / -1.1%)** | **+10.6% (-9.6%)** | **+5.9% (-5.6%)** | **-45.5%** |
| 10% | `kiwi_tile_xor_delete_rank` vs `plain_bloom10` | **-99.6%** (`285.4x`) | **-79.1%** (`52.00` vs `248.27 ms`, `4.77x`) | `-59.9%` (`+149.2%`) | `-66.9%` (`+202.3%` / `+1400%`) | `-11.1%` (`+12.4%`) | `-4.6%` (`+4.8%`) | `+89.6%` |
| 10% | `kiwi_tile_xor_delete_rank` vs `kiwi_page_bloom10` | **-8.4%** (`1.09x`) | **-0.4%** (`52.00` vs `52.20 ms`) | **+76.2% (-43.2%)** | **+182.7% (-64.6% / -83.5%)** | **+5.6% (-5.3%)** | **+5.6% (-5.3%)** | **-40.5%** |
| 10% | `kiwi_tile_xor_delete_rank` vs `kiwi_tile_xor_tight` | **-3.3%** (`1.03x`) | `+4.0%` (`52.00` vs `50.01 ms`) | **+57.8% (-36.6%)** | **+166.9% (-62.5% / -83.3%)** | `-4.6%` (`+4.8%`) | `-0.3%` (`+0.3%`) | `+9.2%` |
| 25% | `kiwi_tile_xor_delete_rank` vs `kiwi_tile_xor_tight` | `+78.1%` (`1.30` vs `0.73 ms`) | `+8.5%` (`44.48` vs `41.00 ms`) | **+17.2% (-14.7%)** | **+56.6% (-36.1% / -76.9%)** | `-0.7%` (`+0.7%`) | **+1.3% (-1.3%)** | `+9.2%` |
| 50% | `kiwi_tile_xor_tight` vs `plain_bloom10` | **-99.8%** (`414.1x`) | **-89.8%** (`30.10` vs `293.98 ms`, `9.77x`) | `-59.8%` (`+148.6%`) | `-66.4%` (`+197.7%` / `+1300%`) | `-4.3%` (`+4.4%`) | `-3.8%` (`+4.0%`) | `+73.7%` |
| 50% | `kiwi_tile_xor_tight` vs `kiwi_page_bloom10` | **-20.2%** (`1.25x`) | **-9.3%** (`30.10` vs `33.17 ms`) | **+21.5% (-17.7%)** | **+14.5% (-12.7% / 0.0%)** | **+18.5% (-15.6%)** | **+7.2% (-6.7%)** | **-45.3%** |
| 50% | `kiwi_tile_xor_delete_rank` vs `plain_bloom10` | **-99.7%** (`376.9x`) | **-88.1%** (`35.10` vs `293.98 ms`, `8.38x`) | `-55.4%` (`+124.4%`) | `-54.6%` (`+120.5%` / `+200%`) | `-10.0%` (`+11.2%`) | `-2.6%` (`+2.7%`) | `+89.6%` |
| 50% | `kiwi_tile_xor_delete_rank` vs `kiwi_page_bloom10` | **-12.4%** (`1.14x`) | `+5.8%` (`35.10` vs `33.17 ms`) | **+34.6% (-25.7%)** | **+54.6% (-35.3% / -78.6%)** | **+11.3% (-10.2%)** | **+8.5% (-7.9%)** | **-40.3%** |
| 50% | `kiwi_tile_xor_delete_rank` vs `kiwi_tile_xor_tight` | `+9.9%` (`0.78` vs `0.71 ms`) | `+16.6%` (`35.10` vs `30.10 ms`) | **+10.7% (-9.7%)** | **+35.1% (-26.0% / -78.6%)** | `-6.0%` (`+6.4%`) | **+1.3% (-1.3%)** | `+9.1%` |
| 75% | `kiwi_tile_xor_tight` vs `plain_bloom10` | **-99.7%** (`385.8x`) | **-94.2%** (`19.58` vs `339.48 ms`, `17.34x`) | `-58.8%` (`+142.9%`) | `-63.8%` (`+176.2%` / `+800%`) | `-8.3%` (`+9.0%`) | `-3.7%` (`+3.9%`) | `+66.4%` |
| 75% | `kiwi_tile_xor_tight` vs `kiwi_page_bloom10` `[tf]` | **-51.1%** (`2.05x`) | -- | **+18.0% (-15.2%)** | **+19.8% (-16.5% / -10.0%)** | **+12.1% (-10.8%)** | **+2.7% (-2.6%)** | **-44.6%** |
| 75% | `kiwi_tile_xor_delete_rank` vs `plain_bloom10` | **-99.8%** (`424.4x`) | **-93.8%** (`20.89` vs `339.48 ms`, `16.25x`) | `-54.7%` (`+120.6%`) | `-54.8%` (`+121.4%` / `+100%`) | `-6.3%` (`+6.7%`) | `-3.7%` (`+3.9%`) | `+81.3%` |
| 75% | `kiwi_tile_xor_delete_rank` vs `kiwi_page_bloom10` `[tf]` | **-55.6%** (`2.25x`) | -- | **+29.9% (-23.0%)** | **+49.5% (-33.1% / -80.0%)** | **+14.5% (-12.7%)** | **+2.7% (-2.6%)** | **-39.6%** |
| 75% | `kiwi_tile_xor_delete_rank` vs `kiwi_tile_xor_tight` | **-9.1%** (`1.10x`) | `+6.7%` (`20.89` vs `19.58 ms`) | **+10.1% (-9.2%)** | **+24.7% (-19.8% / -77.8%)** | **+2.2% (-2.1%)** | `0.0%` (`0.0%`) | `+9.0%` |

Key Suite 2 takeaways:
- **Zero-peel physical reclaim vs `CompactRange`**: For `kiwi_tile_xor_tight`, `RewriteKiWiTableDroppingPages` reduces physical reclaim wall time by **`-65.1%` (`143.22 -> 50.01 ms`, `2.86x`)** at 10% drop, **`-65.0%` (`117.15 -> 41.00 ms`, `2.86x`)** at 25% drop, **`-65.4%` (`86.88 -> 30.10 ms`, `2.89x`)** at 50% drop, and **`-60.4%` (`49.45 -> 19.58 ms`, `2.53x`)** at 75% drop, while avoiding eager filter re-peeling (`10.26--31.42 ms` just to rebuild directory filters without shrinking the SST).
- **Straddling edge-page pruning (`kiwi_tile_xor_delete_rank`)**: Across `10% -> 25% -> 50% -> 75%` drops, `kiwi_tile_xor_delete_rank` slashes `delQ` disk block reads by **`-83.3% / -76.9% / -78.6% / -77.8%`** relative to `kiwi_tile_xor_tight` (`0.90 -> 0.15`, `0.26 -> 0.06`, `0.14 -> 0.03`, `0.09 -> 0.02 io_blk`), boosting `delQ` I/O throughput by **`+166.9% / +56.6% / +35.1% / +24.7%`** (`751.9 -> 1075.3 kops/s`) at the cost of only `2` extra bits/slot (`+9.0%` to `+9.2%` `TableReader` RAM).

#### Suite 2B: Multi-SST Whole-File Secondary Delete (8 x 25k SSTs = 200k Keys, `h = 8`)

| Drop Older SSTs | Config | Delete Time (`ms`) | vs `plain_bloom10` Time | vs `kiwi_page_bloom10` Time | Write Amp (`w_amp`) | Live Files (`B -> A`) | `TableReader` RAM (`B -> A` KB) | vs `plain_bloom10` / `kiwi_page_bloom10` RAM (After) | SST Disk (`B -> A` MB) |
|---|---|---|---|---|---|---|---|---|---|
| 50% (4/8 SSTs) | `plain_bloom10` (Del+Compact) | 307.43 | `0.0%` (`1.00x`) | `+8011.6%` | 16.44 | `8 -> 1` | `437.3 -> 202.7` | `0.0%` / `-68.8%` | `22.22 -> 11.06` |
| 50% (4/8 SSTs) | `kiwi_page_bloom10` | 3.79 | `-98.8%` (`81.12x`) | `0.0%` | **0.00** | `8 -> 4` | `1298.1 -> 649.1` | `+220.2%` / `0.0%` | `22.52 -> 11.26` |
| 50% (4/8 SSTs) | `kiwi_tile_xor_tight` | **2.88** | **-99.1% (`106.75x`)** | **-24.0% (`1.32x`)** | **0.00** | `8 -> 4` | `717.0 -> 358.5` | `+76.9%` / **-44.8%** | `22.46 -> 11.23` |
| 50% (4/8 SSTs) | `kiwi_sst_xor_index_free` | **3.01** | **-99.0% (`102.14x`)** | **-20.6% (`1.26x`)** | **0.00** | `8 -> 4` | **671.2 -> 335.6** | `+65.6%` / **-48.3%** | **22.45 -> 11.22** |
| 75% (6/8 SSTs) | `plain_bloom10` (Del+Compact) | 332.78 | `0.0%` (`1.00x`) | `+6719.3%` | 6.13 | `8 -> 1` | `437.3 -> 106.7` | `0.0%` / `-67.1%` | `22.22 -> 5.54` |
| 75% (6/8 SSTs) | `kiwi_page_bloom10` | **4.88** | `-98.5%` (`68.19x`) | `0.0%` | **0.00** | `8 -> 2` | `1298.1 -> 324.5` | `+204.1%` / `0.0%` | `22.52 -> 5.63` |
| 75% (6/8 SSTs) | `kiwi_tile_xor_tight` | 5.31 | **-98.4% (`62.67x`)** | `+8.8%` (`0.92x`) | **0.00** | `8 -> 2` | `717.0 -> 179.2` | `+67.9%` / **-44.8%** | **22.46 -> 5.61** |
| 75% (6/8 SSTs) | `kiwi_sst_xor_index_free` | 6.15 | **-98.2% (`54.11x`)** | `+26.0%` (`0.79x`) | **0.00** | `8 -> 2` | **671.2 -> 167.8** | `+57.3%` / **-48.3%** | **22.45 -> 5.61** |

Sub-millisecond differences among KiWi configs in Suite 2B (`2.88--6.15 ms`) are dominated by `Flush` + MANIFEST `Sync` + `POSIX unlink`; all KiWi XOR variants achieve **`54x--107x` speedup** and **`-100.0%` write amplification** vs `plain_bloom10`, while cutting surviving `TableReader` RAM by **`-44.8%` to `-48.3%`** vs `kiwi_page_bloom10`.

---

### Suite 3: Delete-Heavy LSM Workloads (`seqdel` & `churn`, `h = 8`, `1 MB` L1)

Preloads `200,000` keys into a leveled LSM (`1 MB` write buffer, `1 MB` target file size, `1 MB` L1 base, multiplier `5`), executes `200,000` mixed write/delete operations (`seqdel`: 30% sequential point deletes + 70% puts; `churn`: 90% delete/reinsert toggling over 20k hot keys + 10% puts), verifies exact iterator state against a ground-truth array, and runs `100,000` present and `100,000` deleted-key `Get()` queries.

#### Full Ingestion Throughput, Write Amp, Post-Workload Query Throughput, & Space Table

| Workload | Config | Write `kops/s` (`us/op`) | `w_amp` | Mem Pres (`ns` / `kops/s`) | Mem Abs (`ns` / `kops/s`) | IO Pres (`us` / `kops/s`) | IO Abs (`us` / `kops/s`) | `io_blk` / Abs | Filter KB (`B -> A`) | `TableReader` KB (`B -> A`) | SST MB (`B -> A`) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `seqdel` | `plain_bloom10` | 520.0 (1.92) | 5.65 | 1562.4 / 640.0 | 613.1 / 1631.1 | 3.73 / 268.1 | 0.88 / 1136.4 | 0.12 | 168 -> 193 | 332 -> 371 | 14.9 -> 16.2 |
| `seqdel` | `kiwi_no_filter` | **580.9 (1.72)** | **4.61** | 7376.5 / 135.6 | 6353.3 / 157.4 | 29.87 / 33.5 | 27.18 / 36.8 | 10.28 | 0 -> 0 | 633 -> 709 | 14.8 -> 16.5 |
| `seqdel` | `kiwi_page_bloom10` | 551.4 (1.81) | 5.27 | 2066.1 / 484.0 | 1082.5 / 923.8 | 4.29 / 233.1 | 1.14 / 877.2 | 0.01 | 272 -> 295 | 906 -> 985 | 15.1 -> 16.3 |
| `seqdel` | `kiwi_tile_xor_baseline` `[tf]` | 509.5 (1.96) | 5.35 | 1757.2 / 569.1 | 906.4 / 1103.3 | 3.87 / 258.4 | 0.95 / 1052.6 | 0.00 | 254 -> 269 | 623 -> 666 | 15.4 -> 16.4 |
| `seqdel` | `kiwi_tile_xor_baseline_16bit` `[tf]` | 530.8 (1.88) | 5.59 | 1678.2 / 595.9 | 1153.5 / 866.9 | 3.79 / 263.9 | 1.51 / 662.3 | 0.17 | 442 -> 506 | 824 -> 919 | 15.8 -> 17.1 |
| `seqdel` | `kiwi_tile_xor_tight` | 515.0 (1.94) | **4.61** | 1984.7 / 503.9 | 992.5 / 1007.6 | 4.09 / 244.5 | 1.11 / 900.9 | 0.05 | 227 -> 268 | 509 -> 615 | 14.9 -> 17.3 |
| `seqdel` | `kiwi_tile_xor_delete_rank` | 520.2 (1.92) | 5.05 | 1776.5 / 562.9 | 877.2 / 1140.0 | **3.86 / 259.1** | 1.02 / 980.4 | 0.06 | 272 -> 304 | 559 -> 626 | 15.1 -> 16.5 |
| `seqdel` | `kiwi_sst_xor_index_free` | 469.6 (2.13) | 4.75 | 1993.8 / 501.6 | **469.5 / 2129.9** | 3.90 / 256.4 | **0.49 / 2040.8** | **0.01** | **229 -> 250** | **498 -> 557** | 15.5 -> 16.9 |
| `seqdel` | `kiwi_tile_xor_tight_16bit` | 518.0 (1.93) | 5.08 | **1646.0 / 607.5** | 845.5 / 1182.7 | 3.90 / 256.4 | 0.86 / 1162.8 | **0.00** | 392 -> 417 | 682 -> 730 | **15.2 -> 16.1** |
| `churn` | `plain_bloom10` | 664.7 (1.50) | 3.97 | **1642.2 / 608.9** | **672.1 / 1487.9** | **3.84 / 260.4** | **0.97 / 1030.9** | 0.14 | 173 -> 193 | 339 -> 368 | 15.5 -> 16.2 |
| `churn` | `kiwi_no_filter` | 639.1 (1.56) | 3.08 | 13110.6 / 76.3 | 12918.1 / 77.4 | 58.21 / 17.2 | 58.63 / 17.1 | 22.50 | 0 -> 0 | 651 -> 749 | 15.2 -> 17.2 |
| `churn` | `kiwi_page_bloom10` | 662.5 (1.51) | 3.25 | 2853.4 / 350.5 | 2113.6 / 473.1 | 5.12 / 195.3 | 2.48 / 403.2 | 0.16 | 281 -> 344 | 937 -> 1110 | 15.6 -> 18.1 |
| `churn` | `kiwi_tile_xor_baseline` `[tf]` | 605.4 (1.65) | 3.00 | 1930.5 / 518.0 | 1227.9 / 814.4 | 4.21 / 237.5 | 1.47 / 680.3 | 0.10 | 248 -> 290 | 616 -> 706 | 15.1 -> 16.8 |
| `churn` | `kiwi_tile_xor_baseline_16bit` `[tf]` | 565.0 (1.77) | 3.13 | 2011.1 / 497.2 | 1312.1 / 762.1 | 4.13 / 242.1 | 1.54 / 649.4 | 0.10 | 426 -> 507 | 791 -> 931 | 15.3 -> 17.2 |
| `churn` | `kiwi_tile_xor_tight` | **670.3 (1.49)** | **2.80** | 2035.0 / 491.4 | 1351.2 / 740.1 | 4.25 / 235.3 | 1.72 / 581.4 | 0.15 | 235 -> 289 | 525 -> 633 | 15.4 -> 17.7 |
| `churn` | `kiwi_tile_xor_delete_rank` | 603.0 (1.66) | 3.26 | 2315.2 / 431.9 | 1582.5 / 631.9 | 4.49 / 222.7 | 1.94 / 515.5 | 0.15 | 280 -> 339 | 570 -> 677 | 15.5 -> 17.6 |
| `churn` | `kiwi_sst_xor_index_free` | **678.0 (1.47)** | **2.49** | **1769.0 / 565.3** | **781.2 / 1280.1** | **3.95 / 253.2** | **1.13 / 885.0** | 0.15 | **218 -> 283** | **474 -> 604** | 14.7 -> 17.8 |
| `churn` | `kiwi_tile_xor_tight_16bit` | 647.2 (1.55) | 2.94 | 2123.3 / 471.0 | 1428.0 / 700.3 | 4.36 / 229.4 | 1.73 / 578.0 | **0.11** | 396 -> 466 | 689 -> 806 | **15.3 -> 17.1** |

#### Suite 3 Exact Percentage Deltas

| Workload | Candidate vs Baseline | Write `ops/s` | `w_amp` | Mem Pres QPS (Lat) | Mem Abs QPS (Lat) | IO Pres QPS (Lat) | IO Abs QPS (Lat) | Final Filter KB | Final `TableReader` KB | Final SST MB |
|---|---|---|---|---|---|---|---|---|---|---|
| `seqdel` | `kiwi_tile_xor_tight` vs `plain_bloom10` | `-0.9%` | **-18.4%** | `-21.3%` (`+27.0%`) | `-38.2%` (`+61.9%`) | `-8.8%` (`+9.7%`) | `-20.7%` (`+26.1%`) | `+38.9%` | `+65.8%` | `+6.8%` |
| `seqdel` | `kiwi_tile_xor_tight` vs `kiwi_page_bloom10` | `-6.6%` | **-12.5%** | **+4.1% (-3.9%)** | **+9.1% (-8.3%)** | **+4.9% (-4.7%)** | **+2.7% (-2.6%)** | **-9.2%** | **-37.6%** | `+6.1%` |
| `seqdel` | `kiwi_sst_xor_index_free` vs `plain_bloom10` | `-9.7%` | **-15.9%** | `-21.6%` (`+27.6%`) | **+30.6% (-23.4%)** | `-4.4%` (`+4.6%`) | **+79.6% (-44.3%)** | `+29.5%` | `+50.1%` | `+4.3%` |
| `seqdel` | `kiwi_sst_xor_index_free` vs `kiwi_page_bloom10` | `-14.8%` | **-9.9%** | **+3.6% (-3.5%)** | **+130.6% (-56.6%)** | **+10.0% (-9.1%)** | **+132.7% (-57.0%)** | **-15.3%** | **-43.5%** | `+3.7%` |
| `seqdel` | `kiwi_tile_xor_tight_16bit` vs `plain_bloom10` | `-0.4%` | **-10.1%** | `-5.1%` (`+5.4%`) | `-27.5%` (`+37.9%`) | `-4.4%` (`+4.6%`) | **+2.3% (-2.3%)** | `+116.1%` | `+96.8%` | **-0.6%** |
| `seqdel` | `kiwi_tile_xor_tight_16bit` vs `kiwi_page_bloom10` | `-6.1%` | **-3.6%** | **+25.5% (-20.3%)** | **+28.0% (-21.9%)** | **+10.0% (-9.1%)** | **+32.6% (-24.6%)** | `+41.4%` | **-25.9%** | **-1.2%** |
| `churn` | `kiwi_tile_xor_tight` vs `plain_bloom10` | **+0.8%** | **-29.5%** | `-19.3%` (`+23.9%`) | `-50.3%` (`+101.0%`) | `-9.6%` (`+10.7%`) | `-43.6%` (`+77.3%`) | `+49.7%` | `+72.0%` | `+9.3%` |
| `churn` | `kiwi_tile_xor_tight` vs `kiwi_page_bloom10` | **+1.2%** | **-13.8%** | **+40.2% (-28.7%)** | **+56.4% (-36.1%)** | **+20.5% (-17.0%)** | **+44.2% (-30.6%)** | **-16.0%** | **-43.0%** | **-2.2%** |
| `churn` | `kiwi_sst_xor_index_free` vs `plain_bloom10` | **+2.0%** | **-37.3%** | `-7.2%` (`+7.7%`) | `-14.0%` (`+16.2%`) | `-2.8%` (`+2.9%`) | `-14.2%` (`+16.5%`) | `+46.6%` | `+64.1%` | `+9.9%` |
| `churn` | `kiwi_sst_xor_index_free` vs `kiwi_page_bloom10` | **+2.3%** | **-23.4%** | **+61.3% (-38.0%)** | **+170.6% (-63.0%)** | **+29.6% (-22.9%)** | **+119.5% (-54.4%)** | **-17.7%** | **-45.6%** | **-1.7%** |
| `churn` | `kiwi_tile_xor_tight_16bit` vs `plain_bloom10` | `-2.6%` | **-25.9%** | `-22.7%` (`+29.3%`) | `-52.9%` (`+112.5%`) | `-11.9%` (`+13.5%`) | `-43.9%` (`+78.4%`) | `+141.5%` | `+119.0%` | `+5.6%` |
| `churn` | `kiwi_tile_xor_tight_16bit` vs `kiwi_page_bloom10` | `-2.3%` | **-9.5%** | **+34.4% (-25.6%)** | **+48.0% (-32.4%)** | **+17.4% (-14.8%)** | **+43.4% (-30.2%)** | `+35.5%` | **-27.4%** | **-5.5%** |

Additional Suite 3 improvements over `kiwi_tile_xor_baseline`:
- On `churn`, `kiwi_sst_xor_index_free` boosts write throughput by **`+12.0%`** (`605.4k -> 678.0k ops/s`), cuts `w_amp` by **`-17.0%`** (`3.00 -> 2.49`), boosts absent mem QPS by **`+57.2%`** (`-36.4%` latency) and absent I/O QPS by **`+30.1%`** (`-23.1%` latency), and cuts post-workload `TableReader` RAM by **`-14.4%`** (`706 -> 604 KB`). `kiwi_tile_xor_tight` boosts `churn` write throughput by **`+10.7%`** (`605.4k -> 670.3k ops/s`), cuts `w_amp` by **`-6.7%`** (`3.00 -> 2.80`), and cuts `TableReader` RAM by **`-10.3%`** (`706 -> 633 KB`).
- On `seqdel`, `kiwi_sst_xor_index_free` outperforms **both** `plain_bloom10` and `kiwi_page_bloom10` on post-workload absent queries: **`+30.6%` mem QPS** (`469.5` vs `613.1 ns`) and **`+79.6%` I/O QPS** (`0.49` vs `0.88 us`, `0.01` vs `0.12 io_blk`) vs `plain_bloom10`, and **`+130.6%` mem QPS** and **`+132.7%` I/O QPS** vs `kiwi_page_bloom10`.

---

## 3. Honest Trade-Offs: Where `plain_bloom10` or `kiwi_page_bloom10` Still Leads

1. **Pinned `TableReader` RAM and Filter `bpk` When Secondary Deletes Are Not Needed (`plain_bloom10` leads)**:
   Because `plain_bloom10` stores no KiWi directory (`0.00 dir_bpk`, no per-page handles or `[min_d, max_d]` fences) and no `page_index` routing bits in its filter (`10.16 flt_bpk`), it pins `402.7 KB` in Suite 1 (`368--371 KB` after Suite 3). Even the leanest KiWi XOR design (`kiwi_sst_xor_index_free`) uses **`+21.1%` to `+45.3%` higher `flt_bpk`** and **`+50.1%` to `+65.2%` more `TableReader` RAM** (`557--665.4 KB`), and `kiwi_tile_xor_tight` uses **`+65.8%` to `+85.1%` more `TableReader` RAM** (`615--745.4 KB`). Do not enable KiWi on column families that never execute `SecondaryDeleteBelow`.
2. **Write Throughput on Small-SST `seqdel` & Single-SST Flush CPU (`kiwi_page_bloom10` leads)**:
   Building a 3-way XOR filter constructs and peels a 3-uniform hypergraph (`O(n)` degree arrays + queue passes) rather than setting bits in local 64 B cache lines. On `seqdel` (`1 MB` SSTs), even though `kiwi_tile_xor_tight` reduces `w_amp` by `-12.5%` (`4.61` vs `5.27`), its write throughput trails `kiwi_page_bloom10` by **`-6.6%`** (`515,021` vs `551,436 ops/s`, i.e., `kiwi_page_bloom10` achieves `+7.1%` higher `ops/s` / `kiwi_tile_xor_tight` is `6.6%` slower), `kiwi_tile_xor_delete_rank` trails by **`-5.7%`** (`520,240 ops/s`), and `kiwi_sst_xor_index_free` trails by **`-14.8%`** (`469,605 ops/s`). Likewise, on Suite 1's `200k`-key flush, peeling one SST-wide hypergraph in `kiwi_sst_xor_index_free` increases flush time by **`+24.0%` to `+28.4%`** vs `kiwi_page_bloom10` (`+13.7%` to `+22.4%` vs `plain_bloom10`).
3. **Per-Tile v2 XOR Absent-Key Lookups vs Whole-SST `plain_bloom10` (`plain_bloom10` leads per-tile v2)**:
   Per-tile v2 XOR configurations (`kiwi_tile_xor_tight`, `kiwi_tile_xor_delete_rank`, `kiwi_tile_xor_tight_16bit`) must binary-search (and in uncached mode read) the SST index block before probing the tile's XOR filter, resulting in **`-41.5%` to `-48.1%` lower absent mem QPS** (`760.1--855.9 ns` vs `444.5 ns`) and **`-43.6%` to `-48.2%` lower absent I/O QPS** (`0.78--0.85 us` vs `0.44 us`) than `plain_bloom10` in Suite 1. `kiwi_sst_xor_index_free` eliminates this penalty (beating `plain_bloom10` in Suite 1 and `seqdel`), though on multi-level `churn` where point tombstones reside in upper levels (`0.14--0.15 io_blk`), `plain_bloom10` still edges out `kiwi_sst_xor_index_free` on point reads by **`+7.2%` to `+14.2%` QPS** (`1642.2` vs `1769.0 ns` present; `672.1` vs `781.2 ns` absent).
4. **Post-Secondary-Delete `delQ` Latency Prior to Compaction (`plain_bloom10` leads after full rewrite)**:
   `plain_bloom10` has no lazy secondary delete: after paying **`248.27--339.48 ms`** to issue point tombstones and force-compact the DB, deleted keys are physically absent from its new Bloom filters (`395.1--400.1 ns` mem, `0.42--0.44 us` I/O, `0.01 io_blk`). Lazy KiWi `SecondaryDeleteBelow` finishes **`276x--424x` faster** (`0.71--0.90 ms`), but until compaction rewrites straddling edge pages, queries for deleted keys still hit the pre-delete XOR slots (`kiwi_tile_xor_delete_rank` narrows the gap to `0.02--0.15 io_blk` and `0.93--1.33 us`, which is still `+120.5%` to `+202.3%` higher I/O latency than post-compaction `plain_bloom10`).
5. **Multi-Version Intra-Tile Keys & Zero-Peel Scope (`kiwi_page_bloom10` compatibility edge)**:
   Because 3-way XOR filters map each distinct `user_key` hash to one slot value, v2 per-tile XOR assumes a user key does not span multiple pages within the same tile (and v3 `kiwi_sst_xor_index_free` automatically falls back to v2 if a multi-version user key crosses a tile boundary). Additionally, `RewriteKiWiTableDroppingPages` supports v1 (`kiwi_page_bloom10`) and v2 (`kiwi_tile_xor_*`), but not v3 (`kiwi_sst_xor_index_free`). Finally, 8-bit XOR has a `~0.38%` (`2^-8`) raw FPR vs `kiwi_page_bloom10`'s over-provisioned `0.07%` (`16.5 bpk`); use `kiwi_tile_xor_tight_16bit` (`<= 0.001%` FPR) when sub-0.1% FPR is required.

---

## 4. Practical Selection Guide

| Workload Profile & Primary Objective | Recommended Configuration | Policy / Option Setup | Expected Payoff |
|---|---|---|---|
| **Read-heavy, negative-lookup-heavy, mixed Get/Put, or high-churn LSM** with minimal RAM & `w_amp` (and whole-SST or standard compaction reclaim) | **`kiwi_sst_xor_index_free`** | `kiwi_tile_pages = 8` (or `4..16`), `FilterPolicy::CreateFromString(cfg, "{id=xorfilter:1;kiwi_file_filter=true}", &p)` | **`2.75--2.95 Mops/s`** absent Get (**`+153%` to `+217%`** vs `kiwi_page_bloom10`, **`+22%` to `+31%`** vs `plain_bloom10`), **`-47.5%` to `-50.0%`** `TableReader` RAM, **`w_amp = 2.49`** on `churn` (**`-37.3%`** vs `plain_bloom10`) |
| **Balanced read/write KiWi deployment** needing on-demand **zero-peel physical page drops** (`RewriteKiWiTableDroppingPages`) and lowest `seqdel` `w_amp` | **`kiwi_tile_xor_tight`** | `kiwi_tile_pages = 8`, `NewXorFilterPolicy(1)` | **`w_amp = 4.61`** on `seqdel` (**`-18.4%`** vs `plain_bloom10`), **`2.53x--2.89x` faster** physical page reclamation than `CompactRange` with 0 XOR peeling, **`-43.3%` to `-45.6%`** `TableReader` RAM vs `kiwi_page_bloom10` |
| **Frequent partial `SecondaryDeleteBelow(floor)` (`10%--75%` drops)** followed by heavy queries against recently expired/deleted keys before compaction | **`kiwi_tile_xor_delete_rank`** | `kiwi_tile_pages = 8`, `FilterPolicy::CreateFromString(cfg, "{id=xorfilter:1;kiwi_delete_rank_bits=2}", &p)` | Cuts straddling edge-page `io_blk` by **`-76.9%` to `-83.3%`** (`0.90 -> 0.15` at 10% drop, `0.02` at 75% drop) and boosts `delQ` I/O QPS by **`+24.7%` to `+166.9%`** over `kiwi_tile_xor_tight` while retaining zero-peel drop support |
| **Latency-critical / cold-storage Present & Absent `Get()`** where false-positive disk reads must be near zero (`<= 0.001%` FPR) | **`kiwi_tile_xor_tight_16bit`** | `kiwi_tile_pages = 8` or `16`, `NewXorFilterPolicy(2)` | **`0.000%--0.001%` FPR**, fastest `h = 16` present mem QPS (**`678.9 kops/s`**, **`+24.7%`** vs `kiwi_page_bloom10`), **`+32.6%`** `seqdel` absent I/O QPS vs `kiwi_page_bloom10`, still **`-24.7%` to `-27.4%`** less `TableReader` RAM than `kiwi_page_bloom10` |
| **Multi-version keys spanning pages inside a single tile**, or write-bound `seqdel` (`1 MB` SSTs) where `+7%` ingest `ops/s` outweighs `1.8x` RAM | `kiwi_page_bloom10` | `kiwi_tile_pages = 8`, `NewBloomFilterPolicy(10)` | Full intra-tile multi-version merge semantics and `+7.1%` higher `seqdel` write `ops/s` (`551.4k` vs `515.0k`) |
| **Workloads that never issue secondary range deletes (`D < floor`)** | `plain_bloom10` | `kiwi_tile_pages = 0`, `NewBloomFilterPolicy(10)` | Lowest `TableReader` RAM (`402.7 KB`, zero KiWi directory overhead) and `10.16 flt_bpk` |
