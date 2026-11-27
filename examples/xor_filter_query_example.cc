//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "rocksdb/db.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/options.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"

using ROCKSDB_NAMESPACE::BlockBasedTableOptions;
using ROCKSDB_NAMESPACE::BLOOM_FILTER_FULL_POSITIVE;
using ROCKSDB_NAMESPACE::BLOOM_FILTER_USEFUL;
using ROCKSDB_NAMESPACE::DB;
using ROCKSDB_NAMESPACE::FilterPolicy;
using ROCKSDB_NAMESPACE::FlushOptions;
using ROCKSDB_NAMESPACE::NewBlockBasedTableFactory;
using ROCKSDB_NAMESPACE::NewBloomFilterPolicy;
using ROCKSDB_NAMESPACE::NewXorFilterPolicy;
using ROCKSDB_NAMESPACE::Options;
using ROCKSDB_NAMESPACE::ReadOptions;
using ROCKSDB_NAMESPACE::Statistics;
using ROCKSDB_NAMESPACE::Status;
using ROCKSDB_NAMESPACE::WriteOptions;

namespace {

const int kNumKeys = 10000;
const int kQueryRounds = 10;
const int kNumAbsentQueries = 1000000;

std::string MakeKey(const char* prefix, int index) {
  return std::string(prefix) + std::to_string(index);
}

Status QueryKeys(DB* db, const std::vector<std::string>& keys,
                 bool expect_found) {
  std::string value;
  for (const std::string& key : keys) {
    Status status = db->Get(ReadOptions(), key, &value);
    if (expect_found) {
      if (!status.ok()) {
        return status;
      }
      if (value != "value") {
        return Status::Corruption("Unexpected value for a present key");
      }
    } else if (!status.IsNotFound()) {
      if (status.ok()) {
        return Status::Corruption("Unexpectedly found an absent key");
      }
      return status;
    }
  }
  return Status::OK();
}

Status TimeQueries(DB* db, const std::vector<std::string>& keys,
                   bool expect_found, double* elapsed_ms) {
  Status status = QueryKeys(db, keys, expect_found);
  if (!status.ok()) {
    return status;
  }

  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  for (int round = 0; round < kQueryRounds; ++round) {
    status = QueryKeys(db, keys, expect_found);
    if (!status.ok()) {
      return status;
    }
  }
  const std::chrono::steady_clock::time_point finish =
      std::chrono::steady_clock::now();
  *elapsed_ms =
      std::chrono::duration<double, std::milli>(finish - start).count();
  return Status::OK();
}

Status ProfileAbsentQueries(DB* db, Statistics* statistics,
                            const std::vector<std::string>& keys,
                            double* elapsed_ms, uint64_t* false_positives,
                            uint64_t* filter_checks) {
  Status status = QueryKeys(db, keys, false);
  if (!status.ok()) {
    return status;
  }

  const uint64_t useful_before =
      statistics->getTickerCount(BLOOM_FILTER_USEFUL);
  const uint64_t positive_before =
      statistics->getTickerCount(BLOOM_FILTER_FULL_POSITIVE);
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  status = QueryKeys(db, keys, false);
  const std::chrono::steady_clock::time_point finish =
      std::chrono::steady_clock::now();
  if (!status.ok()) {
    return status;
  }
  const uint64_t useful_after = statistics->getTickerCount(BLOOM_FILTER_USEFUL);
  const uint64_t positive_after =
      statistics->getTickerCount(BLOOM_FILTER_FULL_POSITIVE);

  *elapsed_ms =
      std::chrono::duration<double, std::milli>(finish - start).count();
  *false_positives = positive_after - positive_before;
  *filter_checks = *false_positives + useful_after - useful_before;
  return Status::OK();
}

void PrintQueryTiming(const char* query_type, double elapsed_ms,
                      size_t query_count) {
  const double queries_per_second = query_count * 1000.0 / elapsed_ms;
  std::cout << "  " << query_type << ": " << query_count << " queries in "
            << std::fixed << std::setprecision(2) << elapsed_ms << " ms ("
            << std::setprecision(0) << queries_per_second << " queries/s)\n";
}

bool RunPolicy(const std::string& db_path, const char* policy_name,
               const FilterPolicy* filter_policy,
               const std::vector<std::string>& present_keys,
               const std::vector<std::string>& absent_keys,
               uint8_t fingerprint_bytes) {
  Options options;
  options.create_if_missing = true;
  options.statistics = ROCKSDB_NAMESPACE::CreateDBStatistics();
  std::shared_ptr<Statistics> statistics = options.statistics;

  BlockBasedTableOptions table_options;
  table_options.filter_policy.reset(filter_policy);
  options.table_factory.reset(NewBlockBasedTableFactory(table_options));

  std::unique_ptr<DB> db;
  Status status = DB::Open(options, db_path, &db);
  if (!status.ok()) {
    std::cerr << "Unable to open " << policy_name
              << " database: " << status.ToString() << '\n';
    return false;
  }

  for (const std::string& key : present_keys) {
    status = db->Put(WriteOptions(), key, "value");
    if (!status.ok()) {
      std::cerr << "Unable to write test key to " << policy_name
                << " database: " << status.ToString() << '\n';
      return false;
    }
  }

  const std::chrono::steady_clock::time_point flush_start =
      std::chrono::steady_clock::now();
  status = db->Flush(FlushOptions());
  if (!status.ok()) {
    std::cerr << "Unable to flush " << policy_name
              << " database: " << status.ToString() << '\n';
    return false;
  }
  const std::chrono::steady_clock::time_point flush_finish =
      std::chrono::steady_clock::now();
  const double flush_ms =
      std::chrono::duration<double, std::milli>(flush_finish - flush_start)
          .count();

  double present_ms;
  status = TimeQueries(db.get(), present_keys, true, &present_ms);
  if (!status.ok()) {
    std::cerr << "Present-key query failed with " << policy_name
              << " filter: " << status.ToString() << '\n';
    return false;
  }

  double absent_ms;
  uint64_t false_positives;
  uint64_t filter_checks;
  status = ProfileAbsentQueries(db.get(), statistics.get(), absent_keys,
                                &absent_ms, &false_positives, &filter_checks);
  if (!status.ok()) {
    std::cerr << "Absent-key query failed with " << policy_name
              << " filter: " << status.ToString() << '\n';
    return false;
  }
  if (filter_checks == 0) {
    std::cerr << "No full-filter lookups were recorded for " << policy_name
              << '\n';
    return false;
  }

  std::cout << policy_name;
  if (fingerprint_bytes != 0) {
    std::cout << " (" << static_cast<int>(fingerprint_bytes)
              << "-byte fingerprint)";
  }
  std::cout << " filter:\n"
            << "  Flush and filter build: " << std::fixed
            << std::setprecision(2) << flush_ms << " ms\n";
  PrintQueryTiming("present keys", present_ms,
                   present_keys.size() * kQueryRounds);
  PrintQueryTiming("absent keys", absent_ms, absent_keys.size());
  std::cout << "  Measured absent-key filter false-positive rate: "
            << std::setprecision(4) << (100.0 * false_positives / filter_checks)
            << "% (" << false_positives << " positive decisions / "
            << filter_checks << " filter checks)\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0]
              << " <xor-database-path-prefix> <bloom-database-path>"
                 " [fingerprint-bytes ...]\n";
    return 1;
  }

  std::vector<std::string> present_keys;
  std::vector<std::string> absent_keys;
  present_keys.reserve(kNumKeys);
  absent_keys.reserve(kNumAbsentQueries);
  for (int i = 0; i < kNumKeys; ++i) {
    present_keys.push_back(MakeKey("present-", i));
  }
  for (int i = 0; i < kNumAbsentQueries; ++i) {
    absent_keys.push_back(MakeKey("absent-", i));
  }

  std::vector<uint8_t> fingerprint_sizes;
  if (argc == 3) {
    fingerprint_sizes = {1, 2, 3, 4, 8};
  } else {
    for (int i = 3; i < argc; ++i) {
      try {
        const int bytes = std::stoi(argv[i]);
        if (bytes < 1 || bytes > 8 || std::to_string(bytes) != argv[i]) {
          std::cerr << "Fingerprint sizes must be integers from 1 to 8 bytes\n";
          return 1;
        }
        fingerprint_sizes.push_back(static_cast<uint8_t>(bytes));
      } catch (const std::exception&) {
        std::cerr << "Invalid fingerprint size: " << argv[i] << '\n';
        return 1;
      }
    }
  }

  for (uint8_t bytes : fingerprint_sizes) {
    const std::string xor_db_path =
        std::string(argv[1]) + "_" + std::to_string(bytes);
    if (xor_db_path == argv[2]) {
      std::cerr << "XOR and Bloom database paths must be different\n";
      return 1;
    }
    if (!RunPolicy(xor_db_path, "XOR", NewXorFilterPolicy(bytes), present_keys,
                   absent_keys, bytes)) {
      return 1;
    }
  }
  if (!RunPolicy(argv[2], "Bloom", NewBloomFilterPolicy(10), present_keys,
                 absent_keys, 0)) {
    return 1;
  }
  return 0;
}
