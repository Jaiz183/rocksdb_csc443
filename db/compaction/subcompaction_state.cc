//  Copyright (c) Meta Platforms, Inc. and affiliates.
//
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/compaction/subcompaction_state.h"

#include "rocksdb/sst_partitioner.h"

namespace ROCKSDB_NAMESPACE {
void SubcompactionState::AggregateCompactionOutputStats(
    InternalStats::CompactionStatsFull& internal_stats) const {
  // Outputs should be closed. By extension, any files created just for
  // range deletes have already been written also.
  assert(compaction_outputs_.HasBuilder() == false);
  assert(proximal_level_outputs_.HasBuilder() == false);
#ifndef NDEBUG
  for (const std::unique_ptr<CompactionOutputs>& outputs : fade_outputs_) {
    assert(!outputs || !outputs->HasBuilder());
  }
#endif

  // FIXME: These stats currently include abandonned output files
  // assert(compaction_outputs_.stats_.num_output_files ==
  //        compaction_outputs_.outputs_.size());
  // assert(proximal_level_outputs_.stats_.num_output_files ==
  //        proximal_level_outputs_.outputs_.size());

  internal_stats.output_level_stats.Add(compaction_outputs_.stats_);
  for (const std::unique_ptr<CompactionOutputs>& outputs : fade_outputs_) {
    if (outputs) {
      internal_stats.output_level_stats.Add(outputs->stats_);
    }
  }
  if (proximal_level_outputs_.HasOutput()) {
    internal_stats.has_proximal_level_output = true;
    internal_stats.proximal_level_stats.Add(proximal_level_outputs_.stats_);
  }
}

OutputIterator SubcompactionState::GetOutputs() const {
  std::vector<const std::vector<CompactionOutputs::Output>*> outputs;
  outputs.push_back(&proximal_level_outputs_.outputs_);
  outputs.push_back(&compaction_outputs_.outputs_);
  for (const std::unique_ptr<CompactionOutputs>& level_outputs :
       fade_outputs_) {
    if (level_outputs) {
      outputs.push_back(&level_outputs->outputs_);
    }
  }
  return OutputIterator(std::move(outputs));
}

void SubcompactionState::Cleanup(Cache* cache, const Status& overall_status) {
  proximal_level_outputs_.Cleanup();
  compaction_outputs_.Cleanup();
  for (const std::unique_ptr<CompactionOutputs>& outputs : fade_outputs_) {
    if (outputs) {
      outputs->Cleanup();
    }
  }

  if (!status.ok() || !overall_status.ok()) {
    for (const auto& out : GetOutputs()) {
      // If this file was inserted into the table cache then remove it here
      // because this compaction was not committed. This is not strictly
      // required because of a backstop TableCache::Evict() in
      // PurgeObsoleteFiles() but is our opportunity to apply
      // uncache_aggressiveness. TODO: instead, put these files into the
      // VersionSet::obsolete_files_ pipeline so that they don't have to
      // be picked up by scanning the DB directory.
      TableCache::ReleaseObsolete(
          cache, out.meta.fd.GetNumber(), nullptr /*handle*/,
          compaction->mutable_cf_options().uncache_aggressiveness);
    }
  }
  // TODO: sub_compact.io_status is not checked like status. Not sure if thats
  // intentional. So ignoring the io_status as of now.
  io_status.PermitUncheckedError();
}

Slice SubcompactionState::SmallestUserKey() const {
  Slice smallest;
  const Comparator* user_cmp =
      compaction->column_family_data()->user_comparator();
  for (const auto& output : GetOutputs()) {
    if (!output.finished || output.meta.smallest.size() == 0) {
      continue;
    }
    Slice key = output.meta.smallest.user_key();
    if (smallest.empty() || user_cmp->Compare(key, smallest) < 0) {
      smallest = key;
    }
  }
  return smallest;
}

Slice SubcompactionState::LargestUserKey() const {
  Slice largest;
  const Comparator* user_cmp =
      compaction->column_family_data()->user_comparator();
  for (const auto& output : GetOutputs()) {
    if (!output.finished || output.meta.largest.size() == 0) {
      continue;
    }
    Slice key = output.meta.largest.user_key();
    if (largest.empty() || user_cmp->Compare(key, largest) > 0) {
      largest = key;
    }
  }
  return largest;
}

Status SubcompactionState::AddToOutput(
    CompactionIterator& iter, int output_level,
    const CompactionFileOpenFunc& open_file_func,
    const CompactionFileCloseFunc& close_file_func,
    const ParsedInternalKey& prev_iter_output_internal_key) {
  // update target output
  current_outputs_ = OutputsForLevel(output_level);
  return current_outputs_->AddToOutput(iter, open_file_func, close_file_func,
                                       prev_iter_output_internal_key);
}

Status SubcompactionState::UpdateBlobGarbageCheckpoint(
    const BlobGarbageMeter* next_key_inflow) {
  const BlobGarbageMeter* const meter =
      compaction_outputs_.GetBlobGarbageMeter();
  if (meter == nullptr) {
    blob_garbage_checkpoint_.reset();
    return Status::OK();
  }
  blob_garbage_checkpoint_ = std::make_unique<BlobGarbageMeter>(*meter);
  if (next_key_inflow == nullptr || next_key_inflow->flows().empty()) {
    return Status::OK();
  }
  blob_garbage_checkpoint_->ProcessOutFlow(*next_key_inflow);
  return Status::OK();
}

}  // namespace ROCKSDB_NAMESPACE
