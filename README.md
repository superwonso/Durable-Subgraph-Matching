# Abstract

Durable subgraph matching schemes have been proposed to discover subgraph patterns that occur consecutively
across multiple snapshots of a temporal graph. However, during candidate selection, the cost of isomorphism test
increases because the entire temporal range must be scanned. In addition, decomposing the query without considering
the characteristics of temporal graphs generates many unnecessary candidates. In this paper, we propose an efficient
durable subgraph matching scheme in temporal graph. The proposed scheme reduces the search space in the candidate
selection phase by applying a filter during initial snapshot merging to remove edges that can never form durable
subgraphs. It also accelerates overall processing by decomposing the query with a selectivity formula that reflects
temporal graph characteristics so that fewer candidates are produced. 

# Evaluation environments

The historical project notes used Windows 11 23H2 and g++ 14.2.0. The audited
evaluation in `evaluation/` was run under WSL2 on an AMD Ryzen AI MAX+ 395
(16 cores/32 threads, 23.46 GiB RAM) with g++ 13.3.0, C++17, and `-O3`.
ROCm 7.2.4 accesses the Radeon 8060S (`gfx1151`) through WSL `/dev/dxg`.
The exact CPU/GPU environment and execution evidence are recorded in
[`evaluation/environment.json`](evaluation/environment.json).

# Implementations

| Directory | Semantics | Build and usage |
| --- | --- | --- |
| `original` | Paper-faithful TD-Tree cardinality by default; explicit consecutive compatibility mode | [`original/README.md`](original/README.md) |
| `ours` | Algorithm 3.1 prefilter + printed temporal root score + exact consecutive matching | [`ours/README.md`](ours/README.md) |
| `ours_gopher` | CPU parameterized temporal candidate DAG, bounded dependency-aware reuse, monotonic pruning, reuse-aware order, compatibility root tasks, and bounded dynamic-prefix parallelism | [`ours_gopher/README.md`](ours_gopher/README.md) |
| `ours_gopher_gpu` | ROCm level-synchronous and resident-continuation Candidate-DAG frontier executors without CPU runtime memo tables | [`ours_gopher_gpu/README.md`](ours_gopher_gpu/README.md) |
| `TurboISO` | Timestamp-collapsed static directed baseline | [`TurboISO/README.md`](TurboISO/README.md) |

The three durable implementations provide Linux build/test scripts:

```bash
./original/build.sh && ./original/run_tests.sh
./ours/build.sh && ./ours/run_tests.sh
./ours_gopher/build.sh && ./ours_gopher/run_tests.sh
./ours_gopher_gpu/build.sh
RUN_GPU_TESTS=1 ./ours_gopher_gpu/run_tests.sh
```

On Windows, each directory also provides PowerShell scripts. For example:

```powershell
powershell -ExecutionPolicy Bypass -File .\ours\build.ps1
powershell -ExecutionPolicy Bypass -File .\original\run_tests.ps1
powershell -ExecutionPolicy Bypass -File .\TurboISO\run_tests.ps1
```

# Temporal Graph File Format

Each data row contains three integers, `src dst time`, and represents the
directed arc `src -> dst` at one snapshot. The datasets do not contain vertex
labels, so sorted raw vertex IDs receive reproducible random labels from
`{A, B, C, D, E}` using `std::mt19937` and seed 42 by default. Reciprocal arcs
remain distinct.

# Method and semantic boundary

The durable implementations use:

```text
td_tree.exe <data_graph> <query_graph> <minimum_duration_k> [label_seed]
```

`original` follows the 2022 paper's common-time-instance cardinality
definition; those instances need not be consecutive. Use
`--duration-mode consecutive` only for a same-semantics comparison with
`ours` and `ours_gopher`. All performance runs use `--count-only`.

`ours_gopher/td_tree_gopher` additionally exposes `--selectivity`, `--order`, `--memo`,
bounded-cache, early-pruning, statistics, digest, CPU-thread, and
`--parallel roots|prefix` options. Its README documents which settings are
used for uninstrumented runtime versus mechanism diagnostics.

`ours_gopher_gpu/td_tree_gopher_gpu` preserves the consecutive semantics and
fixed Candidate-DAG plan on ROCm. `--gpu-variant level|resident` selects the
compatibility level frontier or the new device-resident continuation pipeline.
It is an experimental GPU adaptation, not a literal GPU implementation from
the CPU-only Gopher paper. GPU result records therefore state
`memo_effective=device_fused_no_runtime_memo` and never use a silent CPU
fallback.

The TurboISO converter collapses timestamps by ordered pair because TurboISO
is a static, not durable, comparison baseline.

# Reproducible evaluation

The experiment manifest, isolated runner, witness-backed queries, synthetic
reuse workload, and Excel generator are under [`benchmark/`](benchmark/).
They cover all four supplied datasets, witness-backed Bitcoin query sizes
3/4/5, `k=3/4/5`, raw/filtering cases, explicit selectivity ablation, Gopher
pruning and cache factors, CPU scaling from 1 to 32 threads, and ROCm GPU
cold-start break-even.

```bash
python3 benchmark/run_benchmarks.py \
  --manifest benchmark/experiment_manifest.json \
  --output evaluation/final_gpu_20260729

python3 benchmark/create_excel.py \
  --input-dir evaluation/final_gpu_20260729 \
  --output evaluation/dsm_performance_evaluation_gpu_20260729.xlsx \
  --baseline original_consecutive
```

See [`benchmark/README.md`](benchmark/README.md) for the protocol and
[`evaluation/IMPLEMENTATION_NOTES.md`](evaluation/IMPLEMENTATION_NOTES.md) for
paper-to-code findings and interpretation limits. The completed run is
summarized in [`evaluation/RESULTS.md`](evaluation/RESULTS.md). The validated
14-sheet report, including dedicated CPU single-thread and GPU evaluation
sheets, is
[`evaluation/dsm_performance_evaluation_gpu_20260729.xlsx`](evaluation/dsm_performance_evaluation_gpu_20260729.xlsx).

The focused follow-up for dynamic CPU scheduling and GPU resident execution is
documented in
[`evaluation/PARALLEL_V2_RESULTS.md`](evaluation/PARALLEL_V2_RESULTS.md).
Its validated 486-run, 16-sheet workbook is
[`evaluation/dsm_parallel_v2_evaluation_20260729.xlsx`](evaluation/dsm_parallel_v2_evaluation_20260729.xlsx);
it includes explicit CPU 1-thread results, 1–32-thread scaling, worker
load-balance counters, GPU end-to-end/kernel decomposition, and per-depth
frontier diagnostics.

Those broad/architecture workbooks remain useful as component studies, but
they are not the authoritative dataset-wise baseline comparison after the
raw-versus-filtered input rule was clarified. The two protocol-corrected
workbooks below are the headline artifacts.

The protocol-corrected dataset-wise table fixes TD-Tree consecutive CPU 32T
as the baseline and compares Ours CPU 32T, Ours+Gopher CPU 32T, and the
resident GPU executor over five repetitions. TD-Tree reads each full raw file;
the three proposed methods read the corresponding precomputed `_filtered`
file. Stable-ID labels keep a vertex's synthetic label identical across the
raw/filtered pair. The reported wall time is therefore a precomputed-filter
online/system-level comparison and does not include creation of the filtered
files. The validated workbook is
[`evaluation/dsm_dataset_32t_comparison_20260729.xlsx`](evaluation/dsm_dataset_32t_comparison_20260729.xlsx),
and its interpretation and nonzero match-count audit are in
[`evaluation/DATASET_32T_RESULTS.md`](evaluation/DATASET_32T_RESULTS.md).
The authoritative raw results are in
[`evaluation/dataset_32t_protocol_corrected_20260729/`](evaluation/dataset_32t_protocol_corrected_20260729/).

The controlled Selectivity/candidate-volume extension adds
`ours --prefilter on|off`, the TD-Tree/paper/intended score arms, a common
raw logical-extension counter, Gopher early-pruning by `k`, and GPU resident
memory-budget ablation. The primary Selectivity comparison runs all three
scores on the same full raw file for every dataset; bounded 5% raw samples are
retained only as supplementary factorial experiments. Its 254-run workbook is
[`evaluation/dsm_selectivity_candidate_ablation_20260729.xlsx`](evaluation/dsm_selectivity_candidate_ablation_20260729.xlsx),
with interpretation in
[`evaluation/SELECTIVITY_ABLATION_RESULTS.md`](evaluation/SELECTIVITY_ABLATION_RESULTS.md)
and reproducible raw rows in
[`evaluation/selectivity_ablation_protocol_corrected_20260729/`](evaluation/selectivity_ablation_protocol_corrected_20260729/).
A portable technical summary with native runtime and full-raw Selectivity
charts is
[`evaluation/protocol_corrected_report_20260729/report.html`](evaluation/protocol_corrected_report_20260729/report.html).

# Papers 

An Efficient Durable Subgraph Matching Scheme on Temporal Graphs, D.H. Cha, M.S. dissertation, Dept. of Info. & Comm. Eng., Chungbuk National Univ., Cheongju, Chungbuk, Korea, 2025

# Compared algorithms

| Algorithm | Paper |
| --- | --- |
| DSM | Durable Subgraph Matching on Temporal Graphs |
| TiNLA | TurboISO: Towards Ultrafast and Robust Subgraph Isomorphism Search in Large Graph Databases |
| CECI | CECI: Compact Embedding Cluster Index for Scalable Subgraph Matching |
|  |  |

# Datasets



| Datasets | Link |
| --- | --- |
| bitcoin-temporal | https://www.cs.cornell.edu/~arb/data/temporal-bitcoin/ |
| sx-stackoverflow | https://snap.stanford.edu/data/sx-stackoverflow.html |
| wiki-talk-temporal | https://snap.stanford.edu/data/wiki-talk-temporal.html |
| sx-superuser | https://snap.stanford.edu/data/sx-superuser.html |
|  |  |

# References

| Elements | Link |
| --- | --- |
| TiNLA | https://github.com/bookug/siep |
|  |  |
