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
#include "util/hash.h"

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

uint8_t CeilLog2(size_t n) {
  uint8_t bits = 0;
  size_t v = n > 0 ? n - 1 : 0;
  while (v > 0) {
    v >>= 1;
    ++bits;
  }
  return bits;
}

void TileXorFilterBuilder::AddKey(const Slice& user_key, uint32_t page_index) {
  entries_.push_back(
      Entry{GetSliceHash64(user_key), next_order_++, page_index});
}

Status TileXorFilterBuilder::Finish(size_t num_pages, std::string* out) {
  assert(out != nullptr);
  if (num_pages == 0 || entries_.empty()) {
    Clear();
    return Status::InvalidArgument(
        "KiWi tile XOR filter requires non-empty tile and pages");
  }
  if (fingerprint_bytes_ < 1 || fingerprint_bytes_ > 8) {
    Clear();
    return Status::InvalidArgument(
        "KiWi tile XOR filter fingerprint bytes must be in [1, 8]");
  }
  const uint8_t page_bits = CeilLog2(num_pages);
  if (page_bits > 32) {
    Clear();
    return Status::InvalidArgument("KiWi tile XOR filter: too many pages");
  }
  const uint8_t fp_bits = static_cast<uint8_t>(
      std::min<uint32_t>(uint32_t{fingerprint_bytes_} * 8U, 64U - page_bits));
  const uint8_t slot_bits = static_cast<uint8_t>(fp_bits + page_bits);
  const uint64_t fp_mask = fp_bits == 64
                               ? std::numeric_limits<uint64_t>::max()
                               : (uint64_t{1} << fp_bits) - 1;
  const uint64_t slot_mask = slot_bits == 64
                                 ? std::numeric_limits<uint64_t>::max()
                                 : (uint64_t{1} << slot_bits) - 1;

  // Deduplicate equal hashes keeping the earliest insertion order (newest
  // internal key if versions ever share a tile), because duplicate 3-edges
  // cannot be peeled.
  std::sort(entries_.begin(), entries_.end(),
            [](const Entry& a, const Entry& b) {
              return a.hash != b.hash ? a.hash < b.hash : a.order < b.order;
            });
  entries_.erase(
      std::unique(entries_.begin(), entries_.end(),
                  [](const Entry& a, const Entry& b) {
                    return a.hash == b.hash;
                  }),
      entries_.end());

  if (entries_.size() >
      (std::numeric_limits<uint32_t>::max() - 32) / 2) {
    Clear();
    return Status::InvalidArgument("KiWi tile XOR filter: too many keys");
  }
  const uint32_t num_keys = static_cast<uint32_t>(entries_.size());
  const uint32_t unrounded_len = static_cast<uint32_t>(1.23 * num_keys) + 32;
  const uint32_t seg_len = (unrounded_len + 2) / 3;
  const uint32_t num_slots = 3 * seg_len;

  bool peeled = false;
  uint8_t chosen_seed = 0;
  for (uint32_t seed_i = 0; seed_i < 256; ++seed_i) {
    const uint8_t seed = static_cast<uint8_t>(seed_i);
    degree_.assign(num_slots, 0);
    xor_edge_.assign(num_slots, 0);
    for (uint32_t i = 0; i < num_keys; ++i) {
      const XorFilterHash::XorSlots s =
          XorFilterHash::SeededDerive(entries_[i].hash, seg_len, seed);
      for (size_t k = 0; k < 3; ++k) {
        const uint32_t v = s.idx[k];
        ++degree_[v];
        xor_edge_[v] ^= i;
      }
    }
    queue_.clear();
    for (uint32_t v = 0; v < num_slots; ++v) {
      if (degree_[v] == 1) {
        queue_.push_back(v);
      }
    }
    peel_stack_.clear();
    for (size_t qhead = 0; qhead < queue_.size(); ++qhead) {
      const uint32_t v = queue_[qhead];
      if (degree_[v] != 1) {
        continue;
      }
      const uint32_t e = xor_edge_[v];
      peel_stack_.push_back(PeelStep{e, v});
      const XorFilterHash::XorSlots s =
          XorFilterHash::SeededDerive(entries_[e].hash, seg_len, seed);
      for (size_t k = 0; k < 3; ++k) {
        const uint32_t u = s.idx[k];
        --degree_[u];
        xor_edge_[u] ^= e;
        if (degree_[u] == 1) {
          queue_.push_back(u);
        }
      }
    }
    if (peel_stack_.size() == num_keys) {
      peeled = true;
      chosen_seed = seed;
      break;
    }
  }

  if (!peeled) {
    Clear();
    return Status::Corruption(
        "KiWi tile XOR filter: failed to peel hypergraph over 256 seeds");
  }

  slots_.assign(num_slots, 0);
  while (!peel_stack_.empty()) {
    const PeelStep step = peel_stack_.back();
    peel_stack_.pop_back();
    const Entry& e = entries_[step.edge_idx];
    const XorFilterHash::XorSlots s =
        XorFilterHash::SeededDerive(e.hash, seg_len, chosen_seed);
    const uint64_t fp = s.fp & fp_mask;
    const uint64_t target =
        page_bits == 0 ? fp : ((fp << page_bits) | e.page_index);
    slots_[step.vertex] =
        (slots_[s.idx[0]] ^ slots_[s.idx[1]] ^ slots_[s.idx[2]] ^ target) &
        slot_mask;
  }

  const uint64_t total_bits = uint64_t{num_slots} * slot_bits;
  const size_t payload_bytes = static_cast<size_t>((total_bits + 7) / 8);
  out->clear();
  out->reserve(kHeaderBytes + payload_bytes);
  out->push_back(kFormatVersion);
  out->push_back(static_cast<char>(chosen_seed));
  out->push_back(static_cast<char>(fp_bits));
  out->push_back(static_cast<char>(page_bits));
  PutFixed32(out, seg_len);
  out->resize(kHeaderBytes + payload_bytes, '\0');

  char* payload = out->data() + kHeaderBytes;
  for (uint32_t i = 0; i < num_slots; ++i) {
    const uint64_t val = slots_[i];
    if (val == 0) {
      continue;
    }
    const uint64_t bit_pos = uint64_t{i} * slot_bits;
    size_t byte_pos = static_cast<size_t>(bit_pos >> 3);
    const uint32_t shift = static_cast<uint32_t>(bit_pos & 7);
    payload[byte_pos] |=
        static_cast<char>(static_cast<uint8_t>((val << shift) & 0xffU));
    uint32_t bits_written = 8U - shift;
    while (bits_written < slot_bits) {
      ++byte_pos;
      payload[byte_pos] |= static_cast<char>(
          static_cast<uint8_t>((val >> bits_written) & 0xffU));
      bits_written += 8U;
    }
  }

  Clear();
  return Status::OK();
}

Status TileXorFilterReader::Init(const Slice& encoded, size_t num_pages) {
  if (num_pages == 0 || encoded.size() < TileXorFilterBuilder::kHeaderBytes) {
    return Status::Corruption("KiWi tile XOR filter: truncated header");
  }
  const unsigned char* hdr =
      reinterpret_cast<const unsigned char*>(encoded.data());
  if (hdr[0] != static_cast<unsigned char>(TileXorFilterBuilder::kFormatVersion)) {
    return Status::Corruption("KiWi tile XOR filter: unknown version");
  }
  const uint8_t seed = hdr[1];
  const uint8_t fp_bits = hdr[2];
  const uint8_t page_bits = hdr[3];
  const uint32_t seg_len = DecodeFixed32(encoded.data() + 4);
  if (fp_bits == 0 || fp_bits > 64 || page_bits != CeilLog2(num_pages) ||
      num_pages > std::numeric_limits<uint32_t>::max() ||
      static_cast<uint32_t>(fp_bits) + page_bits > 64 || seg_len == 0) {
    return Status::Corruption("KiWi tile XOR filter: invalid header fields");
  }
  const uint8_t slot_bits = static_cast<uint8_t>(fp_bits + page_bits);
  const uint64_t num_slots = uint64_t{3} * seg_len;
  if (num_slots > std::numeric_limits<uint64_t>::max() / slot_bits) {
    return Status::Corruption("KiWi tile XOR filter: size overflow");
  }
  const uint64_t payload_bytes = (num_slots * slot_bits + 7) / 8;
  if (encoded.size() - TileXorFilterBuilder::kHeaderBytes != payload_bytes) {
    return Status::Corruption("KiWi tile XOR filter: payload length mismatch");
  }
  payload_ = encoded.data() + TileXorFilterBuilder::kHeaderBytes;
  payload_size_ = static_cast<size_t>(payload_bytes);
  seg_len_ = seg_len;
  num_pages_ = static_cast<uint32_t>(num_pages);
  seed_ = seed;
  fp_bits_ = fp_bits;
  page_bits_ = page_bits;
  slot_bits_ = slot_bits;
  fp_mask_ = fp_bits == 64 ? std::numeric_limits<uint64_t>::max()
                           : (uint64_t{1} << fp_bits) - 1;
  page_mask_ = page_bits == 0 ? 0 : (uint64_t{1} << page_bits) - 1;
  slot_mask_ = slot_bits == 64 ? std::numeric_limits<uint64_t>::max()
                               : (uint64_t{1} << slot_bits) - 1;
  return Status::OK();
}

uint64_t TileXorFilterReader::ReadSlot(uint32_t slot) const {
  const uint64_t bit_pos = uint64_t{slot} * slot_bits_;
  const size_t byte_pos = static_cast<size_t>(bit_pos >> 3);
  const uint32_t shift = static_cast<uint32_t>(bit_pos & 7);
  if (LIKELY(byte_pos + sizeof(uint64_t) <= payload_size_ &&
             shift + slot_bits_ <= 64)) {
    return (DecodeFixed64(payload_ + byte_pos) >> shift) & slot_mask_;
  }
  const size_t bytes_needed =
      static_cast<size_t>((uint32_t{slot_bits_} + shift + 7U) / 8U);
  uint64_t low = 0;
  for (size_t i = 0; i < bytes_needed && i < sizeof(uint64_t); ++i) {
    low |= static_cast<uint64_t>(static_cast<uint8_t>(payload_[byte_pos + i]))
           << (i * 8);
  }
  uint64_t val = low >> shift;
  if (bytes_needed > sizeof(uint64_t)) {
    const uint64_t extra =
        static_cast<uint64_t>(static_cast<uint8_t>(payload_[byte_pos + 8]));
    val |= extra << (64U - shift);
  }
  return val & slot_mask_;
}

bool TileXorFilterReader::MayMatchPage(const Slice& user_key,
                                       uint32_t* page_index) const {
  assert(page_index != nullptr);
  const uint64_t hash = GetSliceHash64(user_key);
  const XorFilterHash::XorSlots s =
      XorFilterHash::SeededDerive(hash, seg_len_, seed_);
  const uint64_t combined =
      ReadSlot(s.idx[0]) ^ ReadSlot(s.idx[1]) ^ ReadSlot(s.idx[2]);
  const uint64_t got_fp = page_bits_ == 0 ? combined : (combined >> page_bits_);
  if (got_fp != (s.fp & fp_mask_)) {
    return false;
  }
  const uint32_t p =
      page_bits_ == 0 ? 0U : static_cast<uint32_t>(combined & page_mask_);
  if (p >= num_pages_) {
    return false;
  }
  *page_index = p;
  return true;
}

void DirectoryBuilder::AddTile(const std::vector<DirectoryPage>& pages) {
  assert(!pages.empty());
  assert(num_tiles_ == 0 || !use_tile_filters_);
  use_tile_filters_ = false;
  PutVarint64(&body_, lossless_cast<uint64_t>(pages.size()));
  for (const DirectoryPage& page : pages) {
    page.handle.EncodeTo(&body_);
    PutFixed64(&body_, page.min_delete_key);
    PutFixed64(&body_, page.max_delete_key);
    PutLengthPrefixedSlice(&body_, page.bloom);
    filter_bytes_ += page.bloom.size();
  }
  ++num_tiles_;
}

void DirectoryBuilder::AddTileWithXorFilter(
    const std::vector<DirectoryPage>& pages, const Slice& tile_xor_filter) {
  assert(!pages.empty());
  assert(!tile_xor_filter.empty());
  assert(num_tiles_ == 0 || use_tile_filters_);
  use_tile_filters_ = true;
  PutVarint64(&body_, lossless_cast<uint64_t>(pages.size()));
  PutLengthPrefixedSlice(&body_, tile_xor_filter);
  filter_bytes_ += tile_xor_filter.size();
  for (const DirectoryPage& page : pages) {
    page.handle.EncodeTo(&body_);
    PutFixed64(&body_, page.min_delete_key);
    PutFixed64(&body_, page.max_delete_key);
  }
  ++num_tiles_;
}

void DirectoryBuilder::Finish(std::string* out) const {
  out->clear();
  out->push_back(use_tile_filters_ ? kFormatVersionTileFilter : kFormatVersion);
  PutVarint32(out, num_tiles_);
  out->append(body_);
}

namespace {
// Smallest possible encoded page: 1-byte offset and size varints, two fixed64
// delete keys and (for v1) a 1-byte bloom length. Bounds allocations from
// corrupt counts.
constexpr size_t kMinEncodedPageBytesV1 = 2 + 2 * sizeof(uint64_t) + 1;
constexpr size_t kMinEncodedPageBytesV2 = 2 + 2 * sizeof(uint64_t);
}  // namespace

Directory::Directory() = default;
Directory::~Directory() = default;

Status Directory::Decode(std::string contents) {
  pages_.clear();
  tile_first_page_.clear();
  filters_.clear();
  tile_filters_.clear();
  filter_bytes_ = 0;
  contents_ = std::move(contents);

  const auto corrupt = [this](const char* what) {
    pages_.clear();
    tile_first_page_.clear();
    filters_.clear();
    tile_filters_.clear();
    filter_bytes_ = 0;
    return Status::Corruption(std::string("KiWi directory: ") + what);
  };

  Slice input(contents_);
  if (input.empty()) {
    return corrupt("unknown version");
  }
  const char version = input[0];
  if (version != DirectoryBuilder::kFormatVersion &&
      version != DirectoryBuilder::kFormatVersionTileFilter) {
    return corrupt("unknown version");
  }
  const bool is_tile_filter =
      (version == DirectoryBuilder::kFormatVersionTileFilter);
  const size_t min_page_bytes =
      is_tile_filter ? kMinEncodedPageBytesV2 : kMinEncodedPageBytesV1;
  input.remove_prefix(1);
  uint32_t num_tiles = 0;
  if (!GetVarint32(&input, &num_tiles)) {
    return corrupt("bad tile count");
  }
  const size_t min_tile_bytes =
      is_tile_filter ? (2 + min_page_bytes) : (1 + min_page_bytes);
  if (num_tiles > input.size() / min_tile_bytes) {
    return corrupt("tile count exceeds directory size");
  }
  tile_first_page_.reserve(num_tiles);
  if (is_tile_filter) {
    tile_filters_.reserve(num_tiles);
  }

  uint64_t prev_tile_offset = 0;
  for (uint32_t t = 0; t < num_tiles; ++t) {
    uint64_t num_pages = 0;
    if (!GetVarint64(&input, &num_pages) || num_pages == 0) {
      return corrupt("bad page count");
    }
    if (is_tile_filter) {
      Slice raw_tile_filter;
      if (!GetLengthPrefixedSlice(&input, &raw_tile_filter)) {
        return corrupt("truncated tile filter");
      }
      if (num_pages > input.size() / min_page_bytes) {
        return corrupt("bad page count");
      }
      TileXorFilterReader reader;
      if (!reader.Init(raw_tile_filter, static_cast<size_t>(num_pages)).ok()) {
        return corrupt("bad tile XOR filter");
      }
      tile_filters_.push_back(reader);
      filter_bytes_ += raw_tile_filter.size();
    } else if (num_pages > input.size() / min_page_bytes) {
      return corrupt("bad page count");
    }
    tile_first_page_.push_back(pages_.size());
    for (uint64_t i = 0; i < num_pages; ++i) {
      DirectoryPage page;
      if (!page.handle.DecodeFrom(&input).ok() ||
          !GetFixed64(&input, &page.min_delete_key) ||
          !GetFixed64(&input, &page.max_delete_key)) {
        return corrupt("truncated page record");
      }
      if (!is_tile_filter) {
        if (!GetLengthPrefixedSlice(&input, &page.bloom)) {
          return corrupt("truncated page record");
        }
        filter_bytes_ += page.bloom.size();
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
  if (!is_tile_filter) {
    filters_.reserve(pages_.size());
    for (const DirectoryPage& page : pages_) {
      filters_.emplace_back(
          page.bloom.empty()
              ? nullptr
              : BuiltinFilterPolicy::GetBuiltinFilterBitsReader(page.bloom));
    }
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

bool Directory::TileMayMatch(size_t tile_index, const Slice& user_key,
                             uint32_t* page_index) const {
  assert(tile_index < tile_filters_.size());
  return tile_filters_[tile_index].MayMatchPage(user_key, page_index);
}

bool Directory::PageMayMatch(const DirectoryPage* page,
                             const Slice& user_key) const {
  assert(page >= pages_.data() && page < pages_.data() + pages_.size());
  if (filters_.empty()) {
    return true;
  }
  FilterBitsReader* filter = filters_[page - pages_.data()].get();
  return filter == nullptr || filter->MayMatch(user_key);
}

size_t Directory::ApproximateMemoryUsage() const {
  // The filter readers are small objects that point into contents_.
  constexpr size_t kApproxFilterReaderBytes = 64;
  return contents_.capacity() + pages_.capacity() * sizeof(DirectoryPage) +
         tile_first_page_.capacity() * sizeof(size_t) +
         filters_.capacity() * sizeof(filters_[0]) +
         filters_.size() * kApproxFilterReaderBytes +
         tile_filters_.capacity() * sizeof(TileXorFilterReader);
}

}  // namespace kiwi
}  // namespace ROCKSDB_NAMESPACE
