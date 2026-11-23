//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Workload driver for the tombstone deadline / deadline coalescing draft.
// Public API only, so the same binary source builds against main (baseline);
// the new options are passed as a string in CF_OPTS and are simply omitted for
// the baseline run.
//
// Usage: deadline_bench <db_dir> <run_seconds> <ops_per_second> <out_csv>
// Environment:
//   CF_OPTS   extra column family options, e.g.
//             "tombstone_deadline_seconds=30;deadline_coalescing_window_seconds=10"
//   DEADLINE  deadline in seconds used only for reporting violations (default 30)
//   PRELOAD   keys loaded before the timed phase (default 500000)
//   KEYS      key space (default = PRELOAD)
//   DEL       fraction of timed ops that are point deletes (default 0.1)
//   IDLE_S    idle seconds after the timed phase, still sampled (default 40)
//
// Prints one summary line "RESULT key=value ..." and writes a per-second CSV.

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rocksdb/convenience.h"
#include "rocksdb/db.h"
#include "rocksdb/listener.h"
#include "rocksdb/options.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"
#include "rocksdb/filter_policy.h"

using namespace ROCKSDB_NAMESPACE;

namespace {

double EnvD(const char* n, double d) {
  const char* v = getenv(n);
  return v ? atof(v) : d;
}

std::string KeyStr(uint64_t k) {
  char buf[32];
  snprintf(buf, sizeof(buf), "key%012" PRIu64, k);
  return buf;
}

std::string ValueStr(uint64_t k, uint32_t version) {
  std::string v = KeyStr(k) + "-v" + std::to_string(version) + "-";
  v.resize(100, 'x');
  return v;
}

struct Listener : public EventListener {
  std::mutex mu;
  std::map<std::string, uint64_t> count_by_reason;
  std::map<std::string, uint64_t> bytes_by_reason;
  uint64_t deadline_input_files = 0;
  void OnCompactionCompleted(DB*, const CompactionJobInfo& ci) override {
    std::lock_guard<std::mutex> l(mu);
    const std::string r = GetCompactionReasonString(ci.compaction_reason);
    count_by_reason[r]++;
    bytes_by_reason[r] += ci.stats.total_output_bytes;
    if (ci.compaction_reason == CompactionReason::kTombstoneDeadline) {
      deadline_input_files += ci.input_files.size();
    }
  }
};

struct Sample {
  uint64_t max_age = 0;          // max(now - oldest_ancester_time) over
                                 // non-last-level files with deletions
  uint64_t tombstones = 0;       // deletions in non-last-level files
  uint64_t sst_bytes = 0;
  int last_level = 0;
};

Sample TakeSample(DB* db) {
  Sample s;
  std::vector<LiveFileMetaData> files;
  db->GetLiveFilesMetaData(&files);
  for (const auto& f : files) {
    s.last_level = std::max(s.last_level, f.level);
    s.sst_bytes += f.size;
  }
  const uint64_t now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  for (const auto& f : files) {
    if (f.level == s.last_level || f.num_deletions == 0) {
      continue;
    }
    s.tombstones += f.num_deletions;
    if (f.oldest_ancester_time > 0 && f.oldest_ancester_time <= now) {
      s.max_age = std::max(s.max_age, now - f.oldest_ancester_time);
    }
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s <db_dir> <run_s> <ops_s> <out_csv>\n", argv[0]);
    return 2;
  }
  const std::string db_dir = argv[1];
  const int run_s = atoi(argv[2]);
  const double ops_s = atof(argv[3]);
  const std::string out_csv = argv[4];
  const uint64_t preload = static_cast<uint64_t>(EnvD("PRELOAD", 500000));
  const uint64_t keys = static_cast<uint64_t>(EnvD("KEYS", preload));
  const double del_frac = EnvD("DEL", 0.1);
  const int idle_s = static_cast<int>(EnvD("IDLE_S", 40));
  const uint64_t deadline = static_cast<uint64_t>(EnvD("DEADLINE", 30));
  const char* cf_opts = getenv("CF_OPTS");

  Options base;
  base.create_if_missing = true;
  base.compression = kNoCompression;
  base.write_buffer_size = 1 << 20;
  base.target_file_size_base = 1 << 20;
  base.max_bytes_for_level_base = 4 << 20;
  base.max_bytes_for_level_multiplier = 10;
  base.level0_file_num_compaction_trigger = 4;
  base.max_background_jobs = 4;
  base.statistics = CreateDBStatistics();
  BlockBasedTableOptions bbto;
  bbto.filter_policy.reset(NewBloomFilterPolicy(10));
  base.table_factory.reset(NewBlockBasedTableFactory(bbto));
  auto listener = std::make_shared<Listener>();
  base.listeners.push_back(listener);

  Options options = base;
  if (cf_opts != nullptr && cf_opts[0] != '\0') {
    ConfigOptions cfg;
    cfg.ignore_unknown_options = false;
    ColumnFamilyOptions cf;
    Status s = GetColumnFamilyOptionsFromString(cfg, ColumnFamilyOptions(base),
                                                cf_opts, &cf);
    if (!s.ok()) {
      fprintf(stderr, "bad CF_OPTS: %s\n", s.ToString().c_str());
      return 2;
    }
    options = Options(DBOptions(base), cf);
  }

  DestroyDB(db_dir, options);
  std::unique_ptr<DB> db;
  Status s = DB::Open(options, db_dir, &db);
  if (!s.ok()) {
    fprintf(stderr, "open: %s\n", s.ToString().c_str());
    return 1;
  }
  WriteOptions wo;
  wo.disableWAL = true;

  // version[k] == 0 means absent.
  std::vector<uint32_t> version(keys, 0);
  uint64_t user_bytes = 0;
  for (uint64_t k = 0; k < preload; k++) {
    version[k] = 1;
    std::string key = KeyStr(k);
    std::string val = ValueStr(k, 1);
    user_bytes += key.size() + val.size();
    s = db->Put(wo, key, val);
    if (!s.ok()) {
      fprintf(stderr, "put: %s\n", s.ToString().c_str());
      return 1;
    }
  }
  s = db->WaitForCompact(WaitForCompactOptions());
  if (!s.ok()) {
    fprintf(stderr, "wait: %s\n", s.ToString().c_str());
    return 1;
  }
  options.statistics->Reset();
  {
    std::lock_guard<std::mutex> l(listener->mu);
    listener->count_by_reason.clear();
    listener->bytes_by_reason.clear();
    listener->deadline_input_files = 0;
  }
  user_bytes = 0;

  FILE* csv = fopen(out_csv.c_str(), "w");
  fprintf(csv, "t,phase,max_tomb_age,tombstones_nonlast,sst_bytes,ops\n");

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> ops_done{0};
  uint64_t peak_age = 0;
  uint64_t sum_tomb = 0;
  uint64_t samples = 0;
  uint64_t violation_s = 0;  // seconds with max_age > deadline
  uint64_t idle_peak_age = 0;
  std::thread sampler([&] {
    const auto t0 = std::chrono::steady_clock::now();
    for (int t = 1; t <= run_s + idle_s; t++) {
      std::this_thread::sleep_until(t0 + std::chrono::seconds(t));
      Sample smp = TakeSample(db.get());
      const bool idle = t > run_s;
      fprintf(csv, "%d,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
              t, idle ? "idle" : "run", smp.max_age, smp.tombstones,
              smp.sst_bytes, ops_done.load());
      peak_age = std::max(peak_age, smp.max_age);
      if (idle) {
        idle_peak_age = std::max(idle_peak_age, smp.max_age);
      }
      sum_tomb += smp.tombstones;
      samples++;
      if (smp.max_age > deadline) {
        violation_s++;
      }
    }
    stop = true;
  });

  uint64_t rng = 88172645463325252ull;
  auto next = [&] {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  const auto start = std::chrono::steady_clock::now();
  const uint64_t total_ops = static_cast<uint64_t>(ops_s * run_s);
  for (uint64_t i = 0; i < total_ops; i++) {
    const auto due = start + std::chrono::microseconds(
                                 static_cast<int64_t>(i * 1e6 / ops_s));
    std::this_thread::sleep_until(due);
    const uint64_t k = next() % keys;
    const bool del = (next() % 10000) < del_frac * 10000;
    std::string key = KeyStr(k);
    if (del) {
      version[k] = 0;
      s = db->Delete(wo, key);
      user_bytes += key.size();
    } else {
      version[k]++;
      if (version[k] == 0) {
        version[k] = 1;
      }
      std::string val = ValueStr(k, version[k]);
      s = db->Put(wo, key, val);
      user_bytes += key.size() + val.size();
    }
    if (!s.ok()) {
      fprintf(stderr, "write: %s\n", s.ToString().c_str());
      return 1;
    }
    ops_done++;
  }
  const double run_elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  sampler.join();
  fclose(csv);

  // Verify the full DB against the model.
  bool pass = true;
  {
    std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
    uint64_t expected_k = 0;
    auto advance_expected = [&] {
      while (expected_k < keys && version[expected_k] == 0) {
        expected_k++;
      }
    };
    advance_expected();
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      if (expected_k >= keys || it->key().ToString() != KeyStr(expected_k) ||
          it->value().ToString() != ValueStr(expected_k, version[expected_k])) {
        pass = false;
        fprintf(stderr, "MISMATCH at %s\n", it->key().ToString().c_str());
        break;
      }
      expected_k++;
      advance_expected();
    }
    if (pass && (!it->status().ok() || expected_k < keys)) {
      pass = false;
      fprintf(stderr, "scan ended early: %s\n",
              it->status().ToString().c_str());
    }
  }

  const uint64_t compact_w =
      options.statistics->getTickerCount(COMPACT_WRITE_BYTES);
  const uint64_t flush_w = options.statistics->getTickerCount(FLUSH_WRITE_BYTES);
  const uint64_t stall_us = options.statistics->getTickerCount(STALL_MICROS);
  Sample fin = TakeSample(db.get());
  std::string reasons;
  {
    std::lock_guard<std::mutex> l(listener->mu);
    for (const auto& kv : listener->count_by_reason) {
      reasons += kv.first + ":" + std::to_string(kv.second) + "/" +
                 std::to_string(listener->bytes_by_reason[kv.first] >> 20) +
                 "MB ";
    }
    printf(
        "RESULT opts=\"%s\" pass=%d run_s=%.1f write_amp=%.2f "
        "compact_write_mb=%.1f peak_tomb_age=%" PRIu64
        " idle_peak_tomb_age=%" PRIu64 " violation_s=%" PRIu64
        " avg_tombstones=%.0f final_sst_mb=%.1f stall_ms=%.0f "
        "deadline_input_files=%" PRIu64
        " reasons=[%s]\n",
        cf_opts ? cf_opts : "", pass ? 1 : 0, run_elapsed,
        user_bytes ? double(compact_w + flush_w) / double(user_bytes) : 0.0,
        compact_w / 1048576.0, peak_age, idle_peak_age, violation_s,
        samples ? double(sum_tomb) / double(samples) : 0.0,
        fin.sst_bytes / 1048576.0, stall_us / 1000.0,
        listener->deadline_input_files,
        reasons.c_str());
  }
  db.reset();
  DestroyDB(db_dir, options);
  return pass ? 0 : 1;
}
