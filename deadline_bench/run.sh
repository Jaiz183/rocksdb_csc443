#!/bin/bash
# Build deadline_bench against a Release static lib of this branch and run
# the comparison matrix (the baseline is this branch with the feature off).
#
# Usage: REL=/path/to/build-rel OUT=/path/to/results ./run.sh
# Env: RUN_S (default 90), OPS (default 15000), REPEATS (default 2),
#      DEADLINE (default 30), DB_DIR (default /tmp/deadline_bench_db)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
R="$(cd "$HERE/.." && pwd)"
REL="${REL:?set REL to a Release build dir of this branch}"
OUT="${OUT:-$HERE/results}"
RUN_S="${RUN_S:-90}"; OPS="${OPS:-15000}"; REPEATS="${REPEATS:-2}"
D="${DEADLINE:-30}"; DB_DIR="${DB_DIR:-/tmp/deadline_bench_db}"
mkdir -p "$OUT"
if [ ! -f "$REL/librocksdb.a" ]; then
  cmake -S "$R" -B "$REL" -DCMAKE_BUILD_TYPE=Release -DWITH_TESTS=OFF \
    -DWITH_BENCHMARK_TOOLS=OFF -DWITH_TOOLS=OFF -DWITH_CORE_TOOLS=OFF \
    -DWITH_GFLAGS=OFF -DWITH_SNAPPY=ON -DWITH_LZ4=ON -DWITH_ZSTD=ON \
    -DFAIL_ON_WARNINGS=OFF
  cmake --build "$REL" --target rocksdb -j"${JOBS:-6}"
fi
BIN="$OUT/deadline_bench"
clang++ -std=c++20 -O2 -DNDEBUG -fno-rtti -DHAVE_FULLFSYNC -DLZ4 -DOS_MACOSX \
  -DROCKSDB_LIB_IO_POSIX -DROCKSDB_NO_DYNAMIC_EXTENSION -DROCKSDB_PLATFORM_POSIX \
  -DSNAPPY -DZSTD -I"$R" -I"$R/include" -I/opt/homebrew/include \
  "$HERE/deadline_bench.cc" "$REL/librocksdb.a" -L/opt/homebrew/lib \
  -lsnappy -llz4 -lzstd -lz -lpthread -o "$BIN"

configs=(
  "base|"
  "deadline|tombstone_deadline_seconds=$D"
  "coalesce|tombstone_deadline_seconds=$D;deadline_coalescing_window_seconds=$((D / 3))"
)
for r in $(seq 1 "$REPEATS"); do
  for c in "${configs[@]}"; do
    name="${c%%|*}"; opts="${c#*|}"
    echo "load $(sysctl -n vm.loadavg) run=$name r=$r" | tee -a "$OUT/runs.log"
    CF_OPTS="$opts" DEADLINE="$D" "$BIN" "$DB_DIR" "$RUN_S" "$OPS" \
      "$OUT/${name}_r$r.csv" | tee -a "$OUT/runs.log"
  done
done
