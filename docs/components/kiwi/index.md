# KiWi: delete tiles and secondary range deletes

**EXPERIMENTAL.** The on-disk format, the options and `DB::SecondaryDeleteBelow`
may change without a compatibility guarantee.

KiWi (Key Weaving Storage Layout) is the layout half of Lethe
([Sarkar et al., SIGMOD 2020](https://arxiv.org/abs/2006.04777)). It makes a
*secondary range delete* -- "delete every entry whose attribute D (for example a
timestamp) is below F", where D is unrelated to the sort key -- cheap. Without
it the only way is a full-tree compaction, because entries with a qualifying D
are scattered over every file. FADE, the other half of Lethe (delete-aware
compaction picking), is not implemented here.

| Area | Path |
|------|------|
| Options | `BlockBasedTableOptions::kiwi_tile_pages`, `kiwi_delete_key_offset` in [`include/rocksdb/table.h`](../../../include/rocksdb/table.h) |
| Public API | `DB::SecondaryDeleteBelow()` in [`include/rocksdb/db.h`](../../../include/rocksdb/db.h) |
| Tile packing, directory format | `table/block_based/kiwi_tile.{h,cc}` |
| Writer | `BlockBasedTableBuilder::FlushTile()` in `table/block_based/block_based_table_builder.cc` |
| Reader (iterator, floor) | `table/block_based/kiwi_table_iterator.{h,cc}`, `BlockBasedTable::KiWiGet()` |
| Secondary delete | `db/db_impl/db_impl_kiwi.cc` |
| Per-file floor in the MANIFEST | `FileMetaData::kiwi_delete_floor`, `kKiWiDeleteFloor` in `db/version_edit.{h,cc}` |
| Tests | `table/block_based/kiwi_test.cc`, `DBKiWiTest` and `DBKiWiSecondaryDeleteTest` in `db/db_etc3_test.cc` |

## Layout

Levels and files are unchanged: a level is a set of files that do not overlap
in key. Inside a file, KiWi adds one layer, the **delete tile**:

- A tile is `kiwi_tile_pages` (h) consecutive data blocks, called **pages**.
- Tiles are ordered by key and do not overlap, so everything above the table
  layer still sees a sorted file.
- Inside a tile, pages are ordered by the delete key D, not by key. Inside each
  page, entries stay in internal-key order.
- D is the 8-byte big-endian unsigned integer at byte `kiwi_delete_key_offset`
  of the value of a `kTypeValue` entry. Other entry types (deletions, merge
  operands, blob indexes, wide columns) have no D, sort last in their tile
  (D = `UINT64_MAX`) and are never removed by a secondary delete. A value too
  short to hold D fails the file being written, loudly.

```
[tile 1: page 1 .. page h'] [tile 2: ...] ... [meta blocks] [KiWi directory] [properties] [footer]
```

Pages are ordinary data blocks (compression, checksums and block cache keys are
unchanged). The standard index has **one entry per tile**; its handle spans the
tile's pages, and its key is the usual separator, so index builders and readers
need no change. The **directory** is one extra meta block
(`rocksdb.kiwi.directory`) with, for every page, its block handle, its minimum
and maximum D (the paper's delete fence pointers) and its Bloom filter on the
key. It is read once when the table is opened and pinned in the table reader
(counted in `ApproximateMemoryUsage`), like the paper's in-memory fences and
filters. KiWi files have no whole-file filter.

A file is a KiWi file if and only if it has the table property
`rocksdb.kiwi.tile_pages` (also `.delete_key_offset`, `.min_delete_key`,
`.max_delete_key`). There is no `format_version` bump, so older binaries cannot
tell and must not read KiWi files.

## Reading

- **Point lookup**: the index finds the tile; the filters of its pages are
  probed (one hash per page, the paper's CPU cost); only matching pages are read
  and merged in key order and fed to `GetContext`, so versions of one key spread
  over several pages or tiles resolve exactly as in an ordinary table. Expected
  cost: 1 + h x FPR page reads. `MultiGet` is a loop of `Get`.
- **Iteration**: `KiWiTableIterator` walks the index and merges the pages of the
  current tile (the paper's cost: a tile is always read whole). Both directions,
  `Seek` and `SeekForPrev` are supported, and it implements the pinning that
  `DBIter` needs for backward iteration.
- Not supported on KiWi files: readahead, prefetching and multi-scan.
- Tools that list physical contents (`VerifyChecksum`, `DumpTable`,
  `sst_dump --command=raw`) walk every page.

## Secondary delete

`DB::SecondaryDeleteBelow(cf, floor)` deletes every entry with D < `floor`:

1. The memtable is flushed.
2. The files with entries below `floor` are found from the
   `rocksdb.kiwi.min_delete_key` properties, in one version, without the DB
   mutex.
3. If that version is still current, one `VersionEdit` records `floor` as the
   **delete floor** of all of them (`FileMetaData::kiwi_delete_floor`, a new
   unignorable MANIFEST tag, so older versions refuse to open the DB instead of
   resurrecting entries) and marks them for compaction.
4. Readers of such a file skip every page whose largest D is below the floor
   **without reading it**, and filter the entries of the pages that straddle the
   floor (the paper's edge pages) in memory. Nothing is rewritten, so the cost is
   O(files) of metadata and no data I/O.
5. Background compaction later reads the files through the same filter, so the
   files it writes simply do not contain the deleted entries.

It is all-or-nothing: it returns `Busy` without changing anything if an affected
file is being compacted or the column family changed during the scan, and
`NotSupported` if a live file was not written with KiWi tiles (compact it first).
The `TableCache` drops a cached reader that was opened before the floor was
recorded. Compaction's input-record count check is skipped for inputs with a
floor.

### Contract

Only the "below a floor" form is offered. For any user key, the delete key of a
newer version must not be smaller than that of an older version (true when D is
a write-time timestamp). Then deleting a version implies deleting all older ones
and nothing stale can reappear; a general range [lo, hi) could remove a newer
version and keep an older one. Snapshots and iterators older than the call can
lose entries, and merge operands on top of a deleted base are the application's
concern. Entries written after the call are not affected, whatever their D.

## Differences from the paper

| Paper | Here | Why |
|-------|------|-----|
| Full page drops delete pages in place and edge pages are rewritten in place | The floor is metadata; pages are skipped at read time and physically dropped by the next compaction | SST files are immutable and `FileSystem` has no hole-punching. Delete latency is O(files) with no data I/O, but space is reclaimed later |
| Range delete on D in [lo, hi) | D < floor | Avoids resurrecting older versions (see the contract) |
| Fence pointers and filters in memory | Same, in one pinned directory per file | -- |
| One hash per lookup | One hash per probed page | Each filter reader hashes the key itself; a pre-hashed probe could be added if measurements ask for it |

## Options and limits

- `kiwi_tile_pages` of 0 or 1 disables KiWi. Only newly written files are
  affected; mixing KiWi and ordinary files in one column family works for
  reads, but `SecondaryDeleteBelow` then returns `NotSupported`.
- Compression runs on one thread for KiWi files
  (`CompressionOptions::parallel_threads` is ignored).
- Rejected by `ValidateOptions` (`NotSupported`): `kHashSearch`, partitioned
  filters, `user_defined_index_factory`, user-defined timestamps, compression
  dictionaries. Embedded blob SSTs fail the builder.
- Memory: the tile being written is buffered (about `kiwi_tile_pages` x
  `block_size` per open builder).
- `FileMetaData` grows by 8 bytes per file for every user; the MANIFEST changes
  only when a floor is recorded.

## Testing

`kiwi_test` checks, against a plain table built from the same entries, forward
and backward scans, random walks of all operations, seeks, point lookups and
`MultiGet` (including keys with up to 60 versions spread over tiles), with and
without a delete floor; it also covers the packing and directory formats and
every corruption path. The database-level tests run a random workload against a
model through flushes, leveled compactions, snapshots, range deletes, ingestion
and reopens, and exercise `SecondaryDeleteBelow` including `Busy`,
`NotSupported`, persistence and space reclamation.
