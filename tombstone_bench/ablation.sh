#!/bin/bash
# Ablation for skip-level tombstone compaction: separates the effect of the
# tombstone-density trigger from the effect of skipping levels.
#
#   base       stock behaviour (feature off)
#   trig       density trigger on, but every compaction is one level
#              (mode 2 with ratio 1: no deeper target can qualify)
#   skip       no density trigger; dense size-triggered compactions skip to
#              the deepest feasible level (mode 1)
#   trig+skip  trigger on, skip to the deepest feasible level (mode 1)
#   trig+sav   trigger on, mode 2 (v2: cascade-savings bound; v3: amortized
#              cascade rule) with the default ratio
#   skip+sav   no trigger, mode 2 on dense size picks
#   fade       FADE only (delete_persistence_threshold = $DTH seconds)
#   fade+skip  FADE + skip+sav: dense size picks and dense FADE picks are
#              extended by the mode 2 rule (needs a branch with both features)
#   fade+p, fade+skip+p  the same with fade_precise_tombstone_age=true
#   kiwi       KiWi delete tiles on (KIWI_TILE_PAGES=$KIWI pages), no
#              FADE/skip; needs a branch with KiWi
#   kiwi+fade  KiWi tiles + fade
#   kiwi+fade+skip  KiWi tiles + fade+skip
#   kiwi+fade+skip+p  KiWi tiles + fade+skip+p
#
# A config is "name|cf_opts" or "name|cf_opts|extra env" (e.g. KiWi's
# KIWI_TILE_PAGES, which is a table option set by the driver).
#
# ONLY="base trig+sav" runs just the named configs.
#
# Runs on a deeper tree than run.sh (MULT=5, LBASE_MB=1) so that a start file
# can skip more than one level. Results go to $OUT (one .txt + .csv per run).
# Usage: BIN=/path/to/tombstone_bench tombstone_bench/ablation.sh
set -u
BIN="${BIN:?set BIN to the tombstone_bench binary}"
OUT="${OUT:-$(dirname "$0")/results/ablation}"
DB="${DB:-/tmp/tombstone_bench_db}"
RUN_S="${RUN_S:-60}"
REPEATS="${REPEATS:-2}"
TREE="${TREE:-MULT=5 LBASE_MB=1}"
# Density threshold for every non-base config. Churn files are ~45%
# tombstones, below the 0.5 default, so the default would never fire there.
DENS="${DENS:-0.3}"
# FADE delete persistence threshold (seconds) for the fade configs; also the
# true-tombstone-age limit counted by the benchmark (AGE_LIMIT).
DTH="${DTH:-30}"
KIWI="${KIWI:-4}"
mkdir -p "$OUT"
WORKLOADS=(
  "seqdel|DEL=0.3 DELPAT=seq"
  "churn|DELPAT=churn CHURN=0.9 HOT=20000"
)
T="skip_level_tombstone_compaction"
CONFIGS=(
  "base|"
  "trig|$T=2;skip_level_max_overlap_ratio=1"
  "skip|$T=1;skip_level_tombstone_density_trigger=false"
  "trig+skip|$T=1"
  "trig+sav|$T=2"
  "skip+sav|$T=2;skip_level_tombstone_density_trigger=false"
  "fade|delete_persistence_threshold=$DTH"
  "fade+skip|delete_persistence_threshold=$DTH;$T=2;skip_level_tombstone_density_trigger=false"
  "fade+p|delete_persistence_threshold=$DTH;fade_precise_tombstone_age=true"
  "fade+skip+p|delete_persistence_threshold=$DTH;fade_precise_tombstone_age=true;$T=2;skip_level_tombstone_density_trigger=false"
  "kiwi||KIWI_TILE_PAGES=$KIWI"
  "kiwi+fade|delete_persistence_threshold=$DTH|KIWI_TILE_PAGES=$KIWI"
  "kiwi+fade+skip|delete_persistence_threshold=$DTH;$T=2;skip_level_tombstone_density_trigger=false|KIWI_TILE_PAGES=$KIWI"
  "kiwi+fade+skip+p|delete_persistence_threshold=$DTH;fade_precise_tombstone_age=true;$T=2;skip_level_tombstone_density_trigger=false|KIWI_TILE_PAGES=$KIWI"
)
if [ -n "${ONLY:-}" ]; then
  KEEP=()
  for c in "${CONFIGS[@]}"; do
    case " $ONLY " in *" ${c%%|*} "*) KEEP+=("$c") ;; esac
  done
  CONFIGS=("${KEEP[@]}")
fi
N=${#CONFIGS[@]}
# FIRST_REP > 1 resumes a paused run: earlier repeats are kept as they are.
for rep in $(seq "${FIRST_REP:-1}" "$REPEATS"); do
  for w in "${WORKLOADS[@]}"; do
    name="${w%%|*}"; envs="${w#*|}"
    # rotate config order each repeat to spread machine-load drift
    for i in $(seq 0 $((N - 1))); do
      c=${CONFIGS[$(( (i + rep - 1) % N ))]}
      cname="${c%%|*}"; opts="${c#*|}"; xenv=""
      case "$opts" in *"|"*) xenv="${opts#*|}"; opts="${opts%%|*}" ;; esac
      if [ -n "$opts" ]; then
        opts="$opts;skip_level_tombstone_density_threshold=$DENS"
      fi
      tag="${name}_${cname}_r${rep}"
      # Wait (up to LOAD_WAIT_S) for the 1-minute load average to drop below
      # MAX_LOAD, so background I/O (backups, indexing) does not skew runs.
      if [ -n "${MAX_LOAD:-}" ]; then
        waited=0
        while [ "$waited" -lt "${LOAD_WAIT_S:-600}" ] && awk -v l="$(sysctl -n vm.loadavg | awk '{print $2}')" -v m="$MAX_LOAD" 'BEGIN{exit !(l >= m)}'; do
          sleep 15; waited=$((waited + 15))
        done
      fi
      echo "== $tag load=$(sysctl -n vm.loadavg)" | tee "$OUT/$tag.txt"
      env $TREE $envs $xenv AGE_LIMIT="$DTH" CF_OPTS="$opts" "$BIN" "$DB" "$RUN_S" "$OUT/$tag" \
        >> "$OUT/$tag.txt" 2>&1
      echo "exit=$?" >> "$OUT/$tag.txt"
      tail -n 9 "$OUT/$tag.txt"
    done
  done
done
