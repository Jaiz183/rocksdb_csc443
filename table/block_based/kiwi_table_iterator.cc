//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "table/block_based/kiwi_table_iterator.h"

#include "table/block_based/kiwi_tile.h"
#include "table/merging_iterator.h"

namespace ROCKSDB_NAMESPACE {

KiWiTableIterator::KiWiTableIterator(
    BlockBasedTable* table, const ReadOptions& read_options,
    const InternalKeyComparator& icomp,
    std::unique_ptr<InternalIteratorBase<IndexValue>> index_iter,
    TableReaderCaller caller)
    : table_(table),
      read_options_(read_options),
      icomp_(icomp),
      index_iter_(std::move(index_iter)),
      caller_(caller),
      lookup_context_(caller) {
  assert(table_->get_rep()->kiwi_directory != nullptr);
}

InternalIterator* KiWiTableIterator::NewPageIterator(
    const BlockHandle& handle) {
  Status s;
  return table_->NewDataBlockIterator<DataBlockIter>(
      read_options_, handle, /*input_iter=*/nullptr, BlockType::kData,
      /*get_context=*/nullptr, &lookup_context_, /*prefetch_buffer=*/nullptr,
      /*for_compaction=*/caller_ == TableReaderCaller::kCompaction,
      /*async_read=*/false, s, /*use_block_cache_for_lookup=*/true);
}

void KiWiTableIterator::InitTile() {
  tile_iter_.reset();
  if (!index_iter_->Valid()) {
    return;  // the end of the file, or an index error reported by status()
  }
  // The index entry of a tile points at its first page.
  const kiwi::Directory& dir = *table_->get_rep()->kiwi_directory;
  const uint64_t tile_offset = index_iter_->value().handle.offset();
  const size_t tile_index = dir.FindTile(tile_offset);
  if (tile_index == dir.num_tiles()) {
    status_ = Status::Corruption("KiWi: no tile in the directory at offset " +
                                 std::to_string(tile_offset));
    return;
  }
  const kiwi::Directory::TileView tile = dir.tile(tile_index);
  page_scratch_.clear();
  for (size_t i = 0; i < tile.num_pages; ++i) {
    page_scratch_.push_back(NewPageIterator(tile.pages[i].handle));
  }
  // Takes ownership of the page iterators.
  tile_iter_.reset(NewMergingIterator(
      &icomp_, page_scratch_.data(), static_cast<int>(page_scratch_.size())));
}

void KiWiTableIterator::SkipEmptyTilesForward() {
  while (status_.ok() && tile_iter_ != nullptr && !tile_iter_->Valid() &&
         tile_iter_->status().ok()) {
    index_iter_->Next();
    InitTile();
    if (tile_iter_ != nullptr) {
      tile_iter_->SeekToFirst();
    }
  }
}

void KiWiTableIterator::SeekToFirst() {
  index_iter_->SeekToFirst();
  InitTile();
  if (tile_iter_ != nullptr) {
    tile_iter_->SeekToFirst();
    SkipEmptyTilesForward();
  }
}

void KiWiTableIterator::Seek(const Slice& target) {
  // The first tile whose last key is >= target is the only one that can hold
  // the first key >= target, unless it turns out to have none.
  index_iter_->Seek(target);
  InitTile();
  if (tile_iter_ != nullptr) {
    tile_iter_->Seek(target);
    SkipEmptyTilesForward();
  }
}

void KiWiTableIterator::Next() {
  assert(Valid());
  tile_iter_->Next();
  SkipEmptyTilesForward();
}

// TODO(kiwi): backward iteration is added by the next commit. Until then it
// fails loudly instead of returning wrong results.
void KiWiTableIterator::SeekToLast() {
  tile_iter_.reset();
  status_ = Status::NotSupported("KiWi: backward iteration not implemented");
}

void KiWiTableIterator::SeekForPrev(const Slice& /*target*/) { SeekToLast(); }

void KiWiTableIterator::Prev() { SeekToLast(); }

Status KiWiTableIterator::status() const {
  if (!status_.ok()) {
    return status_;
  }
  if (!index_iter_->status().ok()) {
    return index_iter_->status();
  }
  return tile_iter_ != nullptr ? tile_iter_->status() : Status::OK();
}

}  // namespace ROCKSDB_NAMESPACE
