# Original implementation

This directory contains the corrected TD-tree baseline from *Durable Subgraph
Matching on Temporal Graphs* (Li, Zou, and Li, TKDE 2022/2023). Its default
durability semantics follow Section 2 and Algorithms 1 and 3 of that paper:
a matching is `k`-durable when the cardinality of its common time-instance set
is at least `k`. Those instances do **not** have to be consecutive.

The implementation preserves the baseline's algorithmic differences from
`ours`:

- it retains every directed edge history and does **not** apply Algorithm 3.1's
  consecutive-pair prefilter; and
- it chooses the DFS root by minimizing
  `S_topo(u) = |V_L(u)| / (out_degree_Q(u) + in_degree_Q(u))`.

This numerator/denominator was checked directly against Section 4 of the
TD-Tree paper and is intentional. `260408_cdh.pdf` later describes TD-Tree
with the reciprocal `deg_Q(u) / |V_L(u)|`; that later equation, not this
baseline implementation, is the source of the mismatch. `ours` and
`ours_gopher` retain the later PDF's printed formula as selectivity mode
`paper` and expose `intended = |V_L(u)| / (deg_Q(u) * A_dur(u))` as a
separately labelled, prose-consistent ablation.

The TD-tree is path-specific, as in Algorithm 1: each parent **cell** points to
its own child block, root-to-current tree-path time sets are intersected during
growth, and each leaf cell stores its root-to-leaf time-instance set. Unsupported
subtrees are removed with the cascading semantics of Algorithm 2. Enumeration
joins leaf time sets using Equation (1), uses Section 7.1's sorted path order,
uses Section 7.2's union/difference compaction for leaf blocks, checks non-tree
arcs and injectivity exactly, and reports the exact common time-instance set.

Every data row `u v t` means `u -> v` at time instance `t`. Repeated timestamps
for an ordered pair are deduplicated and its full set is stored compactly as
intervals. Intervals are only a lossless representation: separated runs jointly
count toward cardinality. Reciprocal arcs and self-loops remain separate
directed histories. Sparse external vertex IDs are compacted internally and
restored in output.

The datasets have no vertex labels, so sorted external IDs receive uniformly
random labels from `A` through `E` using `std::mt19937`. The default seed is
`42`; an optional positional seed selects another seed. The same
seed gives the same labels even if input rows are reordered.

For comparisons in which Original reads the raw graph while a proposed method
reads its `_filtered` vertex subset, pass `--label-mode stable-id` to every
executable. In this opt-in mode, each label is derived only from the 32-bit
label seed and the non-negative external vertex ID using the same fixed
64-bit SplitMix finalizer, then reduced modulo five. The same external ID
therefore receives the same A-E label even when the two input files contain
different vertex sets. `--label-mode sequential-rng` is the backward-compatible
default. Timing output records both `label_mode` and `label_seed`.

Query rows are also directed. `A B` means query arc `A -> B`. When multiple
query vertices share a label, give them distinct identities with `id:label`,
for example `left:A center:B` and `center:B right:A`.

For a same-problem comparison with an implementation that requires one common
run of `k` consecutive snapshots, pass `--duration-mode consecutive`. This is
an explicit compatibility mode; `cardinality` is the paper-faithful default.
`k=1` is valid in both modes because Original applies no consecutive-pair
prefilter.

## Build and run

```powershell
./build.ps1
./td_tree.exe ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42
./td_tree.exe ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42 --count-only
./td_tree.exe ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42 `
    --duration-mode consecutive --count-only
./td_tree.exe ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42 `
    --duration-mode consecutive --count-only --threads 32
```

On Linux/macOS:

```bash
./build.sh
./td_tree ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42
./td_tree ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42 \
    --duration-mode consecutive --count-only --threads 32
./td_tree ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42 \
    --label-mode stable-id --duration-mode consecutive --count-only
```

`--count-only` may appear before or after the optional seed. Exact candidate
enumeration and durability checks still run, but individual `Match ...` rows
are omitted. Candidate summaries and the final exact count remain in the result
file. `--duration-mode cardinality|consecutive` may likewise appear before or
after the optional seed (the `--duration-mode=value` spelling is also accepted).
Unknown options, duplicate options, invalid modes, and multiple positional
seeds are rejected with a nonzero exit code.
`--label-mode sequential-rng|stable-id` may also appear before or after the
seed, including the `--label-mode=value` spelling.

`--threads N` enables true root-task parallel enumeration for
`--count-only`; `N` must be from 1 through 1024. Each worker owns its mapping,
injectivity bitmap, TD-tree cell positions, and checked counter while reading
the immutable graph and TD-tree. Counts are reduced with overflow checking
after all workers join. Full mapping output remains serial even when a larger
thread count is requested so match numbering and row order stay deterministic.
Count-only launches the requested worker count even when the query has fewer
root tasks; `workers_with_tasks` exposes that unavoidable workload starvation.
Result and timing files distinguish `threads_requested`, `threads_used`,
`workers_with_tasks`, and root-task min/max. Input loading, preprocessing,
query decomposition, and TD-tree construction remain serial, so headline
32-thread results are end-to-end system measurements rather than pure
enumeration scaling.

The executable writes dataset-specific `matching_results_*.txt` and
`timing_results_*.txt` files in the current directory. Timing separates raw
file parsing (`readTemporalGraph`) from sorting, deduplication, interval
compression, label assignment, compact graph construction, and statistics
(`preprocessTemporalGraph`). It also reports TD-tree build, exact enumeration,
and end-to-end time.
Both result and timing files record `mode: full` or `mode: count-only`.
They also record `duration_mode` and `minimum_duration_k`, so benchmark results
from the two semantics cannot be silently mixed.

Candidate reporting distinguishes the path-cell TD-tree index size
(`candidate_relation_entries`) from dynamic search work. Under
`metric_schema_version=logical_extensions_v1`,
`raw_extension_candidates` counts attempted non-root
query-vertex/data-vertex assignments, `post_structural_candidates` is the
same TD-tree-row count for this executor, and `accepted_extensions` counts
assignments that recurse. Root candidates are reported separately. Only the
raw-extension field is used for cross-engine candidate ratios; later stage
boundaries can differ in fused executors.

Match counts use `uint64_t`. If the exact count would exceed its maximum, the
counter saturates instead of wrapping, enumeration stops, result/timing files
record `count_overflow: true`, the result count is shown as `OVERFLOW`, and the
process exits nonzero.

`td_tree.o` and `TDTree.cpp.old` are stale archival artifacts from the old
implementation. Neither is part of a supported build; the build scripts compile
only `main.cpp`, `query_decomposition.cpp`, `TDTree.cpp`, and `Utils.cpp`.

To run all repository `*unique*_filtered.txt` datasets:

```powershell
./run_unique_filtered.ps1 -QueryGraph ../Dataset/Query5.txt -K 5 -LabelSeed 42
./run_unique_filtered.ps1 -QueryGraph ../Dataset/Query5.txt -K 5 -CountOnly
./run_unique_filtered.ps1 -QueryGraph ../Dataset/Query5.txt -K 5 `
    -DurationMode consecutive -CountOnly
```

The batch runner rebuilds by default and copies each dataset's stdout, matching
results, and timings to `batch_results_original`. This default pattern is for
a same-input comparison with the other implementations. To measure Original's
no-prefilter behavior on raw inputs, select an appropriate raw-file pattern,
for example `-DatasetPattern "*unique-1w.txt"`; avoid a pattern that also
selects `_filtered` files. In particular, a paper-faithful `cardinality`
experiment must use unfiltered histories: a prefiltered file may already have
deleted an edge whose separated time instances should count toward `k`.

## Tests

```powershell
./run_tests.ps1
```

or, on Linux/macOS:

```bash
./run_tests.sh
```

The suite checks no-prefilter history retention, sparse-ID compaction,
reproducible random labels, `S_topo`, recursive DFS decomposition,
path-specific multi-leaf time-set joins, directed and reciprocal arcs, required
non-tree arcs, injective exact matching, paper-cardinality versus consecutive
compatibility semantics, self-loops, and random directed graphs against
brute-force oracles in both duration modes.
It also covers full/count-only output parity, match-row suppression, checked
counter overflow, 1T/32T count parity in both duration modes, serial full-output
fallback, positional seed compatibility, and invalid/duplicate CLI options.
The suite additionally locks the stable-ID mixer to golden labels and verifies
that a shared external ID keeps its label when loaded from a strict vertex
subset.
