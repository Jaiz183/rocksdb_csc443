//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Building blocks of the Key Weaving Storage Layout (KiWi) from the Lethe
// paper. A file is a sequence of delete tiles; a tile is a group of pages
// (data blocks) that are ordered by a secondary "delete key" D, while the
// entries inside each page stay ordered by internal key.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "table/format.h"

namespace ROCKSDB_NAMESPACE {
namespace kiwi {

// Delete key of an entry that has none (anything but kTypeValue). Such entries
// sort after every real delete key and are never dropped by a secondary
// delete, because a secondary delete only drops entries with D < floor.
constexpr uint64_t kNoDeleteKey = std::numeric_limits<uint64_t>::max();

// Size of the delete key inside a value.
constexpr size_t kDeleteKeySize = sizeof(uint64_t);

// Extracts the delete key of an entry: for kTypeValue, the 8-byte big-endian
// unsigned integer at byte `offset` of `value`; kNoDeleteKey for any other
// type. Returns InvalidArgument if a kTypeValue value is too short, so a
// malformed value fails loudly instead of silently escaping secondary deletes.
Status ExtractDeleteKey(ValueType type, const Slice& value, uint32_t offset,
                        uint64_t* delete_key);

// One page of a packed tile. Entries are referenced by their position in the
// tile's insertion order, which is internal key order.
struct TilePage {
  // Positions into the packer, ascending, i.e. in internal key order.
  std::vector<size_t> entries;
  // Smallest and largest delete key among the entries of the page.
  uint64_t min_delete_key = kNoDeleteKey;
  uint64_t max_delete_key = 0;
};

// Collects the entries of one delete tile in internal key order, then packs
// them into at most `max_pages` pages ordered by delete key. Buffers are
// reused across tiles, so steady-state packing does not allocate per entry.
class TilePacker {
 public:
  // `max_pages` is h, the number of pages per tile (>= 1). `page_size` is the
  // target size in bytes of a page, as in BlockBasedTableOptions::block_size.
  TilePacker(uint32_t offset, uint32_t max_pages, size_t page_size)
      : offset_(offset),
        max_pages_(max_pages == 0 ? 1 : max_pages),
        page_size_(page_size == 0 ? 1 : page_size) {}

  // Appends an entry. Entries must be added in internal key order.
  Status Add(const Slice& internal_key, const Slice& value);

  bool empty() const { return refs_.empty(); }
  size_t num_entries() const { return refs_.size(); }

  // Bytes buffered (keys plus values); counts toward the file size estimate.
  size_t buffered_bytes() const { return arena_.size(); }

  // True once the buffer holds a full tile's worth of data (h pages).
  bool full() const {
    return arena_.size() >= static_cast<size_t>(max_pages_) * page_size_;
  }

  Slice key(size_t pos) const {
    const Ref& r = refs_[pos];
    return Slice(arena_.data() + r.key_off, r.key_len);
  }
  Slice value(size_t pos) const {
    const Ref& r = refs_[pos];
    return Slice(arena_.data() + r.key_off + r.key_len, r.value_len);
  }
  uint64_t delete_key(size_t pos) const { return refs_[pos].delete_key; }

  // Last (largest) internal key added; requires !empty().
  Slice last_key() const { return key(refs_.size() - 1); }

  // Packs the buffered entries into pages, ordered by delete key (ties keep
  // internal key order), at most max_pages of roughly equal byte size. Does
  // not clear the buffer: pages refer to entries by position, so call Clear()
  // after the pages have been written.
  void Pack(std::vector<TilePage>* pages) const;

  void Clear() {
    arena_.clear();
    refs_.clear();
  }

 private:
  struct Ref {
    size_t key_off;
    size_t key_len;
    size_t value_len;
    uint64_t delete_key;
  };

  const uint32_t offset_;
  const uint32_t max_pages_;
  const size_t page_size_;
  // Key bytes immediately followed by value bytes, per entry.
  std::string arena_;
  std::vector<Ref> refs_;
};



// Names of the KiWi meta block and user-collected table properties. A file is
// a KiWi file if and only if it has the kPropTilePages property.
constexpr char kDirectoryBlockName[] = "rocksdb.kiwi.directory";
constexpr char kPropTilePages[] = "rocksdb.kiwi.tile_pages";
constexpr char kPropDeleteKeyOffset[] = "rocksdb.kiwi.delete_key_offset";
constexpr char kPropMinDeleteKey[] = "rocksdb.kiwi.min_delete_key";
constexpr char kPropMaxDeleteKey[] = "rocksdb.kiwi.max_delete_key";

// What the directory records about one page: where it is, the range of delete
// keys it holds (the paper's delete fence pointers) and its Bloom filter on
// the sort key.
struct DirectoryPage {
  BlockHandle handle;
  uint64_t min_delete_key = 0;
  uint64_t max_delete_key = 0;
  // Opaque filter bytes produced by the filter policy; may be empty.
  Slice bloom;
};

// Encodes the KiWi directory meta block, one tile at a time in file order.
//   directory := version(1 byte) varint32(num_tiles) tile*
//   tile      := varint64(num_pages) page+
//   page      := BlockHandle fixed64(min_d) fixed64(max_d) lp-bloom
// where lp-bloom is a varint32 length followed by that many filter bytes.
class DirectoryBuilder {
 public:
  static constexpr char kFormatVersion = 1;

  // Appends a tile; `pages` must be non-empty and in file order.
  void AddTile(const std::vector<DirectoryPage>& pages);
  uint32_t num_tiles() const { return num_tiles_; }
  // Produces the full block contents.
  void Finish(std::string* out) const;

 private:
  std::string body_;
  uint32_t num_tiles_ = 0;
};

// The decoded directory of a KiWi file, held in memory for the life of the
// table reader. Page filters point into the owned buffer, so the object is
// neither copyable nor movable.
class Directory {
 public:
  struct TileView {
    const DirectoryPage* pages;
    size_t num_pages;
  };

  Directory() = default;
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;

  // Takes ownership of `contents` and decodes it. Anything malformed or
  // inconsistent is reported as Corruption; the directory is left empty.
  Status Decode(std::string contents);

  size_t num_tiles() const { return tile_first_page_.size(); }
  TileView tile(size_t i) const;

  // Index of the tile whose first page starts at `first_page_offset` (the
  // offset stored in the index entry of the tile), or num_tiles() if none.
  size_t FindTile(uint64_t first_page_offset) const;

  size_t ApproximateMemoryUsage() const;

 private:
  std::string contents_;
  std::vector<DirectoryPage> pages_;
  // Position in pages_ of the first page of each tile.
  std::vector<size_t> tile_first_page_;
};

}  // namespace kiwi
}  // namespace ROCKSDB_NAMESPACE
