# fade_bench

Workload driver for checking baseline RocksDB behaviour and FADE (Lethe).
Public API only; tombstone ages come from a TablePropertiesCollector
(smallest tombstone seqno per SST) plus a seqno -> wall-time map, so they
are independent of FADE's own `oldest_tombstone_time` bookkeeping. Every run
checks the DB against an exact in-memory model (full scan, 200k Gets, 2k
Seeks) before and after reopen.

Build (release RocksDB is built with -fno-rtti, so the driver must be too):

    cmake -S .. -B ../build-rel -DCMAKE_BUILD_TYPE=Release -DWITH_GFLAGS=OFF
    cmake --build ../build-rel -j10 --target rocksdb
    clang++ -std=c++20 -O2 -DNDEBUG -fno-rtti -I../include fade_bench.cc \
        ../build-rel/librocksdb.a -lpthread -o fade_bench
    # against a tree without FADE (e.g. main), add -DFADE_BENCH_NO_FADE

Run the matrix: `./run_matrix.sh <fade_bench> <fade_bench_main> <db dir> <out dir>`

`results/` holds the 2026-11-20 run on an M-series MacBook (10 cores, APFS,
not direct I/O): `summary.csv` (one row per run), `*_ts.csv` (tombstone age
per level every 250 ms), `log.txt`, and `unpaced_repeat_summary.csv`.
