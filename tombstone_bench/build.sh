#!/bin/bash
# Build tombstone_bench against a Release static library of this checkout.
# Usage: BUILD=/path/to/build-rel tombstone_bench/build.sh
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$R/../../builds/skip-rel}"
if [ ! -f "$BUILD/librocksdb.a" ]; then
  cmake -S "$R" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DWITH_TESTS=OFF \
    -DWITH_BENCHMARK_TOOLS=OFF -DWITH_TOOLS=OFF -DWITH_CORE_TOOLS=OFF \
    -DWITH_GFLAGS=OFF -DWITH_SNAPPY=ON -DWITH_LZ4=ON -DWITH_ZSTD=ON \
    -DFAIL_ON_WARNINGS=OFF
  cmake --build "$BUILD" --target rocksdb -j"${JOBS:-5}"
fi
clang++ -std=c++20 -O2 -DNDEBUG -fno-rtti -DHAVE_FULLFSYNC -DLZ4 -DOS_MACOSX \
  -DROCKSDB_LIB_IO_POSIX -DROCKSDB_NO_DYNAMIC_EXTENSION -DROCKSDB_PLATFORM_POSIX \
  -DSNAPPY -DZSTD -I"$R/include" -I/opt/homebrew/include \
  "$R/tombstone_bench/tombstone_bench.cc" "$BUILD/librocksdb.a" -L/opt/homebrew/lib \
  -lsnappy -llz4 -lzstd -lz -lpthread -o "$BUILD/tombstone_bench"
echo "built $BUILD/tombstone_bench"
