#!/bin/bash
# Run the skip-level tombstone comparison: mode 0 (stock behaviour) vs
# 1 (always) vs 2 (cost-based), for each workload, REPEATS times, rotating
# the order of modes per repeat. Results go to $OUT (one .txt + .csv per run).
# Usage: BIN=/path/to/tombstone_bench tombstone_bench/run.sh
set -u
BIN="${BIN:?set BIN to the tombstone_bench binary}"
OUT="${OUT:-$(dirname "$0")/results}"
DB="${DB:-/tmp/tombstone_bench_db}"
RUN_S="${RUN_S:-60}"
REPEATS="${REPEATS:-2}"
mkdir -p "$OUT"
# name|env assignments
WORKLOADS=(
  "uniform|DEL=0.1 DELPAT=uniform"
  "seqdel|DEL=0.3 DELPAT=seq"
  "rangedel|DEL=0.05 RDEL=0.01 RW=200 DELPAT=seq"
)
MODES=(0 1 2)
for rep in $(seq 1 "$REPEATS"); do
  for w in "${WORKLOADS[@]}"; do
    name="${w%%|*}"; envs="${w#*|}"
    # rotate mode order each repeat to spread machine-load drift
    for i in 0 1 2; do
      m=${MODES[$(( (i + rep - 1) % 3 ))]}
      if [ "$m" = 0 ]; then opts=""; else opts="skip_level_tombstone_compaction=$m"; fi
      tag="${name}_m${m}_r${rep}"
      echo "== $tag load=$(sysctl -n vm.loadavg)" | tee "$OUT/$tag.txt"
      env $envs CF_OPTS="$opts" "$BIN" "$DB" "$RUN_S" "$OUT/$tag" >> "$OUT/$tag.txt" 2>&1
      echo "exit=$?" >> "$OUT/$tag.txt"
      tail -n 9 "$OUT/$tag.txt"
    done
  done
done
