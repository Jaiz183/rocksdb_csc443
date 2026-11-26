# FADE + skip-level tombstone compaction for RocksDB: design document

CSC443 group project, branch `fade-plus-skip-level` of `Jaiz183/rocksdb_csc443`.
All numbers below come from benchmark runs whose raw output is committed under
`tombstone_bench/results/`. The development history, including the designs we
rejected and why, is in [RESULTS.md](RESULTS.md) and
[SKIP_LEVEL_LOGIC.md](SKIP_LEVEL_LOGIC.md).

## 0. Summary

We combine three mechanisms in RocksDB's leveled compaction:

1. **FADE** (from Lethe, SIGMOD'20) bounds how long a tombstone can live: every
   point or range delete reaches the last level, where it is purged, within
   D_th seconds.
2. **Skip-level tombstone compaction** (our design) lets a tombstone-heavy
   compaction write straight into a deeper level when an amortized cost model
   says that writes fewer bytes than the normal one-level-at-a-time cascade.
3. **Precise tombstone ages** (our fix to FADE) dates each compaction output
   by the oldest tombstone it actually contains. Before, an output inherited
   the age of tombstones the compaction had dropped, which made FADE do
   needless work on delete-and-reinsert workloads.

Recommended configuration, as a RocksDB options string:

```
delete_persistence_threshold=40;fade_precise_tombstone_age=true;
skip_level_tombstone_compaction=2;skip_level_tombstone_density_threshold=0.3;
skip_level_tombstone_density_trigger=false;skip_level_max_overlap_ratio=20
```

Results (3 repeats, write amp vs stock RocksDB):

| workload | write amp | tombstone age bound |
|---|---|---|
| seqdel (sequential deletes) | **-3.8%** | oldest unpersisted tombstone <= 40 s, never exceeded |
| churn (delete + re-insert of hot keys) | **+9.4%** | <= 40 s, never exceeded |

FADE alone costs +0.8% (seqdel) and +11.9% (churn). Stock RocksDB gives no
bound: on churn its oldest tombstone reached 44 s.

## 1. Background and problem

In a leveled LSM tree, a delete writes a **tombstone**. The tombstone must
travel L0 -> base level -> ... -> last level, being rewritten together with
the data it overlaps at every hop. Only at the last level can it be dropped,
along with the data it shadows.

Two costs follow:

- **Write amplification:** tombstones are rewritten once per level.
- **Unbounded persistence latency:** stock RocksDB moves files only when a
  level is over its size target. A tombstone can sit in an upper level for as
  long as that level stays under target, so a "deleted" key's data can stay
  on disk indefinitely. That is a problem for privacy deletes and for space
  and read cost.

Lethe's FADE fixes the second cost by giving every level a time-to-live, at
the price of extra compactions (more write amp). Our goal: **keep FADE's bound
while getting the extra write amp back**, on delete-heavy workloads.

## 2. System overview

```
write path    Delete/DeleteRange -> memtable records the wall time of its first
              tombstone (MemTable::UpdateOldestTombstoneTime)
flush         L0 file's oldest_tombstone_time = oldest over flushed memtables
              (FlushJob::WriteLevel0Table); saved in the MANIFEST, tag 20
every new     ComputeFadeExpiredFiles: per-level cumulative deadline D_k;
Version       files whose oldest tombstone is older than D_k are "expired"
              ComputeTombstoneDenseFiles (density trigger, off in our config)
picker        SetupInitialFiles: expired FADE files first (unless the tree is
              saturated), else size-based picks
              ShouldTrySkipLevel / TrySetupSkipLevelCompaction: a dense FADE or
              size pick may be extended to a deeper output level t
compaction    each output records the smallest tombstone seqno it receives;
output        FinishCompactionOutputFile dates it: max(inherited time,
              seqno->time(oldest seqno)); FADE turns on seqno->time recording
```

## 3. FADE: bounded tombstone persistence

### 3.1 Tombstone clock (the paper's a_max)

`FileMetaData::oldest_tombstone_time` is the Unix time at which the oldest
tombstone in the file was inserted into the memtable. The age is
`a_max = now - oldest_tombstone_time`.

| event | how the time is set | code |
|---|---|---|
| tombstone written | memtable keeps the time of its first tombstone (one clock read per memtable) | `MemTable::UpdateOldestTombstoneTime`, `MemTable::Add` |
| flush | oldest over the flushed memtables, only if the L0 file holds tombstones | `FlushJob::WriteLevel0Table` |
| compaction | min over overlapping inputs that carry tombstones; with precise ages, see section 5 | `Compaction::MinInputFileOldestTombstoneTime`, `CompactionJob::FinishCompactionOutputFile` |
| persistence | MANIFEST custom tag 20 (`kOldestTombstoneTime`), safe for old readers to ignore | `VersionEdit::EncodeToNewFile4` / `DecodeNewFile4From` |

### 3.2 Per-level deadlines

The **stages** are the levels a tombstone still has to leave: L0, then the
base level down to the last non-empty level minus one. With n stages and size
ratio T = `max_bytes_for_level_multiplier`, Lethe splits D_th exponentially:

```
d_0 = D_th * (T - 1) / (T^n - 1),     d_k = T * d_{k-1}
D_k = d_0 + ... + d_k = D_th * (T^(k+1) - 1) / (T^n - 1),     D_{n-1} = D_th
```

A file at stage k is **expired** when `a_max > D_k`. The deadline is
cumulative, counted from when the tombstone was written, not from when it
reached the level. Deeper levels hold T times more data, so they get T times
more time.

**Implementation detail that matters:** deadlines are whole seconds, clamped to
`[1, D_th]`:

```
deadline_k = min(max(floor(D_k), 1), D_th)
```

and the last stage is forced to exactly D_th.

Effective deadlines on the benchmark tree (T = 5; stages L0, L3, L4, L5; L6 is
last; so n = 4 and T^n - 1 = 624):

| D_th | L0 | L3 | L4 | L5 |
|---|---|---|---|---|
| 10 s | 1 s (0.06) | 1 s (0.38) | 1 s (1.99) | 10 s |
| 20 s | 1 s (0.13) | 1 s (0.77) | 3 s (3.97) | 20 s |
| **40 s** | 1 s (0.26) | 1 s (1.54) | **7 s** (7.95) | **40 s** |

Exact D_k values are in brackets.

### 3.3 Picking

- **Order:** expired files are ordered by level (shallowest first), then oldest
  tombstone, then most tombstones (FADE's "DD" mode).
- **Starvation guard:** FADE compactions run *before* size-based compactions
  only while every level's compaction score is below **1.5**
  (`kFadeYieldScore`). Above that, size-based compaction picks the level and
  FADE only chooses which file in it to compact. This keeps FADE from
  starving size compaction when bandwidth is tight.
- **No trivial moves for kFadeTtl into the bottommost level:** the rewrite is
  what drops the tombstones.

## 4. Skip-level tombstone compaction

### 4.1 Which compactions may skip

- A file is **tombstone-dense** when `d(f) = num_deletions / num_entries >= θ`.
  We use **θ = 0.3** (option default 0.5). Churn's flushed files are about 45%
  tombstones, so 0.5 would never fire there.
- A compaction may be extended when its start inputs are dense *and* it is:
  - a size-based pick (`kLevelL0FilesNum`, `kLevelMaxLevelSize`), or
  - a **FADE pick** (`kFadeTtl`). This is the integration point: FADE decides
    *when* expired tombstones must move; skip-level decides *how far* they go
    in that one rewrite.
- The optional density *trigger*, which starts compactions just because a
  file is dense, is **off** (`skip_level_tombstone_density_trigger=false`). It
  added write amp in every configuration we measured (section 6).

### 4.2 How far to skip: the amortized cascade rule (mode 2)

Notation:

- S = bytes of the start inputs;
- n = normal output level;
- B(t) = total input bytes of a compaction from the start level straight to
  level t (S plus all overlapping files at levels n..t, clean-cut expanded);
- T = size ratio.

In steady state, every further one-level hop of S bytes costs about (1 + T)·S
amortized: S of its own plus T·S of overlap shared with other data. Skipping
from n to t pays the measured extra overlap B(t) - B(n) now. So the estimated
saving is

```
net(t) = (t - n) * (1 + T) * S  -  (B(t) - B(n))
```

The rule takes **the t > n with the largest net(t) > 0** (ties go deeper),
subject to:

- **growth cap:** B(t) <= R · B(n), with **R = 20**
  (`skip_level_max_overlap_ratio`);
- **safety stops** (nothing deeper is considered):
  - a file at level t is being compacted;
  - B(t) > `max_compaction_bytes`;
  - a running compaction writes into any level in (start, t] within this key
    range. That would let newer data land below older data.

If no t > n qualifies, the compaction stays one level deep. Nothing extra
runs, so skip-level can only make an existing compaction cheaper (or leave it
alone).

**Worked example** (from the unit tests; T = 10, S = 100, so one amortized hop
= 1100):

| overlap L2 / L3 / L4 | net(L3) | net(L4) | decision |
|---|---|---|---|
| 1000 / 10000 / 100000 | 1100 - 10000 < 0 | < 0 | no skip |
| 1000 / 500 / 100000 | 1100 - 500 = 600 | 2200 - 100500 < 0 | L1 -> L3 |
| 100 / 100 / - | 1100 - 100 = 1000 | - | L1 -> L3 |

**Why it saves bytes on delete-heavy workloads:**

- **seqdel:** deletes sweep the key space, so lower levels are already thin
  where new tombstones arrive. B(t) - B(n) is small, and one compaction
  replaces two or more rewrites of the same tombstones.
- **churn:** the same hot keys are deleted and re-inserted, so a hot-key
  file's deep overlap is small. L0 -> L4 or L0 -> L5 directly is cheaper than
  going through every level.
- **On top of FADE:** expired tombstones cross several FADE stages in one
  rewrite instead of one FADE compaction per stage (on churn at D_th = 20 s,
  FADE compactions drop from about 95 to 38 per run).

## 5. Precise tombstone ages (our fix to FADE)

### 5.1 The problem we found

Rule before: an output's time = min oldest-tombstone time of the overlapping
inputs. This stays true even when that oldest tombstone is *dropped* by the
compaction. On churn this happens constantly: a re-insert supersedes the
tombstone. The stale old time sticks to files whose tombstones are all recent,
and FADE keeps pushing them into the last level.

Measured at D_th = 40 s on churn:

- FADE ran **14-15 L5 -> L6 compactions per run, writing 70-130 MB**;
- the oldest tombstone actually present was only about **20 s** old, against
  L5's 40 s deadline.

### 5.2 The fix (`fade_precise_tombstone_age=true`)

1. While writing each output file, compaction tracks
   **s_min = the smallest sequence number among the point and range
   tombstones written to that file**
   (`CompactionOutputs::AddToOutput`, `AddRangeDels`).
2. At file finish:

   ```
   t_out = max( t_inherited ,  M(s_min) )
   ```

   - t_inherited is the old rule;
   - M(s) = `SeqnoToTimeMapping::GetProximalTimeBeforeSeqno(s)` is the time of
     the last (seqno, time) sample taken *before* seqno s.
3. FADE makes the DB record those samples: `MinAndMaxPreserveSeconds` treats
   a FADE column family like `preserve_internal_time_seconds = D_th`, so a
   sample is taken every **ceil(D_th / 100) s**, which is 1 s at D_th = 40.

**Why this is safe** (the D_th bound still holds):

- Both terms are lower bounds on the true write time of the oldest surviving
  tombstone.
  - t_inherited: that tombstone came from an input whose own oldest
    tombstone is no newer.
  - M(s_min): the sample predates the write.
- So their max is also a lower bound. Ages can only be over-estimated, never
  under-estimated, and FADE can only compact early, never late.

**What it does not change:** turning on seqno->time recording does not change
seqno zeroing or tiering. Those cutoffs still read
`preserve_internal_time_seconds` and `preclude_last_level_data_seconds`, both
0. The cost is a seqno->time table property per SST, plus a type check per
key written by compaction (only with the option on).

## 6. Major design decisions

| decision | alternatives tried | evidence (RESULTS.md) |
|---|---|---|
| Amortized rule for skip depth (mode 2) | v1 depth/ratio score (never skipped); v2 "skip while B(t)/B(n) <= R" (+18% to +238% write amp, because it charged every hop at the current overlap) | v3 amortized rule: -2.0% / -1.9% vs base |
| Only *extend* existing compactions; density trigger off | trigger on (v2-v4): +38% churn in v4, because it compacted each flushed L0 file alone, 1.00 files per compaction vs 2.00 | v5: trigger on = skip-only, so no benefit; off is simpler |
| Extend FADE picks too | FADE picks always one level | FADE + skip -3.5% (seqdel) vs FADE at D_th = 10 s |
| Raise D_th rather than disable skips near the top | user-proposed "no skip in top X levels"; but per-shape bytes showed the cost was FADE's last stage, and skips were *saving* bytes | churn penalty +103% -> +57% -> +22% at D_th = 10/20/40 s |
| Precise ages via seqno->time | tighten the min-inheritance heuristically (no proof of safety) | 0 stale L5 -> L6 per run; churn -12.1% at 40 s |
| θ = 0.3, R = 20, mode 2 | θ = 0.5 (never fires on churn); R = 10 | v3-v5 runs |

## 7. Tuned parameters

| parameter | option | default | ours | why |
|---|---|---|---|---|
| D_th | `delete_persistence_threshold` | 0 (off) | **40 s** | below stock RocksDB's churn peak age (28-44 s), so the bound is real; 10 s cost +108% on churn; 20 s cost +41% before the age fix and +28% after |
| precise ages | `fade_precise_tombstone_age` | false | **true** | removes the stale last-level compactions (section 5) |
| skip mode | `skip_level_tombstone_compaction` | 0 | **2** | mode 1 (always deepest) ignores cost |
| θ | `skip_level_tombstone_density_threshold` | 0.5 | **0.3** | churn files are about 45% tombstones |
| trigger | `skip_level_tombstone_density_trigger` | true | **false** | section 6 |
| R | `skip_level_max_overlap_ratio` | 20 | **20** | caps a single compaction at 20 x the normal one |
| T | `max_bytes_for_level_multiplier` | 10 | 5 (benchmark tree) | deeper tree for the same data, so skipping matters |
| FADE yield score | constant `kFadeYieldScore` | 1.5 | 1.5 | starvation guard (section 3.3) |

## 8. Benchmark methodology

- **Driver:** `tombstone_bench/tombstone_bench.cc`, public API only.
- **Matrix:** `tombstone_bench/ablation.sh`; tables come from
  `summarize_ablation.py`.
- **Tree:**

  | setting | value |
  |---|---|
  | write buffer and target file size | 1 MB |
  | base level size | 1 MB |
  | T | 5 |
  | L0 compaction trigger | 4 files |
  | background jobs | 4 |
  | compression | none |
  | WAL | off |
  | Bloom filter | 10 bits per key |
  | data | 1M keys of 100-byte values, preloaded |

- **Workloads:** 60 s at 20,000 ops/s, then a drain (flush and wait for
  compactions).
  - **seqdel:** 30% deletes sweeping the key space in order (expiry-style
    deletes).
  - **churn:** 90% of operations toggle one of 20,000 hot keys (delete if
    present, re-insert otherwise), so the same key is deleted at several
    levels at once; the rest are uniform puts.
- **Metrics:**
  - **Write amp** = (compaction + flush bytes written) / user bytes, over the
    timed phase and the drain.
  - **True tombstone age** = the oldest tombstone still above the last level.
    It is measured independently of FADE's bookkeeping, from a
    table-properties collector (smallest tombstone seqno per SST) and a
    seqno -> time map kept by the driver. It is sampled every second, and we
    report the peak and the samples over D_th.
  - Compaction counts by reason and level, e.g. `FadeTtl:L5->L6`.
- **Correctness:** every run checks a full scan against an in-memory model,
  and reports PASS/FAIL.
- **Rigor:**
  - 3 repeats, with config order rotated per repeat.
  - Load-gated: each run waits for the 1-minute load average to drop below
    8. Background I/O (iCloud, backups) had shifted write amp by about 3%.
  - Significance by Welch's t.

## 9. Results

### 9.1 Final system (precise ages), D_th = 40 s and 20 s

Write amp, mean of 3 runs. Welch t in brackets.

| workload | D_th | base | FADE + skip, old ages | FADE + precise | **FADE + skip + precise** | fix effect | skip effect |
|---|---|---|---|---|---|---|---|
| seqdel | 40 s | 9.37 | 9.00 (-3.9%) | 9.44 (+0.8%) | **9.01 (-3.8%)** | +0.1% (t = 0.1) | -4.6% (t = -6.6) |
| seqdel | 20 s | 9.25 | 8.95 (-3.3%) | 9.44 (+2.0%) | **9.02 (-2.6%)** | +0.8% (t = 0.7) | -4.5% (t = -7.3) |
| churn | 40 s | 4.26 | 5.30 (+24.4%) | 4.77 (+11.9%) | **4.66 (+9.4%)** | -12.1% (t = -3.6) | -2.3% (t = -1.2) |
| churn | 20 s | 4.17 | 5.89 (+41.2%) | 5.87 (+40.7%) | **5.32 (+27.5%)** | -9.7% (t = -10.1) | -9.4% (t = -9.6) |

- "fix effect" = FADE + skip + precise vs FADE + skip with old ages.
- "skip effect" = FADE + skip + precise vs FADE + precise.

Tombstone age bound (FADE + skip + precise), and stale FADE L5 -> L6
compactions per run:

| workload | D_th | peak true age | samples over D_th | stale L5 -> L6, old ages | stale L5 -> L6, precise | base peak age |
|---|---|---|---|---|---|---|
| seqdel | 40 s | 5.2-6.3 s | 0 | 0 | 0 | 5.5-6.6 s |
| churn | 40 s | 20.3-23.2 s | 0 | 14-15 | **0** | 28-41 s |
| churn | 20 s | 16.6-18.5 s | 0 | 21-22 | **0** | 35-44 s |

### 9.2 How we got here

Churn, FADE + skip vs base (3 repeats each):

| step | D_th | churn vs base | seqdel vs base |
|---|---|---|---|
| FADE + skip, first combined version | 10 s | +103% | +7.1% |
| raise D_th | 20 s | +57% | -3.7% |
| raise D_th | 40 s | +22% | -4.2% |
| + precise tombstone ages | 40 s | **+9.4%** | **-3.8%** |

Skip-level on its own (no FADE, no bound) saves -3.1% (seqdel) and -3.7%
(churn) vs base.

### 9.3 Colleagues' KiWi merged in (`fade-kiwi-plus-skip-level`)

- KiWi's delete-tile layout is about write-amp neutral: +0.3% (seqdel), +3%
  (churn).
- With KiWi on, skip-level's saving over FADE holds: -7.3% on seqdel at
  D_th = 10 s.
- The precise-age fix merged cleanly. With KiWi tiles on, at D_th = 40 s,
  3 repeats:

  | workload | KiWi + FADE + skip + precise vs base | vs the same without the fix (Welch t) |
  |---|---|---|
  | seqdel | **-2.1%** | -1.4% (t = -2.5) |
  | churn | **+6.4%** | **-14.0%** (t = -5.7) |

  - Stale FADE L5 -> L6 compactions drop from 14 per run to 0, and no run had
    a sample over 40 s.
  - Full table: RESULTS.md on `fade-kiwi-plus-skip-level`.

## 10. Testing

| suite | tests | covers |
|---|---|---|
| `compaction_picker_test` (`SkipLevelPickerTest`) | 20 skip-level of 181 | amortized rule, growth cap, safety stops, trigger rules |
| `db_skip_level_compaction_test` | 8 | end-to-end skips, snapshots, `FadePickSkipsLevelsWhenCheap`, randomized model check with every skip mode x FADE off/on |
| `db_compaction_test` | 390 (6 FADE) | FADE deadlines follow the formula, bound met under a mock clock, starvation guard; all stock compaction tests |
| `seqno_time_test` | 30 (1 new) | `FadePreciseTombstoneAge`: superseded tombstone's time is not inherited |
| `options_test`, `options_settable_test`, `version_edit_test` | | option plumbing, MANIFEST tag |

On the KiWi branch, `kiwi_test` (42), `db_etc3_test` (25) and `table_test`
(5631) also pass, including KiWi secondary deletes through FADE and
skip-level compactions.

## 11. Limitations and future work

- **Churn still costs +9.4% at D_th = 40 s.** That is FADE's intermediate
  stages (L3 at 1 s, L4 at 7 s). Lethe's split gives the top stages
  sub-second budgets that our integer clamp rounds up to 1 s. A per-level
  minimum, or leaving L0 and the base level to size compaction, is the next
  lever.
- **Only one machine (a laptop SSD), 60 s runs, one tree shape.** Longer runs
  and other ratios T would strengthen the claims.
- **Skip-level decides per file;** extra-compaction's per-tombstone routing
  is complementary.
- **FADE's expiry is only checked when a Version is installed,** so an idle
  DB does not act (the same limitation as RocksDB's `ttl`).
- **Data written before `fade_precise_tombstone_age` was turned on** may be
  dated by RocksDB's pre-populated seqno->time mapping, so its age can be
  under-estimated. Data written while the option is on is not affected.

## 12. Code changes against stock RocksDB

These are the changes from `main` (6eeb6d11f) to `fade-plus-skip-level`,
production code only. The complete diff, every changed line, is in
[CODE_CHANGES.diff](CODE_CHANGES.diff) (1685 lines; +1067 / -3 in 26 files).
Tests, benchmark drivers and results come on top: about +1,200 lines of tests,
and `tombstone_bench/` plus `fade_bench/`.

Feature attribution: **F** = FADE, **S** = skip-level, **P** = precise ages.
Line numbers refer to the file on `fade-plus-skip-level`.

### Public API and options

| file | lines | feat | change |
|---|---|---|---|
| `include/rocksdb/advanced_options.h` | 896-943 | F, P | `delete_persistence_threshold` (D_th) and `fade_precise_tombstone_age`, documented |
| `include/rocksdb/advanced_options.h` | 1053-1118 | S | `skip_level_tombstone_compaction` (0/1/2), `_density_threshold`, `_density_trigger`, `skip_level_max_overlap_ratio` |
| `include/rocksdb/listener.h` | 163-169 | F, S | new `CompactionReason::kFadeTtl`, `kSkipLevelTombstone` |
| `options/cf_options.h` | 155-166, 247-254, 357-364 | F, S, P | `MutableCFOptions` fields, constructors, defaults |
| `options/cf_options.cc` | 615-649 | F, S, P | option type info: string names, mutable |
| `options/cf_options.cc` | 1321-1337 | F, S, P | `MutableCFOptions::Dump` log lines |
| `options/options.cc` | 95-103, 434-451 | F, S, P | `AdvancedColumnFamilyOptions` copy-constructor, `Dump` |
| `options/options_helper.cc` | 376-387 | F, S, P | `UpdateColumnFamilyOptions` |
| `db/column_family.cc` | 1656-1671 | S | `ValidateOptions`: mode in 0..2, θ in (0, 1], R finite and >= 1 |

### Tombstone clock and MANIFEST (FADE)

| file | lines | feat | change |
|---|---|---|---|
| `db/memtable.h` | 13, 329-335, 898-901, 1030-1032, 1055-1057 | F | `ApproximateOldestTombstoneTime()`, `oldest_tombstone_time_` atomic |
| `db/memtable.cc` | 232, 407-422, 1315-1318 | F | init; `UpdateOldestTombstoneTime` (one clock read per memtable); called from `Add` for every delete type |
| `db/flush_job.cc` | 14, 1087-1108 | F | `WriteLevel0Table`: L0 file's `oldest_tombstone_time` |
| `db/version_edit.h` | 124-126 | F | `NewFileCustomTag::kOldestTombstoneTime = 20` |
| `db/version_edit.h` | 143-144 | F | `kNoTombstoneTime` |
| `db/version_edit.h` | 316-321 | S | `FileMetaData::from_skip_level_compaction` (in memory, trigger hysteresis) |
| `db/version_edit.h` | 346-360, 483-503 | F | `FileMetaData::oldest_tombstone_time`, `TryGetOldestTombstoneTime`, `FadeInvalidatedEntriesEstimate` |
| `db/version_edit.cc` | 214, 519-524, 683-687, 1194-1197 | F | copy in `AddExistingFile`; encode/decode tag 20; `DebugString` |

### Deadline computation and candidates

| file | lines | feat | change |
|---|---|---|---|
| `db/version_set.h` | 236-252, 640-686, 935-939 | F, S | declarations and accessors: `FadeExpiredFiles`, `FadeLevelDeadline`, `TombstoneDenseFiles`; members |
| `db/version_set.cc` | 15, 3975-3976 | F, S | both computed in `ComputeCompactionScore` |
| `db/version_set.cc` | 3994-3995 | F, S | both lists pruned of files being compacted |
| `db/version_set.cc` | 4044-4088 | S | `ComputeTombstoneDenseFiles`: density >= θ, L1 and below, skips flagged files, densest first |
| `db/version_set.cc` | 4116-4236 | F | `ComputeFadeExpiredFiles`: stages, D_k formula, integer clamp, expiry, DD ordering |

### Compaction picking

| file | lines | feat | change |
|---|---|---|---|
| `db/compaction/compaction_picker_level.cc` | 18, 25-28, 47-49 | F, S | `NeedsCompaction` also true when FADE-expired or dense files exist |
| `db/compaction/compaction_picker_level.cc` | 93-137 | F, S | builder member declarations |
| `db/compaction/compaction_picker_level.cc` | 247-285 | F | `PickFadeCompaction`, `PickFadeCompactionAtLevel` |
| `db/compaction/compaction_picker_level.cc` | 287-304, 318-325, 370-383 | F, S | `SetupInitialFiles`: FADE first unless saturated (score >= 1.5); FADE file within a saturated level; saturated fallback; density trigger |
| `db/compaction/compaction_picker_level.cc` | 660-666 | S | `PickCompaction`: try the skip-level extension |
| `db/compaction/compaction_picker_level.cc` | 728-752 | S | `PickSkipLevelTriggerCompaction` (trigger path, off in our config) |
| `db/compaction/compaction_picker_level.cc` | 753-782 | S, F | `ShouldTrySkipLevel`: size or **FADE** pick, start inputs dense |
| `db/compaction/compaction_picker_level.cc` | 783-946 | S | `TrySetupSkipLevelCompaction`: input gathering, safety stops, mode 1 / mode 2 rule, growth cap, grandparents |
| `db/compaction/compaction.cc` | 595-607 | F, S | `IsTrivialMove`: never for FADE-into-bottommost or skip-level |
| `db/compaction/compaction.h`, `compaction.cc` | 471-476, 1062-1083 | F | `MinInputFileOldestTombstoneTime` |

### Compaction output dating

| file | lines | feat | change |
|---|---|---|---|
| `db/compaction/compaction_outputs.h` | 45-48, 193-197, 357-360 | P | `Output::oldest_tombstone_seqno`, accessor, `track_tombstone_seqno_` |
| `db/compaction/compaction_outputs.cc` | 55-56 | S | mark outputs `from_skip_level_compaction` |
| `db/compaction/compaction_outputs.cc` | 506-513, 901-904, 980-982 | P | track s_min for point and range tombstones; enable flag |
| `db/compaction/compaction_job.cc` | 173-176 | F, S | reason names `FadeTtl`, `SkipLevelTombstone` |
| `db/compaction/compaction_job.cc` | 2445-2482 | F, P | `FinishCompactionOutputFile`: inherited time, then `max(.., M(s_min))` |
| `db/seqno_to_time_mapping.h` | 303-310 | P | `MinAndMaxPreserveSeconds::Combine`: FADE with precise ages enables seqno->time recording over D_th |
| `db/db_impl/db_impl.cc` | 2150-2152, 5111-5113 | P | re-register the recording worker when the FADE options change or a CF is created |

### Build files

| file | lines | change |
|---|---|---|
| `CMakeLists.txt` | 1513 | registers the new `db_skip_level_compaction_test` |
| `Makefile` | 1782-1784 | same |
| `src.mk` | 524 | same |
| `BUCK` | 5103-5108 | same |

### Key code (excerpts)

The amortized rule, `TrySetupSkipLevelCompaction`:

```cpp
const double hop_bytes =
    (1.0 + mutable_cf_options_.max_bytes_for_level_multiplier) *
    static_cast<double>(start_bytes);                       // (1 + T) * S
...
if (j == first_next) { normal_bytes = total_bytes; ... continue; }  // B(n)
const double growth = static_cast<double>(total_bytes) /
                      static_cast<double>(normal_bytes);
if (growth > mutable_cf_options_.skip_level_max_overlap_ratio) break;  // cap R
const double net = static_cast<double>(j - first_next) * hop_bytes -
                   static_cast<double>(total_bytes - normal_bytes);
if (net > 0.0 && net >= best_net) { best_level = j; ... best_net = net; }
```

FADE deadlines, `ComputeFadeExpiredFiles`:

```cpp
cumulative = dth * (std::pow(t, k + 1) - 1.0) / (std::pow(t, n) - 1.0);  // D_k
uint64_t deadline = static_cast<uint64_t>(cumulative);
deadline = std::min<uint64_t>(std::max<uint64_t>(deadline, 1), dth);
...
if (current_time - tombstone_time > deadline) fade_expired_files_.emplace_back(level, f);
```

Precise ages, `FinishCompactionOutputFile`:

```cpp
const SequenceNumber oldest_seqno = outputs.CurrentOutputOldestTombstoneSeqno();
if (oldest_seqno != kMaxSequenceNumber && !seqno_to_time_mapping_.Empty()) {
  const uint64_t mapped =
      seqno_to_time_mapping_.GetProximalTimeBeforeSeqno(oldest_seqno);
  if (mapped != kUnknownTimeBeforeAll && mapped > meta->oldest_tombstone_time) {
    meta->oldest_tombstone_time = mapped;   // t_out = max(t_inherited, M(s_min))
  }
}
```

### Changes visible to users who do not enable the features

- Defaults leave all three features off. Behaviour, cost and on-disk format
  are then those of stock RocksDB, except:
  - one in-memory `bool` and one `uint64_t` per `FileMetaData`;
  - one relaxed atomic load per tombstone written to a memtable;
  - FADE's tombstone clock is not gated on the option. Each memtable records
    its first tombstone's time, and every SST file holding tombstones gets
    `oldest_tombstone_time` in the MANIFEST (tag 20, a varint), even with
    FADE off. Older versions ignore the tag safely. Gating it on
    `delete_persistence_threshold > 0` is a small follow-up if upstreaming;
    it is kept on here so that turning FADE on later has real ages.
- Two new `CompactionReason` values are appended before `kNumOfReasons`.
