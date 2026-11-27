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
#include "db/pinned_iterators_manager.h"
#include "file/writable_file_writer.h"
#include "memory/arena.h"
#include "options/cf_options.h"
#include "port/stack_trace.h"
#include "rocksdb/convenience.h"
#include "rocksdb/env.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"
#include "table/block_based/block.h"
#include "table/block_based/block_based_table_builder.h"
#include "table/block_based/block_based_table_factory.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/block_based/filter_policy_internal.h"
#include "table/block_based/kiwi_tile.h"
#include "table/get_context.h"
#include "table/internal_iterator.h"
#include "table/meta_blocks.h"
#include "table/multiget_context.h"
#include "table/table_builder.h"
#include "test_util/sync_point.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"
#include "util/autovector.h"
#include "util/compression.h"
#include "util/random.h"
#include "util/string_util.h"

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

  ASSERT_OK(
      packer.Add(MakeInternalKey(1, 10, kTypeValue), MakeValue(5, 0, 16)));
  ASSERT_FALSE(packer.empty());
  ASSERT_EQ(packer.num_entries(), 1U);
  ASSERT_EQ(packer.last_key().ToString(), MakeInternalKey(1, 10, kTypeValue));
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
  ASSERT_EQ(packer.Add(MakeInternalKey(1, 1, kTypeValue), std::string(15, 'x'))
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
      const std::string ikey =
          MakeInternalKey(i, 1000 - i, deletion ? kTypeDeletion : kTypeValue);
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
    const size_t want_pages =
        std::clamp((packer.buffered_bytes() + page_size - 1) / page_size,
                   size_t{1}, static_cast<size_t>(max_pages));
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

TEST(KiWiDirectoryTest, V2TileFilterRoundTripAndCorruption) {
  Random64 rnd(4242);
  DirectoryBuilder builder;
  std::vector<std::string> raw_filters;
  std::vector<std::vector<DirectoryPage>> expected_pages;
  uint64_t offset = 0;
  for (size_t t = 0; t < 8; ++t) {
    const size_t num_pages = 1 + rnd.Uniform(8);
    TileXorFilterBuilder fb(/*fingerprint_bytes=*/1 +
                            static_cast<uint8_t>(t % 2));
    std::vector<DirectoryPage> pages(num_pages);
    for (size_t p = 0; p < num_pages; ++p) {
      pages[p].handle = BlockHandle(offset, 64 + rnd.Uniform(200));
      offset += pages[p].handle.size() + 5;
      pages[p].min_delete_key = p * 100;
      pages[p].max_delete_key = p * 100 + 99;
      for (size_t k = 0; k < 20; ++k) {
        fb.AddKey("tile" + std::to_string(t) + "_p" + std::to_string(p) + "_k" +
                      std::to_string(k),
                  static_cast<uint32_t>(p));
      }
    }
    std::string encoded_filter;
    ASSERT_OK(fb.Finish(num_pages, &encoded_filter));
    builder.AddTileWithXorFilter(pages, encoded_filter);
    raw_filters.push_back(std::move(encoded_filter));
    expected_pages.push_back(std::move(pages));
  }
  std::string encoded_dir;
  builder.Finish(&encoded_dir);

  Directory dir;
  ASSERT_OK(DecodeDirectory(encoded_dir, &dir));
  ASSERT_TRUE(dir.has_tile_filters());
  ASSERT_EQ(dir.num_tiles(), 8U);
  ASSERT_EQ(dir.directory_bytes(), encoded_dir.size());
  ASSERT_EQ(dir.filter_bytes(), builder.filter_bytes());
  for (size_t t = 0; t < 8; ++t) {
    const Directory::TileView tv = dir.tile(t);
    ASSERT_EQ(tv.num_pages, expected_pages[t].size());
    for (size_t p = 0; p < tv.num_pages; ++p) {
      ASSERT_EQ(tv.pages[p].handle, expected_pages[t][p].handle);
      ASSERT_EQ(tv.pages[p].min_delete_key,
                expected_pages[t][p].min_delete_key);
      ASSERT_EQ(tv.pages[p].max_delete_key,
                expected_pages[t][p].max_delete_key);
      uint32_t got_page = 9999;
      ASSERT_TRUE(dir.TileMayMatch(
          t, "tile" + std::to_string(t) + "_p" + std::to_string(p) + "_k5",
          &got_page));
      ASSERT_EQ(got_page, p);
    }
  }

  for (size_t len = 0; len < encoded_dir.size(); ++len) {
    Directory bad_dir;
    ASSERT_EQ(DecodeDirectory(encoded_dir.substr(0, len), &bad_dir).code(),
              Status::kCorruption)
        << "len=" << len;
  }
  for (int iter = 0; iter < 1000; ++iter) {
    std::string bad = encoded_dir;
    bad[rnd.Uniform(bad.size())] ^= static_cast<char>(1 + rnd.Uniform(255));
    Directory bad_dir;
    Status s = DecodeDirectory(bad, &bad_dir);
    if (!s.ok()) {
      ASSERT_EQ(s.code(), Status::kCorruption);
      ASSERT_EQ(bad_dir.num_tiles(), 0U);
    }
  }
}

TEST(TileXorFilterTest, RoundTripAndPageLookup) {
  for (uint8_t fp_bytes :
       {uint8_t{1}, uint8_t{2}, uint8_t{3}, uint8_t{4}, uint8_t{8}}) {
    for (size_t num_pages : {size_t{1}, size_t{2}, size_t{3}, size_t{4},
                             size_t{5}, size_t{8}, size_t{16}}) {
      TileXorFilterBuilder builder(fp_bytes);
      const size_t keys_per_page = 64;
      for (size_t p = 0; p < num_pages; ++p) {
        for (size_t k = 0; k < keys_per_page; ++k) {
          builder.AddKey("p" + std::to_string(p) + "_k" + std::to_string(k),
                         static_cast<uint32_t>(p));
        }
      }
      std::string encoded;
      ASSERT_OK(builder.Finish(num_pages, &encoded));
      ASSERT_TRUE(builder.empty());

      TileXorFilterReader reader;
      ASSERT_OK(reader.Init(encoded, num_pages));
      const uint8_t expected_page_bits = CeilLog2(num_pages);
      const uint8_t expected_fp_bits = static_cast<uint8_t>(std::min<uint32_t>(
          uint32_t{fp_bytes} * 8U, 64U - expected_page_bits));
      ASSERT_EQ(reader.page_bits(), expected_page_bits);
      ASSERT_EQ(reader.fp_bits(), expected_fp_bits);
      ASSERT_EQ(reader.slot_bits(), expected_fp_bits + expected_page_bits);

      for (size_t p = 0; p < num_pages; ++p) {
        for (size_t k = 0; k < keys_per_page; ++k) {
          uint32_t got_page = 9999;
          ASSERT_TRUE(reader.MayMatchPage(
              "p" + std::to_string(p) + "_k" + std::to_string(k), &got_page));
          ASSERT_EQ(got_page, p);
        }
      }

      // Check false positive rate on absent keys.
      size_t false_positives = 0;
      const size_t num_absent = 4000;
      for (size_t k = 0; k < num_absent; ++k) {
        uint32_t got_page = 9999;
        if (reader.MayMatchPage("absent_" + std::to_string(k), &got_page)) {
          ASSERT_LT(got_page, num_pages);
          ++false_positives;
        }
      }
      if (fp_bytes >= 2) {
        ASSERT_LE(false_positives, 10U);
      } else {
        // 8-bit fingerprint -> ~1/256 (~15-16 out of 4000).
        ASSERT_LT(false_positives, 60U);
      }
    }
  }
}

TEST(TileXorFilterTest, DuplicateKeyKeepsFirstPosition) {
  TileXorFilterBuilder builder(2);
  builder.AddKey("dup", 1);
  builder.AddKey("dup", 5);
  // Explicit order: page 6 added first with order 20, page 2 added later with
  // smaller order 10 (as when pages are visited in delete-key order).
  builder.AddKey("ordered_dup", 6, /*order=*/20);
  builder.AddKey("ordered_dup", 2, /*order=*/10);
  builder.AddKey("other", 3);
  std::string encoded;
  ASSERT_OK(builder.Finish(8, &encoded));
  TileXorFilterReader reader;
  ASSERT_OK(reader.Init(encoded, 8));
  uint32_t page = 99;
  ASSERT_TRUE(reader.MayMatchPage("dup", &page));
  ASSERT_EQ(page, 1U);
  ASSERT_TRUE(reader.MayMatchPage("ordered_dup", &page));
  ASSERT_EQ(page, 2U);
  ASSERT_TRUE(reader.MayMatchPage("other", &page));
  ASSERT_EQ(page, 3U);
}

TEST(TileXorFilterTest, RejectsMalformedAndTruncatedFilters) {
  TileXorFilterBuilder builder(1);
  for (size_t i = 0; i < 40; ++i) {
    builder.AddKey("k" + std::to_string(i), static_cast<uint32_t>(i % 5));
  }
  std::string encoded;
  ASSERT_OK(builder.Finish(5, &encoded));

  TileXorFilterReader reader;
  for (size_t len = 0; len < encoded.size(); ++len) {
    ASSERT_EQ(reader.Init(encoded.substr(0, len), 5).code(),
              Status::kCorruption)
        << "len=" << len;
  }
  ASSERT_EQ(reader.Init(encoded + "x", 5).code(), Status::kCorruption);
  ASSERT_EQ(reader.Init(encoded, 4).code(), Status::kCorruption);

  std::string bad = encoded;
  bad[0] = 99;  // unknown version
  ASSERT_EQ(reader.Init(bad, 5).code(), Status::kCorruption);

  bad = encoded;
  bad[2] = 0;  // fp_bits == 0
  ASSERT_EQ(reader.Init(bad, 5).code(), Status::kCorruption);

  bad = encoded;
  bad[2] = 63;  // fp_bits + page_bits > 64 (63 + 3 = 66)
  ASSERT_EQ(reader.Init(bad, 5).code(), Status::kCorruption);

  bad = encoded;
  bad[4] = bad[5] = bad[6] = bad[7] = 0;  // seg_len == 0
  ASSERT_EQ(reader.Init(bad, 5).code(), Status::kCorruption);
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
  bool filters = true;  // per-page Bloom filters (or tile/SST XOR if > 0 below)
  int xor_filter_bytes = 0;
  bool xor_file_filter = false;
  const Comparator* comparator = BytewiseComparator();
};

std::shared_ptr<const FilterPolicy> MakeBuildFilterPolicy(
    const BuildParams& bp) {
  if (!bp.filters) {
    return nullptr;
  }
  if (bp.xor_filter_bytes <= 0) {
    return std::shared_ptr<const FilterPolicy>(NewBloomFilterPolicy(10));
  }
  if (!bp.xor_file_filter) {
    return std::shared_ptr<const FilterPolicy>(
        NewXorFilterPolicy(bp.xor_filter_bytes));
  }
  std::string cfg = "{id=xorfilter:" + std::to_string(bp.xor_filter_bytes) +
                    ";kiwi_file_filter=true}";
  std::shared_ptr<const FilterPolicy> p;
  EXPECT_OK(FilterPolicy::CreateFromString(ConfigOptions(), cfg, &p));
  return p;
}

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
  bbto.filter_policy = MakeBuildFilterPolicy(bp);
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
      TableBuilderOptions(
          ioptions, moptions, read_options, write_options, ikc,
          &internal_tbl_prop_coll_factories, bp.compression,
          options.compression_opts,
          TablePropertiesCollectorFactory::Context::kUnknownColumnFamily,
          column_family_name, /*level=*/-1, kUnknownNewestKeyTime),
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
    ASSERT_OK(FindMetaBlockInFile(
        file_reader.get(), out->contents.size(), kBlockBasedTableMagicNumber,
        ioptions, read_options, kDirectoryBlockName, &directory_handle));
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
                 OpenedTable* out, uint64_t delete_floor = 0) {
  out->options.comparator = bp.comparator;
  out->options.statistics = CreateDBStatistics();
  out->ioptions = std::make_unique<ImmutableOptions>(out->options);
  out->moptions = std::make_unique<MutableCFOptions>(out->options);
  out->ikc = std::make_unique<InternalKeyComparator>(out->options.comparator);
  BlockBasedTableOptions bbto;
  bbto.block_size = bp.block_size;
  bbto.filter_policy = MakeBuildFilterPolicy(bp);
  out->factory = std::make_unique<BlockBasedTableFactory>(bbto);
  std::unique_ptr<FSRandomAccessFile> source(
      new test::StringSource(contents, 1, /*mmap=*/false));
  std::unique_ptr<RandomAccessFileReader> file_reader(
      new RandomAccessFileReader(std::move(source), "test"));
  TableReaderOptions reader_options(
      *out->ioptions, out->moptions->prefix_extractor,
      out->moptions->compression_manager.get(), EnvOptions(), *out->ikc,
      /*block_protection_bytes_per_key=*/0,
      /*skip_filters=*/false, /*immortal=*/false,
      /*force_direct_prefetch=*/false, /*level=*/-1,
      /*block_cache_tracer=*/nullptr, out->moptions->write_buffer_size, "",
      /*cur_file_num=*/1, kNullUniqueId64x2, /*largest_seqno=*/0);
  reader_options.kiwi_delete_floor = delete_floor;
  return out->factory->NewTableReader(reader_options, std::move(file_reader),
                                      contents.size(), &out->reader);
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
      e.ikey =
          MakeInternalKey(k, 1000 - v, deletion ? kTypeDeletion : kTypeValue);
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
        ASSERT_EQ(tile.handle.offset(), prev.offset() + prev.size() +
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
      TableBuilderOptions(
          ioptions, moptions, read_options, write_options, ikc,
          &internal_tbl_prop_coll_factories, kNoCompression,
          options.compression_opts,
          TablePropertiesCollectorFactory::Context::kUnknownColumnFamily,
          column_family_name, /*level=*/-1, kUnknownNewestKeyTime),
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
      TableBuilderOptions(
          ioptions, moptions, read_options, write_options, ikc,
          &internal_tbl_prop_coll_factories, kNoCompression,
          options.compression_opts,
          TablePropertiesCollectorFactory::Context::kUnknownColumnFamily,
          column_family_name, /*level=*/-1, kUnknownNewestKeyTime),
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
      TableBuilderOptions(
          ioptions, moptions, read_options, write_options, ikc,
          &internal_tbl_prop_coll_factories, kNoCompression,
          options.compression_opts,
          TablePropertiesCollectorFactory::Context::kUnknownColumnFamily,
          column_family_name, /*level=*/-1, kUnknownNewestKeyTime),
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

TEST(KiWiReaderTest, DeleteFloorIsHandedToKiWiFilesOnly) {
  Random64 rnd(5);
  BuildParams bp;
  BuiltTable kiwi_table;
  BuildKiWiTable(MakeEntries(&rnd, 100, 3, 50), bp, &kiwi_table);
  OpenedTable opened;
  ASSERT_OK(OpenTable(kiwi_table.contents, bp, &opened, /*floor=*/777));
  const auto* reader = static_cast<BlockBasedTable*>(opened.reader.get());
  ASSERT_EQ(reader->KiWiDeleteFloor(), 777U);
  OpenedTable no_floor;
  ASSERT_OK(OpenTable(kiwi_table.contents, bp, &no_floor));
  ASSERT_EQ(no_floor.reader->KiWiDeleteFloor(), 0U);

  // A floor on a file that has no tiles is metadata corruption: fail the open
  // instead of ignoring a delete.
  BuildParams plain_bp = bp;
  plain_bp.tile_pages = 1;
  BuiltTable plain_table;
  BuildKiWiTable(MakeEntries(&rnd, 100, 0, 50), plain_bp, &plain_table);
  OpenedTable plain;
  ASSERT_EQ(OpenTable(plain_table.contents, plain_bp, &plain, 777).code(),
            Status::kCorruption);
  ASSERT_EQ(plain.reader, nullptr);
  OpenedTable plain_ok;
  ASSERT_OK(OpenTable(plain_table.contents, plain_bp, &plain_ok));
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
namespace {

InternalIterator* NewIter(TableReader* reader, Arena* arena = nullptr) {
  // ReadOptions must outlive the iterator.
  static const ReadOptions kReadOptions;
  return reader->NewIterator(kReadOptions, /*prefix_extractor=*/nullptr, arena,
                             /*skip_filters=*/false,
                             TableReaderCaller::kUncategorized);
}

// A table pair built from the same entries: a plain file and a KiWi file.
struct TablePair {
  BuiltTable plain_table;
  BuiltTable kiwi_table;
  OpenedTable plain;
  OpenedTable kiwi;
};

void BuildTablePair(const std::vector<Entry>& entries,
                    const BuildParams& kiwi_bp, TablePair* out) {
  BuildParams plain_bp = kiwi_bp;
  plain_bp.tile_pages = 1;
  BuildKiWiTable(entries, plain_bp, &out->plain_table);
  BuildKiWiTable(entries, kiwi_bp, &out->kiwi_table);
  ASSERT_OK(OpenTable(out->plain_table.contents, plain_bp, &out->plain));
  ASSERT_OK(OpenTable(out->kiwi_table.contents, kiwi_bp, &out->kiwi));
}

std::string MakeInternalKeyFromUserKey(const std::string& user_key,
                                       SequenceNumber seq) {
  return InternalKey(user_key, seq, kTypeValue).Encode().ToString();
}

// Same position and same contents.
void ExpectSameIterator(InternalIterator* expected, InternalIterator* actual) {
  ASSERT_EQ(expected->Valid(), actual->Valid());
  if (expected->Valid()) {
    ASSERT_EQ(expected->key().ToString(), actual->key().ToString());
    ASSERT_EQ(expected->value().ToString(), actual->value().ToString());
  }
  ASSERT_OK(actual->status());
}

// Applies the same random mix of seeks, Next and Prev to both iterators
// (direction changes inside a tile and across tile boundaries included) and
// compares them after every step.
void ExpectSameRandomWalk(InternalIterator* expected, InternalIterator* actual,
                          Random64* rnd, size_t num_keys, int num_ops) {
  for (int op = 0; op < num_ops; ++op) {
    const uint64_t choice = rnd->Uniform(20);
    if (!expected->Valid() || choice == 0) {
      // Reposition; also the only way out of an invalid state.
      char buf[16];
      snprintf(buf, sizeof(buf), "key%08" PRIu64, rnd->Uniform(num_keys + 2));
      const std::string target =
          MakeInternalKeyFromUserKey(buf, 1000 - rnd->Uniform(5));
      switch (rnd->Uniform(4)) {
        case 0:
          expected->SeekToFirst();
          actual->SeekToFirst();
          break;
        case 1:
          expected->SeekToLast();
          actual->SeekToLast();
          break;
        case 2:
          expected->Seek(target);
          actual->Seek(target);
          break;
        default:
          expected->SeekForPrev(target);
          actual->SeekForPrev(target);
      }
    } else if (choice < 11) {
      expected->Next();
      actual->Next();
    } else {
      expected->Prev();
      actual->Prev();
    }
    ExpectSameIterator(expected, actual);
  }
}

}  // namespace

TEST(KiWiIteratorTest, ForwardScanMatchesPlainTable) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);
  for (int iter = 0; iter < 15; ++iter) {
    BuildParams bp;
    bp.tile_pages = 2 + static_cast<uint32_t>(rnd.Uniform(8));
    bp.block_size = 64 + rnd.Uniform(400);
    bp.delete_key_offset = static_cast<uint32_t>(rnd.Uniform(8));
    TablePair tables;
    BuildTablePair(
        MakeEntries(&rnd, 1 + rnd.Uniform(400), bp.delete_key_offset, 60), bp,
        &tables);

    // Both heap and arena allocation.
    Arena arena;
    std::unique_ptr<InternalIterator> expected(
        NewIter(tables.plain.reader.get()));
    InternalIterator* actual_raw =
        NewIter(tables.kiwi.reader.get(), rnd.OneIn(2) ? &arena : nullptr);
    const bool in_arena = arena.MemoryAllocatedBytes() > 0;
    ASSERT_FALSE(actual_raw->Valid());  // initially invalid

    size_t count = 0;
    for (expected->SeekToFirst(), actual_raw->SeekToFirst(); expected->Valid();
         expected->Next(), actual_raw->Next()) {
      ExpectSameIterator(expected.get(), actual_raw);
      ++count;
    }
    ExpectSameIterator(expected.get(), actual_raw);  // both at the end
    ASSERT_FALSE(actual_raw->Valid());
    ASSERT_GT(count, 0U);
    if (in_arena) {
      actual_raw->~InternalIterator();
    } else {
      delete actual_raw;
    }
  }
}

TEST(KiWiIteratorTest, SeekMatchesPlainTable) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);
  for (int iter = 0; iter < 10; ++iter) {
    BuildParams bp;
    bp.tile_pages = 2 + static_cast<uint32_t>(rnd.Uniform(8));
    bp.block_size = 64 + rnd.Uniform(300);
    const size_t num_keys = 20 + rnd.Uniform(300);
    TablePair tables;
    BuildTablePair(MakeEntries(&rnd, num_keys, 0, 60), bp, &tables);
    std::unique_ptr<InternalIterator> expected(
        NewIter(tables.plain.reader.get()));
    std::unique_ptr<InternalIterator> actual(NewIter(tables.kiwi.reader.get()));

    for (int probe = 0; probe < 300; ++probe) {
      // Existing keys, keys between them, before the first and after the last.
      const uint64_t id = rnd.Uniform(num_keys + 2);
      char buf[16];
      snprintf(buf, sizeof(buf), "key%08" PRIu64, id);
      std::string user_key(buf);
      if (rnd.OneIn(3)) {
        user_key.append("x");  // falls between two keys
      }
      const std::string target =
          MakeInternalKeyFromUserKey(user_key, 1000 - rnd.Uniform(5));
      expected->Seek(target);
      actual->Seek(target);
      ExpectSameIterator(expected.get(), actual.get());
      // Keep going: tile boundaries are crossed when stepping.
      for (int step = 0; step < 40 && expected->Valid(); ++step) {
        expected->Next();
        actual->Next();
        ExpectSameIterator(expected.get(), actual.get());
      }
    }
    // Seeking again after reaching the end works.
    expected->SeekToFirst();
    actual->SeekToFirst();
    ExpectSameIterator(expected.get(), actual.get());
  }
}

TEST(KiWiIteratorTest, BackwardScanMatchesPlainTable) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);
  for (int iter = 0; iter < 15; ++iter) {
    BuildParams bp;
    bp.tile_pages = 2 + static_cast<uint32_t>(rnd.Uniform(8));
    bp.block_size = 64 + rnd.Uniform(400);
    TablePair tables;
    BuildTablePair(MakeEntries(&rnd, 1 + rnd.Uniform(400), 0, 60), bp, &tables);
    std::unique_ptr<InternalIterator> expected(
        NewIter(tables.plain.reader.get()));
    std::unique_ptr<InternalIterator> actual(NewIter(tables.kiwi.reader.get()));
    size_t count = 0;
    for (expected->SeekToLast(), actual->SeekToLast(); expected->Valid();
         expected->Prev(), actual->Prev()) {
      ExpectSameIterator(expected.get(), actual.get());
      ++count;
    }
    ExpectSameIterator(expected.get(), actual.get());
    ASSERT_GT(count, 0U);
  }
}

TEST(KiWiIteratorTest, SeekForPrevMatchesPlainTable) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);
  for (int iter = 0; iter < 10; ++iter) {
    BuildParams bp;
    bp.tile_pages = 2 + static_cast<uint32_t>(rnd.Uniform(8));
    bp.block_size = 64 + rnd.Uniform(300);
    const size_t num_keys = 20 + rnd.Uniform(300);
    TablePair tables;
    BuildTablePair(MakeEntries(&rnd, num_keys, 0, 60), bp, &tables);
    std::unique_ptr<InternalIterator> expected(
        NewIter(tables.plain.reader.get()));
    std::unique_ptr<InternalIterator> actual(NewIter(tables.kiwi.reader.get()));
    for (int probe = 0; probe < 300; ++probe) {
      // Existing keys, keys between them, before the first and after the last.
      char buf[16];
      snprintf(buf, sizeof(buf), "key%08" PRIu64, rnd.Uniform(num_keys + 2));
      std::string user_key(buf);
      if (rnd.OneIn(3)) {
        user_key.append("x");
      }
      const std::string target =
          MakeInternalKeyFromUserKey(user_key, 1000 - rnd.Uniform(5));
      expected->SeekForPrev(target);
      actual->SeekForPrev(target);
      ExpectSameIterator(expected.get(), actual.get());
      for (int step = 0; step < 40 && expected->Valid(); ++step) {
        expected->Prev();
        actual->Prev();
        ExpectSameIterator(expected.get(), actual.get());
      }
    }
  }
}

// Mixes every operation, including direction changes in the middle of a tile
// and across tile boundaries, and checks each step against the plain table.
TEST(KiWiIteratorTest, RandomWalkMatchesPlainTable) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);
  for (int iter = 0; iter < 10; ++iter) {
    BuildParams bp;
    bp.tile_pages = 2 + static_cast<uint32_t>(rnd.Uniform(8));
    bp.block_size = 64 + rnd.Uniform(300);
    const size_t num_keys = 20 + rnd.Uniform(200);
    TablePair tables;
    BuildTablePair(MakeEntries(&rnd, num_keys, 0, 60), bp, &tables);
    std::unique_ptr<InternalIterator> expected(
        NewIter(tables.plain.reader.get()));
    std::unique_ptr<InternalIterator> actual(NewIter(tables.kiwi.reader.get()));
    ExpectSameRandomWalk(expected.get(), actual.get(), &rnd, num_keys, 3000);
  }
}

// DBIter walks backward over the versions of a key only if values stay valid
// after the iterator moved on, even to another tile.
TEST(KiWiIteratorTest, ValuesStayPinnedAfterLeavingATile) {
  Random64 rnd(61);
  BuildParams bp;
  bp.tile_pages = 4;
  bp.block_size = 128;
  TablePair tables;
  BuildTablePair(MakeEntries(&rnd, 200, 0, 40), bp, &tables);
  ASSERT_GT(tables.kiwi_table.tiles.size(), 5U);

  PinnedIteratorsManager pinned_iters_mgr;
  pinned_iters_mgr.StartPinning();
  std::unique_ptr<InternalIterator> it(NewIter(tables.kiwi.reader.get()));
  it->SetPinnedItersMgr(&pinned_iters_mgr);
  std::vector<Slice> values;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ASSERT_TRUE(it->IsValuePinned());
    values.push_back(it->value());
  }
  ASSERT_OK(it->status());

  // Slices taken from tiles that were left are still readable and correct.
  std::unique_ptr<InternalIterator> expected(
      NewIter(tables.plain.reader.get()));
  size_t i = 0;
  for (expected->SeekToFirst(); expected->Valid(); expected->Next(), ++i) {
    ASSERT_LT(i, values.size());
    ASSERT_EQ(values[i].ToString(), expected->value().ToString());
  }
  ASSERT_EQ(i, values.size());
  // Without pinning nothing is reported as pinned.
  pinned_iters_mgr.ReleasePinnedData();
  it->SeekToFirst();
  ASSERT_FALSE(it->IsValuePinned());
}

TEST(KiWiIteratorTest, CorruptPageSurfacesAsError) {
  Random64 rnd(21);
  BuildParams bp;
  TablePair tables;
  BuildTablePair(MakeEntries(&rnd, 200, 0, 50), bp, &tables);
  ASSERT_GT(tables.kiwi_table.tiles.size(), 3U);

  // Corrupt one page in the middle of the file.
  const TileLayout& tile =
      tables.kiwi_table.tiles[tables.kiwi_table.tiles.size() / 2];
  std::string bad = tables.kiwi_table.contents;
  const BlockHandle& page = tile.pages[tile.pages.size() / 2];
  bad[page.offset() + page.size() / 2] ^= 0x55;
  OpenedTable opened;
  ASSERT_OK(OpenTable(bad, bp, &opened));  // pages are read lazily

  std::unique_ptr<InternalIterator> it(NewIter(opened.reader.get()));
  size_t seen = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ++seen;
  }
  // The scan stops with an error at the corrupted tile; it never silently
  // skips it.
  ASSERT_EQ(it->status().code(), Status::kCorruption);
  ASSERT_FALSE(it->Valid());
  ASSERT_LT(seen, tables.kiwi_table.props->num_entries);
}

namespace {

struct GetResult {
  GetContext::GetState state = GetContext::kNotFound;
  std::string value;
};

// Looks up `user_key` as of `snapshot_seq`.
Status DoGet(const OpenedTable& table, const std::string& user_key,
             SequenceNumber snapshot_seq, GetResult* out) {
  PinnableSlice value;
  const InternalKey lkey(user_key, snapshot_seq, kValueTypeForSeek);
  GetContext get_context(BytewiseComparator(), nullptr, nullptr,
                         table.options.statistics.get(), GetContext::kNotFound,
                         user_key, &value, nullptr, nullptr, nullptr, nullptr,
                         true /* do_merge */, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr);
  static const ReadOptions kReadOptions;
  Status s =
      table.reader->Get(kReadOptions, lkey.Encode(), &get_context, nullptr);
  get_context.ReportCounters();  // as Version::Get() does
  out->state = get_context.State();
  out->value = value.ToString();
  return s;
}

// Keys with 1..max_versions versions each, newest first.
std::vector<Entry> MakeVersionedEntries(Random64* rnd, size_t num_keys,
                                        size_t max_versions) {
  std::vector<Entry> entries;
  for (size_t k = 0; k < num_keys; ++k) {
    const size_t versions = 1 + rnd->Uniform(max_versions);
    for (size_t v = 0; v < versions; ++v) {
      const bool deletion = rnd->OneIn(6);
      Entry e;
      e.ikey =
          MakeInternalKey(k, 1000 - v, deletion ? kTypeDeletion : kTypeValue);
      if (!deletion) {
        e.value = MakeValue(rnd->Next(), 0, kDeleteKeySize + rnd->Uniform(40));
      }
      entries.push_back(std::move(e));
    }
  }
  return entries;
}

}  // namespace

TEST(KiWiGetTest, GetMatchesPlainTable) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);
  for (int iter = 0; iter < 10; ++iter) {
    BuildParams bp;
    bp.tile_pages = 2 + static_cast<uint32_t>(rnd.Uniform(8));
    bp.block_size = 64 + rnd.Uniform(300);
    bp.filters = !rnd.OneIn(4);
    const size_t num_keys = 10 + rnd.Uniform(60);
    // Up to 60 versions: many keys span several pages and tiles.
    TablePair tables;
    BuildTablePair(MakeVersionedEntries(&rnd, num_keys, 60), bp, &tables);

    for (int probe = 0; probe < 400; ++probe) {
      char buf[16];
      snprintf(buf, sizeof(buf), "key%08" PRIu64, rnd.Uniform(num_keys + 2));
      std::string user_key(buf);
      if (rnd.OneIn(4)) {
        user_key.append("x");  // between two keys: absent
      }
      const SequenceNumber snapshot =
          rnd.OneIn(4) ? kMaxSequenceNumber : 1000 - rnd.Uniform(70);
      GetResult expected;
      GetResult actual;
      ASSERT_OK(DoGet(tables.plain, user_key, snapshot, &expected));
      ASSERT_OK(DoGet(tables.kiwi, user_key, snapshot, &actual));
      ASSERT_EQ(expected.state, actual.state) << user_key << "@" << snapshot;
      ASSERT_EQ(expected.value, actual.value) << user_key << "@" << snapshot;
    }
  }
}

TEST(KiWiGetTest, PageFiltersAvoidReadingPages) {
  Random64 rnd(17);
  BuildParams bp;
  bp.tile_pages = 8;
  bp.block_size = 256;
  const std::vector<Entry> entries = MakeVersionedEntries(&rnd, 400, 1);
  auto data_block_accesses = [](const OpenedTable& t) {
    Statistics* stats = t.options.statistics.get();
    return stats->getTickerCount(BLOCK_CACHE_DATA_HIT) +
           stats->getTickerCount(BLOCK_CACHE_DATA_MISS);
  };

  // Absent keys that fall inside the key range, so a tile is always found.
  auto lookup_absent = [&](OpenedTable* opened) {
    for (uint64_t k = 0; k < 400; ++k) {
      char buf[16];
      snprintf(buf, sizeof(buf), "key%08" PRIu64 "x", k);
      GetResult r;
      ASSERT_OK(DoGet(*opened, buf, kMaxSequenceNumber, &r));
      ASSERT_EQ(r.state, GetContext::kNotFound);
    }
  };

  BuiltTable with_filters;
  bp.filters = true;
  BuildKiWiTable(entries, bp, &with_filters);
  OpenedTable filtered;
  ASSERT_OK(OpenTable(with_filters.contents, bp, &filtered));
  lookup_absent(&filtered);

  BuiltTable without_filters;
  bp.filters = false;
  BuildKiWiTable(entries, bp, &without_filters);
  OpenedTable unfiltered;
  ASSERT_OK(OpenTable(without_filters.contents, bp, &unfiltered));
  lookup_absent(&unfiltered);

  // Without filters every lookup opens all pages of a tile; with them almost
  // none (the Bloom false positive rate is about 1%).
  ASSERT_GT(data_block_accesses(unfiltered), 400U * 4);
  ASSERT_LT(data_block_accesses(filtered), data_block_accesses(unfiltered) / 4);
  ASSERT_GT(filtered.options.statistics->getTickerCount(BLOOM_FILTER_USEFUL),
            0U);
  ASSERT_EQ(unfiltered.options.statistics->getTickerCount(BLOOM_FILTER_USEFUL),
            0U);
}

TEST(KiWiGetTest, PresentKeysCountAsTruePositives) {
  Random64 rnd(23);
  BuildParams bp;
  bp.tile_pages = 4;
  BuiltTable table;
  const std::vector<Entry> entries = MakeVersionedEntries(&rnd, 100, 1);
  BuildKiWiTable(entries, bp, &table);
  OpenedTable opened;
  ASSERT_OK(OpenTable(table.contents, bp, &opened));
  size_t found = 0;
  for (uint64_t k = 0; k < 100; ++k) {
    char buf[16];
    snprintf(buf, sizeof(buf), "key%08" PRIu64, k);
    GetResult r;
    ASSERT_OK(DoGet(opened, buf, kMaxSequenceNumber, &r));
    found += r.state == GetContext::kFound ? 1 : 0;
  }
  ASSERT_GT(found, 0U);
  ASSERT_GE(opened.options.statistics->getTickerCount(
                BLOOM_FILTER_FULL_TRUE_POSITIVE),
            found);
}

// MultiGet on a KiWi file is a loop of Get(): same results for every key.
TEST(KiWiGetTest, MultiGetMatchesGet) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);
  BuildParams bp;
  bp.tile_pages = 4;
  const size_t num_keys = 120;
  TablePair tables;
  BuildTablePair(MakeVersionedEntries(&rnd, num_keys, 20), bp, &tables);

  // A sorted batch of present and absent keys.
  std::set<std::string> batch;
  while (batch.size() < MultiGetContext::MAX_BATCH_SIZE) {
    char buf[16];
    snprintf(buf, sizeof(buf), "key%08" PRIu64, rnd.Uniform(num_keys + 2));
    batch.insert(rnd.OneIn(4) ? std::string(buf) + "x" : std::string(buf));
  }
  const std::vector<std::string> user_keys(batch.begin(), batch.end());
  // KeyContext points at its key, so the slices must outlive the batch.
  const std::vector<Slice> key_slices(user_keys.begin(), user_keys.end());
  const SequenceNumber snapshot = 1000 - rnd.Uniform(10);

  for (OpenedTable* table : {&tables.kiwi, &tables.plain}) {
    std::vector<PinnableSlice> values(user_keys.size());
    std::vector<Status> statuses(user_keys.size());
    autovector<GetContext, MultiGetContext::MAX_BATCH_SIZE> get_contexts;
    autovector<KeyContext, MultiGetContext::MAX_BATCH_SIZE> key_contexts;
    autovector<KeyContext*, MultiGetContext::MAX_BATCH_SIZE> sorted_keys;
    for (size_t i = 0; i < user_keys.size(); ++i) {
      get_contexts.emplace_back(BytewiseComparator(), nullptr, nullptr, nullptr,
                                GetContext::kNotFound, user_keys[i], &values[i],
                                nullptr, nullptr, nullptr, nullptr,
                                true /* do_merge */, nullptr, nullptr, nullptr,
                                nullptr, nullptr, nullptr);
      key_contexts.emplace_back(nullptr, key_slices[i], &values[i], nullptr,
                                nullptr, &statuses[i]);
      key_contexts.back().get_context = &get_contexts.back();
    }
    for (auto& key_context : key_contexts) {
      sorted_keys.emplace_back(&key_context);
    }
    static const ReadOptions kReadOptions;
    MultiGetContext ctx(&sorted_keys, 0, sorted_keys.size(), snapshot,
                        kReadOptions, FileSystem::Default().get(), nullptr);
    MultiGetContext::Range range = ctx.GetMultiGetRange();
    table->reader->MultiGet(kReadOptions, &range, nullptr);

    for (size_t i = 0; i < user_keys.size(); ++i) {
      ASSERT_OK(statuses[i]);
      GetResult expected;
      ASSERT_OK(DoGet(tables.plain, user_keys[i], snapshot, &expected));
      ASSERT_EQ(get_contexts[i].State(), expected.state) << user_keys[i];
      ASSERT_EQ(values[i].ToString(), expected.value) << user_keys[i];
    }
  }
}

TEST(KiWiGetTest, CorruptPageFailsGetOfItsKeysOnly) {
  Random64 rnd(29);
  BuildParams bp;
  BuiltTable table;
  const std::vector<Entry> entries = MakeVersionedEntries(&rnd, 300, 1);
  BuildKiWiTable(entries, bp, &table);
  ASSERT_GT(table.tiles.size(), 4U);
  // Corrupt every page of one tile in the middle: all its keys are affected.
  const TileLayout& tile = table.tiles[table.tiles.size() / 2];
  std::string bad = table.contents;
  for (const BlockHandle& page : tile.pages) {
    bad[page.offset() + page.size() / 2] ^= 0x55;
  }
  OpenedTable opened;
  ASSERT_OK(OpenTable(bad, bp, &opened));

  OpenedTable intact;
  ASSERT_OK(OpenTable(table.contents, bp, &intact));
  size_t corrupt = 0;
  size_t ok = 0;
  for (uint64_t k = 0; k < 300; ++k) {
    char buf[16];
    snprintf(buf, sizeof(buf), "key%08" PRIu64, k);
    GetResult expected;
    ASSERT_OK(DoGet(intact, buf, kMaxSequenceNumber, &expected));
    GetResult actual;
    Status s = DoGet(opened, buf, kMaxSequenceNumber, &actual);
    if (s.ok()) {
      // Untouched tiles still answer correctly.
      ASSERT_EQ(expected.state, actual.state);
      ASSERT_EQ(expected.value, actual.value);
      ++ok;
    } else {
      ASSERT_EQ(s.code(), Status::kCorruption);
      ++corrupt;
    }
  }
  ASSERT_GT(corrupt, 0U);
  ASSERT_GT(ok, corrupt);
}

// Everything that walks the index (checksum verification, dumps, prefetch,
// size estimates) must treat an index entry of a KiWi file as a tile of pages.
TEST(KiWiAuditTest, VerifyChecksumChecksEveryPage) {
  Random64 rnd(41);
  BuildParams bp;
  BuiltTable table;
  BuildKiWiTable(MakeEntries(&rnd, 300, 0, 50), bp, &table);
  ASSERT_GT(table.tiles.size(), 3U);
  OpenedTable intact;
  ASSERT_OK(OpenTable(table.contents, bp, &intact));
  // A tile is read as its pages, not as one oversized block.
  ASSERT_OK(intact.reader->VerifyChecksum(
      ReadOptions(), TableReaderCaller::kUserVerifyChecksum));

  // Every single page is covered: corrupting any of them is detected.
  for (const TileLayout& tile : table.tiles) {
    for (const BlockHandle& page : tile.pages) {
      std::string bad = table.contents;
      bad[page.offset() + page.size() / 2] ^= 0x55;
      OpenedTable opened;
      ASSERT_OK(OpenTable(bad, bp, &opened));
      ASSERT_EQ(opened.reader
                    ->VerifyChecksum(ReadOptions(),
                                     TableReaderCaller::kUserVerifyChecksum)
                    .code(),
                Status::kCorruption);
    }
  }
}

TEST(KiWiAuditTest, KVPairsAndDumpCoverEveryPage) {
  Random64 rnd(43);
  BuildParams bp;
  const std::vector<Entry> entries = MakeEntries(&rnd, 200, 0, 50);
  BuiltTable table;
  BuildKiWiTable(entries, bp, &table);
  OpenedTable opened;
  ASSERT_OK(OpenTable(table.contents, bp, &opened));
  size_t num_pages = 0;
  for (const TileLayout& tile : table.tiles) {
    num_pages += tile.pages.size();
  }

  auto* reader = static_cast<BlockBasedTable*>(opened.reader.get());
  std::vector<KVPairBlock> blocks;
  ASSERT_OK(reader->GetKVPairsFromDataBlocks(ReadOptions(), &blocks));
  ASSERT_EQ(blocks.size(), num_pages);
  std::map<std::string, std::string> seen;
  for (const KVPairBlock& block : blocks) {
    for (const auto& kv : block) {
      ASSERT_TRUE(seen.insert(kv).second);
    }
  }
  std::map<std::string, std::string> expected;
  for (const Entry& e : entries) {
    expected[e.ikey] = e.value;
  }
  ASSERT_EQ(seen, expected);

  // The human readable dump lists every page and every entry.
  const std::string path = test::PerThreadDBPath("kiwi_dump_test");
  std::unique_ptr<WritableFile> out;
  ASSERT_OK(Env::Default()->NewWritableFile(path, &out, EnvOptions()));
  ASSERT_OK(reader->DumpTable(out.get()));
  ASSERT_OK(out->Close());
  std::string dump;
  ASSERT_OK(ReadFileToString(Env::Default(), path, &dump));
  ASSERT_OK(Env::Default()->DeleteFile(path));
  size_t dumped_blocks = 0;
  for (size_t pos = dump.find("Data Block # "); pos != std::string::npos;
       pos = dump.find("Data Block # ", pos + 1)) {
    ++dumped_blocks;
  }
  ASSERT_EQ(dumped_blocks, num_pages);
  ASSERT_EQ(dump.find("Error reading the block"), std::string::npos);
}

TEST(KiWiAuditTest, PrefetchLoadsEveryPage) {
  Random64 rnd(47);
  BuildParams bp;
  BuiltTable table;
  BuildKiWiTable(MakeEntries(&rnd, 200, 0, 50), bp, &table);
  OpenedTable opened;
  ASSERT_OK(OpenTable(table.contents, bp, &opened));
  auto* reader = static_cast<BlockBasedTable*>(opened.reader.get());
  ASSERT_FALSE(reader->TEST_BlockInCache(table.tiles[1].pages[0]));
  ASSERT_OK(reader->Prefetch(ReadOptions(), nullptr, nullptr));
  for (const TileLayout& tile : table.tiles) {
    for (const BlockHandle& page : tile.pages) {
      ASSERT_TRUE(reader->TEST_BlockInCache(page));
    }
  }
}

TEST(KiWiAuditTest, SizeEstimatesAreMonotonicAndBounded) {
  Random64 rnd(53);
  BuildParams bp;
  const std::vector<Entry> entries = MakeEntries(&rnd, 400, 0, 50);
  BuiltTable table;
  BuildKiWiTable(entries, bp, &table);
  OpenedTable opened;
  ASSERT_OK(OpenTable(table.contents, bp, &opened));
  uint64_t prev = 0;
  for (const Entry& e : entries) {
    const uint64_t offset = opened.reader->ApproximateOffsetOf(
        ReadOptions(), e.ikey, TableReaderCaller::kUncategorized);
    ASSERT_GE(offset, prev);
    ASSERT_LE(offset, table.contents.size());
    prev = offset;
  }
  ASSERT_GT(prev, 0U);  // the last keys are far from the start of the file

  std::vector<TableReader::Anchor> anchors;
  ASSERT_OK(opened.reader->ApproximateKeyAnchors(ReadOptions(), anchors));
  ASSERT_FALSE(anchors.empty());
  ASSERT_LE(anchors.size(), 129U);
  uint64_t total = 0;
  for (const auto& anchor : anchors) {
    total += anchor.range_size;
  }
  // Anchors partition the data region of the file.
  ASSERT_GT(total, 0U);
  ASSERT_LE(total, table.props->data_size);
}

namespace {

// Entries minus those a secondary delete removed (kTypeValue with a delete key
// below `floor`), i.e. what a reader of the file must see.
std::vector<Entry> SurvivingEntries(const std::vector<Entry>& entries,
                                    uint32_t delete_key_offset,
                                    uint64_t floor) {
  std::vector<Entry> survivors;
  for (const Entry& e : entries) {
    uint64_t d = 0;
    EXPECT_OK(ExtractDeleteKey(ExtractValueType(e.ikey), e.value,
                               delete_key_offset, &d));
    if (!(ExtractValueType(e.ikey) == kTypeValue && d < floor)) {
      survivors.push_back(e);
    }
  }
  return survivors;
}

// A KiWi file opened with a delete floor, next to a plain file holding exactly
// the entries that survive the floor.
struct FloorTables {
  BuiltTable kiwi_table;
  BuiltTable plain_table;
  OpenedTable kiwi;
  OpenedTable plain;
  size_t num_survivors = 0;
};

void BuildFloorTables(const std::vector<Entry>& entries, const BuildParams& bp,
                      uint64_t floor, FloorTables* out) {
  BuildKiWiTable(entries, bp, &out->kiwi_table);
  ASSERT_OK(OpenTable(out->kiwi_table.contents, bp, &out->kiwi, floor));
  BuildParams plain_bp = bp;
  plain_bp.tile_pages = 1;
  const std::vector<Entry> survivors =
      SurvivingEntries(entries, bp.delete_key_offset, floor);
  out->num_survivors = survivors.size();
  BuildKiWiTable(survivors, plain_bp, &out->plain_table);
  ASSERT_OK(OpenTable(out->plain_table.contents, plain_bp, &out->plain));
}

}  // namespace

TEST(KiWiDeleteFloorTest, ReadsMatchAPlainFileOfTheSurvivors) {
  const uint64_t seed = Env::Default()->NowMicros();
  SCOPED_TRACE("seed=" + std::to_string(seed));
  Random64 rnd(seed);
  for (int iter = 0; iter < 12; ++iter) {
    BuildParams bp;
    bp.tile_pages = 2 + static_cast<uint32_t>(rnd.Uniform(7));
    bp.block_size = 64 + rnd.Uniform(300);
    bp.filters = !rnd.OneIn(4);
    const size_t num_keys = 30 + rnd.Uniform(150);
    const std::vector<Entry> entries = MakeEntries(&rnd, num_keys, 0, 40);
    // Delete keys span the whole 64-bit range or a handful of small values, so
    // floors fall inside, between and outside of the pages' delete key ranges.
    const uint64_t floors[] = {1, 4, rnd.Next(), rnd.Next() / 2, kNoDeleteKey};
    const uint64_t floor = floors[rnd.Uniform(5)];
    FloorTables tables;
    BuildFloorTables(entries, bp, floor, &tables);

    // Scans in both directions and a random walk of every operation.
    std::unique_ptr<InternalIterator> expected(
        NewIter(tables.plain.reader.get()));
    std::unique_ptr<InternalIterator> actual(NewIter(tables.kiwi.reader.get()));
    for (expected->SeekToFirst(), actual->SeekToFirst(); expected->Valid();
         expected->Next(), actual->Next()) {
      ExpectSameIterator(expected.get(), actual.get());
    }
    ExpectSameIterator(expected.get(), actual.get());
    for (expected->SeekToLast(), actual->SeekToLast(); expected->Valid();
         expected->Prev(), actual->Prev()) {
      ExpectSameIterator(expected.get(), actual.get());
    }
    ExpectSameIterator(expected.get(), actual.get());
    ExpectSameRandomWalk(expected.get(), actual.get(), &rnd, num_keys, 1500);

    // Point lookups and snapshots see the same.
    for (int probe = 0; probe < 200; ++probe) {
      char buf[16];
      snprintf(buf, sizeof(buf), "key%08" PRIu64, rnd.Uniform(num_keys + 2));
      const SequenceNumber snapshot =
          rnd.OneIn(3) ? kMaxSequenceNumber : 1000 - rnd.Uniform(4);
      GetResult want;
      GetResult got;
      ASSERT_OK(DoGet(tables.plain, buf, snapshot, &want));
      ASSERT_OK(DoGet(tables.kiwi, buf, snapshot, &got));
      ASSERT_EQ(want.state, got.state) << buf;
      ASSERT_EQ(want.value, got.value) << buf;
    }
  }
}

TEST(KiWiDeleteFloorTest, FullyDeletedPagesAreNotRead) {
  Random64 rnd(71);
  BuildParams bp;
  bp.tile_pages = 8;
  // Only values, so that no page holds a tombstone (which has no delete key).
  std::vector<Entry> entries;
  for (uint64_t k = 0; k < 400; ++k) {
    Entry e;
    e.ikey = MakeInternalKey(k, 1000, kTypeValue);
    e.value = MakeValue(1000 + rnd.Uniform(1000), 0, 40);
    entries.push_back(std::move(e));
  }
  auto data_block_reads = [](const OpenedTable& t) {
    Statistics* stats = t.options.statistics.get();
    return stats->getTickerCount(BLOCK_CACHE_DATA_HIT) +
           stats->getTickerCount(BLOCK_CACHE_DATA_MISS);
  };
  auto scan = [](OpenedTable* t) {
    std::unique_ptr<InternalIterator> it(NewIter(t->reader.get()));
    size_t n = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ++n;
    }
    EXPECT_OK(it->status());
    return n;
  };

  // A floor above every delete key deletes everything without reading a page.
  FloorTables all_gone;
  BuildFloorTables(entries, bp, /*floor=*/1000000, &all_gone);
  ASSERT_EQ(scan(&all_gone.kiwi), 0U);
  ASSERT_EQ(data_block_reads(all_gone.kiwi), 0U);
  GetResult r;
  ASSERT_OK(DoGet(all_gone.kiwi, "key00000005", kMaxSequenceNumber, &r));
  ASSERT_EQ(r.state, GetContext::kNotFound);
  ASSERT_EQ(data_block_reads(all_gone.kiwi), 0U);

  // A floor in the middle reads only the pages that still hold entries: fewer
  // than the whole file, and at most the surviving ones plus the edge pages.
  FloorTables half;
  BuildFloorTables(entries, bp, /*floor=*/1500, &half);
  const size_t survivors = scan(&half.kiwi);
  ASSERT_EQ(survivors, half.num_survivors);
  ASSERT_GT(survivors, 0U);
  ASSERT_LT(survivors, entries.size());
  OpenedTable untouched;
  ASSERT_OK(OpenTable(half.kiwi_table.contents, bp, &untouched));
  ASSERT_EQ(scan(&untouched), entries.size());
  ASSERT_LT(data_block_reads(half.kiwi), data_block_reads(untouched));
}

TEST(KiWiTileXorTableTest, ReadsMatchPlainFileAndProbeSinglePage) {
  Random64 rnd(2026);
  for (int fp_bytes : {1, 2}) {
    for (uint32_t tile_pages : {2U, 4U, 8U}) {
      SCOPED_TRACE("fp_bytes=" + std::to_string(fp_bytes) +
                   " tile_pages=" + std::to_string(tile_pages));
      BuildParams bp;
      bp.tile_pages = tile_pages;
      bp.block_size = 256;
      bp.filters = true;
      bp.xor_filter_bytes = fp_bytes;

      // Unique user keys (required for per-tile XOR+page filters).
      const size_t num_keys = 300;
      std::vector<Entry> entries;
      entries.reserve(num_keys);
      for (size_t k = 0; k < num_keys; ++k) {
        Entry e;
        e.ikey = MakeInternalKey(k, 1000, kTypeValue);
        e.value = MakeValue(100 + (k % 100), 0, 32 + rnd.Uniform(24));
        entries.push_back(std::move(e));
      }

      BuiltTable kiwi_table;
      BuildKiWiTable(entries, bp, &kiwi_table);
      ASSERT_EQ(kiwi_table.props->filter_size, 0U);
      const auto& up = kiwi_table.props->user_collected_properties;
      ASSERT_EQ(up.at(kPropDirectoryBytes),
                std::to_string(kiwi_table.directory.size()));
      ASSERT_GT(std::stoull(up.at(kPropFilterBytes)), 0U);

      OpenedTable kiwi;
      ASSERT_OK(OpenTable(kiwi_table.contents, bp, &kiwi));
      const Directory* dir =
          static_cast<const BlockBasedTable*>(kiwi.reader.get())
              ->TEST_GetKiWiDirectory();
      ASSERT_NE(dir, nullptr);
      ASSERT_TRUE(dir->has_tile_filters());

      BuildParams plain_bp = bp;
      plain_bp.tile_pages = 1;
      plain_bp.xor_filter_bytes = 0;
      BuiltTable plain_table;
      BuildKiWiTable(entries, plain_bp, &plain_table);
      OpenedTable plain;
      ASSERT_OK(OpenTable(plain_table.contents, plain_bp, &plain));

      // Forward & backward scans match plain file.
      std::unique_ptr<InternalIterator> expected(NewIter(plain.reader.get()));
      std::unique_ptr<InternalIterator> actual(NewIter(kiwi.reader.get()));
      for (expected->SeekToFirst(), actual->SeekToFirst(); expected->Valid();
           expected->Next(), actual->Next()) {
        ExpectSameIterator(expected.get(), actual.get());
      }
      ExpectSameIterator(expected.get(), actual.get());
      for (expected->SeekToLast(), actual->SeekToLast(); expected->Valid();
           expected->Prev(), actual->Prev()) {
        ExpectSameIterator(expected.get(), actual.get());
      }
      ExpectSameIterator(expected.get(), actual.get());

      // VerifyChecksum and DumpTable work on v2 directories too.
      ASSERT_OK(kiwi.reader->VerifyChecksum(
          ReadOptions(), TableReaderCaller::kUserVerifyChecksum));

      // Present-key Gets probe exactly 1 filter and read at most 1 data block.
      Statistics* stats = kiwi.options.statistics.get();
      for (size_t k = 0; k < num_keys; k += 7) {
        char buf[16];
        snprintf(buf, sizeof(buf), "key%08" PRIu64, static_cast<uint64_t>(k));
        const uint64_t checks_before =
            stats->getTickerCount(BLOOM_FILTER_USEFUL) +
            stats->getTickerCount(BLOOM_FILTER_FULL_POSITIVE);
        const uint64_t reads_before =
            stats->getTickerCount(BLOCK_CACHE_DATA_HIT) +
            stats->getTickerCount(BLOCK_CACHE_DATA_MISS);
        GetResult want;
        GetResult got;
        ASSERT_OK(DoGet(plain, buf, kMaxSequenceNumber, &want));
        ASSERT_OK(DoGet(kiwi, buf, kMaxSequenceNumber, &got));
        ASSERT_EQ(got.state, want.state);
        ASSERT_EQ(got.value, want.value);
        const uint64_t checks_after =
            stats->getTickerCount(BLOOM_FILTER_USEFUL) +
            stats->getTickerCount(BLOOM_FILTER_FULL_POSITIVE);
        const uint64_t reads_after =
            stats->getTickerCount(BLOCK_CACHE_DATA_HIT) +
            stats->getTickerCount(BLOCK_CACHE_DATA_MISS);
        ASSERT_EQ(checks_after - checks_before, 1U);
        ASSERT_LE(reads_after - reads_before, 1U);
      }

      // Absent keys probe 1 filter per Get and skip data reads on negatives.
      uint64_t useful = 0;
      for (size_t k = 0; k < 100; ++k) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key00000100_absent_%04zu", k);
        const uint64_t useful_before =
            stats->getTickerCount(BLOOM_FILTER_USEFUL);
        const uint64_t reads_before =
            stats->getTickerCount(BLOCK_CACHE_DATA_HIT) +
            stats->getTickerCount(BLOCK_CACHE_DATA_MISS);
        GetResult got;
        ASSERT_OK(DoGet(kiwi, buf, kMaxSequenceNumber, &got));
        ASSERT_EQ(got.state, GetContext::kNotFound);
        const uint64_t useful_delta =
            stats->getTickerCount(BLOOM_FILTER_USEFUL) - useful_before;
        const uint64_t reads_delta =
            stats->getTickerCount(BLOCK_CACHE_DATA_HIT) +
            stats->getTickerCount(BLOCK_CACHE_DATA_MISS) - reads_before;
        if (useful_delta == 1U) {
          ASSERT_EQ(reads_delta, 0U);
          ++useful;
        }
      }
      ASSERT_GT(useful, 90U);

      // Delete floor drops pages without reconstructing the XOR filter.
      const uint64_t floor = 150;
      OpenedTable floored;
      ASSERT_OK(OpenTable(kiwi_table.contents, bp, &floored, floor));
      const std::vector<Entry> survivors =
          SurvivingEntries(entries, bp.delete_key_offset, floor);
      BuildKiWiTable(survivors, plain_bp, &plain_table);
      OpenedTable plain_floored;
      ASSERT_OK(OpenTable(plain_table.contents, plain_bp, &plain_floored));
      for (size_t k = 0; k < num_keys; k += 5) {
        char buf[16];
        snprintf(buf, sizeof(buf), "key%08" PRIu64, static_cast<uint64_t>(k));
        GetResult want;
        GetResult got;
        ASSERT_OK(DoGet(plain_floored, buf, kMaxSequenceNumber, &want));
        ASSERT_OK(DoGet(floored, buf, kMaxSequenceNumber, &got));
        ASSERT_EQ(got.state, want.state) << buf;
        ASSERT_EQ(got.value, want.value) << buf;
      }
    }
  }
}

TEST(TileXorFilterTest, TightSizing) {
  // Verify tight +4 slot sizing produces <= 14 bits/key on 250 keys (h=4,
  // 1-byte fingerprint => 10-bit slots * ~1.25x = ~12.5 bpk).
  TileXorFilterBuilder builder(/*fingerprint_bytes=*/1);
  const size_t n = 250;
  for (size_t i = 0; i < n; ++i) {
    builder.AddKey("tight_k" + std::to_string(i), static_cast<uint32_t>(i % 4));
  }
  std::string encoded;
  ASSERT_OK(builder.Finish(/*num_pages=*/4, &encoded));
  ASSERT_LT(encoded.size() * 8.0 / n, 14.0);

  TileXorFilterReader reader;
  ASSERT_OK(reader.Init(encoded, /*num_pages=*/4));
  for (size_t i = 0; i < n; ++i) {
    uint32_t got_page = 9999;
    ASSERT_TRUE(reader.MayMatchPage("tight_k" + std::to_string(i), &got_page));
    ASSERT_EQ(got_page, static_cast<uint32_t>(i % 4));
  }
}

TEST(KiWiDirectoryTest, ApplyDeleteFloorAndV3RoundTrip) {
  // v2 per-tile directory with dead-tile memory compaction.
  {
    DirectoryBuilder db;
    uint64_t off = 0;
    for (size_t t = 0; t < 3; ++t) {
      TileXorFilterBuilder fb(/*fingerprint_bytes=*/2);
      std::vector<DirectoryPage> pages(4);
      for (size_t p = 0; p < 4; ++p) {
        pages[p].handle = BlockHandle(off, 100);
        off += 105;
        pages[p].min_delete_key = (t * 4 + p) * 100;
        pages[p].max_delete_key = (t * 4 + p) * 100 + 99;
        for (size_t k = 0; k < 20; ++k) {
          fb.AddKey("t" + std::to_string(t) + "_p" + std::to_string(p) + "_" +
                        std::to_string(k),
                    static_cast<uint32_t>(p),
                    static_cast<uint32_t>(p * 20 + k));
        }
      }
      std::string f;
      ASSERT_OK(fb.Finish(4, &f));
      db.AddTileWithXorFilter(pages, f);
    }
    std::string enc;
    db.Finish(&enc);
    Directory dir;
    ASSERT_OK(dir.Decode(enc));
    const size_t mem_before = dir.ApproximateMemoryUsage();
    ASSERT_EQ(dir.directory_bytes(), enc.size());

    // Floor = 550 kills tile 0 completely ([0..399]) and page 0 of tile 1
    // ([400..499]), leaving page 1 ([500..599]) as the straddling edge page.
    dir.ApplyDeleteFloor(550);
    ASSERT_TRUE(dir.TileAllDeleted(0, 550));
    ASSERT_FALSE(dir.TileAllDeleted(1, 550));
    ASSERT_LT(dir.ApproximateMemoryUsage(), mem_before);

    uint32_t page = 9999;
    ASSERT_FALSE(dir.TileMayMatch(0, "t0_p0_0", &page));
    ASSERT_FALSE(dir.TileMayMatch(1, "t1_p0_0", &page));
    ASSERT_TRUE(dir.TileMayMatch(1, "t1_p1_2", &page));
    ASSERT_EQ(page, 1U);
    ASSERT_TRUE(dir.TileMayMatch(2, "t2_p3_3", &page));
    ASSERT_EQ(page, 3U);
  }

  // v3 SST-wide XOR filter directory round-trip & corruption handling.
  {
    DirectoryBuilder db;
    TileXorFilterBuilder ffb(/*fingerprint_bytes=*/2);
    uint64_t off = 0;
    std::vector<uint64_t> first_offsets;
    for (size_t t = 0; t < 4; ++t) {
      std::vector<DirectoryPage> pages(4);
      for (size_t p = 0; p < 4; ++p) {
        pages[p].handle = BlockHandle(off, 120);
        off += 125;
        pages[p].min_delete_key = p * 100;
        pages[p].max_delete_key = p * 100 + 99;
        for (size_t k = 0; k < 15; ++k) {
          ffb.AddKey("v3_t" + std::to_string(t) + "_p" + std::to_string(p) +
                         "_" + std::to_string(k),
                     static_cast<uint32_t>(p),
                     static_cast<uint32_t>(t * 60 + p * 15 + k));
        }
      }
      first_offsets.push_back(pages[0].handle.offset());
      db.AddTileWithoutFilter(pages);
    }
    std::string fenc;
    ASSERT_OK(ffb.Finish(4, &fenc));
    db.SetFileXorFilter(fenc);
    std::string enc;
    db.Finish(&enc);

    Directory dir;
    ASSERT_OK(dir.Decode(enc));
    ASSERT_TRUE(dir.has_file_filter());
    ASSERT_FALSE(dir.has_tile_filters());
    ASSERT_EQ(dir.num_tiles(), 4U);
    for (size_t t = 0; t < 4; ++t) {
      ASSERT_EQ(dir.FindTile(first_offsets[t]), t);
      uint32_t page = 9999;
      ASSERT_TRUE(
          dir.FileMayMatch("v3_t" + std::to_string(t) + "_p2_3", &page));
      ASSERT_EQ(page, 2U);
    }
    for (size_t len = 0; len < enc.size(); ++len) {
      Directory bad;
      ASSERT_EQ(DecodeDirectory(enc.substr(0, len), &bad).code(),
                Status::kCorruption);
    }
  }
}

TEST(KiWiTileXorTableTest,
     FileFilterSkipsIndexAndFallsBackOnCrossTileDuplicates) {
  const size_t num_keys = 360;
  std::vector<Entry> entries;
  entries.reserve(num_keys);
  for (size_t k = 0; k < num_keys; ++k) {
    Entry e;
    e.ikey = MakeInternalKey(k, 1000, kTypeValue);
    e.value = MakeValue(100 + ((k * 37) % 1000), 0, 48);
    entries.push_back(std::move(e));
  }

  // Whole-SST v3 XOR filter skips index block on absent-key Gets, matches
  // plain table on present/floored Gets, and falls back to v2 automatically
  // when a multi-version user key spans a tile boundary.
  BuildParams bp;
  bp.tile_pages = 4;
  bp.block_size = 256;
  bp.filters = true;
  bp.xor_filter_bytes = 2;
  bp.xor_file_filter = true;
  BuiltTable tbl;
  BuildKiWiTable(entries, bp, &tbl);
  OpenedTable opened;
  ASSERT_OK(OpenTable(tbl.contents, bp, &opened));
  const Directory* dir =
      static_cast<const BlockBasedTable*>(opened.reader.get())
          ->TEST_GetKiWiDirectory();
  ASSERT_NE(dir, nullptr);
  ASSERT_TRUE(dir->has_file_filter());
  ASSERT_FALSE(dir->has_tile_filters());

  Statistics* st = opened.options.statistics.get();
  const uint64_t idx_before = st->getTickerCount(BLOCK_CACHE_INDEX_HIT) +
                              st->getTickerCount(BLOCK_CACHE_INDEX_MISS);
  for (size_t k = 0; k < 50; ++k) {
    GetResult r;
    ASSERT_OK(DoGet(opened, "missing_key_" + std::to_string(k),
                    kMaxSequenceNumber, &r));
    ASSERT_EQ(r.state, GetContext::kNotFound);
  }
  const uint64_t idx_after = st->getTickerCount(BLOCK_CACHE_INDEX_HIT) +
                             st->getTickerCount(BLOCK_CACHE_INDEX_MISS);
  ASSERT_EQ(idx_after, idx_before)
      << "expected 0 index lookups on v3 negatives";

  BuildParams plain_bp = bp;
  plain_bp.tile_pages = 1;
  plain_bp.xor_filter_bytes = 0;
  plain_bp.xor_file_filter = false;
  BuiltTable plain_tbl;
  BuildKiWiTable(entries, plain_bp, &plain_tbl);
  OpenedTable plain;
  ASSERT_OK(OpenTable(plain_tbl.contents, plain_bp, &plain));
  for (size_t k = 0; k < num_keys; k += 5) {
    char buf[16];
    snprintf(buf, sizeof(buf), "key%08" PRIu64, static_cast<uint64_t>(k));
    GetResult want;
    GetResult got;
    ASSERT_OK(DoGet(plain, buf, kMaxSequenceNumber, &want));
    ASSERT_OK(DoGet(opened, buf, kMaxSequenceNumber, &got));
    ASSERT_EQ(got.state, want.state) << buf;
    ASSERT_EQ(got.value, want.value) << buf;
  }

  const uint64_t floor = 550;
  OpenedTable floored;
  ASSERT_OK(OpenTable(tbl.contents, bp, &floored, floor));
  const std::vector<Entry> survivors =
      SurvivingEntries(entries, bp.delete_key_offset, floor);
  BuiltTable plain_floor_tbl;
  BuildKiWiTable(survivors, plain_bp, &plain_floor_tbl);
  OpenedTable plain_floored;
  ASSERT_OK(OpenTable(plain_floor_tbl.contents, plain_bp, &plain_floored));
  for (size_t k = 0; k < num_keys; k += 5) {
    char buf[16];
    snprintf(buf, sizeof(buf), "key%08" PRIu64, static_cast<uint64_t>(k));
    GetResult want;
    GetResult got;
    ASSERT_OK(DoGet(plain_floored, buf, kMaxSequenceNumber, &want));
    ASSERT_OK(DoGet(floored, buf, kMaxSequenceNumber, &got));
    ASSERT_EQ(got.state, want.state) << buf;
    ASSERT_EQ(got.value, want.value) << buf;
  }

  // Multi-version user key spanning tile boundary triggers v2 fallback.
  std::vector<Entry> dup_entries;
  for (size_t k = 0; k < 60; ++k) {
    const size_t copies = (k == 15) ? 25 : 1;
    for (size_t v = 0; v < copies; ++v) {
      Entry e;
      e.ikey = MakeInternalKey(k, 1000 - v, kTypeValue);
      e.value = MakeValue(100 + v, 0, 64);
      dup_entries.push_back(std::move(e));
    }
  }
  BuiltTable dup_tbl;
  BuildKiWiTable(dup_entries, bp, &dup_tbl);
  OpenedTable dup_opened;
  ASSERT_OK(OpenTable(dup_tbl.contents, bp, &dup_opened));
  const Directory* dup_dir =
      static_cast<const BlockBasedTable*>(dup_opened.reader.get())
          ->TEST_GetKiWiDirectory();
  ASSERT_FALSE(dup_dir->has_file_filter());
  ASSERT_TRUE(dup_dir->has_tile_filters());
  GetResult newest;
  ASSERT_OK(DoGet(dup_opened, "key00000015", kMaxSequenceNumber, &newest));
  ASSERT_EQ(newest.state, GetContext::kFound);
  ASSERT_EQ(newest.value, MakeValue(100, 0, 64));
}

#endif  // !NDEBUG

}  // namespace kiwi
}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
