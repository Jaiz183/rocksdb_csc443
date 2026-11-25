//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Release-mode benchmark evaluating KiWi secondary range deletes interleaved
// with point Get queries across delete/query splits and tile sizes.
//
// Candidates:
//   1. plain_bloom10          : BlockBasedTable (h = 0) + Bloom(10 bpk)
//   2. kiwi_page_bloom10      : KiWi (h in {4,8,16}) + Bloom(10 bpk), Dir v1
//   3. kiwi_tile_xor_baseline : KiWi (h in {4,8,16}) + XorFilter(1B), Dir v2
//   4. kiwi_sst_xor_baseline  : KiWi (h in {4,8,16}) + SST XorFilter(1B), Dir
//   v3

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "rocksdb/cache.h"
#include "rocksdb/convenience.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/iterator.h"
#include "rocksdb/metadata.h"
#include "rocksdb/options.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/slice.h"
#include "rocksdb/statistics.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "rocksdb/table_properties.h"
#include "rocksdb/write_batch.h"
#include "table/block_based/kiwi_tile.h"

namespace ROCKSDB_NAMESPACE {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kNumKeys = 120000;
constexpr size_t kValueSize = 100;
constexpr uint64_t kDeleteWindow = 1000;
constexpr size_t kMaxQueries = 100000;
constexpr uint64_t kWarmCacheBytes = 512ULL << 20;

constexpr size_t kWriteBufferSize = 2ULL << 20;
constexpr size_t kTargetFileSizeBase = 2ULL << 20;
constexpr uint64_t kMaxBytesForLevelBase = 4ULL << 20;
constexpr double kLevelMultiplier = 4.0;

constexpr int kMaxBusyRetries = 64;

void CheckOk(const Status& s, const char* context) {
  if (!s.ok()) {
    fprintf(stderr, "ERROR [%s]: %s\n", context, s.ToString().c_str());
    std::abort();
  }
}

double ElapsedNs(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::nano>(b - a).count();
}

std::string MakeKey(uint64_t k) {
  char buf[24];
  snprintf(buf, sizeof(buf), "key%012" PRIu64, k);
  return std::string(buf);
}

std::string MakeAbsentKey(uint64_t k) {
  char buf[28];
  snprintf(buf, sizeof(buf), "key%012" PRIu64 "z", k);
  return std::string(buf);
}

std::string MakeValue(uint64_t delete_key) {
  std::string v(kValueSize, 'v');
  for (size_t i = 0; i < 8; ++i) {
    v[i] = static_cast<char>((delete_key >> (8 * (7 - i))) & 0xffU);
  }
  return v;
}

enum class CandidateKind {
  kPlainBloom10,
  kKiWiPageBloom10,
  kKiWiTileXorBaseline,
  kKiWiSstXorBaseline,
};

struct ConfigSpec {
  std::string name;
  uint32_t tile_pages;
  CandidateKind kind;
};

struct SplitSpec {
  uint32_t del_pct;
  uint32_t query_pct;
  std::string label;
};

struct QuerySpec {
  std::string key;
  uint64_t delete_key;  // 0 for absent keys; [1 .. kNumKeys] for existing keys
};

struct PreloadDataset {
  std::vector<std::string> present_keys;
  std::vector<size_t> insert_order;
  std::vector<uint64_t> key_delete_val;
  std::vector<QuerySpec> queries;
  uint64_t preload_user_bytes = 0;
};

struct SpaceStats {
  uint64_t sst_bytes = 0;
  uint64_t filter_bytes = 0;
  uint64_t directory_bytes = 0;
  uint64_t index_bytes = 0;
  uint64_t table_readers_mem = 0;
  uint64_t num_entries = 0;
  uint64_t num_files = 0;

  double filter_bpk() const {
    return num_entries == 0 ? 0.0 : (8.0 * filter_bytes) / num_entries;
  }

  double dir_bpk() const {
    return num_entries == 0 ? 0.0 : (8.0 * directory_bytes) / num_entries;
  }
};

struct BenchResult {
  std::string config_name;
  std::string split_label;
  uint32_t tile_pages = 0;
  uint32_t del_pct = 0;
  uint32_t query_pct = 0;
  uint64_t deleted_keys = 0;
  uint64_t num_range_dels = 0;
  uint64_t num_queries = 0;

  // Uncached / I/O-inclusive pass metrics
  double wall_ms = 0.0;
  double total_ops_per_s = 0.0;
  double raw_ops_per_s = 0.0;
  double del_wall_ms = 0.0;
  double del_ms_per_rdel = 0.0;
  double del_keys_per_s = 0.0;
  double range_dels_per_s = 0.0;
  double io_qps = 0.0;
  double io_query_us = 0.0;
  double io_blk_per_q = 0.0;

  // Write & space amplification + memory consumption
  double w_amp = 0.0;
  double wl_write_mb = 0.0;
  double sp_amp = 0.0;
  double flt_bpk = 0.0;
  double dir_bpk = 0.0;
  double tr_kb_before = 0.0;
  double tr_kb_after = 0.0;
  double sst_mb_before = 0.0;
  double sst_mb_after = 0.0;
  double sst_mb_compacted = 0.0;
  uint64_t files_before = 0;
  uint64_t files_after = 0;

  // In-memory pass metrics (512 MB LRUCache, kBlockCacheTier)
  double mem_qps = 0.0;
  double mem_query_ns = 0.0;
};

PreloadDataset BuildDataset() {
  PreloadDataset ds;
  ds.present_keys.resize(kNumKeys);
  ds.insert_order.resize(kNumKeys);
  ds.key_delete_val.resize(kNumKeys, 0);
  for (size_t i = 0; i < kNumKeys; ++i) {
    ds.present_keys[i] = MakeKey(i);
    ds.insert_order[i] = i;
  }

  std::mt19937_64 preload_rng(42);
  std::shuffle(ds.insert_order.begin(), ds.insert_order.end(), preload_rng);
  for (size_t pos = 0; pos < kNumKeys; ++pos) {
    const size_t key_id = ds.insert_order[pos];
    const uint64_t d_val = pos + 1;
    ds.key_delete_val[key_id] = d_val;
    ds.preload_user_bytes += ds.present_keys[key_id].size() + kValueSize;
  }

  ds.queries.resize(kMaxQueries);
  std::mt19937_64 query_rng(1337);
  for (size_t i = 0; i < kMaxQueries; ++i) {
    if (i % 2 == 0) {
      const size_t key_id = query_rng() % kNumKeys;
      ds.queries[i].key = ds.present_keys[key_id];
      ds.queries[i].delete_key = ds.key_delete_val[key_id];
    } else {
      const size_t key_id = query_rng() % (kNumKeys - 1);
      ds.queries[i].key = MakeAbsentKey(key_id);
      ds.queries[i].delete_key = 0;
    }
  }
  return ds;
}

std::shared_ptr<const FilterPolicy> MakeFilterPolicy(CandidateKind kind) {
  switch (kind) {
    case CandidateKind::kPlainBloom10:
    case CandidateKind::kKiWiPageBloom10:
      return std::shared_ptr<const FilterPolicy>(NewBloomFilterPolicy(10));
    case CandidateKind::kKiWiTileXorBaseline:
      return std::shared_ptr<const FilterPolicy>(NewXorFilterPolicy(1));
    case CandidateKind::kKiWiSstXorBaseline: {
      std::shared_ptr<const FilterPolicy> policy;
      Status s = FilterPolicy::CreateFromString(
          ConfigOptions(), "{id=xorfilter:1;kiwi_file_filter=true}", &policy);
      CheckOk(s, "FilterPolicy::CreateFromString(kiwi_file_filter)");
      return policy;
    }
  }
  return nullptr;
}

Options MakeOptions(const ConfigSpec& cfg, bool enable_cache,
                    const std::shared_ptr<Cache>& cache,
                    const std::shared_ptr<Statistics>& stats) {
  Options opts;
  opts.create_if_missing = true;
  opts.compression = kNoCompression;
  opts.write_buffer_size = kWriteBufferSize;
  opts.target_file_size_base = kTargetFileSizeBase;
  opts.max_bytes_for_level_base = kMaxBytesForLevelBase;
  opts.max_bytes_for_level_multiplier = kLevelMultiplier;
  opts.level0_file_num_compaction_trigger = 4;
  opts.max_background_jobs = 4;
  opts.statistics = stats;

  BlockBasedTableOptions bbto;
  bbto.block_size = 4096;
  bbto.kiwi_tile_pages = cfg.tile_pages;
  bbto.kiwi_delete_key_offset = 0;
  bbto.filter_policy = MakeFilterPolicy(cfg.kind);
  if (enable_cache) {
    bbto.block_cache = cache;
    bbto.no_block_cache = false;
  } else {
    bbto.block_cache.reset();
    bbto.no_block_cache = true;
  }
  opts.table_factory.reset(NewBlockBasedTableFactory(bbto));
  return opts;
}

uint64_t LookupUserPropUint64(const UserCollectedProperties& uprops,
                              const char* prop_name) {
  const UserCollectedProperties::const_iterator it = uprops.find(prop_name);
  if (it == uprops.end()) {
    return 0;
  }
  return std::stoull(it->second);
}

SpaceStats MeasureSpace(DB* db, bool is_kiwi) {
  // Ensure every live SST's TableReader (with any updated delete floor) is
  // opened in TableCache before querying rocksdb.estimate-table-readers-mem.
  {
    std::unique_ptr<Iterator> open_it(db->NewIterator(ReadOptions()));
    open_it->SeekToFirst();
    CheckOk(open_it->status(), "MeasureSpace open_it");
  }

  SpaceStats sp;
  std::vector<LiveFileMetaData> files;
  db->GetLiveFilesMetaData(&files);
  for (const auto& f : files) {
    sp.sst_bytes += f.size;
    ++sp.num_files;
  }

  TablePropertiesCollection props;
  Status st = db->GetPropertiesOfAllTables(&props);
  CheckOk(st, "GetPropertiesOfAllTables");
  for (const auto& kv : props) {
    const std::shared_ptr<const TableProperties>& p = kv.second;
    sp.num_entries += p->num_entries;
    sp.index_bytes += p->index_size;
    if (is_kiwi) {
      sp.filter_bytes += LookupUserPropUint64(p->user_collected_properties,
                                              kiwi::kPropFilterBytes);
      sp.directory_bytes += LookupUserPropUint64(p->user_collected_properties,
                                                 kiwi::kPropDirectoryBytes);
    } else {
      sp.filter_bytes += p->filter_size;
    }
  }

  uint64_t tr_mem = 0;
  if (db->GetIntProperty("rocksdb.estimate-table-readers-mem", &tr_mem)) {
    sp.table_readers_mem = tr_mem;
  }
  return sp;
}

void PreloadDB(DB* db, const PreloadDataset& ds) {
  WriteOptions wo;
  wo.disableWAL = true;
  for (size_t pos = 0; pos < kNumKeys; ++pos) {
    const size_t key_id = ds.insert_order[pos];
    const uint64_t d_val = pos + 1;
    CheckOk(db->Put(wo, ds.present_keys[key_id], MakeValue(d_val)),
            "PreloadDB Put");
  }
  CheckOk(db->Flush(FlushOptions()), "PreloadDB Flush");
  CheckOk(db->WaitForCompact(WaitForCompactOptions()),
          "PreloadDB WaitForCompact");
}

void WarmCacheByScan(DB* db) {
  ReadOptions ro;
  ro.fill_cache = true;
  std::unique_ptr<Iterator> it(db->NewIterator(ro));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
  }
  CheckOk(it->status(), "WarmCacheByScan");
}

// Executes one secondary range delete advancing floor to `next_floor`.
// Returns tombstone user bytes written (0 for KiWi; sum of deleted key sizes
// for plain_bloom10).
uint64_t ExecuteSecondaryRangeDelete(DB* db, bool is_kiwi,
                                     uint64_t next_floor) {
  if (is_kiwi) {
    Status s = db->SecondaryDeleteBelow(next_floor);
    int retries = 0;
    while (s.IsBusy() && retries < kMaxBusyRetries) {
      CheckOk(db->WaitForCompact(WaitForCompactOptions()),
              "WaitForCompact on SecondaryDeleteBelow Busy");
      s = db->SecondaryDeleteBelow(next_floor);
      ++retries;
    }
    CheckOk(s, "SecondaryDeleteBelow");
    return 0;
  }

  WriteBatch batch;
  uint64_t tombstone_user_bytes = 0;
  uint64_t matched = 0;
  {
    std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      uint64_t d_val = 0;
      CheckOk(kiwi::ExtractDeleteKey(kTypeValue, it->value(), 0, &d_val),
              "ExtractDeleteKey in plain scan");
      if (d_val < next_floor) {
        const Slice k = it->key();
        CheckOk(batch.Delete(k), "WriteBatch::Delete");
        tombstone_user_bytes += k.size();
        ++matched;
      }
    }
    CheckOk(it->status(), "Plain secondary delete scan");
  }
  if (matched != kDeleteWindow) {
    fprintf(stderr,
            "ERROR: expected %" PRIu64
            " keys in secondary delete window, got "
            "%" PRIu64 " (next_floor=%" PRIu64 ")\n",
            kDeleteWindow, matched, next_floor);
    std::abort();
  }
  WriteOptions wo;
  wo.disableWAL = true;
  CheckOk(db->Write(wo, &batch), "Plain secondary delete WriteBatch");
  CheckOk(db->Flush(FlushOptions()), "Plain secondary delete Flush");
  return tombstone_user_bytes;
}

// Runs query slice [q_start, q_end) and verifies every Get against exact
// ground truth (D >= current_floor). Accumulates query elapsed ns and block
// reads.
void ExecuteQuerySlice(DB* db, const std::vector<QuerySpec>& queries,
                       size_t q_start, size_t q_end, uint64_t current_floor,
                       bool mem_only, std::string* val_scratch,
                       double* query_wall_ns, uint64_t* query_block_reads) {
  if (q_start >= q_end) {
    return;
  }
  ReadOptions ro;
  if (mem_only) {
    ro.read_tier = kBlockCacheTier;
  }
  SetPerfLevel(PerfLevel::kEnableCount);
  get_perf_context()->Reset();

  const Clock::time_point q0 = Clock::now();
  for (size_t qi = q_start; qi < q_end; ++qi) {
    const QuerySpec& q = queries[qi];
    const Status s = db->Get(ro, q.key, val_scratch);
    if (mem_only && s.IsIncomplete()) {
      fprintf(stderr, "ERROR: cache miss under kBlockCacheTier for key %s\n",
              q.key.c_str());
      std::abort();
    }
    const bool expect_found = (q.delete_key >= current_floor);
    if (expect_found) {
      if (!s.ok()) {
        fprintf(stderr,
                "ERROR: expected key %s (D=%" PRIu64 " >= floor=%" PRIu64
                ") to be found, got: %s\n",
                q.key.c_str(), q.delete_key, current_floor,
                s.ToString().c_str());
        std::abort();
      }
      uint64_t got_d = 0;
      CheckOk(kiwi::ExtractDeleteKey(kTypeValue, *val_scratch, 0, &got_d),
              "Verify Get ExtractDeleteKey");
      if (val_scratch->size() != kValueSize || got_d != q.delete_key) {
        fprintf(stderr,
                "ERROR: value mismatch for key %s: expected D=%" PRIu64
                ", got D=%" PRIu64 "\n",
                q.key.c_str(), q.delete_key, got_d);
        std::abort();
      }
    } else if (!s.IsNotFound()) {
      fprintf(stderr,
              "ERROR: expected key %s (D=%" PRIu64 " < floor=%" PRIu64
              ") to be NotFound, got: %s\n",
              q.key.c_str(), q.delete_key, current_floor, s.ToString().c_str());
      std::abort();
    }
  }
  const Clock::time_point q1 = Clock::now();

  const uint64_t blk_reads = get_perf_context()->block_read_count;
  SetPerfLevel(PerfLevel::kDisable);
  if (mem_only && blk_reads != 0) {
    fprintf(stderr,
            "ERROR: expected 0 block reads in in-memory pass, got %" PRIu64
            "\n",
            blk_reads);
    std::abort();
  }

  *query_wall_ns += ElapsedNs(q0, q1);
  *query_block_reads += blk_reads;
}

void RemoveDirAll(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

BenchResult RunConfigSplit(const std::string& root, const ConfigSpec& cfg,
                           const SplitSpec& split, const PreloadDataset& ds) {
  const bool is_kiwi = (cfg.tile_pages >= 2);
  BenchResult res;
  res.config_name = cfg.name;
  res.split_label = split.label;
  res.tile_pages = cfg.tile_pages;
  res.del_pct = split.del_pct;
  res.query_pct = split.query_pct;
  res.num_range_dels = split.del_pct;
  res.deleted_keys = res.num_range_dels * kDeleteWindow;
  res.num_queries = static_cast<uint64_t>(split.query_pct) * 1000ULL;

  // =========================================================================
  // Pass 1: Uncached / I/O-inclusive pass (no_block_cache = true)
  // =========================================================================
  {
    const std::string db_dir = root + "/io_" + cfg.name + "_h" +
                               std::to_string(cfg.tile_pages) + "_" +
                               split.label;
    RemoveDirAll(db_dir);

    std::shared_ptr<Statistics> stats = CreateDBStatistics();
    Options opts = MakeOptions(cfg, /*enable_cache=*/false, nullptr, stats);
    std::unique_ptr<DB> db;
    CheckOk(DB::Open(opts, db_dir, &db), "Pass1 DB::Open");

    PreloadDB(db.get(), ds);
    const SpaceStats sp_before = MeasureSpace(db.get(), is_kiwi);
    const uint64_t write_after_preload =
        stats->getTickerCount(FLUSH_WRITE_BYTES) +
        stats->getTickerCount(COMPACT_WRITE_BYTES);

    double del_wall_ns = 0.0;
    double query_wall_ns = 0.0;
    uint64_t query_blk_reads = 0;
    uint64_t workload_tombstone_bytes = 0;
    uint64_t current_floor = 1;
    std::string val_scratch;
    val_scratch.reserve(kValueSize);

    const Clock::time_point loop_t0 = Clock::now();
    if (res.num_range_dels == 0) {
      ExecuteQuerySlice(db.get(), ds.queries, 0,
                        static_cast<size_t>(res.num_queries), current_floor,
                        /*mem_only=*/false, &val_scratch, &query_wall_ns,
                        &query_blk_reads);
    } else {
      for (uint64_t r = 0; r < res.num_range_dels; ++r) {
        current_floor = 1 + (r + 1) * kDeleteWindow;
        const Clock::time_point d0 = Clock::now();
        workload_tombstone_bytes +=
            ExecuteSecondaryRangeDelete(db.get(), is_kiwi, current_floor);
        const Clock::time_point d1 = Clock::now();
        del_wall_ns += ElapsedNs(d0, d1);

        const size_t q_start =
            static_cast<size_t>((r * res.num_queries) / res.num_range_dels);
        const size_t q_end = static_cast<size_t>(((r + 1) * res.num_queries) /
                                                 res.num_range_dels);
        ExecuteQuerySlice(db.get(), ds.queries, q_start, q_end, current_floor,
                          /*mem_only=*/false, &val_scratch, &query_wall_ns,
                          &query_blk_reads);
      }
    }
    const Clock::time_point loop_t1 = Clock::now();
    const double total_wall_ns = ElapsedNs(loop_t0, loop_t1);

    // Let any scheduled non-bottommost compactions settle before measuring
    // write amplification, memory consumption, and space amplification.
    CheckOk(db->WaitForCompact(WaitForCompactOptions()),
            "Pass1 post-workload WaitForCompact");

    const uint64_t total_write_bytes =
        stats->getTickerCount(FLUSH_WRITE_BYTES) +
        stats->getTickerCount(COMPACT_WRITE_BYTES);
    const uint64_t wl_write_bytes =
        total_write_bytes >= write_after_preload
            ? (total_write_bytes - write_after_preload)
            : 0;
    const SpaceStats sp_after = MeasureSpace(db.get(), is_kiwi);

    // Forced bottommost compaction to measure exact post-workload space amp.
    CompactRangeOptions cro;
    cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
    CheckOk(db->CompactRange(cro, nullptr, nullptr),
            "Pass1 forced CompactRange");
    CheckOk(db->WaitForCompact(WaitForCompactOptions()),
            "Pass1 post-CompactRange WaitForCompact");
    const SpaceStats sp_compacted = MeasureSpace(db.get(), is_kiwi);

    const double total_wall_s = total_wall_ns / 1e9;
    res.wall_ms = total_wall_ns / 1e6;
    res.total_ops_per_s =
        static_cast<double>(res.deleted_keys + res.num_queries) / total_wall_s;
    res.raw_ops_per_s =
        static_cast<double>(res.num_range_dels + res.num_queries) /
        total_wall_s;

    if (res.num_range_dels > 0) {
      const double del_wall_s = del_wall_ns / 1e9;
      res.del_wall_ms = del_wall_ns / 1e6;
      res.del_ms_per_rdel = res.del_wall_ms / res.num_range_dels;
      res.del_keys_per_s = static_cast<double>(res.deleted_keys) / del_wall_s;
      res.range_dels_per_s =
          static_cast<double>(res.num_range_dels) / del_wall_s;
    }

    if (res.num_queries > 0) {
      const double query_wall_s = query_wall_ns / 1e9;
      res.io_qps = static_cast<double>(res.num_queries) / query_wall_s;
      res.io_query_us = (query_wall_ns / 1e3) / res.num_queries;
      res.io_blk_per_q = static_cast<double>(query_blk_reads) / res.num_queries;
    }

    const uint64_t total_user_bytes =
        ds.preload_user_bytes + workload_tombstone_bytes;
    res.w_amp = static_cast<double>(total_write_bytes) / total_user_bytes;
    res.wl_write_mb = static_cast<double>(wl_write_bytes) / (1024.0 * 1024.0);
    res.sp_amp =
        sp_compacted.sst_bytes == 0
            ? 1.0
            : static_cast<double>(sp_after.sst_bytes) / sp_compacted.sst_bytes;

    res.flt_bpk = sp_before.filter_bpk();
    res.dir_bpk = sp_before.dir_bpk();
    res.tr_kb_before =
        static_cast<double>(sp_before.table_readers_mem) / 1024.0;
    res.tr_kb_after = static_cast<double>(sp_after.table_readers_mem) / 1024.0;
    res.sst_mb_before =
        static_cast<double>(sp_before.sst_bytes) / (1024.0 * 1024.0);
    res.sst_mb_after =
        static_cast<double>(sp_after.sst_bytes) / (1024.0 * 1024.0);
    res.sst_mb_compacted =
        static_cast<double>(sp_compacted.sst_bytes) / (1024.0 * 1024.0);
    res.files_before = sp_before.num_files;
    res.files_after = sp_after.num_files;

    db.reset();
    RemoveDirAll(db_dir);
  }

  // =========================================================================
  // Pass 2: In-memory query pass (512 MB LRUCache, kBlockCacheTier, 0 I/O)
  // =========================================================================
  if (res.num_queries > 0) {
    const std::string db_dir = root + "/mem_" + cfg.name + "_h" +
                               std::to_string(cfg.tile_pages) + "_" +
                               split.label;
    RemoveDirAll(db_dir);

    std::shared_ptr<Cache> cache = NewLRUCache(kWarmCacheBytes);
    std::shared_ptr<Statistics> stats = CreateDBStatistics();
    Options opts = MakeOptions(cfg, /*enable_cache=*/true, cache, stats);
    std::unique_ptr<DB> db;
    CheckOk(DB::Open(opts, db_dir, &db), "Pass2 DB::Open");

    PreloadDB(db.get(), ds);

    double mem_query_wall_ns = 0.0;
    uint64_t mem_blk_reads = 0;
    uint64_t current_floor = 1;
    std::string val_scratch;
    val_scratch.reserve(kValueSize);

    if (res.num_range_dels == 0) {
      WarmCacheByScan(db.get());
      ExecuteQuerySlice(db.get(), ds.queries, 0,
                        static_cast<size_t>(res.num_queries), current_floor,
                        /*mem_only=*/true, &val_scratch, &mem_query_wall_ns,
                        &mem_blk_reads);
    } else {
      for (uint64_t r = 0; r < res.num_range_dels; ++r) {
        current_floor = 1 + (r + 1) * kDeleteWindow;
        ExecuteSecondaryRangeDelete(db.get(), is_kiwi, current_floor);

        // Settle any scheduled compaction and warm newly reopened TableReaders
        // and surviving blocks before timing kBlockCacheTier queries.
        CheckOk(db->WaitForCompact(WaitForCompactOptions()),
                "Pass2 post-delete WaitForCompact");
        WarmCacheByScan(db.get());

        const size_t q_start =
            static_cast<size_t>((r * res.num_queries) / res.num_range_dels);
        const size_t q_end = static_cast<size_t>(((r + 1) * res.num_queries) /
                                                 res.num_range_dels);
        ExecuteQuerySlice(db.get(), ds.queries, q_start, q_end, current_floor,
                          /*mem_only=*/true, &val_scratch, &mem_query_wall_ns,
                          &mem_blk_reads);
      }
    }

    res.mem_query_ns = mem_query_wall_ns / res.num_queries;
    res.mem_qps =
        static_cast<double>(res.num_queries) * 1e9 / mem_query_wall_ns;

    db.reset();
    RemoveDirAll(db_dir);
  }

  return res;
}

std::string FmtFixed(double v, int prec) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%.*f", prec, v);
  return std::string(buf);
}

std::string FmtArrow(double a, double b, int prec) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.*f->%.*f", prec, a, prec, b);
  return std::string(buf);
}

void PrintRow(uint32_t display_h, const BenchResult& r) {
  const std::string h_str = r.tile_pages == 0
                                ? ("0(" + std::to_string(display_h) + ")")
                                : std::to_string(r.tile_pages);
  const std::string del_keys_s =
      r.num_range_dels == 0 ? "N/A" : FmtFixed(r.del_keys_per_s / 1e3, 1);
  const std::string rdel_s =
      r.num_range_dels == 0 ? "N/A" : FmtFixed(r.range_dels_per_s, 1);
  const std::string del_ms =
      r.num_range_dels == 0 ? "N/A" : FmtFixed(r.del_ms_per_rdel, 3);
  const std::string io_qps =
      r.num_queries == 0 ? "N/A" : FmtFixed(r.io_qps / 1e3, 1);
  const std::string io_us =
      r.num_queries == 0 ? "N/A" : FmtFixed(r.io_query_us, 2);
  const std::string io_blk =
      r.num_queries == 0 ? "N/A" : FmtFixed(r.io_blk_per_q, 2);
  const std::string mem_qps =
      r.num_queries == 0 ? "N/A" : FmtFixed(r.mem_qps / 1e3, 1);
  const std::string mem_ns =
      r.num_queries == 0 ? "N/A" : FmtFixed(r.mem_query_ns, 1);

  std::cout << std::left << std::setw(6) << h_str << std::setw(8)
            << r.split_label << std::setw(24) << r.config_name << std::right
            << std::setw(9) << FmtFixed(r.wall_ms, 1) << std::setw(10)
            << FmtFixed(r.total_ops_per_s / 1e3, 1) << std::setw(11)
            << del_keys_s << std::setw(9) << rdel_s << std::setw(9) << del_ms
            << std::setw(9) << io_qps << std::setw(8) << io_us << std::setw(7)
            << io_blk << std::setw(10) << mem_qps << std::setw(9) << mem_ns
            << std::setw(7) << FmtFixed(r.w_amp, 2) << std::setw(8)
            << FmtFixed(r.wl_write_mb, 2) << std::setw(7)
            << FmtFixed(r.sp_amp, 2) << std::setw(8) << FmtFixed(r.flt_bpk, 2)
            << std::setw(8) << FmtFixed(r.dir_bpk, 2) << std::setw(15)
            << FmtArrow(r.tr_kb_before, r.tr_kb_after, 1) << std::setw(15)
            << FmtArrow(r.sst_mb_before, r.sst_mb_after, 2) << '\n';
}

void PrintTableHeader() {
  std::cout << std::left << std::setw(6) << "h" << std::setw(8) << "split"
            << std::setw(24) << "config" << std::right << std::setw(9)
            << "wall_ms" << std::setw(10) << "kops/s" << std::setw(11)
            << "del_k/s" << std::setw(9) << "rdel/s" << std::setw(9) << "del_ms"
            << std::setw(9) << "io_kqps" << std::setw(8) << "io_us"
            << std::setw(7) << "io_blk" << std::setw(10) << "mem_kqps"
            << std::setw(9) << "mem_ns" << std::setw(7) << "w_amp"
            << std::setw(8) << "wl_MB" << std::setw(7) << "sp_amp"
            << std::setw(8) << "flt_bpk" << std::setw(8) << "dir_bpk"
            << std::setw(15) << "tr_KB(B->A)" << std::setw(15)
            << "sst_MB(B->A)\n";
  std::cout << std::string(197, '-') << '\n';
}

void EmitCsvRow(const BenchResult& r) {
  char buf[512];
  snprintf(buf, sizeof(buf),
           "%" PRIu32 ",%s,%s,%" PRIu32 ",%" PRIu32
           ",%.4f,%.2f,%.2f,%.4f,%.4f,%.2f,%.2f,%.2f,%.4f,%.4f,%.2f,%.4f,"
           "%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.4f,%.4f,%.4f,%" PRIu64
           ",%" PRIu64,
           r.tile_pages, r.split_label.c_str(), r.config_name.c_str(),
           r.del_pct, r.query_pct, r.wall_ms, r.total_ops_per_s,
           r.raw_ops_per_s, r.del_wall_ms, r.del_ms_per_rdel, r.del_keys_per_s,
           r.range_dels_per_s, r.io_qps, r.io_query_us, r.io_blk_per_q,
           r.mem_qps, r.mem_query_ns, r.w_amp, r.wl_write_mb, r.sp_amp,
           r.flt_bpk, r.dir_bpk, r.tr_kb_before, r.tr_kb_after, r.sst_mb_before,
           r.sst_mb_after, r.sst_mb_compacted, r.files_before, r.files_after);
  std::cout << buf << '\n';
}

int RunBenchmark() {
  const Clock::time_point bench_t0 = Clock::now();
  const std::string root = "/tmp/kiwi_tile_filter_bench_" +
                           std::to_string(Env::Default()->NowMicros());
  RemoveDirAll(root);
  std::filesystem::create_directories(root);

  const PreloadDataset ds = BuildDataset();
  const std::vector<SplitSpec> splits = {
      {0, 100, "0-100"}, {25, 75, "25-75"}, {50, 50, "50-50"},
      {75, 25, "75-25"}, {100, 0, "100-0"},
  };
  const std::vector<uint32_t> tile_sizes = {4, 8, 16};

  std::cout << "==============================================================="
               "=================\n"
            << "KiWi Secondary Range Delete + Point Query Benchmark (Release)\n"
            << "  Preload : " << kNumKeys
            << " keys, 4 KB blocks, 100 B values (8 B BE delete key at offset "
               "0)\n"
            << "  LSM     : write_buf=2MB, target_sst=2MB, L1_base=4MB, "
               "mult=4.0, uncompressed\n"
            << "  Budget  : 100,000 key-equivalent ops/split (1 range del = "
            << kDeleteWindow << " keys)\n"
            << "==============================================================="
               "=================\n";

  // Run plain_bloom10 once per split (independent of tile size h) and reuse
  // across h = 4, 8, 16 comparisons.
  const ConfigSpec plain_cfg{"plain_bloom10", 0, CandidateKind::kPlainBloom10};
  std::vector<BenchResult> plain_results;
  plain_results.reserve(splits.size());
  for (size_t si = 0; si < splits.size(); ++si) {
    plain_results.push_back(RunConfigSplit(root, plain_cfg, splits[si], ds));
  }

  std::vector<BenchResult> all_kiwi_results;
  for (uint32_t h : tile_sizes) {
    std::cout << "\n=== Tile Size h = " << h << " pages ===\n";
    PrintTableHeader();

    const std::vector<ConfigSpec> kiwi_cfgs = {
        {"kiwi_page_bloom10", h, CandidateKind::kKiWiPageBloom10},
        {"kiwi_tile_xor_baseline", h, CandidateKind::kKiWiTileXorBaseline},
        {"kiwi_sst_xor_baseline", h, CandidateKind::kKiWiSstXorBaseline},
    };

    for (size_t si = 0; si < splits.size(); ++si) {
      PrintRow(h, plain_results[si]);
      for (const auto& kcfg : kiwi_cfgs) {
        const BenchResult kr = RunConfigSplit(root, kcfg, splits[si], ds);
        PrintRow(h, kr);
        all_kiwi_results.push_back(kr);
      }
      if (si + 1 < splits.size()) {
        std::cout << std::string(197, '.') << '\n';
      }
    }
  }

  // Emit exact CSV block for downstream analysis / verification.
  std::cout << "\nCSV_BEGIN\n"
            << "h,split,config,del_pct,query_pct,wall_ms,total_ops_per_s,"
               "raw_ops_per_s,del_wall_ms,del_ms_per_rdel,del_keys_per_s,"
               "range_dels_per_s,io_qps,io_query_us,io_blk_per_q,mem_qps,"
               "mem_query_ns,w_amp,wl_write_mb,sp_amp,flt_bpk,dir_bpk,"
               "tr_kb_before,tr_kb_after,sst_mb_before,sst_mb_after,"
               "sst_mb_compacted,files_before,files_after\n";
  for (const auto& r : plain_results) {
    EmitCsvRow(r);
  }
  for (const auto& r : all_kiwi_results) {
    EmitCsvRow(r);
  }
  std::cout << "CSV_END\n";

  RemoveDirAll(root);
  const double total_bench_s = ElapsedNs(bench_t0, Clock::now()) / 1e9;
  std::cout << "\nCompleted all benchmark runs cleanly in " << std::fixed
            << std::setprecision(2) << total_bench_s << " s.\n";
  return 0;
}

}  // namespace
}  // namespace ROCKSDB_NAMESPACE

int main() { return ROCKSDB_NAMESPACE::RunBenchmark(); }
