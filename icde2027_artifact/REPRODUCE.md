# Reproducing the released measurements

Use Linux, Python 3.10+, CMake, a C++17 compiler, OpenMP and GNU `/usr/bin/time`.
See [SOURCES.md](SOURCES.md) for snapshot roles and [DATASETS.md](DATASETS.md)
for data acquisition, time bins, labels and the two Bitcoin inputs. No raw
dataset, unpublished draft or private workspace is required by the code.

Commands below are run from `icde2027_artifact/`. Python preparation/run
commands are **dry-run by default**. Add `--execute` only when ready. Store and
label preparation can be large; the full performance schedule takes many
hours. No full experiment was repeated merely to create this release.

## 1. Build and smoke test

```sh
bash scripts/build.sh --jobs 4
python3 scripts/smoke_test.py --help
```

The snapshots preserve measured algorithms. `v7` supplies DFS/probe/W-store/
final; `v8` supplies the raw-bitset control and TD-Tree/TiNLA baseline adapters;
`v9` supplies W6 padding and block-union follow-ups; `profile_v9` supplies the
E5 profile instrument. `v6` is retained for the historical optimization ladder.
Build outputs are local and are not committed. `--build-root DIR` is accepted
by build, store and run scripts.

## 2. Prepare a new data cohort

Download/uncompress a provider file first. For example, weekly SuperUser:

```sh
python3 scripts/prepare_data.py convert --input /data/source/sx-superuser.txt --output /data/dsm-cohort/su-week --mode unix-week --dataset sx_superuser --execute
python3 scripts/prepare_data.py convert --input /data/dsm-cohort/su-week/raw.txt --output /data/dsm-cohort/su-f2 --mode weekly --dataset sx_superuser --execute
python3 scripts/prepare_data.py bind --data-root /data/dsm-cohort --graph /data/dsm-cohort/su-week/raw.txt --reference-sha 44bef58c9dc8f7887360a604e918f0ef7397e730c7496c9ef1738bdc6fcbc0cb --execute
python3 scripts/prepare_data.py bind --data-root /data/dsm-cohort --graph /data/dsm-cohort/su-f2/f2.txt --reference-sha 2c44e53e1569c97d14b33ffe4979d9a4c2c54c997da76e51bd63f24b9bf55e61 --execute
```

The two `convert` operations produce new outputs and refuse existing outputs.
For monthly E4 data, use `--mode month` directly on Unix timestamps; for E7
day statistics use `--mode day --stats-only`. Bind the resulting raw/F2 files
to the corresponding IDs listed in `configs/graphs.json`. Binding records
actual SHA, reference SHA and whether the cohort is regenerated or byte-exact.
If archived data are already arranged under their recorded relative paths,
`--data-root` can point to that archive without binding. This archive fallback
checks the original SHA. `--archive-exact` explicitly enforces it when binding.

For an explicit-label case, prepare labels for each needed boundary:

```sh
python3 scripts/prepare_data.py labels --case m4v2-sx_superuser-q04-diamond-probe --boundary prepared --data-root /data/dsm-cohort --execute
python3 scripts/prepare_data.py labels --case m4v2-sx_superuser-q04-diamond-probe --boundary raw --data-root /data/dsm-cohort --execute
```

`--case all` prepares all explicit cases, but requires every input to be bound
first. Shared input/cardinality maps are reused; no selected query changes.

## 3. Build reusable stores

```sh
python3 scripts/build_stores.py --case system__sx_superuser__path3_k3 --data-root /data/dsm-cohort --execute
```

This creates W(k=2,3,4,5) with `v7/build_wstore` and raw-bitset k=1 with
`v8/build_raw_bitset_store_v8`. Store headers bind the exact input SHA. Build
the five weekly file identities separately; do not replace either Bitcoin
input with the other. Store build/loading costs remain available in their
native reports and are not included in the prepared-query timer.

## 4. Inspect and execute cases/panels

Configure the host with SMT off and CPUs 0–15 available. The measured protocol
uses 16 threads, affinity 0–15, 28672 MiB address-space limit, fresh processes,
count-only, zero warmups and no page-cache flush. Scripts validate rather than
change host SMT/affinity. They do not use elevated privileges.

```sh
python3 scripts/run_case.py --case system__sx_superuser__path3_k3 --arm final --data-root /data/dsm-cohort --output /data/new-results/one-case
python3 scripts/run_panel.py --panel E1 --data-root /data/dsm-cohort --output /data/new-results/E1
```

The first prints the native command; the second prints panel size and its
first command. Add `--execute` for actual measurements. The `--output` folder
must not be the published `results/` directory. `run_panel.py` resumes complete
journaled records, refuses unfinished per-run directories, and never retries.
Creating `STOP_AFTER_CURRENT` inside its output folder stops between runs.
Every successful consecutive count must match the frozen reference; mismatch
creates `STOP.json`. E3 cardinality checks its documented lower bound against
the consecutive count rather than incorrectly requiring equal counts.

| Manifest | Scope and repetitions |
|---|---|
| `V8_MAIN` | Published 495-cell baseline/ladder layout; replay of the recorded repeat allocation, including fixed historical budget skips |
| `E1` | 14 frozen cases × bitset/W/W6; 12 nominal |
| `E6` | Frozen union35 × W/union/final; heavy 12, small 10, SuperUser small 15; shared cases use one cell |
| `E3` | Eight TD-Tree raw cardinality checks and one system Bitcoin consecutive check; once, 1800 s |
| `E4A_SCREEN` | 24 monthly Bitcoin DFS screening cases; once, 600 s |
| `E4A` | The **published selected** 22 monthly Bitcoin cases × five arms × six repeats |
| `E4B_SCREEN` | 44 monthly StackOverflow DFS screening cases; once |
| `E4B` | Empty: the published screening found no case meeting 20 ms; no invented repeated measurements |
| `E5` | Three TD-Tree profile cases; once, diagnostic rather than a performance arm |
| `E1_COUNTERS` | Fourteen extra bitset counter observations; diagnostic |

`E1_REAL_CORRECTNESS` and `E6_REAL_CORRECTNESS` expose the recorded real-input
count checks, but they are optional and not an expensive mandatory gate for
public reruns. All schedules preserve the published interleaved case/arm order.
The v9 performance policy reduces a cell to six repetitions if its first wall
exceeds 60 s and skips remaining repetitions after a first timeout. V8 used a
different 48-hour forecast decision, limited to eligible heavy TD/TiNLA cells;
the public `V8_MAIN` replays that **already published allocation**, rather than
claiming to redo its old forecast. All timeout caps are 600 s except E3.
No interval estimates, replacements of TO/OOM, or new query selection occur.
E4A reruns deliberately keep the published selected panel rather than selecting
different queries based on new machine timings. Its original screen rule was
successful DFS within 120 s; E4B's was at least 20 ms with fixed stratification.

## 5. Output and timer definitions

Each run saves the native JSON/timing text, stdout/stderr, command and binary
SHA, process result, and a final record. A panel adds `raw_runs.jsonl`, host
environment and the manifest identity. Host/child swap observations mark a run
rejected without retry. TO/OOM values are not imputed.

`enumeration_s` is the native enumeration phase. `search_s` is enumeration plus
query-specific new index preparation (`enum_plus_build_nanoseconds`) for ours;
for baselines it is native enumeration. `query_s` adds planning and TD/relation
construction, with reusable W-store loading subtracted from planning. This is
the prepared-input baseline comparison boundary. All exclude graph file read,
F2 preparation and reusable-store construction. `wall_s` separately records
the full fresh-process lifetime and must not be mistaken for those online
metrics. The native source remains the authority for its timer fields.

Use [RESULTS.md](RESULTS.md) and `scripts/summarize_results.py` for inspecting
the released evidence; new cohort results must not be mixed into published
tables. Normal input SHA verification is cached per file stat within a panel.
We did not add exhaustive semantic audits, bootstraps or hours-long release
experiments: the author requested a paper-appropriate reproducibility package,
not those additional expensive gates.
