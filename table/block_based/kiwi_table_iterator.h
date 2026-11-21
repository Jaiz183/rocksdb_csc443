//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <memory>
#include <vector>

#include "db/dbformat.h"
#include "rocksdb/options.h"
#include "table/block_based/block_based_table_reader.h"
#include "table/internal_iterator.h"
#include "trace_replay/block_cache_tracer.h"

namespace ROCKSDB_NAMESPACE {

// Iterates a file written with KiWi delete tiles in internal key order.
//
// Tiles are disjoint and ordered by key, so the iterator walks the index (one
// entry per tile) like an ordinary table iterator. Inside a tile the pages are
// ordered by delete key, not by key, so the iterator opens all pages of the
// current tile and merges them. This is the paper's cost for range scans: a
// tile is always read whole.
//
// Not supported on KiWi files: readahead, prefetching and multi-scan.
class KiWiTableIterator : public InternalIterator {
 public:
  KiWiTableIterator(
      BlockBasedTable* table, const ReadOptions& read_options,
      const InternalKeyComparator& icomp,
      std::unique_ptr<InternalIteratorBase<IndexValue>> index_iter,
      TableReaderCaller caller);

  bool Valid() const override {
    return tile_iter_ != nullptr && status_.ok() && tile_iter_->Valid();
  }
  void SeekToFirst() override;
  void SeekToLast() override;
  void Seek(const Slice& target) override;
  void SeekForPrev(const Slice& target) override;
  void Next() override;
  void Prev() override;
  Slice key() const override {
    assert(Valid());
    return tile_iter_->key();
  }
  Slice value() const override {
    assert(Valid());
    return tile_iter_->value();
  }
  Status status() const override;

 private:
  // Positions tile_iter_ on the tile the index iterator points at, or clears it
  // if the index iterator is not valid or the tile cannot be opened.
  void InitTile();
  // Opens a page as a block iterator; errors surface through its status.
  InternalIterator* NewPageIterator(const BlockHandle& handle);
  // After the current tile ran out, moves on to the first entry of the next
  // non-empty tile.
  void SkipEmptyTilesForward();
  // Likewise, moves on to the last entry of the previous non-empty tile.
  void SkipEmptyTilesBackward();

  BlockBasedTable* const table_;
  // Must outlive the iterator, like for every TableReader::NewIterator().
  const ReadOptions& read_options_;
  const InternalKeyComparator& icomp_;
  std::unique_ptr<InternalIteratorBase<IndexValue>> index_iter_;
  const TableReaderCaller caller_;
  BlockCacheLookupContext lookup_context_;
  // Merges the pages of the current tile; null when not positioned.
  std::unique_ptr<InternalIterator> tile_iter_;
  std::vector<InternalIterator*> page_scratch_;
  Status status_;
};

}  // namespace ROCKSDB_NAMESPACE
