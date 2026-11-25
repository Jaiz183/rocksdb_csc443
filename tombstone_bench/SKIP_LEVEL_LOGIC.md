# Skip-level tombstone compaction: how it works

Branch `skip-level-tombstone-draft` (CSC443 draft). This note describes the
logic as of v5. The benchmark numbers behind each design step are in
[RESULTS.md](RESULTS.md).

## 1. The idea

In leveled compaction a tombstone written at the top of the tree moves down one
level per compaction: L0 -> base -> base+1 -> ... -> last level. At every hop it
is read and rewritten together with all the data it overlaps at the next level.
It can be dropped only when it reaches the last level (or the bottommost level
for its key range).

A skip-level compaction takes a tombstone-dense start file and every
overlapping file in the next few levels and writes the result straight into a
deeper level t (a multi-level compaction). The tombstones cross all of those
levels in one rewrite instead of one rewrite per level. If t is the last level
they are dropped at once, along with the data they shadow.

Skipping is not free: the compaction also reads and rewrites everything it
overlaps on the levels in between. The design question is **when** a skip
writes less than the normal cascade, and **how deep** to go.

## 2. Terms

| symbol | meaning |
|---|---|
| density d(f) | `num_deletions / num_entries` of a file (range deletions count as deletions) |
| θ | `skip_level_tombstone_density_threshold` (default 0.5); a file is *dense* if d ≥ θ |
| start | level of the start file(s) |
| n | normal output level: the base level if start = 0, else start + 1 |
| last | last non-empty level |
| S | bytes of the start inputs (for L0, all overlapping L0 files) |
| B(t) | total input bytes of a compaction from start straight to level t: S plus the overlapping, clean-cut-expanded files at levels n..t |
| T | `max_bytes_for_level_multiplier` (the fan-out) |
| R | `skip_level_max_overlap_ratio` (default 20) |

## 3. Where it hooks into the compaction picker

Code: `db/compaction/compaction_picker_level.cc` (`LevelCompactionBuilder`).

```
PickCompaction()
  SetupInitialFiles()
    1. size-based picks (L0 file count, level size scores)    <- stock RocksDB
    2. if none: PickSkipLevelTriggerCompaction()              <- density trigger
    3. if none: marked files, TTL, periodic, ...              <- stock RocksDB
  if the trigger already set up a skip, or
     (ShouldTrySkipLevel() and TrySetupSkipLevelCompaction()):
       return the skip-level compaction
  else: normal one-level compaction                           <- stock RocksDB
```

So there are two ways in:

### 3a. Extending a size-based pick ("skip" in the ablation)

`ShouldTrySkipLevel()` returns true when:

- the feature is on (mode ≠ 0);
- the pick is an ordinary size-based one (`kLevelL0FilesNum` or
  `kLevelMaxLevelSize`), not a trivial move or an intra-L0 compaction;
- the picked start inputs together are dense (total deletions ≥ θ × total
  entries).

RocksDB has already decided this compaction will run now, and which files it
starts from. The feature only asks whether it should go deeper than n. If
`TrySetupSkipLevelCompaction()` finds no profitable deeper target, the pick
continues as a normal one-level compaction. Nothing is added and nothing is
delayed.

### 3b. The density trigger ("trig" in the ablation)

With `skip_level_tombstone_density_trigger = true` (the default),
`VersionStorageInfo::ComputeTombstoneDenseFiles()` (in `db/version_set.cc`)
lists candidate files every time compaction scores are recomputed.

A file is a candidate when it is:

- in **L1 or below** (v5: never L0, see section 6);
- strictly above the last non-empty level;
- not being compacted;
- dense (d ≥ θ);
- **not** marked `from_skip_level_compaction` (hysteresis, see below).

Candidates are sorted densest first, with ties going to the shallower level.

`PickSkipLevelTriggerCompaction()` runs only when no size-based compaction is
due, so it never starves normal compaction. It walks the candidates in order:

- It expands each one to a clean cut and calls `TrySetupSkipLevelCompaction()`.
- It uses the **first** candidate for which a target deeper than n is
  profitable.
- If no candidate can skip, it picks nothing. It never runs a one-level
  compaction just because a file is dense: that costs an extra level crossing,
  and the output is usually still dense, so it would be picked again at the
  next level (measured in v2/v3, section 6).

**Hysteresis.** Every output file of a `kSkipLevelTombstone` compaction gets
`FileMetaData::from_skip_level_compaction = true`. The flag lives in memory
only and is not written to the MANIFEST; after a reopen it is false again,
which is harmless. Flagged files are not trigger candidates until some other
compaction rewrites them into new files without the flag.

This matters because a skip compaction that stopped short of the last level
often writes files that are still dense. Without the flag the trigger would
pick them again right away and pay the deeper overlap a second time. The flag
does not affect size-based picks.

## 4. How deep to go: `TrySetupSkipLevelCompaction()`

### 4a. Gathering inputs and safety stops

Start from the start inputs. For L0, take every L0 file overlapping their range,
as RocksDB does, and only if no other L0 compaction is running. Then for
j = n, n+1, ..., last:

1. Add the files at level j that overlap the key range gathered so far, and
   expand them to a clean cut.
2. **Stop** (no deeper j is allowed) if:
   - a file at level j is already being compacted;
   - B(j) > `max_compaction_bytes`;
   - a running compaction writes into any level in (start, j] within this key
     range. Moving newer data past that level would put it below older data
     the running compaction is about to write there, which breaks sequence
     ordering.
3. Score level j by the mode's rule (below).

The result is the best level that was scored before any stop. If that level is
not deeper than n, the function returns false and nothing changes.

On success it sets `compaction_inputs_` (one entry per level start..t),
`output_level_ = t`, the grandparents (the first level below t with
overlapping files), and `compaction_reason_ = kSkipLevelTombstone`.

### 4b. Mode 1: always

Take the deepest level that passes the safety stops. Cost is ignored. This mode
is the "upper bound on tombstone removal" setting.

### 4c. Mode 2: amortized cascade (v3+)

Compare two ways of moving the S start bytes from n down to t:

- **Cascade.** The first hop start -> n costs B(n) either way. Every later
  one-level hop of S bytes costs about **(1 + T) · S** in steady state: S bytes
  of its own plus about T · S of overlap at the next level. That overlap is
  shared with all the other data flowing through, so (1 + T) · S is the
  per-hop cost charged to *these* bytes.
- **Skip.** Pay the extra overlap that is actually there now: B(t) − B(n).

Estimated saving of skipping to t:

```
net(t) = (t − n) · (1 + T) · S  −  (B(t) − B(n))
```

The rule takes the t with the largest net(t) > 0 (ties go deeper), and never
lets B(t) exceed R · B(n). It skips only where the overlap below n is well
under the tree's average, for example where the levels below are sparse in this
key range. Most of the time no deeper t qualifies and the compaction stays one
level deep.

Worked example (from the unit tests, T = 10, S = 100, one hop ≈ 1100):

| overlap at L2 / L3 / L4 | net(L3) | net(L4) | result |
|---|---|---|---|
| 1000 / 10000 / 100000 | 1100 − 10000 < 0 | 2200 − 110000 < 0 | no skip |
| 1000 / 500 / 100000 | 1100 − 500 = 600 | 2200 − 100500 < 0 | skip to L3 |
| 100 / 100 / – | 1100 − 100 = 1000 | – | skip to L3 |

## 5. Why this saves write amp on seqdel and churn

- The saving comes only from size-driven compactions that would run anyway (3a).
  When the tombstone-heavy data meets a sparse region below, one compaction
  replaces two or more one-level rewrites of the same tombstones.
- **seqdel:** deletes sweep the key space in order, so parts of the lower
  levels are already thinned out where new tombstones arrive.
- **churn:** the hot keys are deleted and re-inserted over and over, so the
  deep overlap for a hot-key file is small. The base level is often nearly
  empty in its range, and L0 -> base+1 directly is cheaper than L0 -> base
  followed by base -> base+1.
- The gain is modest (a few percent) because mode 2 skips only where it is
  clearly cheaper. Most compactions are unchanged.

## 6. Design history

| version | change | what the numbers showed |
|---|---|---|
| v1 | mode 2 scored depth/ratio | degenerate: never skipped |
| v2 | mode 2 = cascade-savings bound, B(t)/B(n) ≤ R | +18..+238% write amp: it charged the cascade at the *current* overlap at every level, ignoring that the overlap is shared (amortized), so it skipped far too often |
| v3 | mode 2 = amortized rule (4c) | skip-only −2.0% (seqdel), −1.9% (churn); trigger configs still +112%/+394% |
| v4 | trigger only runs compactions that actually skip; hysteresis flag | 5 reps: skip-only −2.8% / −2.4%; trigger + skip still +4.6% / +38% |
| v5 | trigger ignores L0 files | see RESULTS.md "v5" |

**Why v4's trigger still cost write amp (confirmed in v5).**
`results/diag_v4_l0` adds a counter of L0 files per compaction out of L0:

| v4, 1 run | L0 compactions | L0 files each | MB read per L0 file |
|---|---|---|---|
| churn base | 56 | 2.00 | 0.85 |
| churn skip only | 56 | 2.00 | 1.7 |
| churn trigger + skip | 113 | **1.00** | **2.8** |
| seqdel trigger + skip | 86 (56 trigger) | **1.51** | |

What the counter shows:

- Every L0 file spans nearly the whole key range, so a compaction out of L0
  rereads about the same overlap however many L0 files it takes.
- The trigger picked each dense L0 file as soon as it was flushed, so it paid
  that overlap once per file instead of once per batch.

The amortized rule could not see this. It compares "skip now" with "cascade
now", but the real alternative to a trigger compaction is "wait for the L0
file-count trigger, which batches files". Charging the trigger for its whole
compaction instead of B(t) − B(n) was considered and rejected. Churn's real
per-level cost is about 1.2 × S, far below the (1 + T) · S = 6 S estimate, so
the estimate would still have approved these compactions.

v5 therefore removes L0 from the trigger's candidates:

- Dense L0 files wait for the L0 file-count compaction, which still skips
  levels through 3a.
- The trigger keeps working for L1 and below. There, files are compacted one at
  a time anyway, so waiting does not batch anything.

## 7. Options

| option | default | meaning |
|---|---|---|
| `skip_level_tombstone_compaction` | 0 | 0 off, 1 always (deepest feasible), 2 amortized cascade |
| `skip_level_tombstone_density_threshold` | 0.5 | density θ that makes a file dense |
| `skip_level_tombstone_density_trigger` | true | dense L1+ files also trigger compactions (3b); false = only extend size picks (3a) |
| `skip_level_max_overlap_ratio` | 20 | mode 2 cap: B(t) ≤ R · B(n) |

All four are mutable through `SetOptions()`. With the feature off
(`skip_level_tombstone_compaction = 0`), the picker and
`ComputeTombstoneDenseFiles()` return immediately; the only per-file cost is
the in-memory `from_skip_level_compaction` bool.

## 8. Correctness notes

- Inputs from every level start..t go into one compaction, so its merging
  iterator sees all versions of each key and keeps the usual newest-wins and
  snapshot rules. Tombstones still needed by a snapshot are kept (tested in
  `SnapshotKeepsTombstonesButDataIsCorrect`).
- Every intermediate level is clean-cut expanded, and the stop rules in 4a stop
  the skip at any level that a running compaction writes into within this key
  range. This keeps newer data from ending up below older data.
- `RandomizedAgainstModel` in `db/db_skip_level_compaction_test.cc` checks
  random puts, deletes and range deletes with auto compaction against an
  in-memory model in all three modes (0, 1, 2), and every benchmark run verifies all keys at the end.
