//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "port/stack_trace.h"
#include "rocksdb/env.h"
#include "table/block_based/kiwi_tile.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"
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

}  // namespace kiwi
}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
