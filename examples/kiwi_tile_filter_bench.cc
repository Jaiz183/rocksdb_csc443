//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Benchmark driver for KiWi per-tile XOR+page filters (`tile-filter` branch).
//
// Measures three workload families across pre-XOR baselines and tile-XOR
// configurations:
//   1. Query-heavy workload (present Get, absent Get, 95% Get / 5% Put).
//   2. Flush + secondary page-drop delete workload, comparing:
//        - plain_bloom10 (full compaction rewrite baseline)
//        - kiwi_no_filter (no filters)
//        - kiwi_page_bloom10 (original KiWi per-page Bloom filters)
//        - kiwi_tile_xor1 / xor2 (lazy fp||page_idx floor invalidation)
//        - kiwi_tile_xor1 eager reconstruction (re-reading surviving pages
//          of affected tiles and re-peeling TileXorFilterBuilder)
//   3. Delete-heavy LSM workloads from tombstone_bench:
//        - seqdel (30% sequential point deletes + 70% puts)
//        - churn  (90% delete/reinsert toggling of 20k hot keys + 10% puts)
//
// Every query benchmark reports BOTH:
//   - Memory-access time (512 MB LRUCache warmed via full scan,
//     ReadTier::kBlockCacheTier, asserting PerfContext::block_read_count == 0).
//   - I/O-inclusive time (no_block_cache = true, reporting block_read_count
//     and block_read_byte per query).
//
// Space/memory complexity (filter bytes, bits/key, KiWi directory bytes,
// TableReader pinned memory, and SST bytes) is reported before and after.

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
#include <vector>

#include "file/random_access_file_reader.h"
#include "options/cf_options.h"
#include "rocksdb/cache.h"
#include "rocksdb/convenience.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/metadata.h"
#include "rocksdb/options.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"
#include "rocksdb/table_properties.h"
#include "table/block_based/block.h"
#include "table/block_based/block_based_table_factory.h"
#include "table/block_based/kiwi_tile.h"
#include "table/meta_blocks.h"

namespace ROCKSDB_NAMESPACE {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kValueSize = 100;
constexpr uint64_t kWarmCacheBytes = 512ULL << 20;

double ElapsedMs(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
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

std::string MakeValue(uint64_t delete_key, uint32_t ver) {
  std::string v(kValueSize, 'v');
  for (size_t i = 0; i < 8; ++i) {
    v[i] = static_cast<char>((delete_key >> (8 * (7 - i))) & 0xff);
  }
  char tag[12];
  snprintf(tag, sizeof(tag), "-%08x", ver);
  memcpy(v.data() + 8, tag, 9);
  return v;
}

struct ConfigSpec {
  std::string name;
  uint32_t tile_pages;
  enum class FilterKind { kNone, kBloom10, kXor1, kXor2 } filter_kind;
};

std::shared_ptr<const FilterPolicy> MakeFilterPolicy(
    ConfigSpec::FilterKind kind) {
  switch (kind) {
    case ConfigSpec::FilterKind::kNone:
      return nullptr;
    case ConfigSpec::FilterKind::kBloom10:
      return std::shared_ptr<const FilterPolicy>(NewBloomFilterPolicy(10));
    case ConfigSpec::FilterKind::kXor1:
      return std::shared_ptr<const FilterPolicy>(NewXorFilterPolicy(1));
    case ConfigSpec::FilterKind::kXor2:
      return std::shared_ptr<const FilterPolicy>(NewXorFilterPolicy(2));
  }
  return nullptr;
}

Options MakeBaseOptions(const ConfigSpec& spec, bool enable_cache,
                        const std::shared_ptr<Cache>& cache,
                        const std::shared_ptr<Statistics>& stats,
                        size_t write_buffer_size = 4 << 20,
                        size_t target_file_size = 4 << 20,
                        size_t level_base = 8 << 20,
                        double level_mult = 10.0) {
  Options opts;
  opts.create_if_missing = true;
  opts.compression = kNoCompression;
  opts.write_buffer_size = write_buffer_size;
  opts.target_file_size_base = target_file_size;
  opts.max_bytes_for_level_base = level_base;
  opts.max_bytes_for_level_multiplier = level_mult;
  opts.level0_file_num_compaction_trigger = 4;
  opts.max_background_jobs = 4;
  opts.statistics = stats;

  BlockBasedTableOptions bbto;
  bbto.block_size = 4096;
  bbto.kiwi_tile_pages = spec.tile_pages;
  bbto.kiwi_delete_key_offset = 0;
  bbto.filter_policy = MakeFilterPolicy(spec.filter_kind);
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

SpaceStats MeasureSpace(DB* db, bool is_kiwi) {
  SpaceStats s;
  std::vector<LiveFileMetaData> files;
  db->GetLiveFilesMetaData(&files);
  for (const auto& f : files) {
    s.sst_bytes += f.size;
    ++s.num_files;
  }
  TablePropertiesCollection props;
  Status st = db->GetPropertiesOfAllTables(&props);
  if (st.ok()) {
    for (const auto& kv : props) {
      const auto& p = kv.second;
      s.num_entries += p->num_entries;
      s.index_bytes += p->index_size;
      if (is_kiwi) {
        auto it_f = p->user_collected_properties.find(kiwi::kPropFilterBytes);
        if (it_f != p->user_collected_properties.end()) {
          s.filter_bytes += std::stoull(it_f->second);
        }
        auto it_d =
            p->user_collected_properties.find(kiwi::kPropDirectoryBytes);
        if (it_d != p->user_collected_properties.end()) {
          s.directory_bytes += std::stoull(it_d->second);
        }
      } else {
        s.filter_bytes += p->filter_size;
      }
    }
  }
  uint64_t tr_mem = 0;
  if (db->GetIntProperty("rocksdb.estimate-table-readers-mem", &tr_mem)) {
    s.table_readers_mem = tr_mem;
  }
  return s;
}

struct QueryMetrics {
  double mem_ns_per_op = 0;
  double mem_qps = 0;
  double mem_filter_checks_per_op = 0;
  double mem_fp_rate = 0;
  double mem_cache_hits_per_op = 0;
  double io_us_per_op = 0;
  double io_qps = 0;
  double io_blocks_per_op = 0;
  double io_bytes_per_op = 0;
};

void WarmCacheByScan(DB* db) {
  ReadOptions ro;
  ro.fill_cache = true;
  std::unique_ptr<Iterator> it(db->NewIterator(ro));
  uint64_t count = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    ++count;
  }
  if (!it->status().ok()) {
    fprintf(stderr, "WarmCacheByScan error: %s\n",
            it->status().ToString().c_str());
    std::abort();
  }
  (void)count;
}

template <typename KeyFn>
void RunPointQueries(DB* db, Statistics* stats, size_t count, KeyFn&& key_fn,
                     bool expect_found, bool mem_only, double* ns_per_op,
                     double* qps, double* filter_checks_per_op,
                     double* fp_rate, double* cache_hits_per_op,
                     double* io_blocks_per_op, double* io_bytes_per_op) {
  if (!stats->Reset().ok()) {
    std::abort();
  }
  ReadOptions ro;
  if (mem_only) {
    ro.read_tier = kBlockCacheTier;
  }
  SetPerfLevel(PerfLevel::kEnableCount);
  get_perf_context()->Reset();

  const auto t0 = Clock::now();
  std::string val;
  for (size_t i = 0; i < count; ++i) {
    const std::string key = key_fn(i);
    Status s = db->Get(ro, key, &val);
    if (mem_only && s.IsIncomplete()) {
      fprintf(stderr,
              "ERROR: cache miss under kBlockCacheTier for key %s\n",
              key.c_str());
      std::abort();
    }
    if (expect_found && !s.ok()) {
      fprintf(stderr, "ERROR: expected found for %s: %s\n", key.c_str(),
              s.ToString().c_str());
      std::abort();
    }
    if (!expect_found && !s.IsNotFound()) {
      fprintf(stderr, "ERROR: expected NotFound for %s: %s\n", key.c_str(),
              s.ToString().c_str());
      std::abort();
    }
  }
  const auto t1 = Clock::now();

  if (mem_only && get_perf_context()->block_read_count != 0) {
    fprintf(stderr,
            "ERROR: expected 0 disk block reads in mem-only query pass, got "
            "%" PRIu64 "\n",
            get_perf_context()->block_read_count);
    std::abort();
  }

  const double total_ns = ElapsedNs(t0, t1);
  *ns_per_op = total_ns / static_cast<double>(count);
  *qps = static_cast<double>(count) * 1e9 / total_ns;
  const uint64_t useful = stats->getTickerCount(BLOOM_FILTER_USEFUL);
  const uint64_t pos = stats->getTickerCount(BLOOM_FILTER_FULL_POSITIVE);
  const uint64_t checks = useful + pos;
  *filter_checks_per_op = static_cast<double>(checks) / count;
  *fp_rate = checks == 0 ? 0.0 : static_cast<double>(pos) / checks;
  *cache_hits_per_op =
      static_cast<double>(get_perf_context()->block_cache_hit_count) / count;
  *io_blocks_per_op =
      static_cast<double>(get_perf_context()->block_read_count) / count;
  *io_bytes_per_op =
      static_cast<double>(get_perf_context()->block_read_byte) / count;
  SetPerfLevel(PerfLevel::kDisable);
}

QueryMetrics BenchmarkQueries(const ConfigSpec& spec, const std::string& db_dir,
                              const std::vector<std::string>& keys,
                              bool expect_found, size_t query_count,
                              size_t write_buf_sz, size_t target_sz,
                              size_t level_base, double level_mult) {
  QueryMetrics m;
  {
    auto cache = NewLRUCache(kWarmCacheBytes);
    auto stats = CreateDBStatistics();
    Options opts = MakeBaseOptions(spec, /*enable_cache=*/true, cache, stats,
                                   write_buf_sz, target_sz, level_base,
                                   level_mult);
    std::unique_ptr<DB> db;
    Status s = DB::Open(opts, db_dir, &db);
    if (!s.ok()) {
      fprintf(stderr, "Open warm failed: %s\n", s.ToString().c_str());
      std::abort();
    }
    WarmCacheByScan(db.get());
    RunPointQueries(
        db.get(), stats.get(), query_count,
        [&](size_t i) { return keys[i % keys.size()]; }, expect_found,
        /*mem_only=*/true, &m.mem_ns_per_op, &m.mem_qps,
        &m.mem_filter_checks_per_op, &m.mem_fp_rate, &m.mem_cache_hits_per_op,
        &m.io_blocks_per_op, &m.io_bytes_per_op);
  }
  {
    auto stats = CreateDBStatistics();
    Options opts = MakeBaseOptions(spec, /*enable_cache=*/false, nullptr, stats,
                                   write_buf_sz, target_sz, level_base,
                                   level_mult);
    std::unique_ptr<DB> db;
    Status s = DB::Open(opts, db_dir, &db);
    if (!s.ok()) {
      fprintf(stderr, "Open uncached failed: %s\n", s.ToString().c_str());
      std::abort();
    }
    double unused_ns = 0, unused_checks = 0, unused_fp = 0, unused_hits = 0;
    const size_t io_queries = std::min<size_t>(query_count, 100000);
    RunPointQueries(
        db.get(), stats.get(), io_queries,
        [&](size_t i) { return keys[i % keys.size()]; }, expect_found,
        /*mem_only=*/false, &unused_ns, &m.io_qps, &unused_checks, &unused_fp,
        &unused_hits, &m.io_blocks_per_op, &m.io_bytes_per_op);
    m.io_us_per_op = unused_ns / 1000.0;
  }
  return m;
}

// Simulates eager tile-XOR filter reconstruction on page drop ("before
// avoiding reconstruction"): for every tile with at least one page whose
// max_delete_key < floor, reads all surviving pages from disk, extracts
// surviving user keys, re-peels TileXorFilterBuilder, and re-encodes the
// Directory.
struct EagerReconstructStats {
  double total_ms = 0;
  double page_read_ms = 0;
  double xor_peel_ms = 0;
  uint64_t total_tiles = 0;
  uint64_t tiles_touched = 0;
  uint64_t pages_dropped = 0;
  uint64_t pages_read = 0;
  uint64_t rebuilt_filter_bytes = 0;
  uint64_t rebuilt_dir_bytes = 0;
};

EagerReconstructStats SimulateEagerTileXorReconstruction(
    const std::string& db_dir, uint64_t floor, uint8_t fp_bytes,
    uint32_t delete_key_offset) {
  EagerReconstructStats res;
  const auto total_t0 = Clock::now();
  Env* env = Env::Default();
  FileSystem* fs = env->GetFileSystem().get();
  std::vector<std::string> entries;
  Status s = env->GetChildren(db_dir, &entries);
  if (!s.ok()) {
    fprintf(stderr, "GetChildren failed: %s\n", s.ToString().c_str());
    std::abort();
  }
  Options tmp_opts;
  ImmutableOptions iopts(tmp_opts);
  const ReadOptions ro;

  for (const std::string& fname : entries) {
    if (fname.size() < 4 || fname.substr(fname.size() - 4) != ".sst") {
      continue;
    }
    const std::string path = db_dir + "/" + fname;
    uint64_t fsize = 0;
    if (!env->GetFileSize(path, &fsize).ok()) {
      continue;
    }
    std::unique_ptr<FSRandomAccessFile> raf;
    if (!fs->NewRandomAccessFile(path, FileOptions(), &raf, nullptr).ok()) {
      std::abort();
    }
    RandomAccessFileReader reader(std::move(raf), path);
    BlockHandle dir_handle;
    if (!FindMetaBlockInFile(&reader, fsize, kBlockBasedTableMagicNumber,
                             iopts, ro, kiwi::kDirectoryBlockName, &dir_handle)
             .ok()) {
      continue;
    }
    std::string raw_dir(dir_handle.size(), '\0');
    Slice dir_slice;
    IOStatus ios = reader.Read(dir_handle.offset(), dir_handle.size(),
                               IOOptions(), &dir_slice, raw_dir.data(),
                               nullptr);
    if (!ios.ok()) {
      std::abort();
    }
    raw_dir.assign(dir_slice.data(), dir_slice.size());
    kiwi::Directory dir;
    if (!dir.Decode(std::move(raw_dir)).ok()) {
      std::abort();
    }

    kiwi::DirectoryBuilder dir_builder;
    kiwi::TileXorFilterBuilder xor_builder(fp_bytes);
    res.total_tiles += dir.num_tiles();
    for (size_t t = 0; t < dir.num_tiles(); ++t) {
      const kiwi::Directory::TileView tv = dir.tile(t);
      bool any_dropped = false;
      bool any_surviving = false;
      for (size_t p = 0; p < tv.num_pages; ++p) {
        if (tv.pages[p].max_delete_key < floor) {
          any_dropped = true;
          ++res.pages_dropped;
        } else {
          any_surviving = true;
        }
      }
      if (!any_surviving) {
        continue;
      }
      ++res.tiles_touched;
      if (!any_dropped) {
        // Still must re-read & rebuild if an edge page shrunk, or copy if
        // untouched; check whether any page straddles floor.
        bool has_edge = false;
        for (size_t p = 0; p < tv.num_pages; ++p) {
          if (tv.pages[p].min_delete_key < floor) {
            has_edge = true;
            break;
          }
        }
        if (!has_edge) {
          // Even when untouched, copy existing filter slice? In our
          // DirectoryBuilder API we re-encode from surviving keys only when
          // pages dropped/straddled; for untouched tiles keep original size.
          // Here simulate full surviving tile rebuild only for affected tiles.
          std::vector<kiwi::DirectoryPage> kept_pages(
              tv.pages, tv.pages + tv.num_pages);
          // Read pages if rebuilding whole dir or reuse: read pages to count
          // untouched tiles cleanly as zero-cost copy by building a 1-key
          // placeholder only? No, let's preserve exact size by rebuilding from
          // original filter if untouched: skip re-read for untouched tiles.
          (void)kept_pages;
          continue;
        }
      }

      std::vector<kiwi::DirectoryPage> new_pages;
      uint32_t new_page_idx = 0;
      for (size_t p = 0; p < tv.num_pages; ++p) {
        if (tv.pages[p].max_delete_key < floor) {
          continue;
        }
        const BlockHandle& h = tv.pages[p].handle;
        const auto r0 = Clock::now();
        std::unique_ptr<char[]> buf(new char[h.size()]);
        Slice blk_slice;
        IOStatus rs = reader.Read(h.offset(), h.size(), IOOptions(), &blk_slice,
                                  buf.get(), nullptr);
        const auto r1 = Clock::now();
        res.page_read_ms += ElapsedMs(r0, r1);
        ++res.pages_read;
        if (!rs.ok()) {
          std::abort();
        }
        if (blk_slice.data() != buf.get()) {
          memcpy(buf.get(), blk_slice.data(), h.size());
        }
        BlockContents bc(std::move(buf), h.size());
        Block block(std::move(bc));
        std::unique_ptr<DataBlockIter> it(block.NewDataIterator(
            BytewiseComparator(), kDisableGlobalSequenceNumber));
        const auto p0 = Clock::now();
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
          uint64_t d = kiwi::kNoDeleteKey;
          if (!kiwi::ExtractDeleteKey(ExtractValueType(it->key()), it->value(),
                                      delete_key_offset, &d)
                   .ok()) {
            std::abort();
          }
          if (d >= floor) {
            xor_builder.AddKey(ExtractUserKey(it->key()), new_page_idx);
          }
        }
        const auto p1 = Clock::now();
        res.xor_peel_ms += ElapsedMs(p0, p1);
        new_pages.push_back(tv.pages[p]);
        ++new_page_idx;
      }
      if (!xor_builder.empty()) {
        const auto p0 = Clock::now();
        std::string new_filter;
        if (!xor_builder.Finish(new_pages.size(), &new_filter).ok()) {
          std::abort();
        }
        dir_builder.AddTileWithXorFilter(new_pages, new_filter);
        const auto p1 = Clock::now();
        res.xor_peel_ms += ElapsedMs(p0, p1);
      }
    }
    const auto f0 = Clock::now();
    std::string rebuilt_dir;
    dir_builder.Finish(&rebuilt_dir);
    const auto f1 = Clock::now();
    res.xor_peel_ms += ElapsedMs(f0, f1);
    res.rebuilt_filter_bytes += dir_builder.filter_bytes();
    res.rebuilt_dir_bytes += rebuilt_dir.size();
  }
  res.total_ms = ElapsedMs(total_t0, Clock::now());
  return res;
}

void PrintDivider(const char* title) {
  std::cout << "\n============================================================"
            << "====================\n"
            << title
            << "\n============================================================"
            << "====================\n";
}

void RunSuite1QueryHeavy(const std::string& root) {
  PrintDivider("SUITE 1: QUERY-HEAVY WORKLOAD (200k unique keys, 4KB blocks)");
  const size_t kNumKeys = 200000;
  const size_t kNumQueries = 300000;
  std::vector<std::string> present_keys;
  std::vector<std::string> absent_keys;
  present_keys.reserve(kNumKeys);
  absent_keys.reserve(kNumQueries);
  std::mt19937_64 rng(42);
  std::vector<size_t> order(kNumKeys);
  for (size_t i = 0; i < kNumKeys; ++i) {
    order[i] = i;
    present_keys.push_back(MakeKey(i));
  }
  std::shuffle(order.begin(), order.end(), rng);
  for (size_t i = 0; i < kNumQueries; ++i) {
    absent_keys.push_back(MakeAbsentKey(rng() % (kNumKeys - 1)));
  }

  for (uint32_t h : {4U, 8U, 16U}) {
    std::cout << "\n--- Tile size h = " << h << " ---\n";
    std::cout << std::left << std::setw(20) << "config" << std::right
              << std::setw(10) << "flush_ms" << std::setw(10) << "mem_pres"
              << std::setw(10) << "mem_abs" << std::setw(10) << "mem_mix"
              << std::setw(10) << "io_pres" << std::setw(10) << "io_abs"
              << std::setw(10) << "abs_fp%" << std::setw(9) << "f_chk"
              << std::setw(9) << "blk_hit" << std::setw(9) << "io_blk"
              << std::setw(8) << "flt_bpk" << std::setw(8) << "dir_bpk"
              << std::setw(10) << "tr_KB" << std::setw(10) << "sst_MB\n";

    std::vector<ConfigSpec> configs;
    if (h == 4) {
      configs.push_back({"plain_bloom10", 0, ConfigSpec::FilterKind::kBloom10});
    }
    configs.push_back({"kiwi_no_filter", h, ConfigSpec::FilterKind::kNone});
    configs.push_back(
        {"kiwi_page_bloom10", h, ConfigSpec::FilterKind::kBloom10});
    configs.push_back({"kiwi_tile_xor1", h, ConfigSpec::FilterKind::kXor1});
    configs.push_back({"kiwi_tile_xor2", h, ConfigSpec::FilterKind::kXor2});

    for (const auto& cfg : configs) {
      const std::string db_dir =
          root + "/s1_" + cfg.name + "_h" + std::to_string(h);
      std::filesystem::remove_all(db_dir);
      auto cache = NewLRUCache(kWarmCacheBytes);
      auto stats = CreateDBStatistics();
      Options opts = MakeBaseOptions(cfg, /*enable_cache=*/true, cache, stats,
                                     /*write_buf=*/64 << 20,
                                     /*target_sz=*/64 << 20,
                                     /*level_base=*/64 << 20, 10.0);
      std::unique_ptr<DB> db;
      if (!DB::Open(opts, db_dir, &db).ok()) {
        std::abort();
      }
      WriteOptions wo;
      wo.disableWAL = true;
      for (size_t pos = 0; pos < kNumKeys; ++pos) {
        const size_t idx = order[pos];
        if (!db->Put(wo, present_keys[idx], MakeValue(pos + 1, 1)).ok()) {
          std::abort();
        }
      }
      const auto f0 = Clock::now();
      if (!db->Flush(FlushOptions()).ok()) {
        std::abort();
      }
      const double flush_ms = ElapsedMs(f0, Clock::now());
      if (!db->WaitForCompact(WaitForCompactOptions()).ok()) {
        std::abort();
      }
      SpaceStats sp = MeasureSpace(db.get(), cfg.tile_pages >= 2);

      // Shuffle query probe order so cache/CPU accesses are non-sequential.
      std::vector<std::string> q_present = present_keys;
      std::shuffle(q_present.begin(), q_present.end(), rng);
      db.reset();

      QueryMetrics mp = BenchmarkQueries(cfg, db_dir, q_present, true,
                                         kNumQueries, 64 << 20, 64 << 20,
                                         64 << 20, 10.0);
      QueryMetrics ma = BenchmarkQueries(cfg, db_dir, absent_keys, false,
                                         kNumQueries, 64 << 20, 64 << 20,
                                         64 << 20, 10.0);

      // 95% Get / 5% Put read-heavy mix (warm cache).
      double mix_ns = 0;
      {
        auto mix_cache = NewLRUCache(kWarmCacheBytes);
        auto mix_stats = CreateDBStatistics();
        Options mopts = MakeBaseOptions(cfg, true, mix_cache, mix_stats,
                                        64 << 20, 64 << 20, 64 << 20, 10.0);
        std::unique_ptr<DB> mdb;
        if (!DB::Open(mopts, db_dir, &mdb).ok()) {
          std::abort();
        }
        WarmCacheByScan(mdb.get());
        ReadOptions ro;
        ro.read_tier = kBlockCacheTier;
        SetPerfLevel(PerfLevel::kEnableCount);
        get_perf_context()->Reset();
        const auto t0 = Clock::now();
        std::string val;
        for (size_t i = 0; i < 200000; ++i) {
          if (i % 20 == 0) {
            const size_t k = i % kNumKeys;
            if (!mdb->Put(wo, present_keys[k],
                          MakeValue(kNumKeys + i + 1, 2))
                     .ok()) {
              std::abort();
            }
          } else if (i % 2 == 0) {
            if (!mdb->Get(ro, q_present[i % q_present.size()], &val).ok()) {
              std::abort();
            }
          } else {
            if (!mdb->Get(ro, absent_keys[i % absent_keys.size()], &val)
                     .IsNotFound()) {
              std::abort();
            }
          }
        }
        mix_ns = ElapsedNs(t0, Clock::now()) / 200000.0;
        if (get_perf_context()->block_read_count != 0) {
          fprintf(stderr, "Mix pass had unexpected I/O!\n");
          std::abort();
        }
        SetPerfLevel(PerfLevel::kDisable);
      }

      std::cout << std::left << std::setw(20) << cfg.name << std::right
                << std::fixed << std::setprecision(1) << std::setw(10)
                << flush_ms << std::setw(10) << mp.mem_ns_per_op << std::setw(10)
                << ma.mem_ns_per_op << std::setw(10) << mix_ns << std::setw(10)
                << std::setprecision(2) << mp.io_us_per_op << std::setw(10)
                << ma.io_us_per_op << std::setw(10) << std::setprecision(3)
                << (100.0 * ma.mem_fp_rate) << std::setw(9)
                << std::setprecision(2) << ma.mem_filter_checks_per_op
                << std::setw(9) << ma.mem_cache_hits_per_op << std::setw(9)
                << ma.io_blocks_per_op << std::setw(8) << sp.filter_bpk()
                << std::setw(8) << sp.dir_bpk() << std::setw(10)
                << (sp.table_readers_mem / 1024.0) << std::setw(10)
                << (sp.sst_bytes / (1024.0 * 1024.0)) << '\n';
      std::filesystem::remove_all(db_dir);
    }
  }
}

void RunSuite2SecondaryDeleteAndReconstruction(const std::string& root) {
  PrintDivider(
      "SUITE 2: FLUSH + SECONDARY DELETE & FILTER RECONSTRUCTION (h=8, 200k)");
  const size_t kNumKeys = 200000;
  const uint32_t h = 8;
  std::vector<std::string> keys;
  keys.reserve(kNumKeys);
  std::vector<size_t> order(kNumKeys);
  for (size_t i = 0; i < kNumKeys; ++i) {
    keys.push_back(MakeKey(i));
    order[i] = i;
  }
  std::mt19937_64 rng(777);
  std::shuffle(order.begin(), order.end(), rng);

  const std::vector<ConfigSpec> configs = {
      {"plain_bloom10", 0, ConfigSpec::FilterKind::kBloom10},
      {"kiwi_no_filter", h, ConfigSpec::FilterKind::kNone},
      {"kiwi_page_bloom10", h, ConfigSpec::FilterKind::kBloom10},
      {"kiwi_tile_xor1", h, ConfigSpec::FilterKind::kXor1},
      {"kiwi_tile_xor2", h, ConfigSpec::FilterKind::kXor2},
  };

  for (const auto& cfg : configs) {
    std::cout << "\n--- Config: " << cfg.name << " ---\n";
    for (double frac : {0.10, 0.25, 0.50, 0.75}) {
      const std::string db_dir =
          root + "/s2_" + cfg.name + "_" + std::to_string(int(frac * 100));
      std::filesystem::remove_all(db_dir);
      auto cache = NewLRUCache(kWarmCacheBytes);
      auto stats = CreateDBStatistics();
      Options opts = MakeBaseOptions(cfg, true, cache, stats, 64 << 20,
                                     64 << 20, 64 << 20, 10.0);
      std::unique_ptr<DB> db;
      if (!DB::Open(opts, db_dir, &db).ok()) {
        std::abort();
      }
      WriteOptions wo;
      wo.disableWAL = true;
      for (size_t pos = 0; pos < kNumKeys; ++pos) {
        const size_t idx = order[pos];
        if (!db->Put(wo, keys[idx], MakeValue(pos + 1, 1)).ok()) {
          std::abort();
        }
      }
      if (!db->Flush(FlushOptions()).ok() ||
          !db->WaitForCompact(WaitForCompactOptions()).ok()) {
        std::abort();
      }
      const SpaceStats sp_before = MeasureSpace(db.get(), cfg.tile_pages >= 2);

      const uint64_t floor =
          1 + static_cast<uint64_t>(frac * static_cast<double>(kNumKeys));
      EagerReconstructStats eager;
      if (cfg.filter_kind == ConfigSpec::FilterKind::kXor1 ||
          cfg.filter_kind == ConfigSpec::FilterKind::kXor2) {
        eager = SimulateEagerTileXorReconstruction(
            db_dir, floor,
            cfg.filter_kind == ConfigSpec::FilterKind::kXor1 ? 1 : 2, 0);
      }

      double delete_ms = 0;
      if (cfg.tile_pages == 0) {
        // Stock RocksDB without KiWi must run Delete + full compaction
        // (measure Delete of deleted keys + Flush + CompactRange(kForce)).
        const auto d0 = Clock::now();
        for (size_t pos = 0; pos + 1 < floor; ++pos) {
          if (!db->Delete(wo, keys[order[pos]]).ok()) {
            std::abort();
          }
        }
        if (!db->Flush(FlushOptions()).ok()) {
          std::abort();
        }
        CompactRangeOptions cro;
        cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
        if (!db->CompactRange(cro, nullptr, nullptr).ok()) {
          std::abort();
        }
        delete_ms = ElapsedMs(d0, Clock::now());
      } else {
        const auto d0 = Clock::now();
        Status ds = db->SecondaryDeleteBelow(floor);
        delete_ms = ElapsedMs(d0, Clock::now());
        if (!ds.ok()) {
          fprintf(stderr, "SecondaryDeleteBelow failed: %s\n",
                  ds.ToString().c_str());
          std::abort();
        }
      }
      const SpaceStats sp_after_del =
          MeasureSpace(db.get(), cfg.tile_pages >= 2);

      // Measure post-delete query performance on deleted vs surviving keys.
      std::vector<std::string> del_keys;
      std::vector<std::string> surv_keys;
      for (size_t pos = 0; pos < kNumKeys; ++pos) {
        if (pos + 1 < floor) {
          del_keys.push_back(keys[order[pos]]);
        } else {
          surv_keys.push_back(keys[order[pos]]);
        }
      }
      std::shuffle(del_keys.begin(), del_keys.end(), rng);
      std::shuffle(surv_keys.begin(), surv_keys.end(), rng);
      db.reset();

      const QueryMetrics q_del = BenchmarkQueries(cfg, db_dir, del_keys, false,
                                                  100000, 64 << 20, 64 << 20,
                                                  64 << 20, 10.0);
      const QueryMetrics q_surv = BenchmarkQueries(cfg, db_dir, surv_keys, true,
                                                   100000, 64 << 20, 64 << 20,
                                                   64 << 20, 10.0);

      // Reclaim physically with forced compaction and measure final space.
      double compact_ms = 0;
      SpaceStats sp_after_compact = sp_after_del;
      if (cfg.tile_pages >= 2) {
        auto cstats = CreateDBStatistics();
        Options copts = MakeBaseOptions(cfg, false, nullptr, cstats, 64 << 20,
                                        64 << 20, 64 << 20, 10.0);
        std::unique_ptr<DB> cdb;
        if (!DB::Open(copts, db_dir, &cdb).ok()) {
          std::abort();
        }
        CompactRangeOptions cro;
        cro.bottommost_level_compaction = BottommostLevelCompaction::kForce;
        const auto c0 = Clock::now();
        if (!cdb->CompactRange(cro, nullptr, nullptr).ok()) {
          std::abort();
        }
        compact_ms = ElapsedMs(c0, Clock::now());
        sp_after_compact = MeasureSpace(cdb.get(), true);
      }

      std::cout << "  drop=" << std::setw(2) << int(frac * 100) << "%"
                << std::fixed << std::setprecision(2)
                << " | lazy_del=" << delete_ms << "ms"
                << " | eager_recon=" << eager.total_ms << "ms"
                << " (io=" << eager.page_read_ms
                << "ms, peel=" << eager.xor_peel_ms << "ms)"
                << " | compact=" << compact_ms << "ms"
                << " | delQ(mem=" << std::setprecision(1) << q_del.mem_ns_per_op
                << "ns, io=" << std::setprecision(2) << q_del.io_us_per_op
                << "us, blk=" << q_del.io_blocks_per_op << ")"
                << " | survQ(mem=" << std::setprecision(1)
                << q_surv.mem_ns_per_op << "ns, io=" << std::setprecision(2)
                << q_surv.io_us_per_op << "us)"
                << " | flt(before/lazy/eager/comp)="
                << (sp_before.filter_bytes / 1024.0) << "/"
                << (sp_after_del.filter_bytes / 1024.0) << "/"
                << (eager.rebuilt_filter_bytes / 1024.0) << "/"
                << (sp_after_compact.filter_bytes / 1024.0) << "KB"
                << " | sst(before/lazy/comp)="
                << (sp_before.sst_bytes / (1024.0 * 1024.0)) << "/"
                << (sp_after_del.sst_bytes / (1024.0 * 1024.0)) << "/"
                << (sp_after_compact.sst_bytes / (1024.0 * 1024.0)) << "MB"
                << " | tr_mem=" << (sp_before.table_readers_mem / 1024.0) << "->"
                << (sp_after_compact.table_readers_mem / 1024.0) << "KB\n";

      std::filesystem::remove_all(db_dir);
    }
  }
}

void RunSuite3TombstoneBenchWorkloads(const std::string& root) {
  PrintDivider(
      "SUITE 3: DELETE-HEAVY LSM WORKLOADS (seqdel & churn, h=8, 1MB L1)");
  const uint64_t kKeys = 200000;
  const uint64_t kOps = 200000;
  const uint32_t h = 8;
  const size_t kWriteBuf = 1 << 20;
  const size_t kTargetSz = 1 << 20;
  const size_t kLevelBase = 1 << 20;
  const double kMult = 5.0;

  const std::vector<ConfigSpec> configs = {
      {"plain_bloom10", 0, ConfigSpec::FilterKind::kBloom10},
      {"kiwi_no_filter", h, ConfigSpec::FilterKind::kNone},
      {"kiwi_page_bloom10", h, ConfigSpec::FilterKind::kBloom10},
      {"kiwi_tile_xor1", h, ConfigSpec::FilterKind::kXor1},
      {"kiwi_tile_xor2", h, ConfigSpec::FilterKind::kXor2},
  };

  for (const char* wname : {"seqdel", "churn"}) {
    const bool is_churn = (std::string(wname) == "churn");
    std::cout << "\n--- Workload: " << wname << " ---\n";
    std::cout << std::left << std::setw(18) << "config" << std::right
              << std::setw(10) << "ops/s" << std::setw(9) << "del_us"
              << std::setw(8) << "w_amp" << std::setw(10) << "mem_pres"
              << std::setw(10) << "mem_abs" << std::setw(10) << "io_pres"
              << std::setw(10) << "io_abs" << std::setw(9) << "io_blk"
              << std::setw(14) << "flt(B->A KB)" << std::setw(14)
              << "tr(B->A KB)" << std::setw(14) << "sst(B->A MB)\n";

    for (const auto& cfg : configs) {
      const std::string db_dir = root + "/s3_" + wname + "_" + cfg.name;
      std::filesystem::remove_all(db_dir);
      auto cache = NewLRUCache(kWarmCacheBytes);
      auto stats = CreateDBStatistics();
      Options opts = MakeBaseOptions(cfg, true, cache, stats, kWriteBuf,
                                     kTargetSz, kLevelBase, kMult);
      std::unique_ptr<DB> db;
      if (!DB::Open(opts, db_dir, &db).ok()) {
        std::abort();
      }
      WriteOptions wo;
      wo.disableWAL = true;
      std::mt19937_64 rng(12345);
      std::vector<uint32_t> model(kKeys, 0);
      std::vector<std::string> keys(kKeys);
      for (uint64_t i = 0; i < kKeys; ++i) {
        keys[i] = MakeKey(i);
      }

      uint64_t dclock = 1;
      for (uint64_t i = 0; i < kKeys; ++i) {
        const uint64_t k = rng() % kKeys;
        model[k] = 1;
        if (!db->Put(wo, keys[k], MakeValue(dclock++, 1)).ok()) {
          std::abort();
        }
      }
      if (!db->Flush(FlushOptions()).ok() ||
          !db->WaitForCompact(WaitForCompactOptions()).ok()) {
        std::abort();
      }
      const SpaceStats sp_before =
          MeasureSpace(db.get(), cfg.tile_pages >= 2);
      const uint64_t w0 = stats->getTickerCount(FLUSH_WRITE_BYTES) +
                          stats->getTickerCount(COMPACT_WRITE_BYTES);

      uint64_t user_bytes = 0;
      uint64_t seq_cursor = 0;
      uint64_t dels = 0;
      const uint64_t kHot = 20000;
      const uint64_t kHotStride = kKeys / kHot;
      std::uniform_real_distribution<double> U(0.0, 1.0);

      const auto t0 = Clock::now();
      for (uint64_t op = 0; op < kOps; ++op) {
        const double r = U(rng);
        if (is_churn) {
          uint64_t k = 0;
          bool do_del = false;
          if (r < 0.90) {
            k = (rng() % kHot) * kHotStride;
            do_del = (model[k] != 0);
          } else {
            k = rng() % kKeys;
          }
          if (do_del) {
            if (!db->Delete(wo, keys[k]).ok()) {
              std::abort();
            }
            model[k] = 0;
            user_bytes += keys[k].size();
            ++dels;
          } else {
            model[k] = 2;
            if (!db->Put(wo, keys[k], MakeValue(dclock++, 2)).ok()) {
              std::abort();
            }
            user_bytes += keys[k].size() + kValueSize;
          }
        } else {
          if (r < 0.30) {
            const uint64_t k = seq_cursor;
            seq_cursor = (seq_cursor + 1) % kKeys;
            if (!db->Delete(wo, keys[k]).ok()) {
              std::abort();
            }
            model[k] = 0;
            user_bytes += keys[k].size();
            ++dels;
          } else {
            const uint64_t k = rng() % kKeys;
            model[k] = 2;
            if (!db->Put(wo, keys[k], MakeValue(dclock++, 2)).ok()) {
              std::abort();
            }
            user_bytes += keys[k].size() + kValueSize;
          }
        }
      }
      if (!db->Flush(FlushOptions()).ok() ||
          !db->WaitForCompact(WaitForCompactOptions()).ok()) {
        std::abort();
      }
      const double elapsed_s = ElapsedMs(t0, Clock::now()) / 1000.0;
      const uint64_t w1 = stats->getTickerCount(FLUSH_WRITE_BYTES) +
                          stats->getTickerCount(COMPACT_WRITE_BYTES);
      const double write_amp =
          user_bytes == 0 ? 0.0 : static_cast<double>(w1 - w0) / user_bytes;
      const SpaceStats sp_after = MeasureSpace(db.get(), cfg.tile_pages >= 2);

      // Verify full scan against ground-truth model and collect present/absent
      // keys for query benchmarks.
      std::vector<std::string> present_keys;
      std::vector<std::string> deleted_keys;
      for (uint64_t k = 0; k < kKeys; ++k) {
        if (model[k] != 0) {
          present_keys.push_back(keys[k]);
        } else {
          deleted_keys.push_back(keys[k]);
        }
      }
      {
        std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions()));
        size_t idx = 0;
        for (it->SeekToFirst(); it->Valid(); it->Next(), ++idx) {
          if (idx >= present_keys.size() ||
              it->key().ToString() != present_keys[idx]) {
            fprintf(stderr, "Model mismatch on %s (%s)!\n", wname,
                    cfg.name.c_str());
            std::abort();
          }
        }
        if (!it->status().ok() || idx != present_keys.size()) {
          fprintf(stderr, "Model count mismatch on %s (%s): %zu vs %zu\n",
                  wname, cfg.name.c_str(), idx, present_keys.size());
          std::abort();
        }
      }
      std::shuffle(present_keys.begin(), present_keys.end(), rng);
      std::shuffle(deleted_keys.begin(), deleted_keys.end(), rng);
      db.reset();

      const QueryMetrics mp = BenchmarkQueries(
          cfg, db_dir, present_keys, true, 100000, kWriteBuf, kTargetSz,
          kLevelBase, kMult);
      const QueryMetrics ma = BenchmarkQueries(
          cfg, db_dir, deleted_keys, false, 100000, kWriteBuf, kTargetSz,
          kLevelBase, kMult);

      char flt_buf[32], tr_buf[32], sst_buf[32];
      snprintf(flt_buf, sizeof(flt_buf), "%.0f->%.0f",
               sp_before.filter_bytes / 1024.0,
               sp_after.filter_bytes / 1024.0);
      snprintf(tr_buf, sizeof(tr_buf), "%.0f->%.0f",
               sp_before.table_readers_mem / 1024.0,
               sp_after.table_readers_mem / 1024.0);
      snprintf(sst_buf, sizeof(sst_buf), "%.1f->%.1f",
               sp_before.sst_bytes / (1024.0 * 1024.0),
               sp_after.sst_bytes / (1024.0 * 1024.0));

      std::cout << std::left << std::setw(18) << cfg.name << std::right
                << std::fixed << std::setprecision(0) << std::setw(10)
                << (kOps / elapsed_s) << std::setw(9) << std::setprecision(2)
                << (elapsed_s * 1e6 / kOps) << std::setw(8) << write_amp
                << std::setw(10) << std::setprecision(1) << mp.mem_ns_per_op
                << std::setw(10) << ma.mem_ns_per_op << std::setw(10)
                << std::setprecision(2) << mp.io_us_per_op << std::setw(10)
                << ma.io_us_per_op << std::setw(9) << ma.io_blocks_per_op
                << std::setw(14) << flt_buf << std::setw(14) << tr_buf
                << std::setw(14) << sst_buf << '\n';
      std::filesystem::remove_all(db_dir);
    }
  }
}

}  // namespace
}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  std::string root = "/tmp/kiwi_tile_filter_bench_db";
  if (argc > 1) {
    root = argv[1];
  }
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  ROCKSDB_NAMESPACE::RunSuite1QueryHeavy(root);
  ROCKSDB_NAMESPACE::RunSuite2SecondaryDeleteAndReconstruction(root);
  ROCKSDB_NAMESPACE::RunSuite3TombstoneBenchWorkloads(root);

  std::filesystem::remove_all(root);
  return 0;
}
