//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// [CSC443 draft] Tombstone workload driver. Public API only, so the same
// source builds against stock RocksDB (main) and against feature branches.
//
// Usage: tombstone_bench <db_dir> <run_seconds> <out_prefix>
// Env knobs:
//   CF_OPTS   extra column-family options string, parsed with
//             GetColumnFamilyOptionsFromString (unknown options are errors),
//             e.g. "skip_level_tombstone_compaction=2"
//   KEYS=1000000   key space          PRELOAD=1000000 keys loaded first
//   OPS=20000      target ops/s       DEL=0.1  point-delete fraction
//   RDEL=0         range-delete fraction   RW=100 range width (keys)
//   DELPAT=uniform|seq|churn   uniform: delete random keys; seq: deletes
//                        sweep the key space in order (expiry-style deletes);
//                        churn: see CHURN/HOT below
//   CHURN=0.9 HOT=20000  (DELPAT=churn only) a CHURN fraction of ops toggles
//                        one of HOT keys spread evenly over the key space:
//                        delete it if present, else re-insert it. Each hot key
//                        is deleted and re-inserted many times, so tombstones
//                        and live versions of the same key sit at several
//                        levels at once. Other ops are uniform puts; DEL and
//                        RDEL are ignored.
//   MULT=10 LBASE_MB=4   max_bytes_for_level_multiplier / _base (smaller
//                        values give a deeper tree for the same data)
//   SEED=1
// Output: human-readable summary on stdout, <out_prefix>.csv (1 row/second).
// Exits non-zero if the final full scan disagrees with the in-memory model.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "rocksdb/convenience.h"
#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/listener.h"
#include "rocksdb/metadata.h"
#include "rocksdb/options.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"

using namespace ROCKSDB_NAMESPACE;
using SClock = std::chrono::steady_clock;

namespace {
const int kValueSize = 100;

std::string Key(uint64_t k) {
  char b[24];
  snprintf(b, sizeof(b), "key%012" PRIu64, k);
  return b;
}
std::string Val(uint64_t k, uint32_t ver) {
  char b[32];
  snprintf(b, sizeof(b), "%012" PRIu64 "-%08x", k, ver);
  std::string v(b);
  v.resize(kValueSize, 'v');
  return v;
}
double EnvD(const char* n, double d) {
  const char* s = getenv(n);
  return s ? atof(s) : d;
}
std::string EnvS(const char* n, const char* d) {
  const char* s = getenv(n);
  return s ? s : d;
}

struct Listener : public EventListener {
  std::mutex mu;
  std::map<std::string, std::pair<int, uint64_t>> by_reason;  // count, bytes
  std::map<std::string, std::pair<int, uint64_t>> by_shape;   // "L1->L3"
  // Shapes of SkipLevelTombstone compactions only.
  std::map<std::string, std::pair<int, uint64_t>> skip_shape;
  std::atomic<int> errors{0};
  void OnCompactionCompleted(DB*, const CompactionJobInfo& ci) override {
    if (!ci.status.ok()) {
      errors++;
    }
    char shape[32];
    snprintf(shape, sizeof(shape), "L%d->L%d", ci.base_input_level,
             ci.output_level);
    std::lock_guard<std::mutex> l(mu);
    const std::string reason = GetCompactionReasonString(ci.compaction_reason);
    if (reason == "SkipLevelTombstone") {
      auto& k = skip_shape[shape];
      k.first++;
      k.second += ci.stats.total_output_bytes;
    }
    auto& r = by_reason[reason];
    r.first++;
    r.second += ci.stats.total_output_bytes;
    auto& s = by_shape[shape];
    s.first++;
    s.second += ci.stats.total_output_bytes;
  }
  void OnBackgroundError(BackgroundErrorReason, Status* s) override {
    errors++;
    fprintf(stderr, "BACKGROUND ERROR: %s\n", s->ToString().c_str());
  }
};

struct Sample {
  uint64_t sst_bytes = 0;
  uint64_t tomb_upper = 0;  // tombstones in levels above the last non-empty
  uint64_t tomb_total = 0;
  int64_t max_tomb_age = 0;  // upper bound via oldest_ancester_time
  int files = 0;
  std::string per_level;
};

Sample TakeSample(DB* db) {
  Sample s;
  std::vector<LiveFileMetaData> files;
  db->GetLiveFilesMetaData(&files);
  int last = 0;
  for (const auto& f : files) {
    last = std::max(last, f.level);
  }
  const int64_t now = static_cast<int64_t>(time(nullptr));
  std::vector<uint64_t> lvl_bytes(8, 0);
  for (const auto& f : files) {
    s.files++;
    s.sst_bytes += f.size;
    s.tomb_total += f.num_deletions;
    if (f.level < 8) {
      lvl_bytes[f.level] += f.size;
    }
    if (f.level < last && f.num_deletions > 0) {
      s.tomb_upper += f.num_deletions;
      if (f.oldest_ancester_time > 0) {
        s.max_tomb_age = std::max(
            s.max_tomb_age, now - static_cast<int64_t>(f.oldest_ancester_time));
      }
    }
  }
  for (int l = 0; l < 7; l++) {
    s.per_level += std::to_string(lvl_bytes[l] >> 10) + (l < 6 ? "/" : "");
  }
  return s;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <db_dir> <run_seconds> <out_prefix>\n", argv[0]);
    return 2;
  }
  const std::string dbdir = argv[1];
  const double run_s = atof(argv[2]);
  const std::string prefix = argv[3];
  const uint64_t kKeys = static_cast<uint64_t>(EnvD("KEYS", 1000000));
  const uint64_t kPreload = static_cast<uint64_t>(EnvD("PRELOAD", 1000000));
  const double ops_s = EnvD("OPS", 20000);
  const double del_frac = EnvD("DEL", 0.1);
  const double rdel_frac = EnvD("RDEL", 0.0);
  const uint64_t rw = static_cast<uint64_t>(EnvD("RW", 100));
  const std::string delpat = EnvS("DELPAT", "uniform");
  const bool seq_del = delpat == "seq";
  const bool churn = delpat == "churn";
  const double churn_frac = EnvD("CHURN", 0.9);
  const uint64_t hot = std::max<uint64_t>(
      1, std::min(kKeys, static_cast<uint64_t>(EnvD("HOT", 20000))));
  const uint64_t hot_stride = kKeys / hot;
  const std::string cf_opts = EnvS("CF_OPTS", "");

  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 1 << 20;
  options.target_file_size_base = 1 << 20;
  options.max_bytes_for_level_base =
      static_cast<uint64_t>(EnvD("LBASE_MB", 4) * (1 << 20));
  options.max_bytes_for_level_multiplier = EnvD("MULT", 10);
  options.level0_file_num_compaction_trigger = 4;
  options.max_background_jobs = 4;
  options.compression = kNoCompression;
  options.statistics = CreateDBStatistics();
  BlockBasedTableOptions bbto;
  bbto.filter_policy.reset(NewBloomFilterPolicy(10));
  options.table_factory.reset(NewBlockBasedTableFactory(bbto));
  auto listener = std::make_shared<Listener>();
  options.listeners.push_back(listener);
  if (!cf_opts.empty()) {
    ConfigOptions cfg;
    cfg.ignore_unknown_options = false;
    ColumnFamilyOptions cfo;
    Status s = GetColumnFamilyOptionsFromString(
        cfg, ColumnFamilyOptions(options), cf_opts, &cfo);
    if (!s.ok()) {
      fprintf(stderr, "bad CF_OPTS: %s\n", s.ToString().c_str());
      return 2;
    }
    options = Options(DBOptions(options), cfo);
  }

  DestroyDB(dbdir, options);
  std::unique_ptr<DB> db;
  Status s = DB::Open(options, dbdir, &db);
  if (!s.ok()) {
    fprintf(stderr, "open: %s\n", s.ToString().c_str());
    return 2;
  }
  WriteOptions wo;
  wo.disableWAL = true;

  // Model: key -> version (0 = absent).
  std::vector<uint32_t> model(kKeys, 0);
  std::mt19937_64 rng(static_cast<uint64_t>(EnvD("SEED", 1)));
  uint64_t user_bytes = 0;
  uint32_t ver = 1;

  // Preload in random order.
  for (uint64_t i = 0; i < kPreload; i++) {
    uint64_t k = rng() % kKeys;
    model[k] = ver;
    std::string key = Key(k);
    s = db->Put(wo, key, Val(k, ver));
    if (!s.ok()) {
      fprintf(stderr, "put: %s\n", s.ToString().c_str());
      return 2;
    }
    user_bytes += key.size() + kValueSize;
  }
  ver++;
  db->WaitForCompact(WaitForCompactOptions());
  const uint64_t preload_bytes = user_bytes;
  const uint64_t comp0 =
      options.statistics->getTickerCount(COMPACT_WRITE_BYTES);
  const uint64_t flush0 = options.statistics->getTickerCount(FLUSH_WRITE_BYTES);
  {
    std::lock_guard<std::mutex> l(listener->mu);
    listener->by_reason.clear();
    listener->by_shape.clear();
    listener->skip_shape.clear();
  }

  FILE* csv = fopen((prefix + ".csv").c_str(), "w");
  fprintf(csv,
          "t,ops,sst_bytes,tomb_upper,tomb_total,max_tomb_age_ub,files,"
          "level_kb\n");
  uint64_t ops = 0, puts = 0, dels = 0, rdels = 0;
  uint64_t seq_cursor = 0;
  double sum_tomb_upper = 0, sum_sst = 0;
  int64_t peak_age = 0;
  int nsamples = 0;
  const auto t0 = SClock::now();
  auto next_sample = t0 + std::chrono::seconds(1);
  std::uniform_real_distribution<double> U(0, 1);
  while (true) {
    const auto now = SClock::now();
    const double el = std::chrono::duration<double>(now - t0).count();
    if (el >= run_s) {
      break;
    }
    if (now >= next_sample) {
      Sample smp = TakeSample(db.get());
      fprintf(csv,
              "%.0f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRId64
              ",%d,%s\n",
              el, ops, smp.sst_bytes, smp.tomb_upper, smp.tomb_total,
              smp.max_tomb_age, smp.files, smp.per_level.c_str());
      sum_tomb_upper += smp.tomb_upper;
      sum_sst += smp.sst_bytes;
      peak_age = std::max(peak_age, smp.max_tomb_age);
      nsamples++;
      next_sample += std::chrono::seconds(1);
    }
    // Rate limit in batches of 100 ops.
    if (ops % 100 == 0 && ops > el * ops_s) {
      std::this_thread::sleep_for(std::chrono::microseconds(500));
      continue;
    }
    const double r = U(rng);
    if (churn) {
      uint64_t k;
      bool del = false;
      if (r < churn_frac) {
        k = (rng() % hot) * hot_stride;
        del = model[k] != 0;
      } else {
        k = rng() % kKeys;
      }
      std::string key = Key(k);
      if (del) {
        s = db->Delete(wo, key);
        model[k] = 0;
        user_bytes += key.size();
        dels++;
      } else {
        s = db->Put(wo, key, Val(k, ver));
        model[k] = ver;
        user_bytes += key.size() + kValueSize;
        puts++;
      }
    } else if (r < rdel_frac) {
      uint64_t k = seq_del ? seq_cursor : rng() % kKeys;
      uint64_t e = std::min(kKeys, k + rw);
      s = db->DeleteRange(wo, db->DefaultColumnFamily(), Key(k), Key(e));
      for (uint64_t i = k; i < e; i++) {
        model[i] = 0;
      }
      if (seq_del) {
        seq_cursor = (e >= kKeys) ? 0 : e;
      }
      user_bytes += 2 * Key(k).size();
      rdels++;
    } else if (r < rdel_frac + del_frac) {
      uint64_t k;
      if (seq_del) {
        k = seq_cursor;
        seq_cursor = (seq_cursor + 1) % kKeys;
      } else {
        k = rng() % kKeys;
      }
      std::string key = Key(k);
      s = db->Delete(wo, key);
      model[k] = 0;
      user_bytes += key.size();
      dels++;
    } else {
      uint64_t k = rng() % kKeys;
      std::string key = Key(k);
      s = db->Put(wo, key, Val(k, ver));
      model[k] = ver;
      user_bytes += key.size() + kValueSize;
      puts++;
    }
    if (!s.ok()) {
      fprintf(stderr, "write: %s\n", s.ToString().c_str());
      return 2;
    }
    ops++;
    if (ops % 100000 == 0) {
      ver++;
    }
  }
  const double run_el =
      std::chrono::duration<double>(SClock::now() - t0).count();
  Sample end_run = TakeSample(db.get());

  // Drain: flush + wait for compactions, then final snapshot of the tree.
  db->Flush(FlushOptions());
  db->WaitForCompact(WaitForCompactOptions());
  Sample drained = TakeSample(db.get());
  fclose(csv);

  const uint64_t comp_bytes =
      options.statistics->getTickerCount(COMPACT_WRITE_BYTES) - comp0;
  const uint64_t flush_bytes =
      options.statistics->getTickerCount(FLUSH_WRITE_BYTES) - flush0;
  const uint64_t run_user = user_bytes - preload_bytes;

  // Verify full scan against the model.
  uint64_t live_bytes = 0, live_keys = 0, mismatches = 0;
  {
    std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
    uint64_t k = 0;
    auto next_live = [&](uint64_t from) {
      while (from < kKeys && model[from] == 0) {
        from++;
      }
      return from;
    };
    k = next_live(0);
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      if (k >= kKeys || it->key().ToString() != Key(k) ||
          it->value().ToString() != Val(k, model[k])) {
        mismatches++;
        if (mismatches < 5) {
          fprintf(stderr, "MISMATCH at %s\n", it->key().ToString().c_str());
        }
      }
      live_keys++;
      live_bytes += it->key().size() + it->value().size();
      k = next_live(k + 1);
    }
    if (!it->status().ok()) {
      mismatches++;
    }
    if (k < kKeys) {
      mismatches++;
      fprintf(stderr, "MISSING key %s\n", Key(k).c_str());
    }
  }
  const bool pass = mismatches == 0 && listener->errors == 0;

  printf("cf_opts=\"%s\" run_s=%.1f ops=%" PRIu64 " (puts=%" PRIu64
         " dels=%" PRIu64 " rdels=%" PRIu64 ") ops/s=%.0f\n",
         cf_opts.c_str(), run_el, ops, puts, dels, rdels, ops / run_el);
  printf(
      "write_amp=%.3f  (compaction %.1f MB + flush %.1f MB) / user %.1f "
      "MB\n",
      static_cast<double>(comp_bytes + flush_bytes) / run_user,
      comp_bytes / 1048576.0, flush_bytes / 1048576.0, run_user / 1048576.0);
  printf("avg_upper_tombstones=%.0f  peak_tomb_age_ub=%" PRId64
         "s  avg_sst_MB=%.2f\n",
         nsamples ? sum_tomb_upper / nsamples : 0.0, peak_age,
         nsamples ? sum_sst / nsamples / 1048576.0 : 0.0);
  printf("end_of_run: sst_MB=%.2f upper_tombs=%" PRIu64 " total_tombs=%" PRIu64
         " levels_KB=%s\n",
         end_run.sst_bytes / 1048576.0, end_run.tomb_upper, end_run.tomb_total,
         end_run.per_level.c_str());
  printf(
      "drained:    sst_MB=%.2f upper_tombs=%" PRIu64 " total_tombs=%" PRIu64
      " levels_KB=%s space_amp=%.3f\n",
      drained.sst_bytes / 1048576.0, drained.tomb_upper, drained.tomb_total,
      drained.per_level.c_str(),
      live_bytes ? static_cast<double>(drained.sst_bytes) / live_bytes : 0.0);
  {
    std::lock_guard<std::mutex> l(listener->mu);
    printf("compactions by reason:");
    for (auto& kv : listener->by_reason) {
      printf(" %s=%d(%.1fMB)", kv.first.c_str(), kv.second.first,
             kv.second.second / 1048576.0);
    }
    printf("\ncompactions by shape: ");
    for (auto& kv : listener->by_shape) {
      printf(" %s=%d(%.1fMB)", kv.first.c_str(), kv.second.first,
             kv.second.second / 1048576.0);
    }
    printf("\nskip-level shapes:     ");
    for (auto& kv : listener->skip_shape) {
      printf(" %s=%d(%.1fMB)", kv.first.c_str(), kv.second.first,
             kv.second.second / 1048576.0);
    }
    printf("\n");
  }
  printf("verify: live_keys=%" PRIu64 " mismatches=%" PRIu64
         " bg_errors=%d -> %s\n",
         live_keys, mismatches, listener->errors.load(),
         pass ? "PASS" : "FAIL");
  db->Close();
  DestroyDB(dbdir, options);
  return pass ? 0 : 1;
}
