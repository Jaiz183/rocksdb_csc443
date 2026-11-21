//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "file/writable_file_writer.h"
#include "options/cf_options.h"
#include "port/stack_trace.h"
#include "rocksdb/convenience.h"
#include "rocksdb/env.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/table.h"
#include "table/block_based/block.h"
#include "table/block_based/block_based_table_factory.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/block_based/filter_policy_internal.h"
#include "table/block_based/kiwi_tile.h"
#include "table/meta_blocks.h"
#include "table/table_builder.h"
#include "test_util/sync_point.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"
#include "util/compression.h"
#include "util/random.h"

namespace ROCKSDB_NAMESPACE {
namespace kiwi {

namespace {

// Builds a value holding `delete_key` as 8 big-endian bytes at `offset`,
// surrounded by filler so the value is `total_size` bytes long.
std::string MakeValue(uint64_t delete_key, uint32_t offset, size_t total_size) {
  std::string v(std::max<size_t>(total_size, offset + kDeleteKeySize), 'x');
  for (size_t i = 0; i < kDeleteKeySize; ++i) {
    v[offset + i] = static_cast<char>((delete_key >> (8 * (7 - i))) & 0xff);
  }
  return v;
}

std::string MakeInternalKey(uint64_t key_id, SequenceNumber seq,
                            ValueType type) {
  // Fixed-width user keys so byte order equals numeric order.
  char buf[16];
  snprintf(buf, sizeof(buf), "key%08" PRIu64, key_id);
  return InternalKey(buf, seq, type).Encode().ToString();
}

}  // namespace

TEST(KiWiTileTest, ExtractDeleteKeyBigEndian) {
  uint64_t d = 0;
  ASSERT_OK(ExtractDeleteKey(kTypeValue, MakeValue(0x0102030405060708ULL, 0, 8),
                             0, &d));
  ASSERT_EQ(d, 0x0102030405060708ULL);

  // Non-zero offset, with other bytes around the delete key.
  ASSERT_OK(ExtractDeleteKey(kTypeValue, MakeValue(42, 5, 40), 5, &d));
  ASSERT_EQ(d, 42U);

  // Extremes round-trip.
  ASSERT_OK(ExtractDeleteKey(kTypeValue, MakeValue(0, 3, 11), 3, &d));
  ASSERT_EQ(d, 0U);
  ASSERT_OK(ExtractDeleteKey(
      kTypeValue, MakeValue(std::numeric_limits<uint64_t>::max(), 3, 11), 3,
      &d));
  ASSERT_EQ(d, kNoDeleteKey);
}

TEST(KiWiTileTest, ExtractDeleteKeyShortValueFailsLoudly) {
  uint64_t d = 123;
  // Exactly offset + 8 bytes is fine; one byte less is not.
  ASSERT_OK(ExtractDeleteKey(kTypeValue, MakeValue(7, 4, 12), 4, &d));
  ASSERT_EQ(d, 7U);
  std::string s11(11, 'x');
  ASSERT_EQ(ExtractDeleteKey(kTypeValue, Slice(s11), 4, &d).code(),
            Status::kInvalidArgument);
  // Empty value and tiny value.
  ASSERT_EQ(ExtractDeleteKey(kTypeValue, Slice(), 0, &d).code(),
            Status::kInvalidArgument);
  std::string s7(7, 'x');
  ASSERT_EQ(ExtractDeleteKey(kTypeValue, Slice(s7), 0, &d).code(),
            Status::kInvalidArgument);
  // A huge offset must not overflow the bounds check.
  std::string big(64, 'x');
  ASSERT_EQ(ExtractDeleteKey(kTypeValue, Slice(big),
                             std::numeric_limits<uint32_t>::max(), &d)
                .code(),
            Status::kInvalidArgument);
}

TEST(KiWiTileTest, ExtractDeleteKeyNonValueTypes) {
  // Entries without a delete key are never droppable, whatever the value is.
  const ValueType kTypes[] = {kTypeDeletion, kTypeSingleDeletion, kTypeMerge,
                              kTypeBlobIndex, kTypeWideColumnEntity};
  for (ValueType t : kTypes) {
    uint64_t d = 0;
    ASSERT_OK(ExtractDeleteKey(t, Slice(), 100, &d));
    ASSERT_EQ(d, kNoDeleteKey) << static_cast<int>(t);
  }
}

TEST(KiWiTileTest, PackerEmptyAndReuse) {
  TilePacker packer(/*offset=*/0, /*max_pages=*/4, /*page_size=*/64);
  ASSERT_TRUE(packer.empty());
  ASSERT_FALSE(packer.full());
  std::vector<TilePage> pages;
  packer.Pack(&pages);
  ASSERT_TRUE(pages.empty());

  ASSERT_OK(packer.Add(MakeInternalKey(1, 10, kTypeValue),
                       MakeValue(5, 0, 16)));
  ASSERT_FALSE(packer.empty());
  ASSERT_EQ(packer.num_entries(), 1U);
  ASSERT_EQ(packer.last_key().ToString(),
            MakeInternalKey(1, 10, kTypeValue));
  packer.Pack(&pages);
  ASSERT_EQ(pages.size(), 1U);
  ASSERT_EQ(pages[0].entries, std::vector<size_t>{0});
  ASSERT_EQ(pages[0].min_delete_key, 5U);
  ASSERT_EQ(pages[0].max_delete_key, 5U);

  packer.Clear();
  ASSERT_TRUE(packer.empty());
  ASSERT_EQ(packer.buffered_bytes(), 0U);
  packer.Pack(&pages);
  ASSERT_TRUE(pages.empty());
}

TEST(KiWiTileTest, PackerRejectsShortValue) {
  TilePacker packer(/*offset=*/8, /*max_pages=*/4, /*page_size=*/64);
  ASSERT_EQ(packer.Add(MakeInternalKey(1, 1, kTypeValue),
                       std::string(15, 'x'))
                .code(),
            Status::kInvalidArgument);
  // A failed Add leaves nothing buffered.
  ASSERT_TRUE(packer.empty());
  // Deletions carry no value and are accepted.
  ASSERT_OK(packer.Add(MakeInternalKey(2, 2, kTypeDeletion), Slice()));
  ASSERT_EQ(packer.num_entries(), 1U);
}

TEST(KiWiTileTest, PackerOrdersPagesByDeleteKey) {
  // 6 entries of ~32 bytes with deliberately shuffled delete keys; 3 pages.
  TilePacker packer(/*offset=*/0, /*max_pages=*/3, /*page_size=*/70);
  const uint64_t ds[] = {50, 10, 40, 20, 60, 30};
  for (uint64_t i = 0; i < 6; ++i) {
    ASSERT_OK(packer.Add(MakeInternalKey(i, 100 - i, kTypeValue),
                         MakeValue(ds[i], 0, 16)));
  }
  std::vector<TilePage> pages;
  packer.Pack(&pages);
  ASSERT_EQ(pages.size(), 3U);
  // Page 0 holds the two smallest D (10, 20) = positions 1 and 3, in key order.
  ASSERT_EQ(pages[0].entries, (std::vector<size_t>{1, 3}));
  ASSERT_EQ(pages[0].min_delete_key, 10U);
  ASSERT_EQ(pages[0].max_delete_key, 20U);
  ASSERT_EQ(pages[1].entries, (std::vector<size_t>{2, 5}));
  ASSERT_EQ(pages[2].entries, (std::vector<size_t>{0, 4}));
  ASSERT_EQ(pages[2].max_delete_key, 60U);
}

TEST(KiWiTileTest, PackerKeepsNonDroppableEntriesLast) {
  TilePacker packer(/*offset=*/0, /*max_pages=*/2, /*page_size=*/40);
  ASSERT_OK(packer.Add(MakeInternalKey(1, 5, kTypeDeletion), Slice()));
  ASSERT_OK(packer.Add(MakeInternalKey(2, 4, kTypeValue), MakeValue(9, 0, 16)));
  ASSERT_OK(packer.Add(MakeInternalKey(3, 3, kTypeValue), MakeValue(1, 0, 16)));
  std::vector<TilePage> pages;
  packer.Pack(&pages);
  // The tombstone has delete key kNoDeleteKey, so it ends in the last page and
  // that page can never be skipped by a secondary delete.
  ASSERT_FALSE(pages.empty());
  ASSERT_EQ(pages.back().max_delete_key, kNoDeleteKey);
  ASSERT_EQ(pages.front().min_delete_key, 1U);
}

TEST(KiWiTileTest, PackerFullTracksBufferedBytes) {
  TilePacker packer(/*offset=*/0, /*max_pages=*/4, /*page_size=*/100);
  size_t added = 0;
  while (!packer.full()) {
    ASSERT_OK(packer.Add(MakeInternalKey(added, 1, kTypeValue),
                         MakeValue(added, 0, 24)));
    ++added;
    ASSERT_LT(added, 1000U);
  }
  ASSERT_GE(packer.buffered_bytes(), 400U);
  // Overshoots by less than one entry.
  ASSERT_LT(packer.buffered_bytes(), 400U + 64U);
}

// Randomized check of the packing invariants over many tile shapes.
TEST(KiWiTileTest, PackerRandomInvariants) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);

  for (int iter = 0; iter < 300; ++iter) {
    const uint32_t offset = static_cast<uint32_t>(rnd.Uniform(12));
    const uint32_t max_pages = 1 + static_cast<uint32_t>(rnd.Uniform(16));
    const size_t page_size = 32 + rnd.Uniform(400);
    const size_t n = 1 + rnd.Uniform(300);
    TilePacker packer(offset, max_pages, page_size);

    std::vector<uint64_t> expected_d;
    size_t max_entry_bytes = 0;
    for (size_t i = 0; i < n; ++i) {
      // Few distinct delete keys sometimes, to exercise ties.
      const uint64_t d = rnd.OneIn(3) ? rnd.Uniform(8) : rnd.Next();
      const bool deletion = rnd.OneIn(10);
      const std::string ikey = MakeInternalKey(
          i, 1000 - i, deletion ? kTypeDeletion : kTypeValue);
      const std::string value =
          deletion ? std::string() : MakeValue(d, offset, 8 + rnd.Uniform(60));
      ASSERT_OK(packer.Add(ikey, value));
      expected_d.push_back(deletion ? kNoDeleteKey : d);
      max_entry_bytes = std::max(max_entry_bytes, ikey.size() + value.size());
    }

    std::vector<TilePage> pages;
    packer.Pack(&pages);

    ASSERT_GE(pages.size(), 1U);
    ASSERT_LE(pages.size(), max_pages);
    const size_t want_pages = std::clamp(
        (packer.buffered_bytes() + page_size - 1) / page_size, size_t{1},
        static_cast<size_t>(max_pages));
    ASSERT_LE(pages.size(), want_pages);
    const size_t target =
        (packer.buffered_bytes() + want_pages - 1) / want_pages;

    std::set<size_t> seen;
    for (size_t p = 0; p < pages.size(); ++p) {
      const TilePage& page = pages[p];
      ASSERT_FALSE(page.entries.empty());
      uint64_t lo = kNoDeleteKey;
      uint64_t hi = 0;
      size_t bytes = 0;
      for (size_t i = 0; i < page.entries.size(); ++i) {
        const size_t pos = page.entries[i];
        ASSERT_TRUE(seen.insert(pos).second) << "entry in two pages";
        if (i > 0) {
          // Internal key order inside a page.
          ASSERT_LT(page.entries[i - 1], pos);
        }
        ASSERT_EQ(packer.delete_key(pos), expected_d[pos]);
        lo = std::min(lo, expected_d[pos]);
        hi = std::max(hi, expected_d[pos]);
        bytes += packer.key(pos).size() + packer.value(pos).size();
      }
      ASSERT_EQ(page.min_delete_key, lo);
      ASSERT_EQ(page.max_delete_key, hi);
      // Pages do not overlap in delete key (equal keys may straddle a cut).
      if (p > 0) {
        ASSERT_LE(pages[p - 1].max_delete_key, page.min_delete_key);
      }
      // A page closes as soon as it reaches the target size, so it exceeds it
      // by less than one entry (the last page may be smaller).
      ASSERT_LT(bytes, target + max_entry_bytes + 1);
    }
    ASSERT_EQ(seen.size(), n);

    // Clear() makes the packer reusable.
    packer.Clear();
    ASSERT_TRUE(packer.empty());
    ASSERT_EQ(packer.buffered_bytes(), 0U);
  }
}

namespace {

// A random directory: tiles of 1..16 pages at ascending file offsets.
struct RandomDirectory {
  // Owns the bloom bytes that the pages point to.
  std::vector<std::string> blooms;
  std::vector<std::vector<DirectoryPage>> tiles;
  std::string encoded;
};

void MakeRandomDirectory(Random64* rnd, size_t num_tiles,
                         RandomDirectory* out) {
  size_t total_pages = 0;
  std::vector<size_t> pages_per_tile;
  for (size_t t = 0; t < num_tiles; ++t) {
    pages_per_tile.push_back(1 + rnd->Uniform(16));
    total_pages += pages_per_tile.back();
  }
  // Reserve so the Slices into `blooms` stay valid.
  out->blooms.reserve(total_pages);
  uint64_t offset = rnd->Uniform(100);
  DirectoryBuilder builder;
  for (size_t t = 0; t < num_tiles; ++t) {
    std::vector<DirectoryPage> tile;
    for (size_t i = 0; i < pages_per_tile[t]; ++i) {
      DirectoryPage page;
      const uint64_t size = 1 + rnd->Uniform(5000);
      page.handle = BlockHandle(offset, size);
      offset += size + 5;  // block trailer
      page.min_delete_key = rnd->Uniform(1000000);
      page.max_delete_key = page.min_delete_key + rnd->Uniform(1000);
      out->blooms.push_back(
          std::string(rnd->OneIn(4) ? 0 : rnd->Uniform(80), 'b'));
      page.bloom = out->blooms.back();
      tile.push_back(page);
    }
    builder.AddTile(tile);
    out->tiles.push_back(std::move(tile));
  }
  builder.Finish(&out->encoded);
}

Status DecodeDirectory(const std::string& encoded, Directory* dir) {
  return dir->Decode(encoded);
}

}  // namespace

TEST(KiWiDirectoryTest, EmptyDirectory) {
  DirectoryBuilder builder;
  std::string encoded;
  builder.Finish(&encoded);
  Directory dir;
  ASSERT_OK(DecodeDirectory(encoded, &dir));
  ASSERT_EQ(dir.num_tiles(), 0U);
  ASSERT_EQ(dir.FindTile(0), 0U);
}

TEST(KiWiDirectoryTest, RoundTripAndFindTile) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);
  for (int iter = 0; iter < 100; ++iter) {
    const size_t num_tiles = 1 + rnd.Uniform(40);
    RandomDirectory expected;
    MakeRandomDirectory(&rnd, num_tiles, &expected);

    Directory dir;
    ASSERT_OK(DecodeDirectory(expected.encoded, &dir));
    ASSERT_EQ(dir.num_tiles(), num_tiles);
    ASSERT_GT(dir.ApproximateMemoryUsage(), expected.encoded.size());
    for (size_t t = 0; t < num_tiles; ++t) {
      const Directory::TileView tile = dir.tile(t);
      ASSERT_EQ(tile.num_pages, expected.tiles[t].size());
      for (size_t i = 0; i < tile.num_pages; ++i) {
        const DirectoryPage& got = tile.pages[i];
        const DirectoryPage& want = expected.tiles[t][i];
        ASSERT_EQ(got.handle, want.handle);
        ASSERT_EQ(got.min_delete_key, want.min_delete_key);
        ASSERT_EQ(got.max_delete_key, want.max_delete_key);
        ASSERT_EQ(got.bloom.ToString(), want.bloom.ToString());
      }
      // The index entry of a tile carries its first page's offset.
      const uint64_t first = expected.tiles[t][0].handle.offset();
      ASSERT_EQ(dir.FindTile(first), t);
      ASSERT_EQ(dir.FindTile(first + 1), num_tiles) << "not a tile start";
    }
    ASSERT_EQ(dir.FindTile(std::numeric_limits<uint64_t>::max()), num_tiles);
  }
}

TEST(KiWiDirectoryTest, EveryTruncationIsCorruption) {
  Random64 rnd(301);
  RandomDirectory expected;
  MakeRandomDirectory(&rnd, 5, &expected);
  for (size_t len = 0; len < expected.encoded.size(); ++len) {
    Directory dir;
    Status s = DecodeDirectory(expected.encoded.substr(0, len), &dir);
    ASSERT_EQ(s.code(), Status::kCorruption) << "prefix length " << len;
    // A rejected directory is left empty.
    ASSERT_EQ(dir.num_tiles(), 0U);
  }
}

TEST(KiWiDirectoryTest, RejectsMalformedDirectories) {
  Random64 rnd(7);
  RandomDirectory good;
  MakeRandomDirectory(&rnd, 3, &good);
  Directory dir;

  std::string bad = good.encoded;
  bad[0] = 99;  // unknown version
  ASSERT_EQ(DecodeDirectory(bad, &dir).code(), Status::kCorruption);

  bad = good.encoded + "x";  // trailing bytes
  ASSERT_EQ(DecodeDirectory(bad, &dir).code(), Status::kCorruption);

  // Absurd tile count (varint32 max) must fail without a huge allocation.
  bad = std::string(1, DirectoryBuilder::kFormatVersion) +
        std::string("\xff\xff\xff\xff\x0f");
  ASSERT_EQ(DecodeDirectory(bad, &dir).code(), Status::kCorruption);

  // A tile with zero pages.
  bad = std::string(1, DirectoryBuilder::kFormatVersion) + std::string("\x01") +
        std::string("\x00", 1);
  ASSERT_EQ(DecodeDirectory(bad, &dir).code(), Status::kCorruption);

  // Inverted delete key range.
  DirectoryPage page;
  page.handle = BlockHandle(0, 10);
  page.min_delete_key = 9;
  page.max_delete_key = 3;
  DirectoryBuilder inverted;
  inverted.AddTile({page});
  inverted.Finish(&bad);
  ASSERT_EQ(DecodeDirectory(bad, &dir).code(), Status::kCorruption);

  // Tiles whose start offsets do not increase.
  DirectoryPage a;
  a.handle = BlockHandle(100, 10);
  DirectoryPage b;
  b.handle = BlockHandle(100, 10);
  DirectoryBuilder unordered;
  unordered.AddTile({a});
  unordered.AddTile({b});
  unordered.Finish(&bad);
  ASSERT_EQ(DecodeDirectory(bad, &dir).code(), Status::kCorruption);

  // The original is still fine.
  ASSERT_OK(DecodeDirectory(good.encoded, &dir));
}

// Corrupted bytes must never crash the decoder, and whatever it accepts must
// still be internally consistent.
TEST(KiWiDirectoryTest, RandomCorruptionIsSafe) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);
  RandomDirectory good;
  MakeRandomDirectory(&rnd, 20, &good);
  for (int iter = 0; iter < 2000; ++iter) {
    std::string bad = good.encoded;
    const int flips = 1 + static_cast<int>(rnd.Uniform(4));
    for (int f = 0; f < flips; ++f) {
      bad[rnd.Uniform(bad.size())] ^= static_cast<char>(1 + rnd.Uniform(255));
    }
    Directory dir;
    Status s = DecodeDirectory(bad, &dir);
    if (s.ok()) {
      for (size_t t = 0; t < dir.num_tiles(); ++t) {
        const Directory::TileView tile = dir.tile(t);
        ASSERT_GE(tile.num_pages, 1U);
        ASSERT_EQ(dir.FindTile(tile.pages[0].handle.offset()), t);
      }
    } else {
      ASSERT_EQ(s.code(), Status::kCorruption);
      ASSERT_EQ(dir.num_tiles(), 0U);
    }
  }
}

#ifndef NDEBUG
namespace {

struct Entry {
  std::string ikey;
  std::string value;
};

// What the builder wrote for one tile.
struct TileLayout {
  std::vector<BlockHandle> pages;
  BlockHandle handle;  // what the index entry of the tile points to
};

struct BuiltTable {
  std::string contents;
  std::vector<TileLayout> tiles;
  std::unique_ptr<TableProperties> props;
  std::string directory;  // contents of the KiWi directory meta block
  uint64_t directory_offset = 0;
};

struct BuildParams {
  uint32_t tile_pages = 4;
  size_t block_size = 256;
  uint32_t delete_key_offset = 0;
  CompressionType compression = kNoCompression;
  uint32_t parallel_threads = 1;
  bool filters = true;  // per-page Bloom filters
  const Comparator* comparator = BytewiseComparator();
};

// Writes `entries` (already in internal key order) as a KiWi table, recording
// the handles of the pages and tiles through sync points.
void BuildKiWiTable(const std::vector<Entry>& entries, const BuildParams& bp,
                    BuiltTable* out) {
  Options options;
  options.comparator = bp.comparator;
  options.compression_opts.parallel_threads = bp.parallel_threads;
  const ImmutableOptions ioptions(options);
  const MutableCFOptions moptions(options);
  InternalKeyComparator ikc(options.comparator);

  BlockBasedTableOptions bbto;
  bbto.kiwi_tile_pages = bp.tile_pages;
  bbto.block_size = bp.block_size;
  bbto.kiwi_delete_key_offset = bp.delete_key_offset;
  if (bp.filters) {
    bbto.filter_policy.reset(NewBloomFilterPolicy(10));
  } else {
    bbto.filter_policy = nullptr;
  }
  BlockBasedTableFactory factory(bbto);

  std::unique_ptr<FSWritableFile> sink(new test::StringSink());
  std::unique_ptr<WritableFileWriter> file_writer(new WritableFileWriter(
      std::move(sink), "" /* don't care */, FileOptions()));
  InternalTblPropCollFactories internal_tbl_prop_coll_factories;
  std::string column_family_name;
  const ReadOptions read_options;
  const WriteOptions write_options;

  out->tiles.clear();
  TileLayout current;
  SyncPoint::GetInstance()->SetCallBack(
      "BlockBasedTableBuilder::FlushTile:PageWritten", [&](void* arg) {
        current.pages.push_back(*static_cast<BlockHandle*>(arg));
      });
  SyncPoint::GetInstance()->SetCallBack(
      "BlockBasedTableBuilder::FlushTile:TileWritten", [&](void* arg) {
        current.handle = *static_cast<BlockHandle*>(arg);
        out->tiles.push_back(current);
        current = TileLayout();
      });
  SyncPoint::GetInstance()->EnableProcessing();

  std::unique_ptr<TableBuilder> builder(factory.NewTableBuilder(
      TableBuilderOptions(ioptions, moptions, read_options, write_options, ikc,
                          &internal_tbl_prop_coll_factories, bp.compression,
                          options.compression_opts,
                          TablePropertiesCollectorFactory::Context::
                              kUnknownColumnFamily,
                          column_family_name, /*level=*/-1,
                          kUnknownNewestKeyTime),
      file_writer.get()));
  for (const Entry& e : entries) {
    builder->Add(e.ikey, e.value);
  }
  ASSERT_OK(builder->status());
  ASSERT_OK(builder->Finish());
  ASSERT_OK(file_writer->Flush(IOOptions()));
  SyncPoint::GetInstance()->DisableProcessing();
  SyncPoint::GetInstance()->ClearAllCallBacks();

  ASSERT_EQ(builder->NumEntries(), entries.size());
  auto* ss = static_cast<test::StringSink*>(file_writer->writable_file());
  out->contents = ss->contents();
  ASSERT_EQ(out->contents.size(), builder->FileSize());

  std::unique_ptr<FSRandomAccessFile> source(
      new test::StringSource(out->contents, 72242, true));
  std::unique_ptr<RandomAccessFileReader> file_reader(
      new RandomAccessFileReader(std::move(source), "test"));
  ASSERT_OK(ReadTableProperties(file_reader.get(), out->contents.size(),
                                kBlockBasedTableMagicNumber, ioptions,
                                read_options, &out->props));

  out->directory.clear();
  if (bp.tile_pages >= 2) {
    BlockHandle directory_handle;
    ASSERT_OK(FindMetaBlockInFile(file_reader.get(), out->contents.size(),
                                  kBlockBasedTableMagicNumber, ioptions,
                                  read_options, kDirectoryBlockName,
                                  &directory_handle));
    // Stored uncompressed; the checksum trailer follows the payload.
    out->directory = out->contents.substr(directory_handle.offset(),
                                          directory_handle.size());
    out->directory_offset = directory_handle.offset();
  }
}

// A table reader and everything it refers to for as long as it lives: the
// reader keeps references to the options, comparator and table factory.
struct OpenedTable {
  Options options;
  std::unique_ptr<ImmutableOptions> ioptions;
  std::unique_ptr<MutableCFOptions> moptions;
  std::unique_ptr<InternalKeyComparator> ikc;
  std::unique_ptr<BlockBasedTableFactory> factory;
  std::unique_ptr<TableReader> reader;
};

// Opens `contents` (a file written by BuildKiWiTable) with the block-based
// table reader.
Status OpenTable(const std::string& contents, const BuildParams& bp,
                 OpenedTable* out) {
  out->options.comparator = bp.comparator;
  out->ioptions = std::make_unique<ImmutableOptions>(out->options);
  out->moptions = std::make_unique<MutableCFOptions>(out->options);
  out->ikc = std::make_unique<InternalKeyComparator>(out->options.comparator);
  BlockBasedTableOptions bbto;
  bbto.block_size = bp.block_size;
  if (bp.filters) {
    bbto.filter_policy.reset(NewBloomFilterPolicy(10));
  }
  out->factory = std::make_unique<BlockBasedTableFactory>(bbto);
  std::unique_ptr<FSRandomAccessFile> source(
      new test::StringSource(contents, 1, /*mmap=*/false));
  std::unique_ptr<RandomAccessFileReader> file_reader(
      new RandomAccessFileReader(std::move(source), "test"));
  return out->factory->NewTableReader(
      TableReaderOptions(*out->ioptions, out->moptions->prefix_extractor,
                         out->moptions->compression_manager.get(),
                         EnvOptions(), *out->ikc,
                         /*block_protection_bytes_per_key=*/0,
                         /*skip_filters=*/false, /*immortal=*/false,
                         /*force_direct_prefetch=*/false, /*level=*/-1,
                         /*block_cache_tracer=*/nullptr,
                         out->moptions->write_buffer_size, "",
                         /*cur_file_num=*/1, kNullUniqueId64x2,
                         /*largest_seqno=*/0),
      std::move(file_reader), contents.size(), &out->reader);
}

// Entries in internal key order: ascending user keys, several versions each
// (newest first), a mix of values and deletions.
std::vector<Entry> MakeEntries(Random64* rnd, size_t num_keys,
                               uint32_t delete_key_offset,
                               size_t max_value_size) {
  std::vector<Entry> entries;
  for (size_t k = 0; k < num_keys; ++k) {
    const size_t versions = 1 + rnd->Uniform(3);
    for (size_t v = 0; v < versions; ++v) {
      const bool deletion = rnd->OneIn(8);
      Entry e;
      e.ikey = MakeInternalKey(k, 1000 - v, deletion ? kTypeDeletion
                                                      : kTypeValue);
      if (!deletion) {
        // Few distinct delete keys sometimes, to exercise ties.
        const uint64_t d = rnd->OneIn(3) ? rnd->Uniform(8) : rnd->Next();
        e.value = MakeValue(d, delete_key_offset,
                            kDeleteKeySize + rnd->Uniform(max_value_size));
      }
      entries.push_back(std::move(e));
    }
  }
  return entries;
}

// Decodes an uncompressed page into its entries.
void DecodePage(const std::string& file, const BlockHandle& handle,
                const InternalKeyComparator& ikc, std::vector<Entry>* out) {
  std::string bytes = file.substr(handle.offset(), handle.size());
  BlockContents contents;
  contents.data = Slice(bytes);
  Block block(std::move(contents));
  std::unique_ptr<DataBlockIter> iter(block.NewDataIterator(
      ikc.user_comparator(), kDisableGlobalSequenceNumber));
  out->clear();
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    out->push_back(Entry{iter->key().ToString(), iter->value().ToString()});
  }
  ASSERT_OK(iter->status());
}

}  // namespace

TEST(KiWiBuilderTest, WritesTilesWithKiWiInvariants) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);
  InternalKeyComparator ikc(BytewiseComparator());

  for (int iter = 0; iter < 20; ++iter) {
    BuildParams bp;
    bp.tile_pages = 2 + static_cast<uint32_t>(rnd.Uniform(7));
    bp.block_size = 64 + rnd.Uniform(400);
    bp.delete_key_offset = static_cast<uint32_t>(rnd.Uniform(10));
    bp.filters = !rnd.OneIn(4);
    const std::vector<Entry> entries =
        MakeEntries(&rnd, 50 + rnd.Uniform(300), bp.delete_key_offset, 60);

    BuiltTable table;
    BuildKiWiTable(entries, bp, &table);
    ASSERT_GE(table.tiles.size(), 1U);

    std::map<std::string, std::string> expected;
    for (const Entry& e : entries) {
      expected[e.ikey] = e.value;
    }
    std::map<std::string, std::string> seen;
    size_t total_pages = 0;
    Directory dir;
    ASSERT_OK(dir.Decode(table.directory));
    ASSERT_EQ(dir.num_tiles(), table.tiles.size());
    uint64_t file_min_d = kNoDeleteKey;
    uint64_t file_max_d = 0;
    std::string prev_tile_last_key;
    for (size_t t = 0; t < table.tiles.size(); ++t) {
      const TileLayout& tile = table.tiles[t];
      ASSERT_GE(tile.pages.size(), 1U);
      ASSERT_LE(tile.pages.size(), bp.tile_pages);
      total_pages += tile.pages.size();

      // The tile handle spans exactly its pages; each page is followed by its
      // block trailer, and tiles are laid out back to back.
      ASSERT_EQ(tile.handle.offset(), tile.pages.front().offset());
      ASSERT_EQ(tile.handle.offset() + tile.handle.size(),
                tile.pages.back().offset() + tile.pages.back().size());
      for (size_t i = 1; i < tile.pages.size(); ++i) {
        ASSERT_EQ(tile.pages[i].offset(),
                  tile.pages[i - 1].offset() + tile.pages[i - 1].size() +
                      BlockBasedTable::kBlockTrailerSize);
      }
      if (t > 0) {
        const BlockHandle& prev = table.tiles[t - 1].handle;
        ASSERT_EQ(tile.handle.offset(),
                  prev.offset() + prev.size() +
                      BlockBasedTable::kBlockTrailerSize);
      }

      // The directory describes exactly the pages that were written, and the
      // index entry of the tile (its first page offset) finds the tile.
      ASSERT_EQ(dir.FindTile(tile.handle.offset()), t);
      const Directory::TileView dir_tile = dir.tile(t);
      ASSERT_EQ(dir_tile.num_pages, tile.pages.size());

      std::string tile_first_key, tile_last_key;
      uint64_t prev_page_max_d = 0;
      for (size_t i = 0; i < tile.pages.size(); ++i) {
        std::vector<Entry> page;
        DecodePage(table.contents, tile.pages[i], ikc, &page);
        ASSERT_EQ(dir_tile.pages[i].handle, tile.pages[i]);
        std::unique_ptr<BuiltinFilterBitsReader> bloom;
        if (bp.filters) {
          ASSERT_FALSE(dir_tile.pages[i].bloom.empty());
          bloom.reset(BuiltinFilterPolicy::GetBuiltinFilterBitsReader(
              dir_tile.pages[i].bloom));
        } else {
          ASSERT_TRUE(dir_tile.pages[i].bloom.empty());
        }
        ASSERT_FALSE(page.empty());
        uint64_t min_d = kNoDeleteKey;
        uint64_t max_d = 0;
        for (size_t j = 0; j < page.size(); ++j) {
          if (j > 0) {
            // Entries stay in internal key order inside a page.
            ASSERT_LT(ikc.Compare(page[j - 1].ikey, page[j].ikey), 0);
          }
          uint64_t d = 0;
          ASSERT_OK(ExtractDeleteKey(ExtractValueType(page[j].ikey),
                                     page[j].value, bp.delete_key_offset, &d));
          min_d = std::min(min_d, d);
          max_d = std::max(max_d, d);
          ASSERT_TRUE(seen.emplace(page[j].ikey, page[j].value).second)
              << "entry written twice";
          if (bloom != nullptr) {
            // No false negatives for any key of the page.
            ASSERT_TRUE(bloom->MayMatch(ExtractUserKey(page[j].ikey)));
          }
          if (tile_first_key.empty() ||
              ikc.Compare(page[j].ikey, tile_first_key) < 0) {
            tile_first_key = page[j].ikey;
          }
          if (tile_last_key.empty() ||
              ikc.Compare(page[j].ikey, tile_last_key) > 0) {
            tile_last_key = page[j].ikey;
          }
        }
        // The directory's delete key fences match the page contents.
        ASSERT_EQ(dir_tile.pages[i].min_delete_key, min_d);
        ASSERT_EQ(dir_tile.pages[i].max_delete_key, max_d);
        file_min_d = std::min(file_min_d, min_d);
        file_max_d = std::max(file_max_d, max_d);
        // Pages of a tile are ordered by delete key.
        if (i > 0) {
          ASSERT_LE(prev_page_max_d, min_d);
        }
        prev_page_max_d = max_d;
      }
      // Tiles do not overlap in key.
      if (t > 0) {
        ASSERT_LT(ikc.Compare(prev_tile_last_key, tile_first_key), 0);
      }
      prev_tile_last_key = tile_last_key;
    }
    ASSERT_EQ(seen, expected);
    ASSERT_EQ(table.props->num_entries, entries.size());
    ASSERT_EQ(table.props->num_data_blocks, total_pages);
    // KiWi files have no whole-file filter; page filters live in the directory.
    ASSERT_EQ(table.props->filter_size, 0U);
    const auto& up = table.props->user_collected_properties;
    ASSERT_EQ(up.at(kPropTilePages), std::to_string(bp.tile_pages));
    ASSERT_EQ(up.at(kPropDeleteKeyOffset),
              std::to_string(bp.delete_key_offset));
    ASSERT_EQ(up.at(kPropMinDeleteKey), std::to_string(file_min_d));
    ASSERT_EQ(up.at(kPropMaxDeleteKey), std::to_string(file_max_d));
  }
}

TEST(KiWiBuilderTest, ManyTilesWithTinyPages) {
  Random64 rnd(5);
  BuildParams bp;
  bp.tile_pages = 3;
  bp.block_size = 100;
  const std::vector<Entry> entries = MakeEntries(&rnd, 400, 0, 40);
  BuiltTable table;
  BuildKiWiTable(entries, bp, &table);
  // ~100 bytes/entry-key-and-value, 3 pages of 100 bytes per tile.
  ASSERT_GT(table.tiles.size(), 10U);
}

TEST(KiWiBuilderTest, ShortValueFailsTheBuilder) {
  Options options;
  const ImmutableOptions ioptions(options);
  const MutableCFOptions moptions(options);
  InternalKeyComparator ikc(options.comparator);
  BlockBasedTableOptions bbto;
  bbto.kiwi_tile_pages = 4;
  bbto.kiwi_delete_key_offset = 4;  // needs values of at least 12 bytes
  BlockBasedTableFactory factory(bbto);
  std::unique_ptr<FSWritableFile> sink(new test::StringSink());
  std::unique_ptr<WritableFileWriter> file_writer(new WritableFileWriter(
      std::move(sink), "" /* don't care */, FileOptions()));
  InternalTblPropCollFactories internal_tbl_prop_coll_factories;
  std::string column_family_name;
  const ReadOptions read_options;
  const WriteOptions write_options;
  std::unique_ptr<TableBuilder> builder(factory.NewTableBuilder(
      TableBuilderOptions(ioptions, moptions, read_options, write_options, ikc,
                          &internal_tbl_prop_coll_factories, kNoCompression,
                          options.compression_opts,
                          TablePropertiesCollectorFactory::Context::
                              kUnknownColumnFamily,
                          column_family_name, /*level=*/-1,
                          kUnknownNewestKeyTime),
      file_writer.get()));
  builder->Add(MakeInternalKey(1, 5, kTypeValue), MakeValue(1, 4, 12));
  ASSERT_OK(builder->status());
  builder->Add(MakeInternalKey(2, 4, kTypeValue), std::string(11, 'x'));
  ASSERT_EQ(builder->status().code(), Status::kInvalidArgument);
  // Finish() reports the sticky error and closes the builder.
  ASSERT_EQ(builder->Finish().code(), Status::kInvalidArgument);
}

TEST(KiWiBuilderTest, UserDefinedTimestampsAreNotSupported) {
  Options options;
  options.comparator = test::BytewiseComparatorWithU64TsWrapper();
  const ImmutableOptions ioptions(options);
  const MutableCFOptions moptions(options);
  InternalKeyComparator ikc(options.comparator);
  BlockBasedTableOptions bbto;
  bbto.kiwi_tile_pages = 4;
  BlockBasedTableFactory factory(bbto);
  std::unique_ptr<FSWritableFile> sink(new test::StringSink());
  std::unique_ptr<WritableFileWriter> file_writer(new WritableFileWriter(
      std::move(sink), "" /* don't care */, FileOptions()));
  InternalTblPropCollFactories internal_tbl_prop_coll_factories;
  std::string column_family_name;
  const ReadOptions read_options;
  const WriteOptions write_options;
  std::unique_ptr<TableBuilder> builder(factory.NewTableBuilder(
      TableBuilderOptions(ioptions, moptions, read_options, write_options, ikc,
                          &internal_tbl_prop_coll_factories, kNoCompression,
                          options.compression_opts,
                          TablePropertiesCollectorFactory::Context::
                              kUnknownColumnFamily,
                          column_family_name, /*level=*/-1,
                          kUnknownNewestKeyTime),
      file_writer.get()));
  ASSERT_EQ(builder->status().code(), Status::kNotSupported);
  builder->Abandon();
}

TEST(KiWiBuilderTest, EstimatedFileSizeCountsBufferedTile) {
  Options options;
  const ImmutableOptions ioptions(options);
  const MutableCFOptions moptions(options);
  InternalKeyComparator ikc(options.comparator);
  BlockBasedTableOptions bbto;
  bbto.kiwi_tile_pages = 8;
  bbto.block_size = 4096;
  BlockBasedTableFactory factory(bbto);
  std::unique_ptr<FSWritableFile> sink(new test::StringSink());
  std::unique_ptr<WritableFileWriter> file_writer(new WritableFileWriter(
      std::move(sink), "" /* don't care */, FileOptions()));
  InternalTblPropCollFactories internal_tbl_prop_coll_factories;
  std::string column_family_name;
  const ReadOptions read_options;
  const WriteOptions write_options;
  std::unique_ptr<TableBuilder> builder(factory.NewTableBuilder(
      TableBuilderOptions(ioptions, moptions, read_options, write_options, ikc,
                          &internal_tbl_prop_coll_factories, kNoCompression,
                          options.compression_opts,
                          TablePropertiesCollectorFactory::Context::
                              kUnknownColumnFamily,
                          column_family_name, /*level=*/-1,
                          kUnknownNewestKeyTime),
      file_writer.get()));
  uint64_t last = builder->EstimatedFileSize();
  for (uint64_t i = 0; i < 100; ++i) {
    builder->Add(MakeInternalKey(i, 1, kTypeValue), MakeValue(i, 0, 100));
    // A tile holds 8 x 4KB, so nothing is written yet, but the size estimate
    // used to cut compaction outputs keeps growing.
    ASSERT_EQ(builder->FileSize(), 0U);
    ASSERT_GT(builder->EstimatedFileSize(), last);
    last = builder->EstimatedFileSize();
  }
  ASSERT_OK(builder->Finish());
  ASSERT_GT(builder->FileSize(), 0U);
}

TEST(KiWiBuilderTest, ParallelCompressionRequestIsDowngraded) {
  CompressionType type = kNoCompression;
  for (CompressionType t : GetSupportedCompressions()) {
    if (t != kNoCompression) {
      type = t;
      break;
    }
  }
  if (type == kNoCompression) {
    ROCKSDB_GTEST_SKIP("No compression library available");
    return;
  }
  Random64 rnd(11);
  BuildParams bp;
  bp.compression = type;
  bp.parallel_threads = 4;
  const std::vector<Entry> entries = MakeEntries(&rnd, 300, 0, 60);
  BuiltTable table;
  BuildKiWiTable(entries, bp, &table);
  ASSERT_GT(table.tiles.size(), 1U);
  size_t pages = 0;
  for (const TileLayout& t : table.tiles) {
    pages += t.pages.size();
  }
  ASSERT_EQ(table.props->num_data_blocks, pages);
}
TEST(KiWiReaderTest, DirectoryIsLoadedAndPinned) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);
  for (int iter = 0; iter < 10; ++iter) {
    BuildParams bp;
    bp.tile_pages = 2 + static_cast<uint32_t>(rnd.Uniform(7));
    bp.block_size = 64 + rnd.Uniform(300);
    bp.filters = !rnd.OneIn(4);
    BuiltTable table;
    BuildKiWiTable(MakeEntries(&rnd, 100 + rnd.Uniform(200), 0, 50), bp,
                   &table);

    OpenedTable opened;
    ASSERT_OK(OpenTable(table.contents, bp, &opened));
    const kiwi::Directory* dir =
        static_cast<BlockBasedTable*>(opened.reader.get())
            ->TEST_GetKiWiDirectory();
    ASSERT_NE(dir, nullptr);

    Directory expected;
    ASSERT_OK(expected.Decode(table.directory));
    ASSERT_EQ(dir->num_tiles(), expected.num_tiles());
    for (size_t t = 0; t < dir->num_tiles(); ++t) {
      const Directory::TileView got = dir->tile(t);
      const Directory::TileView want = expected.tile(t);
      ASSERT_EQ(got.num_pages, want.num_pages);
      for (size_t i = 0; i < got.num_pages; ++i) {
        ASSERT_EQ(got.pages[i].handle, want.pages[i].handle);
        ASSERT_EQ(got.pages[i].min_delete_key, want.pages[i].min_delete_key);
        ASSERT_EQ(got.pages[i].max_delete_key, want.pages[i].max_delete_key);
        ASSERT_EQ(got.pages[i].bloom.ToString(),
                  want.pages[i].bloom.ToString());
      }
    }
    // The pinned directory is part of the reader's accounted memory.
    ASSERT_GE(opened.reader->ApproximateMemoryUsage(), table.directory.size());
  }
}

TEST(KiWiReaderTest, PlainFileHasNoDirectory) {
  Random64 rnd(3);
  BuildParams bp;
  bp.tile_pages = 1;  // KiWi off
  BuiltTable table;
  BuildKiWiTable(MakeEntries(&rnd, 100, 0, 50), bp, &table);
  ASSERT_TRUE(table.tiles.empty());
  OpenedTable opened;
  ASSERT_OK(OpenTable(table.contents, bp, &opened));
  const auto* table_reader = static_cast<BlockBasedTable*>(opened.reader.get());
  ASSERT_EQ(table_reader->TEST_GetKiWiDirectory(), nullptr);
}

TEST(KiWiReaderTest, CorruptDirectoryFailsOpen) {
  Random64 rnd(9);
  BuildParams bp;
  BuiltTable table;
  BuildKiWiTable(MakeEntries(&rnd, 200, 0, 50), bp, &table);
  ASSERT_GT(table.directory.size(), 8U);
  // Flip one payload byte of the directory: the checksum must catch it.
  std::string bad = table.contents;
  bad[table.directory_offset + table.directory.size() / 2] ^= 0x55;
  OpenedTable opened;
  ASSERT_EQ(OpenTable(bad, bp, &opened).code(), Status::kCorruption);
  // The untouched file still opens.
  OpenedTable intact;
  ASSERT_OK(OpenTable(table.contents, bp, &intact));
}
#endif  // !NDEBUG

}  // namespace kiwi
}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
