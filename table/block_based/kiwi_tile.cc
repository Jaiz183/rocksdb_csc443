//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "table/block_based/kiwi_tile.h"

#include <algorithm>
#include <numeric>

#include "util/cast_util.h"

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

}  // namespace kiwi
}  // namespace ROCKSDB_NAMESPACE
