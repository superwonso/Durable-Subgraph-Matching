# Benchmark suite

This directory contains the reproducible evaluation matrix for:

- `original_cardinality`: the 2022 TD-Tree paper's cardinality semantics;
- `original_consecutive`: TD-Tree in an explicit consecutive-duration
  compatibility mode;
- `ours`: the method described in `appendix/260408_cdh.pdf`;
- `ours_gopher`: the CPU DAG/memo/reuse extension, including root-task and
  bounded dynamic-prefix schedulers;
- `ours_gopher_gpu`: the ROCm level-synchronous and resident-continuation
  Candidate-DAG executors.

The cardinality implementation is intentionally assigned a different
`semantic_group`. Its result count and runtime are reported as a
paper-faithful reference, not as a denominator for speedups against the three
consecutive implementations.

## Build and validate

From the repository root:

```bash
python3 -m pip install -r benchmark/requirements.txt
./original/build.sh
./ours/build.sh
./ours_gopher/build.sh
./ours_gopher_gpu/build.sh

python3 benchmark/run_benchmarks.py \
  --manifest benchmark/experiment_manifest.json \
  --list
```

`--list` checks executable, data, query, and algorithm references without
running a benchmark. The complete manifest currently contains 30 cases,
26 configured algorithm variants, and 996 isolated process runs.

Run the fast integration check first:

```bash
python3 benchmark/run_benchmarks.py \
  --manifest benchmark/smoke_manifest.json \
  --output evaluation/smoke
```

Then run the complete matrix:

```bash
python3 benchmark/run_benchmarks.py \
  --manifest benchmark/experiment_manifest.json \
  --output evaluation/final_gpu_20260729
```

### Parallel architecture v2 follow-up

The focused CPU/GPU architecture comparison uses a separate frozen/current
manifest. It includes explicit CPU 1-thread controls, physical-core-first
affinity, balanced and one-hot-root skew workloads, a paper-selectivity
negative control, GPU break-even sizes, a real Bitcoin query, per-depth GPU
diagnostics, and frozen pre-change binaries:

```bash
python3 benchmark/run_benchmarks.py \
  --manifest benchmark/parallel_v2_manifest.json \
  --output evaluation/parallel_v2_20260729

python3 benchmark/validate_results.py \
  evaluation/parallel_v2_20260729 \
  --manifest benchmark/parallel_v2_manifest.json

PYTHONPATH=benchmark:/tmp/dsm_pydeps \
python3 benchmark/create_excel.py \
  --input-dir evaluation/parallel_v2_20260729 \
  --output evaluation/dsm_parallel_v2_evaluation_20260729.xlsx \
  --title "DSM Parallel Architecture v2 Evaluation (2026-07-29)"
```

The completed focused matrix contains 14 cases, 28 algorithm configurations,
486 isolated runs, and 111 actual GPU runs. Its interpretation report is
[`evaluation/PARALLEL_V2_RESULTS.md`](../evaluation/PARALLEL_V2_RESULTS.md).

### Dataset-wise TD-Tree 32T baseline comparison

The protocol-corrected four-way table uses a true 32-thread TD-Tree
consecutive baseline on each full raw file, while Ours 32T, full Ours+Gopher
32T, and the resident GPU executor read the paired precomputed `_filtered`
file. All four use `--label-mode stable-id`, so the same external vertex keeps
the same synthetic label across the raw/filtered pair:

```bash
python3 benchmark/run_benchmarks.py \
  --manifest benchmark/dataset_32t_manifest.json \
  --output evaluation/dataset_32t_protocol_corrected_20260729

python3 benchmark/validate_results.py \
  evaluation/dataset_32t_protocol_corrected_20260729 \
  --manifest benchmark/dataset_32t_manifest.json

PYTHONPATH=benchmark:/tmp/dsm_pydeps \
python3 benchmark/create_dataset_32t_excel.py \
  --input-dir evaluation/dataset_32t_protocol_corrected_20260729 \
  --filter-materialization-dir evaluation/filter_materialization_20260729 \
  --output evaluation/dsm_dataset_32t_comparison_20260729.xlsx
```

The completed matrix contains four datasets, four methods, five repetitions,
and 80 isolated runs. It explicitly reports exact match counts, requested and
actual CPU threads, workers that received tasks, GPU fallback state, medians,
quartiles, and speedups. Because filtered files are precomputed, these
speedups are online/system-level raw-baseline-versus-filtered-proposed
comparisons; they exclude filtered-file construction and are not the paper's
filter-inclusive end-to-end measurement. See
[`evaluation/DATASET_32T_RESULTS.md`](../evaluation/DATASET_32T_RESULTS.md).

The one-time raw-to-`_filtered` cost is measured separately with the
repository materializer:

```bash
python3 benchmark/run_filter_materialization_benchmark.py \
  --repeats 5 \
  --output-dir evaluation/filter_materialization_20260729
```

This utility is pinned to one logical CPU. Its primary metric is full
application-process wall time; the narrower internal core and serialization
timers do not include the complete raw read/group/close boundary. Generated
artifacts remain under temporary directories, and the first result for each
dataset is canonical-sorted and compared with the supplied `_filtered` file.
See
[`evaluation/FILTERING_TIME_RESULTS.md`](../evaluation/FILTERING_TIME_RESULTS.md).

Interrupted work can be resumed without rerunning completed identities:

```bash
python3 benchmark/run_benchmarks.py \
  --manifest benchmark/experiment_manifest.json \
  --output evaluation/final_gpu_20260729 \
  --resume
```

Selection flags may be repeated:

```bash
python3 benchmark/run_benchmarks.py \
  --manifest benchmark/experiment_manifest.json \
  --output evaluation/gopher_scaling \
  --case gopher_reuse_star_cpu_scaling_k3 \
  --algorithm ours_gopher_reuse_t1 \
  --algorithm ours_gopher_reuse_t2 \
  --algorithm ours_gopher_reuse_t4 \
  --algorithm ours_gopher_reuse_t8 \
  --algorithm ours_gopher_reuse_t16 \
  --algorithm ours_gopher_reuse_t32
```

## What is measured

The manifest covers the experiment axes described in `260408_cdh.pdf` and
additional mechanism checks:

- the four supplied real datasets with a common four-edge query shape;
- query sizes of 3, 4, and 5 edges at `k=3`;
- `k=3,4,5` for a fixed three-edge query;
- query size and `k` increased together;
- path and cycle topology at the same edge count;
- raw-input preprocessing on Superuser and Wiki-Talk;
- history-preserving 5% samples for the larger Stack Overflow and Bitcoin
  inputs;
- Gopher pruning on/off followed by a 2x2 memo factorial: no memo,
  structural-only, temporal/longest-run-only, and both memo families, plus a
  separate reuse-aware-order comparison;
- cache budgets of 0, 64, 256, and 1024 MiB, plus cost-aware admission on/off;
- CPU scaling at 1, 2, 4, 8, 16, and 32 threads.

The CPU scaling and reuse experiments use
`benchmark/data/reuse_star_scaling.txt`, a deterministic 4,843,093-row
workload generated with:

```text
centers=5000
leaves_per_label=20
neighbors_per_label=8
transient_edges=100000
label_seed=42
```

This larger workload avoids drawing thread-scaling conclusions from the
roughly 0.16-second default synthetic case.

The first ablation stage does not disable all temporal checks. It keeps the
candidate-local requirement `maxRun(S) >= k` and losslessly drops interval
fragments shorter than `k`; it disables only the early branch rejection after
the current partial mapping's support `I(phi)` is intersected with `S`.

Every `--engine gopher` configuration already compiles and executes the
Candidate-DAG. Consequently structural-only and temporal-only are independent
cache components, not cumulative stages that introduce the DAG. In tables,
`--memo dag` means both memo families are enabled.

## Measurement protocol

Every run:

1. uses label seed 42 and count-only output;
2. executes in its own directory so fixed output names cannot collide;
3. is wrapped by GNU `time` and GNU `timeout`;
4. records stdout, stderr, resource use, stage timings, match count, and all
   available Gopher JSON/statistics fields;
5. is repeated three times by default, with five repetitions for the synthetic
   Gopher diagnostics.

Algorithms are interleaved by repetition, and their first position is rotated
deterministically per case. This limits implementation-order and operating
system page-cache bias. The workbook should use medians as the headline value
and retain dispersion and execution position for auditability. File-read,
preprocessing, index-build, enumeration, and end-to-end timings remain
separate; an end-to-end result should not be presented as pure search time.

Primary real-data, paper-axis, and CPU-scaling Gopher variants explicitly use
`--statistics off --result-digest off`, so diagnostic accounting is not charged
only to the proposed runtime. Separately named `*_stats_*`, memo-ablation,
cache-capacity, and admission variants use `--statistics on` to collect the
mechanism counters and write `gopher_metrics.json` in their isolated run
directory. The smoke manifest additionally enables the digest and DAG
cross-check because it is a correctness run rather than a timing claim.

`correctness.csv` reports `pass` only when every expected algorithm in the
semantic group completed every repetition, each implementation was stable
across repetitions, and all counts agreed. A timeout, error, overflow, or
missing implementation is `missing`; conflicting concrete counts are `fail`.
A one-algorithm cardinality group therefore verifies repeated determinism, not
cross-semantic equivalence.

The output directory contains:

- `raw_runs.jsonl`: one typed record per process;
- `runs.csv`: the same records as a flat table;
- `summary.csv`: medians, ranges, means, and standard deviations;
- `correctness.csv`: semantic-group count reconciliation;
- `gpu_depths.csv`: one row per GPU repetition and Candidate-DAG depth,
  including frontier width, load-skew, grid, kernel, and round-trip fields
  when collected;
- `candidate_rows.csv`: normalized per-query-vertex TD relation/candidate
  summaries;
- `selectivity_rows.csv`: every query vertex's label count, degree, average
  lifespan, score, root flag, and order position;
- `manifest.used.json`: the exact configuration used;
- `environment.json`, `dataset_inventory.csv`, and `methodology.csv`: copied
  reporting sidecars used by the Excel generator;
- `validation_receipt.json`: final run, correctness, workbook, and binary
  integrity receipt for the completed evaluation;
- `runs/<identity>/`: command stdout, stderr, GNU time output, matcher output,
  and Gopher metrics JSON.

Validate the completed matrix before reporting it:

```bash
python3 benchmark/validate_results.py \
  evaluation/final_gpu_20260729 \
  --manifest benchmark/experiment_manifest.json
```

Generate and then independently re-open the Excel workbook:

```bash
python3 benchmark/create_excel.py \
  --input-dir evaluation/final_gpu_20260729 \
  --output evaluation/dsm_performance_evaluation_gpu_20260729.xlsx \
  --baseline original_consecutive

python3 benchmark/create_excel.py \
  --validate-only evaluation/dsm_performance_evaluation_gpu_20260729.xlsx
```

The focused 254-run Selectivity/candidate/pruning/GPU-memory matrix uses:

```bash
python3 benchmark/run_benchmarks.py \
  --manifest benchmark/selectivity_ablation_manifest.json \
  --output evaluation/selectivity_ablation_protocol_corrected_20260729

PYTHONPATH=.deps python3 benchmark/create_selectivity_ablation_excel.py \
  --input-dir evaluation/selectivity_ablation_protocol_corrected_20260729 \
  --output evaluation/dsm_selectivity_candidate_ablation_20260729.xlsx
```

## Data and interpretation boundaries

The corrected headline and full-raw Selectivity cases use stable-ID labelling:
the label is a deterministic function of label seed and external vertex ID,
not input traversal order. The stable witness queries in
`benchmark/queries/stable_id/` are therefore label-aligned across raw and
filtered inputs. The legacy default `sequential-rng` remains available for
backward compatibility, but it must not be used to compare a raw file with a
vertex-subset filtered file.

The primary Selectivity score comparison uses the same complete unfiltered
file for TD-Tree, paper, and intended modes on all four datasets, including the
43.7-million-row Stack Overflow and 94.7-million-row Bitcoin inputs. Existing
deterministic 5% raw edge-history samples remain in the workbook only as
resource-bounded supplementary factorial experiments. Filtering and Gopher
component ablations use the input variant appropriate to the isolated
component and expose that variant explicitly.

The sampled real-data queries are concrete-witness queries, not the unreleased
query set used for the figures in `260408_cdh.pdf`. Results reproduce the
paper's experimental axes, but they are not numerical replications of the
paper's hardware, query population, or published bars.

For the fixed-query `k=3,4,5` series, the Wiki-Talk path query was sampled with
a witness at `k=5` and the identical query file is reused at all three
thresholds. The combined size/threshold cases likewise use query files sampled
at their tested maximum `k`; a zero result therefore cannot be attributed only
to an accidentally unsupported sampled pattern.

ROCm GPU results are valid only when the Radeon device is detected through
WSL `/dev/dxg` and a real HIP transfer/kernel/verification test passes. The
`ours_gopher_gpu` prototype is a fused device-resident Candidate-DAG frontier
executor without the CPU dependency-key runtime memo tables. Its report uses
both a one-thread CPU no-memo reference and the full CPU Gopher configurations;
kernel-only timing is never presented as end-to-end speedup.

The completed 996-run matrix takes roughly one hour on the current
16-core/32-thread Ryzen plus Radeon host. Five-edge queries can grow
combinatorially; their 1800-second bounds intentionally turn non-completion into
an auditable result rather than allowing an unbounded run. Use `--case` first
when estimating a different machine's runtime, then continue with `--resume`.

## Extending a case

Shared case flags are appended with `extra_flags`. Flags that apply only to one
implementation can be supplied with `algorithm_flags`:

```json
{
  "id": "example",
  "data": "{root}/path/to/data.txt",
  "query": "{root}/path/to/query.qry",
  "k": 3,
  "algorithms": ["ours", "ours_gopher_reuse_t1"],
  "extra_flags": [],
  "algorithm_flags": {
    "ours_gopher_reuse_t1": ["--cache-min-work", "256"]
  }
}
```

Do not place a Gopher-only option in `extra_flags`, because the same list is
passed to every selected algorithm.
