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

namespace {
// Hides the entries a KiWi secondary delete removed: kTypeValue entries whose
// delete key is below the floor. Everything else is passed through.
class DeleteFloorIterator : public InternalIterator {
 public:
  DeleteFloorIterator(std::unique_ptr<InternalIterator> iter,
                      uint32_t delete_key_offset, uint64_t floor)
      : iter_(std::move(iter)), offset_(delete_key_offset), floor_(floor) {}

  bool Valid() const override { return status_.ok() && iter_->Valid(); }
  void SeekToFirst() override {
    iter_->SeekToFirst();
    SkipDeletedForward();
  }
  void SeekToLast() override {
    iter_->SeekToLast();
    SkipDeletedBackward();
  }
  void Seek(const Slice& target) override {
    iter_->Seek(target);
    SkipDeletedForward();
  }
  void SeekForPrev(const Slice& target) override {
    iter_->SeekForPrev(target);
    SkipDeletedBackward();
  }
  void Next() override {
    iter_->Next();
    SkipDeletedForward();
  }
  void Prev() override {
    iter_->Prev();
    SkipDeletedBackward();
  }
  Slice key() const override { return iter_->key(); }
  Slice value() const override { return iter_->value(); }
  Status status() const override {
    return status_.ok() ? iter_->status() : status_;
  }
  void SetPinnedItersMgr(PinnedIteratorsManager* mgr) override {
    iter_->SetPinnedItersMgr(mgr);
  }
  bool IsKeyPinned() const override { return iter_->IsKeyPinned(); }
  bool IsValuePinned() const override { return iter_->IsValuePinned(); }

 private:
  // A value too short for its delete key stops the iteration with an error.
  bool IsDeleted() {
    if (ExtractValueType(iter_->key()) != kTypeValue) {
      return false;
    }
    uint64_t delete_key = 0;
    status_ = kiwi::ExtractDeleteKey(kTypeValue, iter_->value(), offset_,
                                     &delete_key);
    return status_.ok() && delete_key < floor_;
  }
  void SkipDeletedForward() {
    while (status_.ok() && iter_->Valid() && IsDeleted()) {
      iter_->Next();
    }
  }
  void SkipDeletedBackward() {
    while (status_.ok() && iter_->Valid() && IsDeleted()) {
      iter_->Prev();
    }
  }

  std::unique_ptr<InternalIterator> iter_;
  const uint32_t offset_;
  const uint64_t floor_;
  Status status_;
};
}  // namespace

InternalIterator* KiWiTableIterator::NewPageIterator(
    BlockBasedTable* table, const ReadOptions& read_options,
    const kiwi::DirectoryPage& page, GetContext* get_context,
    BlockCacheLookupContext* lookup_context, bool for_compaction) {
  const BlockBasedTable::Rep* rep = table->get_rep();
  const uint64_t floor = rep->kiwi_delete_floor;
  if (floor != 0 && page.max_delete_key < floor) {
    return nullptr;  // fully deleted: not even read
  }
  Status s;
  InternalIterator* iter = table->NewDataBlockIterator<DataBlockIter>(
      read_options, page.handle, /*input_iter=*/nullptr, BlockType::kData,
      get_context, lookup_context, /*prefetch_buffer=*/nullptr, for_compaction,
      /*async_read=*/false, s, /*use_block_cache_for_lookup=*/true);
  if (floor != 0 && page.min_delete_key < floor) {
    return new DeleteFloorIterator(std::unique_ptr<InternalIterator>(iter),
                                   rep->kiwi_delete_key_offset, floor);
  }
  return iter;
}

void KiWiTableIterator::ReleaseTile() {
  if (IsPinning()) {
    pinned_iters_mgr_->PinIterator(tile_iter_.release());
  } else {
    tile_iter_.reset();
  }
}

void KiWiTableIterator::InitTile() {
  ReleaseTile();
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
    InternalIterator* page = NewPageIterator(
        table_, read_options_, tile.pages[i], /*get_context=*/nullptr,
        &lookup_context_,
        /*for_compaction=*/caller_ == TableReaderCaller::kCompaction);
    if (page != nullptr) {
      page_scratch_.push_back(page);
    }
  }
  // Takes ownership of the page iterators.
  tile_iter_.reset(NewMergingIterator(&icomp_, page_scratch_.data(),
                                      static_cast<int>(page_scratch_.size())));
  if (pinned_iters_mgr_ != nullptr) {
    tile_iter_->SetPinnedItersMgr(pinned_iters_mgr_);
  }
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

void KiWiTableIterator::SkipEmptyTilesBackward() {
  while (status_.ok() && tile_iter_ != nullptr && !tile_iter_->Valid() &&
         tile_iter_->status().ok()) {
    index_iter_->Prev();
    InitTile();
    if (tile_iter_ != nullptr) {
      tile_iter_->SeekToLast();
    }
  }
}

void KiWiTableIterator::SeekToLast() {
  index_iter_->SeekToLast();
  InitTile();
  if (tile_iter_ != nullptr) {
    tile_iter_->SeekToLast();
    SkipEmptyTilesBackward();
  }
}

void KiWiTableIterator::SeekForPrev(const Slice& target) {
  // The first tile whose last key is >= target may hold the last key <= target;
  // if it holds none, that key is the last one of an earlier tile. When no tile
  // ends at or after target, it is the last key of the file.
  index_iter_->Seek(target);
  if (!index_iter_->Valid()) {
    if (!index_iter_->status().ok()) {
      ReleaseTile();
      return;  // reported by status()
    }
    index_iter_->SeekToLast();
  }
  InitTile();
  if (tile_iter_ != nullptr) {
    tile_iter_->SeekForPrev(target);
    SkipEmptyTilesBackward();
  }
}

void KiWiTableIterator::Prev() {
  assert(Valid());
  tile_iter_->Prev();
  SkipEmptyTilesBackward();
}

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
