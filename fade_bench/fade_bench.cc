// fade_bench: workload driver for checking RocksDB baseline behaviour and the
// FADE (Lethe, SIGMOD'20) delete-persistence compaction.
//
// Uses only the public RocksDB API, so the tombstone-age measurements are an
// independent ground truth rather than FADE's own bookkeeping:
//   * a TablePropertiesCollector records, per SST, the smallest sequence
//     number of any point/range tombstone it contains;
//   * the writer records a (sequence number -> wall time) map, so the age of
//     the oldest live tombstone in every file can be computed at any moment.
//
// Every run also keeps an exact in-memory model of the key space and checks
// the DB against it (full scan + point lookups), before and after reopen.
//
// Usage: fade_bench key=value ...   (see Config below for keys)

#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/listener.h"
#include "rocksdb/options.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"
#include "rocksdb/table_properties.h"

namespace ROCKSDB_NAMESPACE {

namespace {

struct Config {
  std::string db = "/tmp/fade_bench_db";
  std::string label = "run";
  std::string out_prefix = "";        // timeseries CSV path prefix ("" = none)
  std::string summary_csv = "";       // append one summary row here
  uint64_t keys = 1000000;            // key space size, fully preloaded
  int value_size = 112;
  double put_frac = 0.90;             // remaining ops are deletes ...
  double range_del_frac = 0.0;        // ... of which this fraction (of ALL
                                      // ops) are range deletes
  int range_len = 100;                // keys per range delete
  double hot_frac = 0.0;              // >0: 80% of ops go to this key prefix
  double write_secs = 60;
  double read_secs = 10;
  uint64_t rate = 20000;              // target write ops/s, 0 = unpaced
  uint64_t dth = 0;                   // delete_persistence_threshold (s)
  uint64_t b_threshold = 0;           // min b_f to trigger FADE compaction
  bool filter_probe = false;          // probe output-level SST filters
  bool tombstone_route = false;       // route point tombstones per key
  bool dynamic_level_bytes = true;
  uint64_t seed = 42;
  uint64_t write_buffer = 2 << 20;
  uint64_t level_base = 4 << 20;
  uint64_t target_file = 2 << 20;
  double ratio = 10;
  int bg_jobs = 4;
  int verify_gets = 200000;
};

bool ParseArgs(int argc, char** argv, Config* c) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    size_t eq = a.find('=');
    if (eq == std::string::npos) {
      fprintf(stderr, "bad arg %s\n", a.c_str());
      return false;
    }
    std::string k = a.substr(0, eq), v = a.substr(eq + 1);
    if (k == "db") c->db = v;
    else if (k == "label") c->label = v;
    else if (k == "out_prefix") c->out_prefix = v;
    else if (k == "summary_csv") c->summary_csv = v;
    else if (k == "keys") c->keys = std::stoull(v);
    else if (k == "value_size") c->value_size = std::stoi(v);
    else if (k == "put_frac") c->put_frac = std::stod(v);
    else if (k == "range_del_frac") c->range_del_frac = std::stod(v);
    else if (k == "range_len") c->range_len = std::stoi(v);
    else if (k == "hot_frac") c->hot_frac = std::stod(v);
    else if (k == "write_secs") c->write_secs = std::stod(v);
    else if (k == "read_secs") c->read_secs = std::stod(v);
    else if (k == "rate") c->rate = std::stoull(v);
    else if (k == "dth") c->dth = std::stoull(v);
    else if (k == "b_threshold")
      c->b_threshold = std::stoull(v);
    else if (k == "filter_probe")
      c->filter_probe = v == "1" || v == "true";
    else if (k == "tombstone_route")
      c->tombstone_route = v == "1" || v == "true";
    else if (k == "dynamic_level_bytes")
      c->dynamic_level_bytes = v == "1" || v == "true";
    else if (k == "seed") c->seed = std::stoull(v);
    else if (k == "write_buffer") c->write_buffer = std::stoull(v);
    else if (k == "level_base") c->level_base = std::stoull(v);
    else if (k == "target_file") c->target_file = std::stoull(v);
    else if (k == "ratio") c->ratio = std::stod(v);
    else if (k == "bg_jobs") c->bg_jobs = std::stoi(v);
    else if (k == "verify_gets") c->verify_gets = std::stoi(v);
    else {
      fprintf(stderr, "unknown arg %s\n", k.c_str());
      return false;
    }
  }
  return true;
}

double NowSec() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

double CpuSec() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec +
         ru.ru_stime.tv_usec / 1e6;
}

std::string Key(uint64_t k) {
  char buf[32];
  snprintf(buf, sizeof(buf), "key%013" PRIu64, k);
  return buf;
}

std::string Value(uint64_t k, uint32_t ver, int size) {
  std::string v = Key(k) + "#" + std::to_string(ver) + "#";
  v.resize(std::max<size_t>(v.size(), size), 'x');
  return v;
}

// ---- tombstone ground truth -------------------------------------------------

const char* kMinDelSeqProp = "fadebench.min_del_seqno";
const char* kNumDelProp = "fadebench.num_dels";

class TombstoneCollector : public TablePropertiesCollector {
 public:
  Status AddUserKey(const Slice&, const Slice&, EntryType type,
                    SequenceNumber seq, uint64_t) override {
    if (type == kEntryDelete || type == kEntrySingleDelete ||
        type == kEntryDeleteWithTimestamp || type == kEntryRangeDeletion) {
      min_seq_ = std::min(min_seq_, seq);
      num_++;
    }
    return Status::OK();
  }
  Status Finish(UserCollectedProperties* props) override {
    if (num_ > 0) {
      (*props)[kMinDelSeqProp] = std::to_string(min_seq_);
    }
    (*props)[kNumDelProp] = std::to_string(num_);
    return Status::OK();
  }
  UserCollectedProperties GetReadableProperties() const override { return {}; }
  const char* Name() const override { return "FadeBenchTombstoneCollector"; }

 private:
  SequenceNumber min_seq_ = ~SequenceNumber{0};
  uint64_t num_ = 0;
};

class TombstoneCollectorFactory : public TablePropertiesCollectorFactory {
 public:
  TablePropertiesCollector* CreateTablePropertiesCollector(
      TablePropertiesCollectorFactory::Context) override {
    return new TombstoneCollector();
  }
  const char* Name() const override { return "FadeBenchTombstoneCollector"; }
};

// seqno -> wall time (seconds, steady clock). Appended by the writer.
class SeqTimeMap {
 public:
  void Record(SequenceNumber s, double t) {
    std::lock_guard<std::mutex> l(mu_);
    if (v_.empty() || s > v_.back().first) v_.emplace_back(s, t);
  }
  // Time at which sequence number s had certainly been written (first record
  // whose seqno >= s). Under-estimates age by at most one record interval.
  double TimeOf(SequenceNumber s) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = std::lower_bound(
        v_.begin(), v_.end(), std::make_pair(s, -1e300));
    if (it == v_.end()) return v_.empty() ? 0 : v_.back().second;
    return it->second;
  }

 private:
  std::mutex mu_;
  std::vector<std::pair<SequenceNumber, double>> v_;
};

// ---- compaction accounting --------------------------------------------------

class CompactionCounter : public EventListener {
 public:
  void OnCompactionCompleted(DB*, const CompactionJobInfo& ci) override {
    std::lock_guard<std::mutex> l(mu_);
    auto& e = by_reason_[GetReasonName(ci.compaction_reason)];
    e.count++;
    e.write_bytes += ci.stats.total_output_bytes;
    e.read_bytes += ci.stats.total_input_bytes;
    if (ci.compaction_reason == CompactionReason::kFadeTtl) {
      int deepest_output_level = ci.output_level;
      for (const CompactionFileInfo& output : ci.output_file_infos) {
        e.output_level_counts[output.level]++;
        deepest_output_level = std::max(deepest_output_level, output.level);
      }
      if (ci.output_file_infos.empty()) {
        e.output_level_counts[ci.output_level]++;
      }
      if (ci.status.ok()) {
        e.level_skips_sum += static_cast<uint64_t>(
            std::max(0, deepest_output_level - ci.output_level - 1));
        e.level_skips_compactions++;
      }
    }
    if (ci.stats.num_input_files_trivially_moved > 0) e.trivial_moves++;
  }
  struct Entry {
    uint64_t count = 0, trivial_moves = 0, write_bytes = 0, read_bytes = 0;
    uint64_t level_skips_sum = 0, level_skips_compactions = 0;
    std::map<int, uint64_t> output_level_counts;
  };
  std::map<std::string, Entry> Snapshot() {
    std::lock_guard<std::mutex> l(mu_);
    return by_reason_;
  }
  static std::string GetReasonName(CompactionReason r) {
    switch (r) {
      case CompactionReason::kLevelL0FilesNum: return "L0FilesNum";
      case CompactionReason::kLevelMaxLevelSize: return "LevelMaxSize";
      case CompactionReason::kBottommostFiles: return "BottommostFiles";
      case CompactionReason::kTtl: return "Ttl";
      case CompactionReason::kFilesMarkedForCompaction: return "Marked";
      case CompactionReason::kManualCompaction: return "Manual";
#ifndef FADE_BENCH_NO_FADE
      case CompactionReason::kFadeTtl: return "FadeTtl";
#endif
      default: return "Other" + std::to_string(static_cast<int>(r));
    }
  }

 private:
  std::mutex mu_;
  std::map<std::string, Entry> by_reason_;
};

// ---- sampler ------------------------------------------------------------------

struct Sample {
  double t;            // seconds since start of write phase
  double max_age;      // oldest live tombstone age (s), 0 if none
  uint64_t live_dels;  // tombstones present in SSTs
  uint64_t sst_bytes;
  std::vector<double> level_max_age;  // per level
};

struct TombstoneScan {
  double max_age = 0;
  uint64_t live_dels = 0;
  uint64_t sst_bytes = 0;
  std::vector<double> level_max_age;
};

TombstoneScan ScanTombstones(DB* db, SeqTimeMap* stm, int num_levels) {
  TombstoneScan r;
  r.level_max_age.assign(num_levels, 0);
  std::vector<LiveFileMetaData> files;
  db->GetLiveFilesMetaData(&files);
  TablePropertiesCollection props;
  if (!db->GetPropertiesOfAllTables(&props).ok()) return r;
  std::map<std::string, int> level_of;
  for (auto& f : files) {
    level_of[f.directory + "/" + f.relative_filename] = f.level;
    r.sst_bytes += f.size;
  }
  double now = NowSec();
  for (auto& kv : props) {
    const UserCollectedProperties& u = kv.second->user_collected_properties;
    auto n = u.find(kNumDelProp);
    if (n != u.end()) r.live_dels += std::stoull(n->second);
    auto m = u.find(kMinDelSeqProp);
    if (m == u.end()) continue;
    double age = now - stm->TimeOf(std::stoull(m->second));
    r.max_age = std::max(r.max_age, age);
    auto lv = level_of.find(kv.first);
    if (lv != level_of.end() && lv->second < num_levels) {
      r.level_max_age[lv->second] = std::max(r.level_max_age[lv->second], age);
    }
  }
  return r;
}

// ---- model + verification -----------------------------------------------------

// ver[k] == 0 : key absent (deleted), else current version number.
bool Verify(DB* db, const std::vector<uint32_t>& ver, const Config& c,
            std::mt19937_64* rng, std::string* err) {
  ReadOptions ro;
  // Full scan.
  std::unique_ptr<Iterator> it(db->NewIterator(ro));
  uint64_t expect = 0;
  while (expect < ver.size() && ver[expect] == 0) expect++;
  uint64_t seen = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (expect >= ver.size()) {
      *err = "scan: extra key " + it->key().ToString();
      return false;
    }
    if (it->key() != Key(expect) ||
        it->value() != Value(expect, ver[expect], c.value_size)) {
      *err = "scan: got " + it->key().ToString() + " expected " + Key(expect);
      return false;
    }
    seen++;
    expect++;
    while (expect < ver.size() && ver[expect] == 0) expect++;
  }
  if (!it->status().ok()) {
    *err = "scan status " + it->status().ToString();
    return false;
  }
  if (expect != ver.size()) {
    *err = "scan: missing key " + Key(expect);
    return false;
  }
  // Point lookups (live and deleted keys).
  std::uniform_int_distribution<uint64_t> d(0, ver.size() - 1);
  std::string v;
  for (int i = 0; i < c.verify_gets; i++) {
    uint64_t k = d(*rng);
    Status s = db->Get(ro, Key(k), &v);
    if (ver[k] == 0) {
      if (!s.IsNotFound()) {
        *err = "get: deleted key found " + Key(k);
        return false;
      }
    } else if (!s.ok() || v != Value(k, ver[k], c.value_size)) {
      *err = "get: wrong value for " + Key(k) + " " + s.ToString();
      return false;
    }
  }
  // Seek checks.
  for (int i = 0; i < 2000; i++) {
    uint64_t k = d(*rng);
    it->Seek(Key(k));
    uint64_t e = k;
    while (e < ver.size() && ver[e] == 0) e++;
    if (e == ver.size()) {
      if (it->Valid()) {
        *err = "seek: expected end after " + Key(k);
        return false;
      }
    } else if (!it->Valid() || it->key() != Key(e)) {
      *err = "seek: wrong position for " + Key(k);
      return false;
    }
  }
  (void)seen;
  return true;
}

Options MakeOptions(const Config& c, std::shared_ptr<CompactionCounter> cc) {
  Options o;
  o.create_if_missing = true;
  o.write_buffer_size = c.write_buffer;
  o.max_bytes_for_level_base = c.level_base;
  o.target_file_size_base = c.target_file;
  o.max_bytes_for_level_multiplier = c.ratio;
  o.level_compaction_dynamic_level_bytes = c.dynamic_level_bytes;
  o.level0_file_num_compaction_trigger = 4;
  o.max_background_jobs = c.bg_jobs;
  o.compression = kNoCompression;
  o.statistics = CreateDBStatistics();
  o.listeners.push_back(cc);
  o.table_properties_collector_factories.push_back(
      std::make_shared<TombstoneCollectorFactory>());
#ifndef FADE_BENCH_NO_FADE
  o.delete_persistence_threshold = c.dth;
  o.fade_b_compaction_threshold = c.b_threshold;
  o.fade_filter_probe_enabled = c.filter_probe;
  o.fade_tombstone_level_routing_enabled = c.tombstone_route;
#else
  if (c.dth != 0 || c.tombstone_route) {
    fprintf(
        stderr,
        "this build has no FADE; dth and tombstone_route must be disabled\n");
    exit(2);
  }
#endif
  BlockBasedTableOptions t;
  t.filter_policy.reset(NewBloomFilterPolicy(10));
  t.block_cache = NewLRUCache(64 << 20);
  o.table_factory.reset(NewBlockBasedTableFactory(t));
  return o;
}

}  // namespace

int BenchMain(int argc, char** argv) {
  Config c;
  if (!ParseArgs(argc, argv, &c)) return 2;
  std::mt19937_64 rng(c.seed);
  std::vector<uint32_t> ver(c.keys, 1);  // 0 = deleted, else live version
  std::vector<uint32_t> gen(c.keys, 1);  // monotonically increasing version

  auto cc = std::make_shared<CompactionCounter>();
  Options opts = MakeOptions(c, cc);
  if (!DestroyDB(c.db, opts).ok()) return 3;
  std::unique_ptr<DB> dbh;
  Status s = DB::Open(opts, c.db, &dbh);
  DB* db = dbh.get();
  if (!s.ok()) {
    fprintf(stderr, "open: %s\n", s.ToString().c_str());
    return 3;
  }
  WriteOptions wo;
  wo.disableWAL = false;

  // ---- preload (random order, no tombstones) ----
  {
    std::vector<uint64_t> order(c.keys);
    for (uint64_t i = 0; i < c.keys; i++) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);
    for (uint64_t k : order) {
      s = db->Put(wo, Key(k), Value(k, 1, c.value_size));
      if (!s.ok()) { fprintf(stderr, "put %s\n", s.ToString().c_str()); return 4; }
    }
    db->Flush(FlushOptions());
    db->WaitForCompact(WaitForCompactOptions());
  }
  const int num_levels = db->NumberLevels();

  Statistics* st = opts.statistics.get();
  st->Reset();
  auto cc_before = cc->Snapshot();
  (void)cc_before;

  // ---- write phase ----
  SeqTimeMap stm;
  std::vector<Sample> samples;
  std::atomic<bool> stop_sampler{false};
  std::atomic<int> phase{0};  // 0 write, 1 read
  double t0 = NowSec();
  stm.Record(db->GetLatestSequenceNumber(), t0);
  std::mutex samples_mu;
  double read_phase_start = 0;
  std::thread sampler([&] {
    while (!stop_sampler.load()) {
      TombstoneScan ts = ScanTombstones(db, &stm, num_levels);
      {
        std::lock_guard<std::mutex> l(samples_mu);
        samples.push_back({NowSec() - t0, ts.max_age, ts.live_dels,
                           ts.sst_bytes, ts.level_max_age});
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  });

  uint64_t n_put = 0, n_del = 0, n_rdel = 0, user_bytes = 0;
  std::uniform_real_distribution<double> u01(0, 1);
  std::uniform_int_distribution<uint64_t> ukey(0, c.keys - 1);
  uint64_t hot_n = std::max<uint64_t>(1, static_cast<uint64_t>(c.keys * c.hot_frac));
  std::uniform_int_distribution<uint64_t> hkey(0, hot_n - 1);
  auto pick_key = [&]() -> uint64_t {
    if (c.hot_frac > 0 && u01(rng) < 0.8) return hkey(rng);
    return ukey(rng);
  };
  double cpu0 = CpuSec();
  uint64_t ops = 0;
  const double t_end = t0 + c.write_secs;
  while (true) {
    double now = NowSec();
    if (now >= t_end) break;
    if (c.rate > 0) {
      double due = t0 + static_cast<double>(ops) / c.rate;
      if (due > now) {
        std::this_thread::sleep_for(std::chrono::duration<double>(due - now));
      }
    }
    for (int b = 0; b < 100; b++, ops++) {
      double r = u01(rng);
      uint64_t k = pick_key();
      if (r < c.put_frac) {
        ver[k] = ++gen[k];
        std::string key = Key(k), val = Value(k, ver[k], c.value_size);
        user_bytes += key.size() + val.size();
        s = db->Put(wo, key, val);
        n_put++;
      } else if (r < c.put_frac + c.range_del_frac) {
        uint64_t e = std::min<uint64_t>(c.keys, k + c.range_len);
        for (uint64_t i = k; i < e; i++) ver[i] = 0;
        std::string b1 = Key(k), b2 = Key(e);
        user_bytes += b1.size() + b2.size();
        s = db->DeleteRange(wo, db->DefaultColumnFamily(), b1, b2);
        n_rdel++;
      } else {
        ver[k] = 0;
        std::string key = Key(k);
        user_bytes += key.size();
        s = db->Delete(wo, key);
        n_del++;
      }
      if (!s.ok()) { fprintf(stderr, "write %s\n", s.ToString().c_str()); return 5; }
    }
    stm.Record(db->GetLatestSequenceNumber(), NowSec());
  }
  double write_wall = NowSec() - t0;
  double write_cpu = CpuSec() - cpu0;

  uint64_t flush_w = st->getTickerCount(FLUSH_WRITE_BYTES);
  uint64_t comp_w = st->getTickerCount(COMPACT_WRITE_BYTES);
  uint64_t comp_r = st->getTickerCount(COMPACT_READ_BYTES);
  uint64_t wal_w = st->getTickerCount(WAL_FILE_BYTES);

  // ---- read phase (no writes => FADE is not re-evaluated) ----
  read_phase_start = NowSec() - t0;
  phase = 1;
  SetPerfLevel(PerfLevel::kEnableCount);
  double rcpu0 = CpuSec();
  double rt0 = NowSec();
  uint64_t gets = 0, scans = 0, scan_keys = 0;
  uint64_t get_block_reads = 0, get_block_cache_hits = 0;
  uint64_t get_block_read_bytes = 0;
  {
    ReadOptions ro;
    std::string v;
    get_perf_context()->Reset();
    double half = rt0 + c.read_secs / 2;
    while (NowSec() < half) {
      for (int i = 0; i < 100; i++, gets++) {
        Status gs = db->Get(ro, Key(ukey(rng)), &v);
        (void)gs;
      }
    }
    double get_end = NowSec();
    get_block_reads = get_perf_context()->block_read_count;
    get_block_cache_hits = get_perf_context()->block_cache_hit_count;
    get_block_read_bytes = get_perf_context()->block_read_byte;
    uint64_t get_tomb_skip = get_perf_context()->internal_delete_skipped_count;
    (void)get_tomb_skip;
    get_perf_context()->Reset();
    std::unique_ptr<Iterator> it(db->NewIterator(ro));
    while (NowSec() < rt0 + c.read_secs) {
      it->Seek(Key(ukey(rng)));
      for (int j = 0; j < 100 && it->Valid(); j++, it->Next()) scan_keys++;
      scans++;
    }
    (void)get_end;
  }
  double read_wall = NowSec() - rt0;
  double read_cpu = CpuSec() - rcpu0;
  uint64_t scan_del_skipped = get_perf_context()->internal_delete_skipped_count;
  uint64_t scan_rdel_skipped =
      get_perf_context()->internal_range_del_reseek_count;
  SetPerfLevel(PerfLevel::kDisable);

  stop_sampler = true;
  sampler.join();

  // ---- end-of-run state ----
  TombstoneScan final_ts = ScanTombstones(db, &stm, num_levels);
  uint64_t live_keys = 0;
  for (uint32_t x : ver) live_keys += (x != 0);
  uint64_t logical_bytes = live_keys * (Key(0).size() + c.value_size);

  double max_age_write = 0, max_age_read = 0;
  uint64_t viol_samples = 0, write_samples = 0, max_live_dels = 0;
  double sum_live_dels = 0;
  for (auto& sm : samples) {
    if (sm.t <= read_phase_start) {
      max_age_write = std::max(max_age_write, sm.max_age);
      write_samples++;
      sum_live_dels += sm.live_dels;
      max_live_dels = std::max(max_live_dels, sm.live_dels);
      if (c.dth > 0 && sm.max_age > c.dth) viol_samples++;
    } else {
      max_age_read = std::max(max_age_read, sm.max_age);
    }
  }

  // ---- correctness ----
  std::string err;
  bool ok1 = Verify(db, ver, c, &rng, &err);
  std::string err2;
  dbh.reset();
  db = nullptr;
  s = DB::Open(opts, c.db, &dbh);
  db = dbh.get();
  bool ok2 = s.ok() && Verify(db, ver, c, &rng, &err2);
  if (!s.ok()) err2 = "reopen: " + s.ToString();

  auto creasons = cc->Snapshot();
  std::string reasons;
  uint64_t fade_count = 0, fade_wbytes = 0;
  uint64_t fade_level_skips_sum = 0, fade_level_skips_compactions = 0;
  std::string fade_output_levels;
  for (auto& kv : creasons) {
    reasons += kv.first + ":" + std::to_string(kv.second.count) + "/" +
               std::to_string(kv.second.trivial_moves) + "tm ";
    if (kv.first == "FadeTtl") {
      fade_count = kv.second.count;
      fade_wbytes = kv.second.write_bytes;
      fade_level_skips_sum = kv.second.level_skips_sum;
      fade_level_skips_compactions = kv.second.level_skips_compactions;
      for (const auto& level_count : kv.second.output_level_counts) {
        if (!fade_output_levels.empty()) fade_output_levels += ";";
        fade_output_levels += "L" + std::to_string(level_count.first) + ":" +
                              std::to_string(level_count.second);
      }
    }
  }

  double wamp = user_bytes ? static_cast<double>(flush_w + comp_w) / user_bytes : 0;
  double samp = logical_bytes ? static_cast<double>(final_ts.sst_bytes) / logical_bytes : 0;

  printf("== %s dth=%" PRIu64 " b_threshold=%" PRIu64
         " filter_probe=%d tombstone_route=%d dynamic_level_bytes=%d ==\n",
         c.label.c_str(), c.dth, c.b_threshold, c.filter_probe ? 1 : 0,
         c.tombstone_route ? 1 : 0, c.dynamic_level_bytes ? 1 : 0);
  printf("ops put=%" PRIu64 " del=%" PRIu64 " rdel=%" PRIu64
         " wall=%.1fs thr=%.0f ops/s cpu=%.1fs (%.2f us/op)\n",
         n_put, n_del, n_rdel, write_wall, ops / write_wall, write_cpu,
         write_cpu * 1e6 / ops);
  printf("io flush_w=%.1fMB comp_w=%.1fMB comp_r=%.1fMB wal=%.1fMB user=%.1fMB "
         "WA=%.2f\n",
         flush_w / 1e6, comp_w / 1e6, comp_r / 1e6, wal_w / 1e6,
         user_bytes / 1e6, wamp);
  printf("compactions %s| fade=%" PRIu64 " fade_w=%.1fMB\n", reasons.c_str(),
         fade_count, fade_wbytes / 1e6);
  printf("fade_output_levels=%s\n", fade_output_levels.c_str());
  printf("fade_avg_level_skips=%.3f (%" PRIu64 " successful compactions)\n",
         fade_level_skips_compactions
             ? static_cast<double>(fade_level_skips_sum) /
                   fade_level_skips_compactions
             : 0.0,
         fade_level_skips_compactions);
  printf("tombstones max_age(write)=%.1fs max_age(read/idle)=%.1fs "
         "viol_samples=%" PRIu64 "/%" PRIu64 " avg_live=%.0f max_live=%" PRIu64
         " final_live=%" PRIu64 "\n",
         max_age_write, max_age_read, viol_samples, write_samples,
         write_samples ? sum_live_dels / write_samples : 0.0, max_live_dels,
         final_ts.live_dels);
  printf("space sst=%.1fMB logical=%.1fMB SA=%.3f\n", final_ts.sst_bytes / 1e6,
         logical_bytes / 1e6, samp);
  printf("reads gets=%.0f/s scans=%.0f/s (%.1f keys/scan) cpu=%.1fs "
         "tomb_skipped=%" PRIu64 " rdel_reseeks=%" PRIu64 "\n",
         gets / (c.read_secs / 2), scans / (c.read_secs / 2),
         scans ? static_cast<double>(scan_keys) / scans : 0.0, read_cpu,
         scan_del_skipped, scan_rdel_skipped);
  printf(
      "read_amp point_get_blocks_touched/get=%.4f "
      "block_reads/get=%.4f block_read_bytes/get=%.1f\n",
      gets ? static_cast<double>(get_block_reads + get_block_cache_hits) / gets
           : 0.0,
      gets ? static_cast<double>(get_block_reads) / gets : 0.0,
      gets ? static_cast<double>(get_block_read_bytes) / gets : 0.0);
  printf("verify before_reopen=%s after_reopen=%s %s %s\n", ok1 ? "OK" : "FAIL",
         ok2 ? "OK" : "FAIL", err.c_str(), err2.c_str());
  (void)read_wall;

  if (!c.out_prefix.empty()) {
    std::string path = c.out_prefix + c.label + "_dth" + std::to_string(c.dth) +
                       "_ts.csv";
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
      fprintf(f, "t,phase,max_age,live_dels,sst_bytes");
      for (int l = 0; l < num_levels; l++) fprintf(f, ",L%d_max_age", l);
      fprintf(f, "\n");
      for (auto& sm : samples) {
        fprintf(f, "%.2f,%s,%.2f,%" PRIu64 ",%" PRIu64, sm.t,
                sm.t <= read_phase_start ? "write" : "read", sm.max_age,
                sm.live_dels, sm.sst_bytes);
        for (double a : sm.level_max_age) fprintf(f, ",%.2f", a);
        fprintf(f, "\n");
      }
      fclose(f);
    }
  }
  if (!c.summary_csv.empty()) {
    FILE* f = fopen(c.summary_csv.c_str(), "a");
    if (f) {
      fseek(f, 0, SEEK_END);
      if (ftell(f) == 0) {
        fprintf(f,
                "label,dth,b_threshold,filter_probe,tombstone_route,"
                "dynamic_level_bytes,rate,put_"
                "frac,range_del_"
                "frac,hot_frac,"
                "ops,write_wall,"
                "throughput,cpu_s,cpu_us_per_op,flush_w_mb,comp_w_mb,comp_r_mb,"
                "user_mb,wa,compactions,fade_compactions,fade_w_mb,"
                "max_tomb_age_write,max_tomb_age_idle,viol_samples,"
                "write_samples,avg_live_dels,final_live_dels,sst_mb,logical_mb,"
                "sa,gets_per_s,scans_per_s,get_blocks_touched_per_get,"
                "get_block_reads_per_get,get_block_read_bytes_per_get,"
                "scan_tomb_skipped,verify,fade_output_levels,"
                "fade_avg_level_skips,fade_skip_compactions\n");
      }
      uint64_t total_comp = 0;
      for (auto& kv : creasons) total_comp += kv.second.count;
      fprintf(
          f,
          "%s,%" PRIu64 ",%" PRIu64 ",%d,%d,%d,%" PRIu64
          ",%.2f,%.3f,%.2f,%" PRIu64
          ",%.1f,%.0f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.3f,%" PRIu64 ",%" PRIu64
          ",%.1f,%.1f,%.1f,%" PRIu64 ",%" PRIu64 ",%.0f,%" PRIu64
          ",%.1f,%.1f,%.3f,%.0f,%.0f,%.4f,%.4f,%.1f,%" PRIu64
          ",%s,%s,%.3f,%" PRIu64 "\n",
          c.label.c_str(), c.dth, c.b_threshold, c.filter_probe ? 1 : 0,
          c.tombstone_route ? 1 : 0, c.dynamic_level_bytes ? 1 : 0, c.rate,
          c.put_frac, c.range_del_frac, c.hot_frac, ops, write_wall,
          ops / write_wall, write_cpu, write_cpu * 1e6 / ops, flush_w / 1e6,
          comp_w / 1e6, comp_r / 1e6, user_bytes / 1e6, wamp, total_comp,
          fade_count, fade_wbytes / 1e6, max_age_write, max_age_read,
          viol_samples, write_samples,
          write_samples ? sum_live_dels / write_samples : 0.0,
          final_ts.live_dels, final_ts.sst_bytes / 1e6, logical_bytes / 1e6,
          samp, gets / (c.read_secs / 2), scans / (c.read_secs / 2),
          gets ? static_cast<double>(get_block_reads + get_block_cache_hits) /
                     gets
               : 0.0,
          gets ? static_cast<double>(get_block_reads) / gets : 0.0,
          gets ? static_cast<double>(get_block_read_bytes) / gets : 0.0,
          scan_del_skipped, (ok1 && ok2) ? "OK" : "FAIL",
          fade_output_levels.c_str(),
          fade_level_skips_compactions
              ? static_cast<double>(fade_level_skips_sum) /
                    fade_level_skips_compactions
              : 0.0,
          fade_level_skips_compactions);
      fclose(f);
    }
  }
  dbh.reset();
  DestroyDB(c.db, opts);
  return (ok1 && ok2) ? 0 : 1;
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  return ROCKSDB_NAMESPACE::BenchMain(argc, argv);
}
