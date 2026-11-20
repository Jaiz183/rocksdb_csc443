#!/bin/bash
# Runs the baseline-vs-FADE workload matrix with fade_bench.
# Usage: run_matrix.sh <fade_bench (FADE branch)> <fade_bench (pristine main)> <db dir> <results dir>
set -u
BIN=$1
MAINBIN=$2
DB=$3
OUT=$4
mkdir -p "$OUT"
SUM="$OUT/summary.csv"
COMMON="db=$DB out_prefix=$OUT/ summary_csv=$SUM keys=1000000 write_secs=60 read_secs=10"

run() {
  local bin=$1; shift
  echo ">>> $bin $*" | tee -a "$OUT/log.txt"
  "$bin" $COMMON "$@" 2>&1 | tee -a "$OUT/log.txt"
}

# Each workload: pristine main, FADE branch with FADE off, FADE on.
# W0: no deletes -- FADE must be a no-op.
run $MAINBIN label=nodel_main put_frac=1.0
for d in 0 20; do run $BIN label=nodel put_frac=1.0 dth=$d; done
# W1: 10% point deletes, D_th sweep.
run $MAINBIN label=del10_main put_frac=0.9
for d in 0 40 20 10; do run $BIN label=del10 put_frac=0.9 dth=$d; done
# W2: 25% point deletes.
run $MAINBIN label=del25_main put_frac=0.75
for d in 0 20; do run $BIN label=del25 put_frac=0.75 dth=$d; done
# W3: range deletes (1% of ops delete 100 keys) + 9% point deletes.
run $MAINBIN label=rdel_main put_frac=0.9 range_del_frac=0.01
for d in 0 20; do run $BIN label=rdel put_frac=0.9 range_del_frac=0.01 dth=$d; done
# W4: skewed (80% of ops on 10% of keys), 10% deletes.
run $MAINBIN label=skew_main put_frac=0.9 hot_frac=0.1
for d in 0 20; do run $BIN label=skew put_frac=0.9 hot_frac=0.1 dth=$d; done
# W5: unpaced (compaction-bound), 10% deletes.
run $MAINBIN label=unpaced_main put_frac=0.9 rate=0
for d in 0 20; do run $BIN label=unpaced put_frac=0.9 rate=0 dth=$d; done
# Repeats for variance on the main comparison.
for seed in 7 99; do
  run $MAINBIN label=del10_main_s$seed put_frac=0.9 seed=$seed
  for d in 0 20; do run $BIN label=del10_s$seed put_frac=0.9 dth=$d seed=$seed; done
done
