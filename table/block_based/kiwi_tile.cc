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
  order_scratch_.resize(refs_.size());
  std::iota(order_scratch_.begin(), order_scratch_.end(), size_t{0});
  std::sort(order_scratch_.begin(), order_scratch_.end(),
            [this](size_t a, size_t b) {
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
  for (size_t pos : order_scratch_) {
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

uint64_t DeleteRankUpperBound(uint64_t min_d, uint64_t max_d,
                              uint8_t rank_bits, uint32_t bucket) {
  if (rank_bits == 0 || min_d >= max_d) {
    return max_d;
  }
  const uint32_t num_buckets = uint32_t{1} << rank_bits;
  if (bucket + 1 >= num_buckets) {
    return max_d;
  }
  const uint64_t span = max_d - min_d;
  const uint64_t step = (span >> rank_bits) + 1;
  const uint64_t hi_delta = uint64_t{bucket + 1} * step - 1;
  if (hi_delta >= span) {
    return max_d;
  }
  return min_d + hi_delta;
}

uint8_t ComputeDeleteRankBucket(uint64_t min_d, uint64_t max_d,
                                uint8_t rank_bits, uint64_t delete_key) {
  if (rank_bits == 0 || min_d >= max_d) {
    return 0;
  }
  if (delete_key <= min_d) {
    return 0;
  }
  const uint32_t max_bucket = (uint32_t{1} << rank_bits) - 1;
  if (delete_key >= max_d) {
    return static_cast<uint8_t>(max_bucket);
  }
  const uint64_t span = max_d - min_d;
  const uint64_t step = (span >> rank_bits) + 1;
  const uint64_t delta = delete_key - min_d;
  return static_cast<uint8_t>(std::min<uint64_t>(max_bucket, delta / step));
}

bool DeleteRankMaySurviveFloor(uint64_t min_d, uint64_t max_d,
                               uint8_t rank_bits, uint32_t bucket,
                               uint64_t floor) {
  if (floor == 0) {
    return true;
  }
  if (max_d < floor) {
    return false;
  }
  if (rank_bits == 0 || min_d >= floor) {
    return true;
  }
  return DeleteRankUpperBound(min_d, max_d, rank_bits, bucket) >= floor;
}

TileXorFilterBuilder::TileXorFilterBuilder(uint8_t fingerprint_bytes)
    : TileXorFilterBuilder(fingerprint_bytes, 0) {}

TileXorFilterBuilder::TileXorFilterBuilder(uint8_t fingerprint_bytes,
                                           uint8_t rank_bits)
    : fingerprint_bytes_(fingerprint_bytes), rank_bits_(rank_bits) {}

void TileXorFilterBuilder::AddKey(const Slice& user_key, uint32_t page_index) {
  AddKey(user_key, page_index, next_order_++, 0);
}

void TileXorFilterBuilder::AddKey(const Slice& user_key, uint32_t page_index,
                                  uint32_t order, uint8_t rank_bucket) {
  assert(page_index < (uint32_t{1} << 28));
  entries_.push_back(Entry{GetSliceHash64(user_key), order,
                           (page_index << 4) | (uint32_t{rank_bucket} & 0xfU)});
}

namespace {
uint64_t BitMaskFromBits(uint8_t bits) {
  return bits == 64 ? std::numeric_limits<uint64_t>::max()
                    : (bits == 0 ? 0 : (uint64_t{1} << bits) - 1);
}
}  // namespace

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
  if (rank_bits_ > 4) {
    Clear();
    return Status::InvalidArgument(
        "KiWi tile XOR filter rank bits must be in [0, 4]");
  }
  const uint8_t page_bits = CeilLog2(num_pages);
  if (page_bits > 28) {
    Clear();
    return Status::InvalidArgument("KiWi tile XOR filter: too many pages");
  }
  const uint32_t meta_bits = uint32_t{page_bits} + rank_bits_;
  const uint8_t fp_bits = static_cast<uint8_t>(
      std::min<uint32_t>(uint32_t{fingerprint_bytes_} * 8U, 64U - meta_bits));
  const uint8_t slot_bits = static_cast<uint8_t>(fp_bits + meta_bits);
  const uint64_t fp_mask = BitMaskFromBits(fp_bits);
  const uint64_t slot_mask = BitMaskFromBits(slot_bits);
  const uint8_t rank_mask =
      rank_bits_ == 0 ? 0 : static_cast<uint8_t>((1U << rank_bits_) - 1U);

  // Deduplicate equal hashes keeping the earliest insertion order (newest
  // internal key if versions ever share a tile or file) for page_index while
  // taking the max rank bucket across duplicate versions so secondary delete
  // pruning never drops a surviving older version on the same page.
  std::sort(entries_.begin(), entries_.end(),
            [](const Entry& a, const Entry& b) {
              return a.hash != b.hash ? a.hash < b.hash : a.order < b.order;
            });
  size_t write_idx = 0;
  for (size_t read_idx = 0; read_idx < entries_.size(); ++read_idx) {
    if (write_idx == 0 ||
        entries_[read_idx].hash != entries_[write_idx - 1].hash) {
      entries_[write_idx] = entries_[read_idx];
      entries_[write_idx].page_and_rank =
          (entries_[read_idx].page_and_rank & ~0xfU) |
          (entries_[read_idx].page_and_rank & rank_mask);
      ++write_idx;
    } else {
      const uint32_t prev_rank = entries_[write_idx - 1].page_and_rank & 0xfU;
      const uint32_t cur_rank = entries_[read_idx].page_and_rank & rank_mask;
      if (cur_rank > prev_rank) {
        entries_[write_idx - 1].page_and_rank =
            (entries_[write_idx - 1].page_and_rank & ~0xfU) | cur_rank;
      }
    }
  }
  entries_.resize(write_idx);

  if (entries_.size() > (std::numeric_limits<uint32_t>::max() - 32) / 2) {
    Clear();
    return Status::InvalidArgument("KiWi tile XOR filter: too many keys");
  }
  const uint32_t num_keys = static_cast<uint32_t>(entries_.size());
  // Tight 3-way XOR baseline sizing (+4 slots instead of +32); if 8 seeds
  // fail, step seg_len upward slowly until peeling succeeds.
  const uint32_t base_unrounded = static_cast<uint32_t>(1.23 * num_keys) + 4;
  uint32_t seg_len = std::max<uint32_t>(2U, (base_unrounded + 2) / 3);
  derived_.resize(num_keys);

  bool peeled = false;
  uint8_t chosen_seed = 0;
  uint32_t chosen_seg_len = seg_len;
  for (uint32_t attempt = 0; attempt < 256; ++attempt) {
    if (attempt > 0 && (attempt & 7U) == 0U) {
      seg_len += std::max<uint32_t>(1U, seg_len / 64U);
    }
    const uint32_t num_slots = 3 * seg_len;
    const uint8_t seed = static_cast<uint8_t>(attempt);
    degree_.assign(num_slots, 0);
    xor_edge_.assign(num_slots, 0);
    for (uint32_t i = 0; i < num_keys; ++i) {
      const XorFilterHash::XorSlots s =
          XorFilterHash::SeededDerive(entries_[i].hash, seg_len, seed);
      derived_[i] = CachedSlots{{s.idx[0], s.idx[1], s.idx[2]}, s.fp};
      ++degree_[s.idx[0]];
      ++degree_[s.idx[1]];
      ++degree_[s.idx[2]];
      xor_edge_[s.idx[0]] ^= i;
      xor_edge_[s.idx[1]] ^= i;
      xor_edge_[s.idx[2]] ^= i;
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
      const CachedSlots& s = derived_[e];
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
      chosen_seg_len = seg_len;
      break;
    }
  }

  if (!peeled) {
    Clear();
    return Status::Corruption(
        "KiWi tile XOR filter: failed to peel hypergraph over 256 attempts");
  }

  const uint32_t num_slots = 3 * chosen_seg_len;
  slots_.assign(num_slots, 0);
  while (!peel_stack_.empty()) {
    const PeelStep step = peel_stack_.back();
    peel_stack_.pop_back();
    const Entry& e = entries_[step.edge_idx];
    const CachedSlots& s = derived_[step.edge_idx];
    const uint64_t fp = s.fp & fp_mask;
    const uint32_t page_index = e.page_and_rank >> 4;
    const uint32_t rank_bucket = e.page_and_rank & 0xfU;
    uint64_t target = fp;
    if (meta_bits > 0) {
      target = (target << page_bits) | page_index;
      if (rank_bits_ > 0) {
        target = (target << rank_bits_) | rank_bucket;
      }
    }
    slots_[step.vertex] =
        (slots_[s.idx[0]] ^ slots_[s.idx[1]] ^ slots_[s.idx[2]] ^ target) &
        slot_mask;
  }

  const bool use_v2 = (rank_bits_ > 0);
  const size_t header_bytes = use_v2 ? kHeaderBytesV2 : kHeaderBytes;
  const uint64_t total_bits = uint64_t{num_slots} * slot_bits;
  const size_t payload_bytes = static_cast<size_t>((total_bits + 7) / 8);
  out->clear();
  out->reserve(header_bytes + payload_bytes);
  out->push_back(use_v2 ? kFormatVersionAugmented : kFormatVersion);
  out->push_back(static_cast<char>(chosen_seed));
  out->push_back(static_cast<char>(fp_bits));
  out->push_back(static_cast<char>((rank_bits_ << 5) | page_bits));
  PutFixed32(out, chosen_seg_len);
  if (use_v2) {
    PutFixed32(out, 0);  // base_page = 0
  }
  out->resize(header_bytes + payload_bytes);

  // Pack tightly using a 64-bit little-endian streaming accumulator.
  char* payload = out->data() + header_bytes;
  size_t out_pos = 0;
  uint64_t bit_buf = 0;
  uint32_t bit_count = 0;
  for (uint32_t i = 0; i < num_slots; ++i) {
    const uint64_t val = slots_[i];
    bit_buf |= val << bit_count;
    bit_count += slot_bits;
    if (bit_count >= 64) {
      assert(out_pos + sizeof(uint64_t) <= payload_bytes);
      EncodeFixed64(payload + out_pos, bit_buf);
      out_pos += sizeof(uint64_t);
      bit_count -= 64;
      bit_buf = bit_count == 0 ? 0 : (val >> (slot_bits - bit_count));
    }
  }
  while (bit_count > 0) {
    payload[out_pos++] = static_cast<char>(bit_buf & 0xffU);
    bit_buf >>= 8;
    bit_count = bit_count >= 8 ? bit_count - 8 : 0;
  }
  assert(out_pos == payload_bytes);

  Clear();
  return Status::OK();
}

Status TileXorFilterBuilder::ShiftBasePage(const Slice& encoded_filter,
                                           uint32_t dropped_prefix_pages,
                                           size_t surviving_num_pages,
                                           std::string* out) {
  assert(out != nullptr);
  if (surviving_num_pages == 0 ||
      encoded_filter.size() < TileXorFilterBuilder::kHeaderBytes) {
    return Status::Corruption(
        "KiWi tile XOR filter shift: invalid input filter or page count");
  }
  const unsigned char* hdr =
      reinterpret_cast<const unsigned char*>(encoded_filter.data());
  const char version = static_cast<char>(hdr[0]);
  if (version != kFormatVersion && version != kFormatVersionAugmented) {
    return Status::Corruption("KiWi tile XOR filter shift: unknown version");
  }
  const size_t old_hdr_bytes =
      version == kFormatVersionAugmented ? kHeaderBytesV2 : kHeaderBytes;
  if (encoded_filter.size() < old_hdr_bytes) {
    return Status::Corruption("KiWi tile XOR filter shift: truncated header");
  }
  const uint8_t seed = hdr[1];
  const uint8_t fp_bits = hdr[2];
  const uint8_t rank_bits = static_cast<uint8_t>(hdr[3] >> 5);
  const uint8_t page_bits = static_cast<uint8_t>(hdr[3] & 0x1fU);
  const uint32_t seg_len = DecodeFixed32(encoded_filter.data() + 4);
  const uint32_t old_base_page =
      version == kFormatVersionAugmented
          ? DecodeFixed32(encoded_filter.data() + 8)
          : 0U;
  if (dropped_prefix_pages >
      std::numeric_limits<uint32_t>::max() - old_base_page) {
    return Status::InvalidArgument("KiWi tile XOR filter shift: overflow");
  }
  const uint32_t new_base_page = old_base_page + dropped_prefix_pages;
  if (surviving_num_pages >
          std::numeric_limits<uint32_t>::max() - new_base_page ||
      page_bits != CeilLog2(new_base_page + surviving_num_pages)) {
    return Status::InvalidArgument(
        "KiWi tile XOR filter shift: surviving page count mismatch");
  }
  if (dropped_prefix_pages == 0) {
    out->assign(encoded_filter.data(), encoded_filter.size());
    return Status::OK();
  }
  const size_t payload_bytes = encoded_filter.size() - old_hdr_bytes;
  out->clear();
  out->reserve(kHeaderBytesV2 + payload_bytes);
  out->push_back(kFormatVersionAugmented);
  out->push_back(static_cast<char>(seed));
  out->push_back(static_cast<char>(fp_bits));
  out->push_back(static_cast<char>((rank_bits << 5) | page_bits));
  PutFixed32(out, seg_len);
  PutFixed32(out, new_base_page);
  out->append(encoded_filter.data() + old_hdr_bytes, payload_bytes);
  return Status::OK();
}

Status TileXorFilterReader::Init(const Slice& encoded, size_t num_pages) {
  if (num_pages == 0 || encoded.size() < TileXorFilterBuilder::kHeaderBytes) {
    return Status::Corruption("KiWi tile XOR filter: truncated header");
  }
  const unsigned char* hdr =
      reinterpret_cast<const unsigned char*>(encoded.data());
  const char version = static_cast<char>(hdr[0]);
  size_t header_bytes = 0;
  uint32_t base_page = 0;
  if (version == TileXorFilterBuilder::kFormatVersion) {
    header_bytes = TileXorFilterBuilder::kHeaderBytes;
  } else if (version == TileXorFilterBuilder::kFormatVersionAugmented) {
    if (encoded.size() < TileXorFilterBuilder::kHeaderBytesV2) {
      return Status::Corruption("KiWi tile XOR filter: truncated v2 header");
    }
    header_bytes = TileXorFilterBuilder::kHeaderBytesV2;
    base_page = DecodeFixed32(encoded.data() + 8);
  } else {
    return Status::Corruption("KiWi tile XOR filter: unknown version");
  }
  const uint8_t seed = hdr[1];
  const uint8_t fp_bits = hdr[2];
  const uint8_t rank_bits = static_cast<uint8_t>(hdr[3] >> 5);
  const uint8_t page_bits = static_cast<uint8_t>(hdr[3] & 0x1fU);
  const uint32_t seg_len = DecodeFixed32(encoded.data() + 4);
  if (num_pages > std::numeric_limits<uint32_t>::max() ||
      base_page > std::numeric_limits<uint32_t>::max() - num_pages ||
      rank_bits > 4 || fp_bits == 0 || fp_bits > 64 ||
      page_bits != CeilLog2(static_cast<size_t>(base_page) + num_pages) ||
      static_cast<uint32_t>(fp_bits) + page_bits + rank_bits > 64 ||
      seg_len == 0) {
    return Status::Corruption("KiWi tile XOR filter: invalid header fields");
  }
  if (version == TileXorFilterBuilder::kFormatVersion && rank_bits != 0) {
    return Status::Corruption("KiWi tile XOR filter: invalid v1 header fields");
  }
  const uint8_t slot_bits = static_cast<uint8_t>(fp_bits + page_bits + rank_bits);
  const uint64_t num_slots = uint64_t{3} * seg_len;
  if (num_slots > std::numeric_limits<uint64_t>::max() / slot_bits) {
    return Status::Corruption("KiWi tile XOR filter: size overflow");
  }
  const uint64_t payload_bytes = (num_slots * slot_bits + 7) / 8;
  if (encoded.size() - header_bytes != payload_bytes) {
    return Status::Corruption("KiWi tile XOR filter: payload length mismatch");
  }
  payload_ = encoded.data() + header_bytes;
  seg_len_ = seg_len;
  num_pages_ = static_cast<uint32_t>(num_pages);
  base_page_ = base_page;
  min_live_page_ = 0;
  seed_ = seed;
  fp_bits_ = fp_bits;
  page_bits_ = page_bits;
  rank_bits_ = rank_bits;
  return Status::OK();
}

uint64_t TileXorFilterReader::ReadSlot(uint32_t slot, uint8_t slot_bits,
                                       size_t payload_bytes,
                                       uint64_t slot_mask) const {
  const uint64_t bit_pos = uint64_t{slot} * slot_bits;
  const size_t byte_pos = static_cast<size_t>(bit_pos >> 3);
  const uint32_t shift = static_cast<uint32_t>(bit_pos & 7);
  if (LIKELY(byte_pos + sizeof(uint64_t) <= payload_bytes &&
             shift + slot_bits <= 64)) {
    return (DecodeFixed64(payload_ + byte_pos) >> shift) & slot_mask;
  }
  const size_t bytes_needed =
      static_cast<size_t>((uint32_t{slot_bits} + shift + 7U) / 8U);
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
  return val & slot_mask;
}

bool TileXorFilterReader::MayMatchPage(const Slice& user_key,
                                       uint32_t* page_index) const {
  uint32_t unused_rank = 0;
  return MayMatchPage(user_key, page_index, &unused_rank);
}

bool TileXorFilterReader::MayMatchPage(const Slice& user_key,
                                       uint32_t* page_index,
                                       uint32_t* rank_bucket) const {
  assert(page_index != nullptr);
  assert(rank_bucket != nullptr);
  if (UNLIKELY(payload_ == nullptr || min_live_page_ >= num_pages_)) {
    return false;
  }
  const uint8_t slot_bits =
      static_cast<uint8_t>(fp_bits_ + page_bits_ + rank_bits_);
  const size_t payload_bytes =
      static_cast<size_t>((uint64_t{3} * seg_len_ * slot_bits + 7) / 8);
  const uint64_t slot_mask = BitMaskFromBits(slot_bits);
  const uint64_t fp_mask = BitMaskFromBits(fp_bits_);
  const uint64_t hash = GetSliceHash64(user_key);
  const XorFilterHash::XorSlots s =
      XorFilterHash::SeededDerive(hash, seg_len_, seed_);
  const uint64_t combined =
      ReadSlot(s.idx[0], slot_bits, payload_bytes, slot_mask) ^
      ReadSlot(s.idx[1], slot_bits, payload_bytes, slot_mask) ^
      ReadSlot(s.idx[2], slot_bits, payload_bytes, slot_mask);
  const uint8_t meta_bits = static_cast<uint8_t>(page_bits_ + rank_bits_);
  const uint64_t got_fp = meta_bits == 0 ? combined : (combined >> meta_bits);
  if (got_fp != (s.fp & fp_mask)) {
    return false;
  }
  const uint64_t page_mask = BitMaskFromBits(page_bits_);
  const uint32_t encoded_p =
      page_bits_ == 0
          ? 0U
          : static_cast<uint32_t>((combined >> rank_bits_) & page_mask);
  if (encoded_p < base_page_) {
    return false;
  }
  const uint32_t local_p = encoded_p - base_page_;
  if (local_p >= num_pages_ || local_p < min_live_page_) {
    return false;
  }
  *page_index = local_p;
  *rank_bucket =
      rank_bits_ == 0
          ? 0U
          : static_cast<uint32_t>(combined & BitMaskFromBits(rank_bits_));
  return true;
}

void DirectoryBuilder::AddTile(const std::vector<DirectoryPage>& pages) {
  assert(!pages.empty());
  assert(num_tiles_ == 0 || (!use_tile_filters_ && !use_file_filter_));
  use_tile_filters_ = false;
  use_file_filter_ = false;
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
  assert(num_tiles_ == 0 || (use_tile_filters_ && !use_file_filter_));
  use_tile_filters_ = true;
  use_file_filter_ = false;
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

void DirectoryBuilder::AddTileWithoutFilter(
    const std::vector<DirectoryPage>& pages) {
  assert(!pages.empty());
  assert(num_tiles_ == 0 || use_file_filter_);
  use_file_filter_ = true;
  use_tile_filters_ = false;
  PutVarint64(&body_, lossless_cast<uint64_t>(pages.size()));
  for (const DirectoryPage& page : pages) {
    page.handle.EncodeTo(&body_);
    PutFixed64(&body_, page.min_delete_key);
    PutFixed64(&body_, page.max_delete_key);
  }
  ++num_tiles_;
}

void DirectoryBuilder::SetFileXorFilter(const Slice& file_xor_filter) {
  assert(!file_xor_filter.empty());
  assert(num_tiles_ == 0 || use_file_filter_);
  use_file_filter_ = true;
  use_tile_filters_ = false;
  file_filter_.assign(file_xor_filter.data(), file_xor_filter.size());
  filter_bytes_ += file_filter_.size();
}

void DirectoryBuilder::Finish(std::string* out) const {
  out->clear();
  if (use_file_filter_) {
    assert(!file_filter_.empty() || num_tiles_ == 0);
    out->push_back(kFormatVersionFileFilter);
    PutVarint32(out, num_tiles_);
    PutLengthPrefixedSlice(out, file_filter_);
    out->append(body_);
  } else {
    out->push_back(use_tile_filters_ ? kFormatVersionTileFilter
                                     : kFormatVersion);
    PutVarint32(out, num_tiles_);
    out->append(body_);
  }
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
  tile_offsets_.clear();
  tile_max_delete_key_.clear();
  raw_tile_filters_.clear();
  filters_.clear();
  tile_filters_.clear();
  has_file_filter_ = false;
  directory_bytes_ = 0;
  filter_bytes_ = 0;
  contents_ = std::move(contents);
  const size_t raw_dir_bytes = contents_.size();

  const auto corrupt = [this](const char* what) {
    pages_.clear();
    tile_first_page_.clear();
    tile_offsets_.clear();
    tile_max_delete_key_.clear();
    raw_tile_filters_.clear();
    filters_.clear();
    tile_filters_.clear();
    has_file_filter_ = false;
    directory_bytes_ = 0;
    filter_bytes_ = 0;
    return Status::Corruption(std::string("KiWi directory: ") + what);
  };

  Slice input(contents_);
  if (input.empty()) {
    return corrupt("unknown version");
  }
  const char version = input[0];
  if (version != DirectoryBuilder::kFormatVersion &&
      version != DirectoryBuilder::kFormatVersionTileFilter &&
      version != DirectoryBuilder::kFormatVersionFileFilter) {
    return corrupt("unknown version");
  }
  const bool is_tile_filter =
      (version == DirectoryBuilder::kFormatVersionTileFilter);
  const bool is_file_filter =
      (version == DirectoryBuilder::kFormatVersionFileFilter);
  const size_t min_page_bytes = (is_tile_filter || is_file_filter)
                                    ? kMinEncodedPageBytesV2
                                    : kMinEncodedPageBytesV1;
  input.remove_prefix(1);
  uint32_t num_tiles = 0;
  if (!GetVarint32(&input, &num_tiles)) {
    return corrupt("bad tile count");
  }
  Slice raw_file_filter;
  if (is_file_filter) {
    if (!GetLengthPrefixedSlice(&input, &raw_file_filter)) {
      return corrupt("truncated file filter");
    }
    if (num_tiles > 0 && raw_file_filter.empty()) {
      return corrupt("missing file filter");
    }
  }
  const size_t min_tile_bytes =
      is_tile_filter ? (2 + min_page_bytes) : (1 + min_page_bytes);
  if (num_tiles > input.size() / min_tile_bytes) {
    return corrupt("tile count exceeds directory size");
  }
  tile_first_page_.reserve(num_tiles);
  tile_offsets_.reserve(num_tiles);
  tile_max_delete_key_.reserve(num_tiles);
  if (is_tile_filter) {
    tile_filters_.reserve(num_tiles);
    raw_tile_filters_.reserve(num_tiles);
  }

  uint64_t prev_tile_offset = 0;
  size_t max_tile_pages = 0;
  std::vector<Slice> tmp_tile_slices;
  if (is_tile_filter) {
    tmp_tile_slices.reserve(num_tiles);
  }
  for (uint32_t t = 0; t < num_tiles; ++t) {
    uint64_t num_pages = 0;
    if (!GetVarint64(&input, &num_pages) || num_pages == 0) {
      return corrupt("bad page count");
    }
    max_tile_pages = std::max<size_t>(max_tile_pages,
                                      static_cast<size_t>(num_pages));
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
      tmp_tile_slices.push_back(raw_tile_filter);
      filter_bytes_ += raw_tile_filter.size();
    } else if (num_pages > input.size() / min_page_bytes) {
      return corrupt("bad page count");
    }
    tile_first_page_.push_back(pages_.size());
    uint64_t tile_max_d = 0;
    for (uint64_t i = 0; i < num_pages; ++i) {
      DirectoryPage page;
      if (!page.handle.DecodeFrom(&input).ok() ||
          !GetFixed64(&input, &page.min_delete_key) ||
          !GetFixed64(&input, &page.max_delete_key)) {
        return corrupt("truncated page record");
      }
      if (!is_tile_filter && !is_file_filter) {
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
        tile_offsets_.push_back(prev_tile_offset);
      }
      tile_max_d = std::max(tile_max_d, page.max_delete_key);
      pages_.push_back(page);
    }
    tile_max_delete_key_.push_back(tile_max_d);
  }
  if (!input.empty()) {
    return corrupt("trailing bytes");
  }
  if (is_file_filter && num_tiles > 0) {
    if (!file_filter_.Init(raw_file_filter, max_tile_pages).ok()) {
      return corrupt("bad file XOR filter");
    }
    filter_bytes_ = raw_file_filter.size();
    has_file_filter_ = true;
  }

  directory_bytes_ = raw_dir_bytes;
  // The directory is pinned in memory for the life of the table reader. In
  // v2/v3 modes, discard serialized page records (already decoded into pages_)
  // and retain only raw filter bytes inside contents_.
  pages_.shrink_to_fit();
  tile_first_page_.shrink_to_fit();
  tile_offsets_.shrink_to_fit();
  tile_max_delete_key_.shrink_to_fit();
  if (is_tile_filter) {
    std::string compacted;
    compacted.reserve(filter_bytes_);
    std::vector<std::pair<size_t, size_t>> spans;
    spans.reserve(num_tiles);
    for (uint32_t t = 0; t < num_tiles; ++t) {
      const size_t off = compacted.size();
      const Slice& raw = tmp_tile_slices[t];
      compacted.append(raw.data(), raw.size());
      spans.emplace_back(off, raw.size());
    }
    contents_.swap(compacted);
    contents_.shrink_to_fit();
    raw_tile_filters_.reserve(num_tiles);
    for (uint32_t t = 0; t < num_tiles; ++t) {
      const Slice raw(contents_.data() + spans[t].first, spans[t].second);
      raw_tile_filters_.push_back(raw);
      const size_t hdr_bytes =
          raw[0] == TileXorFilterBuilder::kFormatVersionAugmented
              ? TileXorFilterBuilder::kHeaderBytesV2
              : TileXorFilterBuilder::kHeaderBytes;
      tile_filters_[t].RelocatePayload(raw.data() + hdr_bytes);
    }
  } else if (is_file_filter) {
    std::string compacted(raw_file_filter.data(), raw_file_filter.size());
    contents_.swap(compacted);
    contents_.shrink_to_fit();
    if (has_file_filter_) {
      const size_t hdr_bytes =
          contents_[0] == TileXorFilterBuilder::kFormatVersionAugmented
              ? TileXorFilterBuilder::kHeaderBytesV2
              : TileXorFilterBuilder::kHeaderBytes;
      file_filter_.RelocatePayload(contents_.data() + hdr_bytes);
    }
  } else {
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
  auto it = std::lower_bound(tile_offsets_.begin(), tile_offsets_.end(),
                             first_page_offset);
  if (it != tile_offsets_.end() && *it == first_page_offset) {
    return static_cast<size_t>(it - tile_offsets_.begin());
  }
  return tile_offsets_.size();
}

bool Directory::FileMayMatch(const Slice& user_key,
                             uint32_t* page_index) const {
  assert(has_file_filter_);
  return file_filter_.MayMatchPage(user_key, page_index);
}

bool Directory::FileMayMatch(const Slice& user_key, uint32_t* page_index,
                             uint32_t* rank_bucket) const {
  assert(has_file_filter_);
  return file_filter_.MayMatchPage(user_key, page_index, rank_bucket);
}

bool Directory::TileMayMatch(size_t tile_index, const Slice& user_key,
                             uint32_t* page_index) const {
  assert(tile_index < tile_filters_.size());
  return tile_filters_[tile_index].MayMatchPage(user_key, page_index);
}

bool Directory::TileMayMatch(size_t tile_index, const Slice& user_key,
                             uint32_t* page_index,
                             uint32_t* rank_bucket) const {
  assert(tile_index < tile_filters_.size());
  return tile_filters_[tile_index].MayMatchPage(user_key, page_index,
                                                rank_bucket);
}

Slice Directory::raw_tile_filter(size_t tile_index) const {
  if (tile_index >= raw_tile_filters_.size()) {
    return Slice();
  }
  return raw_tile_filters_[tile_index];
}

void Directory::ApplyDeleteFloor(uint64_t floor) {
  if (floor == 0 || tile_filters_.empty()) {
    return;
  }
  size_t surviving_bytes = 0;
  bool any_dead = false;
  for (size_t t = 0; t < tile_filters_.size(); ++t) {
    const TileView tv = tile(t);
    uint32_t dead_prefix = 0;
    while (dead_prefix < tv.num_pages &&
           tv.pages[dead_prefix].max_delete_key < floor) {
      ++dead_prefix;
    }
    tile_filters_[t].SetMinLivePage(dead_prefix);
    if (dead_prefix == tv.num_pages) {
      any_dead = true;
    } else {
      surviving_bytes += raw_tile_filters_[t].size();
    }
  }
  if (!any_dead) {
    return;
  }
  // Free XOR filter bytes belonging to 100%-deleted tiles from memory.
  std::string compacted;
  compacted.reserve(surviving_bytes);
  std::vector<std::pair<size_t, size_t>> spans(tile_filters_.size(), {0, 0});
  for (size_t t = 0; t < tile_filters_.size(); ++t) {
    const TileView tv = tile(t);
    if (tile_filters_[t].min_live_page() >= tv.num_pages) {
      raw_tile_filters_[t] = Slice();
      tile_filters_[t].ClearPayload();
      continue;
    }
    const Slice& raw = raw_tile_filters_[t];
    if (!raw.empty()) {
      const size_t off = compacted.size();
      compacted.append(raw.data(), raw.size());
      spans[t] = {off, raw.size()};
    }
  }
  contents_.swap(compacted);
  contents_.shrink_to_fit();
  for (size_t t = 0; t < tile_filters_.size(); ++t) {
    if (spans[t].second == 0) {
      continue;
    }
    const Slice raw(contents_.data() + spans[t].first, spans[t].second);
    raw_tile_filters_[t] = raw;
    const size_t hdr_bytes =
        raw[0] == TileXorFilterBuilder::kFormatVersionAugmented
            ? TileXorFilterBuilder::kHeaderBytesV2
            : TileXorFilterBuilder::kHeaderBytes;
    tile_filters_[t].RelocatePayload(raw.data() + hdr_bytes);
  }
  filter_bytes_ = contents_.size();
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
         tile_offsets_.capacity() * sizeof(uint64_t) +
         tile_max_delete_key_.capacity() * sizeof(uint64_t) +
         raw_tile_filters_.capacity() * sizeof(Slice) +
         filters_.capacity() * sizeof(filters_[0]) +
         filters_.size() * kApproxFilterReaderBytes +
         tile_filters_.capacity() * sizeof(TileXorFilterReader);
}

}  // namespace kiwi
}  // namespace ROCKSDB_NAMESPACE
