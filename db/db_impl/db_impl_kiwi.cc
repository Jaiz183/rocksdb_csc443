//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <charconv>
#include <set>

#include "db/db_impl/db_impl.h"
#include "file/filename.h"
#include "table/block_based/kiwi_tile.h"
#include "util/cast_util.h"

namespace ROCKSDB_NAMESPACE {

namespace {
// Number of times to start over when the column family changes under us.
constexpr int kMaxSecondaryDeleteAttempts = 8;

bool ParseDecimal(const std::string& text, uint64_t* value) {
  const char* end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, *value);
  return parsed.ec == std::errc() && parsed.ptr == end;
}
}  // namespace

Status DBImpl::SecondaryDeleteBelow(ColumnFamilyHandle* column_family,
                                    uint64_t delete_key_floor) {
  // TODO: plumb Env::IOActivity, Env::IOPriority
  const ReadOptions read_options;
  const WriteOptions write_options;
  if (delete_key_floor == 0) {
    return Status::OK();  // no delete key is below 0
  }
  auto cfh = static_cast_with_check<ColumnFamilyHandleImpl>(column_family);
  ColumnFamilyData* cfd = cfh->cfd();

  // Entries in the memtable are not in any file yet.
  Status s = Flush(FlushOptions(), column_family);
  if (!s.ok()) {
    return s;
  }

  for (int attempt = 0; attempt < kMaxSecondaryDeleteAttempts; ++attempt) {
    // Find the files with entries below the floor in one version, without the
    // mutex (this reads table properties).
    SuperVersion* sv = GetAndRefSuperVersion(cfd);
    const uint64_t scanned_version = sv->current->GetVersionNumber();
    TablePropertiesCollection props;
    s = sv->current->GetPropertiesOfAllTables(read_options, &props);
    ReturnAndCleanupSuperVersion(cfd, sv);
    if (!s.ok()) {
      return s;
    }
    std::set<uint64_t> targets;
    std::set<uint64_t> fully_dead;
    for (const auto& file : props) {
      const auto& user_props = file.second->user_collected_properties;
      const auto min_delete_key = user_props.find(kiwi::kPropMinDeleteKey);
      if (user_props.find(kiwi::kPropTilePages) == user_props.end() ||
          min_delete_key == user_props.end()) {
        // A floor cannot delete from a file without delete tiles; silently
        // leaving its entries would make the delete incomplete.
        return Status::NotSupported(
            "SecondaryDeleteBelow: " + file.first +
            " was not written with KiWi delete tiles; compact it first");
      }
      uint64_t min_key = 0;
      if (!ParseDecimal(min_delete_key->second, &min_key)) {
        return Status::Corruption("Invalid KiWi minimum delete key in " +
                                  file.first);
      }
      uint64_t max_key = kiwi::kNoDeleteKey;
      const auto max_delete_key = user_props.find(kiwi::kPropMaxDeleteKey);
      if (max_delete_key != user_props.end()) {
        if (!ParseDecimal(max_delete_key->second, &max_key)) {
          return Status::Corruption("Invalid KiWi maximum delete key in " +
                                    file.first);
        }
      }
      if (min_key < delete_key_floor) {
        const std::string::size_type slash = file.first.rfind('/');
        const std::string name = slash == std::string::npos
                                     ? file.first
                                     : file.first.substr(slash + 1);
        uint64_t number = 0;
        FileType type = kWalFile;
        if (!ParseFileName(name, &number, &type) || type != kTableFile) {
          return Status::Corruption("Unexpected table file name " + file.first);
        }
        if (max_key < delete_key_floor && file.second->num_entries > 0 &&
            file.second->num_range_deletions == 0) {
          fully_dead.insert(number);
        } else {
          targets.insert(number);
        }
      }
    }
    if (targets.empty() && fully_dead.empty()) {
      return Status::OK();
    }

    // Record the floor of all of them at once, only if nothing changed since
    // the scan: files that appeared meanwhile could hold entries we did not
    // see, and deleting from some files only could let an older version of a
    // key reappear. Files whose entire delete-key range falls below the floor
    // are dropped cleanly from the version with zero compaction write amp.
    VersionEdit edit;
    edit.MarkForegroundOperation();
    edit.SetColumnFamily(cfd->GetID());
    std::vector<FileMetaData*> changed;
    JobContext job_context(next_job_id_.fetch_add(1), true);
    bool retry = false;
    bool busy = false;
    {
      InstrumentedMutexLock l(&mutex_);
      Version* input_version = cfd->current();
      if (input_version->GetVersionNumber() != scanned_version) {
        retry = true;
      } else {
        auto* vstorage = input_version->storage_info();
        for (int level = 0; level < vstorage->num_levels() && !busy; ++level) {
          for (FileMetaData* file : vstorage->LevelFiles(level)) {
            const uint64_t fnum = file->fd.GetNumber();
            const bool is_dead = fully_dead.count(fnum) > 0;
            const bool is_partial = targets.count(fnum) > 0;
            if (!is_dead && !is_partial) {
              continue;
            }
            if (!is_dead && file->kiwi_delete_floor >= delete_key_floor) {
              continue;
            }
            if (file->being_compacted) {
              busy = true;  // its compaction would not know about the floor
              break;
            }
            edit.DeleteFile(level, fnum);
            if (!is_dead) {
              edit.AddFileWithKiWiDeleteFloor(level, *file, delete_key_floor);
            }
            changed.push_back(file);
          }
        }
      }
      if (!retry && !busy && !changed.empty()) {
        for (FileMetaData* file : changed) {
          file->being_compacted = true;
        }
        input_version->Ref();
        s = versions_->LogAndApply(cfd, read_options, write_options, &edit,
                                   &mutex_, directories_.GetDbDir());
        if (s.ok()) {
          InstallSuperVersionAndScheduleWork(
              cfd, job_context.superversion_contexts.data(),
              /*defer_snapshot_refresh=*/false);
        }
        for (FileMetaData* file : changed) {
          file->being_compacted = false;
        }
        input_version->Unref();
        FindObsoleteFiles(&job_context, false);
      }
    }  // lock released here

    LogFlush(immutable_db_options_.info_log);
    if (job_context.HaveSomethingToDelete()) {
      PurgeObsoleteFiles(job_context);
    }
    job_context.Clean();
    if (busy) {
      return Status::Busy(
          "SecondaryDeleteBelow: a compaction is rewriting a file with entries "
          "to delete; nothing was changed, retry later");
    }
    if (!retry) {
      return s;
    }
  }
  return Status::Busy(
      "SecondaryDeleteBelow: the column family kept changing; nothing was "
      "changed, retry later");
}

}  // namespace ROCKSDB_NAMESPACE
