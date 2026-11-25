//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// [CSC443 draft] End-to-end tests for skip-level tombstone compaction
// (AdvancedColumnFamilyOptions::skip_level_tombstone_compaction).

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "db/db_test_util.h"
#include "port/stack_trace.h"
#include "rocksdb/listener.h"
#include "test_util/sync_point.h"

namespace ROCKSDB_NAMESPACE {

namespace {
struct CompactionRecord {
  CompactionReason reason;
  int base_input_level;
  int output_level;
  size_t num_input_files;
};

class RecordingListener : public EventListener {
 public:
  void OnCompactionCompleted(DB* /*db*/, const CompactionJobInfo& ci) override {
    std::lock_guard<std::mutex> lock(mu_);
    records_.push_back({ci.compaction_reason, ci.base_input_level,
                        ci.output_level, ci.input_files.size()});
  }
  std::vector<CompactionRecord> Records() {
    std::lock_guard<std::mutex> lock(mu_);
    return records_;
  }
  void Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    records_.clear();
  }

 private:
  std::mutex mu_;
  std::vector<CompactionRecord> records_;
};

std::string Key(int i) {
  char buf[16];
  snprintf(buf, sizeof(buf), "key%06d", i);
  return buf;
}
}  // namespace

class DBSkipLevelCompactionTest : public DBTestBase {
 public:
  DBSkipLevelCompactionTest()
      : DBTestBase("db_skip_level_compaction_test", /*env_do_fsync=*/false) {}

  Options SkipOptions() {
    Options options = CurrentOptions();
    options.num_levels = 4;
    options.level_compaction_dynamic_level_bytes = false;
    options.disable_auto_compactions = true;
    options.compression = kNoCompression;
    options.write_buffer_size = 4 << 20;
    // Large level targets: no size-triggered compaction in these tests.
    options.max_bytes_for_level_base = 64 << 20;
    options.level0_file_num_compaction_trigger = 8;
    listener_ = std::make_shared<RecordingListener>();
    options.listeners.push_back(listener_);
    return options;
  }

  // L3: key 0..n-1 = "v3", L2: key 0..n-1 = "v2" (value_size bytes),
  // L1: deletes of key 0..n/2-1 plus a few puts (tombstone dense).
  void BuildLayout(int n, int l2_value_size, int l3_value_size) {
    for (int i = 0; i < n; i++) {
      ASSERT_OK(Put(Key(i), "v3" + std::string(l3_value_size, 'c')));
    }
    ASSERT_OK(Flush());
    MoveFilesToLevel(3);
    for (int i = 0; i < n; i++) {
      ASSERT_OK(Put(Key(i), "v2" + std::string(l2_value_size, 'b')));
    }
    ASSERT_OK(Flush());
    MoveFilesToLevel(2);
    for (int i = 0; i < n / 2; i++) {
      ASSERT_OK(Delete(Key(i)));
    }
    ASSERT_OK(Put(Key(n - 1), "v1"));
    ASSERT_OK(Flush());
    MoveFilesToLevel(1);
    ASSERT_EQ("0,1,1,1", FilesPerLevel());
  }

  uint64_t DeletionsAtLevel(int level) {
    std::vector<LiveFileMetaData> files;
    db_->GetLiveFilesMetaData(&files);
    uint64_t n = 0;
    for (const auto& f : files) {
      if (f.level == level) {
        n += f.num_deletions;
      }
    }
    return n;
  }

  uint64_t TotalDeletions() {
    uint64_t n = 0;
    for (int l = 0; l < 4; l++) {
      n += DeletionsAtLevel(l);
    }
    return n;
  }

  void VerifyData(int n, int l2_value_size) {
    for (int i = 0; i < n / 2; i++) {
      ASSERT_EQ("NOT_FOUND", Get(Key(i)));
    }
    for (int i = n / 2; i < n - 1; i++) {
      ASSERT_EQ("v2" + std::string(l2_value_size, 'b'), Get(Key(i)));
    }
    ASSERT_EQ("v1", Get(Key(n - 1)));
    // Full scan agrees with point lookups.
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    int count = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      count++;
    }
    ASSERT_OK(it->status());
    ASSERT_EQ(n - n / 2, count);
  }

  std::vector<CompactionRecord> SkipRecords() {
    std::vector<CompactionRecord> out;
    for (const auto& r : listener_->Records()) {
      if (r.reason == CompactionReason::kSkipLevelTombstone) {
        out.push_back(r);
      }
    }
    return out;
  }

  std::shared_ptr<RecordingListener> listener_;
};

TEST_F(DBSkipLevelCompactionTest, AlwaysModeDropsTombstonesInOneCompaction) {
  Options options = SkipOptions();
  DestroyAndReopen(options);
  const int n = 200;
  BuildLayout(n, 10, 10);
  ASSERT_GT(DeletionsAtLevel(1), 0U);
  listener_->Clear();

  ASSERT_OK(db_->SetOptions({{"skip_level_tombstone_compaction", "1"},
                             {"disable_auto_compactions", "false"}}));
  ASSERT_OK(dbfull()->TEST_WaitForCompact());

  auto skips = SkipRecords();
  ASSERT_EQ(1U, skips.size());
  ASSERT_EQ(1, skips[0].base_input_level);
  ASSERT_EQ(3, skips[0].output_level);
  ASSERT_EQ(3U, skips[0].num_input_files);
  ASSERT_EQ(1U, listener_->Records().size());
  ASSERT_EQ("0,0,0,1", FilesPerLevel());
  ASSERT_EQ(0U, TotalDeletions());
  VerifyData(n, 10);

  Reopen(options);
  VerifyData(n, 10);
}

TEST_F(DBSkipLevelCompactionTest, AlwaysModeFromL0) {
  Options options = SkipOptions();
  DestroyAndReopen(options);
  const int n = 200;
  for (int i = 0; i < n; i++) {
    ASSERT_OK(Put(Key(i), "v3"));
  }
  ASSERT_OK(Flush());
  MoveFilesToLevel(3);
  for (int i = 0; i < n; i++) {
    ASSERT_OK(Put(Key(i), "v2"));
  }
  ASSERT_OK(Flush());
  MoveFilesToLevel(1);
  for (int i = 0; i < n / 2; i++) {
    ASSERT_OK(Delete(Key(i)));
  }
  ASSERT_OK(Flush());
  ASSERT_EQ("1,1,0,1", FilesPerLevel());
  listener_->Clear();

  ASSERT_OK(db_->SetOptions({{"skip_level_tombstone_compaction", "1"},
                             {"disable_auto_compactions", "false"}}));
  ASSERT_OK(dbfull()->TEST_WaitForCompact());
  auto skips = SkipRecords();
  ASSERT_EQ(1U, skips.size());
  ASSERT_EQ(0, skips[0].base_input_level);
  ASSERT_EQ(3, skips[0].output_level);
  ASSERT_EQ("0,0,0,1", FilesPerLevel());
  ASSERT_EQ(0U, TotalDeletions());
  for (int i = 0; i < n; i++) {
    ASSERT_EQ(i < n / 2 ? "NOT_FOUND" : "v2", Get(Key(i)));
  }
}

TEST_F(DBSkipLevelCompactionTest, SnapshotKeepsTombstonesButDataIsCorrect) {
  Options options = SkipOptions();
  DestroyAndReopen(options);
  const int n = 200;
  for (int i = 0; i < n; i++) {
    ASSERT_OK(Put(Key(i), "v3"));
  }
  ASSERT_OK(Flush());
  MoveFilesToLevel(3);
  for (int i = 0; i < n; i++) {
    ASSERT_OK(Put(Key(i), "v2"));
  }
  ASSERT_OK(Flush());
  MoveFilesToLevel(2);
  const Snapshot* snap = db_->GetSnapshot();
  for (int i = 0; i < n / 2; i++) {
    ASSERT_OK(Delete(Key(i)));
  }
  ASSERT_OK(Flush());
  MoveFilesToLevel(1);
  listener_->Clear();

  ASSERT_OK(db_->SetOptions({{"skip_level_tombstone_compaction", "1"},
                             {"disable_auto_compactions", "false"}}));
  ASSERT_OK(dbfull()->TEST_WaitForCompact());
  auto skips = SkipRecords();
  ASSERT_EQ(1U, skips.size());
  ASSERT_EQ(3, skips[0].output_level);
  // Tombstones now sit in the last level, protected by the snapshot, and are
  // not candidates any more (no compaction loop).
  ASSERT_EQ(static_cast<uint64_t>(n / 2), DeletionsAtLevel(3));
  ASSERT_EQ(1U, listener_->Records().size());
  for (int i = 0; i < n; i++) {
    ASSERT_EQ(i < n / 2 ? "NOT_FOUND" : "v2", Get(Key(i)));
    ASSERT_EQ("v2", Get(Key(i), snap));
  }
  db_->ReleaseSnapshot(snap);
}

TEST_F(DBSkipLevelCompactionTest, OffModeDoesNotSkip) {
  Options options = SkipOptions();
  DestroyAndReopen(options);
  const int n = 200;
  BuildLayout(n, 10, 10);
  listener_->Clear();
  ASSERT_OK(db_->SetOptions({{"disable_auto_compactions", "false"}}));
  ASSERT_OK(dbfull()->TEST_WaitForCompact());
  ASSERT_TRUE(SkipRecords().empty());
  ASSERT_EQ("0,1,1,1", FilesPerLevel());
  VerifyData(n, 10);
}

TEST_F(DBSkipLevelCompactionTest, CascadeSavingsBoundsCompactionGrowth) {
  const int n = 200;
  for (bool deep_is_large : {false, true}) {
    SCOPED_TRACE(deep_is_large ? "large L3 overlap" : "small L3 overlap");
    Options options = SkipOptions();
    DestroyAndReopen(options);
    // Small L3 overlap: L2 big, L3 small -> B(L3) ~= B(L2), skip to L3.
    // Large L3 overlap: L2 small, L3 big -> B(L3) is over 5x B(L2), so only
    // one level (L1 -> L2).
    BuildLayout(n, deep_is_large ? 10 : 2000, deep_is_large ? 2000 : 10);
    listener_->Clear();
    ASSERT_OK(db_->SetOptions({{"skip_level_tombstone_compaction", "2"},
                               {"skip_level_max_overlap_ratio", "5"},
                               {"disable_auto_compactions", "false"}}));
    ASSERT_OK(dbfull()->TEST_WaitForCompact());
    auto skips = SkipRecords();
    ASSERT_FALSE(skips.empty());
    ASSERT_EQ(1, skips[0].base_input_level);
    ASSERT_EQ(deep_is_large ? 2 : 3, skips[0].output_level);
    VerifyData(n, deep_is_large ? 10 : 2000);
  }
}

TEST_F(DBSkipLevelCompactionTest, RandomizedAgainstModel) {
  // Mixed puts / deletes / range deletes with auto compaction on, small
  // buffers, all three modes; verify against an in-memory model.
  for (uint32_t mode : {0u, 1u, 2u}) {
    SCOPED_TRACE("mode " + std::to_string(mode));
    Options options = SkipOptions();
    options.disable_auto_compactions = false;
    options.write_buffer_size = 64 << 10;
    options.target_file_size_base = 64 << 10;
    options.max_bytes_for_level_base = 256 << 10;
    options.level0_file_num_compaction_trigger = 2;
    options.skip_level_tombstone_compaction = mode;
    options.skip_level_tombstone_density_threshold = 0.3;
    DestroyAndReopen(options);
    Random rnd(301 + mode);
    std::map<std::string, std::string> model;
    const int kKeys = 5000;
    for (int round = 0; round < 20; round++) {
      for (int op = 0; op < 2000; op++) {
        int k = static_cast<int>(rnd.Uniform(kKeys));
        uint32_t r = rnd.Uniform(100);
        if (r < 55) {
          std::string v = rnd.RandomString(64);
          ASSERT_OK(Put(Key(k), v));
          model[Key(k)] = v;
        } else if (r < 98) {
          ASSERT_OK(Delete(Key(k)));
          model.erase(Key(k));
        } else {
          int e = std::min(kKeys, k + 20);
          ASSERT_OK(db_->DeleteRange(WriteOptions(), db_->DefaultColumnFamily(),
                                     Key(k), Key(e)));
          model.erase(model.lower_bound(Key(k)), model.lower_bound(Key(e)));
        }
      }
      if (round % 5 == 4) {
        ASSERT_OK(dbfull()->TEST_WaitForCompact());
      }
    }
    ASSERT_OK(dbfull()->TEST_WaitForCompact());
    std::unique_ptr<Iterator> it(db_->NewIterator(ReadOptions()));
    auto mit = model.begin();
    for (it->SeekToFirst(); it->Valid(); it->Next(), ++mit) {
      ASSERT_TRUE(mit != model.end());
      ASSERT_EQ(mit->first, it->key().ToString());
      ASSERT_EQ(mit->second, it->value().ToString());
    }
    ASSERT_OK(it->status());
    ASSERT_TRUE(mit == model.end());
    if (mode == 0) {
      ASSERT_TRUE(SkipRecords().empty());
    }
  }
}

TEST_F(DBSkipLevelCompactionTest, InvalidOptionsRejected) {
  Options options = SkipOptions();
  DestroyAndReopen(options);
  ASSERT_NOK(db_->SetOptions({{"skip_level_tombstone_compaction", "3"}}));
  ASSERT_NOK(
      db_->SetOptions({{"skip_level_tombstone_density_threshold", "0"}}));
  ASSERT_NOK(db_->SetOptions({{"skip_level_max_overlap_ratio", "0.5"}}));
  ASSERT_OK(db_->SetOptions({{"skip_level_tombstone_compaction", "2"}}));
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
