//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "table/block_based/kiwi_tile.h"

#include <algorithm>
#include <numeric>

#include "table/block_based/filter_policy_internal.h"
#include "util/cast_util.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {
namespace kiwi {

Status ExtractDeleteKey(ValueType type, const Slice& value, uint32_t offset,
                        uint64_t* delete_key) {
  assert(delete_key != nullptr);
  if (type != kTypeValue) {
    *delete_key = kNoDeleteKey;
    return Status::OK();
  }
  // Compare without overflowing: offset is a uint32_t, sizes are size_t.
  if (value.size() < kDeleteKeySize ||
      value.size() - kDeleteKeySize < lossless_cast<size_t>(offset)) {
    return Status::InvalidArgument(
        "KiWi: value of " + std::to_string(value.size()) +
        " bytes is too short for an 8-byte delete key at offset " +
        std::to_string(offset));
  }
  // Big-endian, independent of host byte order.
  const unsigned char* p =
      reinterpret_cast<const unsigned char*>(value.data()) + offset;
  uint64_t d = 0;
  for (size_t i = 0; i < kDeleteKeySize; ++i) {
    d = (d << 8) | p[i];
  }
  *delete_key = d;
  return Status::OK();
}

Status TilePacker::Add(const Slice& internal_key, const Slice& value) {
  assert(internal_key.size() >= kNumInternalBytes);
  uint64_t d = kNoDeleteKey;
  Status s =
      ExtractDeleteKey(ExtractValueType(internal_key), value, offset_, &d);
  if (!s.ok()) {
    return s;
  }
  refs_.push_back(Ref{arena_.size(), internal_key.size(), value.size(), d});
  arena_.append(internal_key.data(), internal_key.size());
  arena_.append(value.data(), value.size());
  return Status::OK();
}

void TilePacker::Pack(std::vector<TilePage>* pages) const {
  assert(pages != nullptr);
  pages->clear();
  if (refs_.empty()) {
    return;
  }

  // Order entries by delete key; ties keep internal key order so the result
  // is deterministic.
  std::vector<size_t> order(refs_.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::sort(order.begin(), order.end(), [this](size_t a, size_t b) {
    const uint64_t da = refs_[a].delete_key;
    const uint64_t db = refs_[b].delete_key;
    return da != db ? da < db : a < b;
  });

  // Use as many pages as the data needs, up to max_pages_, each holding about
  // the same number of bytes.
  const size_t total = arena_.size();
  const size_t num_pages =
      std::clamp((total + page_size_ - 1) / page_size_, size_t{1},
                 lossless_cast<size_t>(max_pages_));
  const size_t target = (total + num_pages - 1) / num_pages;

  size_t page_bytes = 0;
  pages->emplace_back();
  for (size_t pos : order) {
    if (page_bytes >= target && pages->size() < num_pages) {
      pages->emplace_back();
      page_bytes = 0;
    }
    TilePage& page = pages->back();
    // Pages are filled in ascending delete key order.
    if (page.entries.empty()) {
      page.min_delete_key = refs_[pos].delete_key;
    }
    page.max_delete_key = refs_[pos].delete_key;
    page.entries.push_back(pos);
    page_bytes += refs_[pos].key_len + refs_[pos].value_len;
  }
  // Inside a page, restore internal key order. Positions are insertion order,
  // which is internal key order.
  for (TilePage& page : *pages) {
    std::sort(page.entries.begin(), page.entries.end());
  }
}

void DirectoryBuilder::AddTile(const std::vector<DirectoryPage>& pages) {
  assert(!pages.empty());
  PutVarint64(&body_, lossless_cast<uint64_t>(pages.size()));
  for (const DirectoryPage& page : pages) {
    page.handle.EncodeTo(&body_);
    PutFixed64(&body_, page.min_delete_key);
    PutFixed64(&body_, page.max_delete_key);
    PutLengthPrefixedSlice(&body_, page.bloom);
  }
  ++num_tiles_;
}

void DirectoryBuilder::Finish(std::string* out) const {
  out->clear();
  out->push_back(kFormatVersion);
  PutVarint32(out, num_tiles_);
  out->append(body_);
}

namespace {
// Smallest possible encoded page: 1-byte offset and size varints, two fixed64
// delete keys and a 1-byte bloom length. Bounds allocations from corrupt
// counts.
constexpr size_t kMinEncodedPageBytes = 2 + 2 * sizeof(uint64_t) + 1;
}  // namespace

Directory::Directory() = default;
Directory::~Directory() = default;

Status Directory::Decode(std::string contents) {
  pages_.clear();
  tile_first_page_.clear();
  filters_.clear();
  contents_ = std::move(contents);

  const auto corrupt = [this](const char* what) {
    pages_.clear();
    tile_first_page_.clear();
    filters_.clear();
    return Status::Corruption(std::string("KiWi directory: ") + what);
  };

  Slice input(contents_);
  if (input.empty() || input[0] != DirectoryBuilder::kFormatVersion) {
    return corrupt("unknown version");
  }
  input.remove_prefix(1);
  uint32_t num_tiles = 0;
  if (!GetVarint32(&input, &num_tiles)) {
    return corrupt("bad tile count");
  }
  if (num_tiles > input.size() / (kMinEncodedPageBytes + 1)) {
    return corrupt("tile count exceeds directory size");
  }
  tile_first_page_.reserve(num_tiles);

  uint64_t prev_tile_offset = 0;
  for (uint32_t t = 0; t < num_tiles; ++t) {
    uint64_t num_pages = 0;
    if (!GetVarint64(&input, &num_pages) || num_pages == 0 ||
        num_pages > input.size() / kMinEncodedPageBytes) {
      return corrupt("bad page count");
    }
    tile_first_page_.push_back(pages_.size());
    for (uint64_t i = 0; i < num_pages; ++i) {
      DirectoryPage page;
      if (!page.handle.DecodeFrom(&input).ok() ||
          !GetFixed64(&input, &page.min_delete_key) ||
          !GetFixed64(&input, &page.max_delete_key) ||
          !GetLengthPrefixedSlice(&input, &page.bloom)) {
        return corrupt("truncated page record");
      }
      if (page.min_delete_key > page.max_delete_key) {
        return corrupt("inverted delete key range");
      }
      if (i == 0) {
        // Tile start offsets must strictly increase for FindTile().
        if (t > 0 && page.handle.offset() <= prev_tile_offset) {
          return corrupt("tiles out of order");
        }
        prev_tile_offset = page.handle.offset();
      }
      pages_.push_back(page);
    }
  }
  if (!input.empty()) {
    return corrupt("trailing bytes");
  }
  // The directory is pinned in memory for the life of the table reader.
  pages_.shrink_to_fit();
  filters_.reserve(pages_.size());
  for (const DirectoryPage& page : pages_) {
    filters_.emplace_back(
        page.bloom.empty()
            ? nullptr
            : BuiltinFilterPolicy::GetBuiltinFilterBitsReader(page.bloom));
  }
  return Status::OK();
}

Directory::TileView Directory::tile(size_t i) const {
  assert(i < tile_first_page_.size());
  const size_t begin = tile_first_page_[i];
  const size_t end =
      i + 1 < tile_first_page_.size() ? tile_first_page_[i + 1] : pages_.size();
  return TileView{pages_.data() + begin, end - begin};
}

size_t Directory::FindTile(uint64_t first_page_offset) const {
  size_t lo = 0;
  size_t hi = tile_first_page_.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (pages_[tile_first_page_[mid]].handle.offset() < first_page_offset) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < tile_first_page_.size() &&
      pages_[tile_first_page_[lo]].handle.offset() == first_page_offset) {
    return lo;
  }
  return tile_first_page_.size();
}

bool Directory::PageMayMatch(const DirectoryPage* page,
                             const Slice& user_key) const {
  assert(page >= pages_.data() && page < pages_.data() + pages_.size());
  FilterBitsReader* filter = filters_[page - pages_.data()].get();
  return filter == nullptr || filter->MayMatch(user_key);
}

size_t Directory::ApproximateMemoryUsage() const {
  // The filter readers are small objects that point into contents_.
  constexpr size_t kApproxFilterReaderBytes = 64;
  return contents_.capacity() + pages_.capacity() * sizeof(DirectoryPage) +
         tile_first_page_.capacity() * sizeof(size_t) +
         filters_.capacity() * sizeof(filters_[0]) +
         filters_.size() * kApproxFilterReaderBytes;
}

}  // namespace kiwi
}  // namespace ROCKSDB_NAMESPACE
