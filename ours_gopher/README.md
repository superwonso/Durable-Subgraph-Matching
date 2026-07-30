# Ours + Gopher CPU implementation

This directory is a self-contained CPU implementation of the method in
`appendix/260408_cdh.pdf`, extended with the reusable DAG execution ideas in
`Gopher: Efficient Dynamic Graph Pattern Mining via DAG-Driven Execution`
and `논문 방법론 기여 설명.pdf`.

It retains the exact consecutive-duration semantics and all audited fixes from
`ours/`: strict line parsing, directed reciprocal-edge histories, self-loops,
sparse external-ID compaction, deterministic seeded labels, exact non-tree
checks, checked `uint64_t` match counts, and true count-only output.

## What the Gopher extension does

For each fixed query plan, `CandidateDagCompiler` builds a parameterized DAG
from:

- TD-tree parent-block lookups;
- outgoing/incoming neighbor lookups;
- structural set intersections;
- temporal edge-interval lookups;
- temporal interval intersections; and
- longest-consecutive-run checks.

Every node has a canonical expression and 64-bit static fingerprint.
Commutative/associative intersections are flattened and canonicalized before
hash-consing. A runtime memo key contains the fingerprint, collision-safe node
ID, and only the data-vertex bindings on which that expression depends.
Bindings from unrelated branches are deliberately excluded.

Only branch-invariant results are cached:

- `B(u, phi)`: structural candidate sets;
- `S(u -> v | phi)`: local temporal support; and
- the longest run within that local support.

The current used-vertex set and cumulative active support `I(phi)` are never
cached. Injectivity therefore remains branch-local, and the durable extension
rule remains exact:

```text
I' = I(phi) intersection S
accept the branch only if I' contains a run of at least k snapshots
```

Because later intersections cannot lengthen an interval, empty/short support
is a monotonic pruning condition.

Two deterministic, fixed plans are available:

- `paper`: the DFS plan produced by the 260408 method;
- `reuse`: a parent-before-child topological plan chosen once from TD relation
  sizes, earlier constraints, and reuse potential.

The reuse plan is never changed during search, so all threads execute the same
plan and runtime keys remain stable.

### Implemented planner scope

This is the reproducible **v1 static reuse-aware planner** used in the
evaluation. It implements canonical DAG reuse, minimal binding keys, bounded
memoization, and a one-time order based on TD-relation size, already-bound
constraints, and structural reuse potential. It does **not** claim the
stronger adaptive planner sketched as future development in
`논문 방법론 기여 설명.pdf`: there is no cache-hit-driven online reordering,
learned/online temporal-survival estimator, beam search, or look-ahead union
upper bound. Those policies require a separate deterministic scheduling
protocol and ablation before they can be compared fairly.

## Memoization and admission

`--memo` supports `none`, `structural`, `temporal`, and `dag`. These are cache
factors, not stages that progressively introduce a DAG: every `gopher` engine
configuration compiles and executes the Candidate-DAG. `structural` enables
only the structural cache, `temporal` enables only the interval and
longest-run caches, and `dag` enables both cache families. Thus
none/structural/temporal/dag form a 2x2 memoization factorial. The cache is
sharded, thread-safe, immutable after admission, and bounded by `--cache-mb`.
With both families enabled, half of the budget is structural; the other half
is split 15:1 between interval and longest-run entries.

The default cost-aware threshold is 128 estimated input elements. Cheap
operations are rejected before runtime-key construction or a cache lock, and
their skips are reported as `*_cache_rejected_cost` and
`*_cache_bypassed`. This is why a cheap workload can have the same core
runtime as `--memo none` while still selecting `--memo dag`: the diagnostic
counters show that memo admission was intentionally not exercised.
`--no-cost-aware` is the corresponding admission ablation.

`--cache-mb 0` and `--cache-entry-mb 0` disable lookup itself and are exact
memo-none behavior. Accounted admitted bytes include conservative per-entry
map/control estimates plus dynamic key/value storage and cannot exceed the
configured budget. This counter is not an allocator census: bucket-array and
allocator-specific overhead are reflected by process peak RSS, which is the
authoritative total-memory measurement.

Non-admitted values do not pay for a `shared_ptr` allocation. Intermediate
results move as local values, while immutable TD-tree blocks and temporal edge
histories are returned as borrowed views when no filtering is needed. The
views are safe because both stores are finalized before matching and remain
alive for the whole executor lifetime. Only admitted cache values use shared
immutable ownership.

Detailed metrics include entries, concurrent duplicate inserts, rejection
reason, result elements reused, estimated work saved on hits, maximum reused
result size, and admitted work/size. These support both ordinary hit-rate and
cost-weighted reuse analyses.

## Build

Linux:

```bash
./build.sh
```

Strict GCC/Clang warnings:

```bash
STRICT=1 ./build.sh
```

Windows PowerShell with MinGW:

```powershell
./build.ps1
./build.ps1 -StrictWarnings
```

CMake:

```bash
cmake -S . -B build -DGOPHER_STRICT_WARNINGS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The executable is `td_tree_gopher` (`td_tree_gopher.exe` on Windows).

## Command line

```text
td_tree_gopher DATA QUERY K [LABEL_SEED] [options]

--label-mode sequential-rng|stable-id
--engine gopher|legacy
--selectivity tdtree|paper|intended
--order paper|reuse
--memo none|structural|temporal|dag
--cache-mb N
--cache-entry-mb N
--cache-min-work N
--no-cost-aware
--threads N
--parallel roots|prefix
--prefix-depth N
--tasks-per-thread N
--early-prune on|off
--result-digest on|off
--statistics on|off
--count-only
--verify-dag
--output-dir DIR
--metrics PATH
```

Defaults are the `gopher` engine, `paper` selectivity, `paper` order, `dag`
memoization, a 256 MiB cache, one thread, cost-aware admission, cumulative
pruning on, detailed statistics on, digest off, label seed 42, the
`sequential-rng` label mode, and the compatibility-preserving `roots`
scheduler. Prefix queue capacity defaults to
eight queued tasks per requested worker.

Use `--label-mode stable-id` when this executable reads a supplied `_filtered`
file while Original reads the raw counterpart. It derives each A-E label only
from the seed and external vertex ID with the same fixed 64-bit SplitMix
finalizer used by Original, Ours, and the GPU executable. A shared ID is
therefore label-identical across vertex subsets. The default
`sequential-rng` mode preserves existing results. Timing, stdout, and metrics
JSON record `label_mode` and `label_seed`.

`--selectivity tdtree|paper|intended` is independent of
`--order paper|reuse`.
The selectivity option determines the initial TD-tree DFS decomposition:
`tdtree` uses the 2022 `|V_L|/deg_Q` score, `paper` (default) uses the printed
`260408_cdh.pdf` equation
`deg_Q/(|V_L|*A_dur)`, while `intended` uses the prose-consistent
`|V_L|/(deg_Q*A_dur)`. The order option then either retains that DFS order or
constructs the fixed reuse-aware parent-before-child schedule. The
selectivity mode, selected root, and score are recorded in result, timing, and
metrics JSON outputs.

With `--statistics on`, candidate accounting uses
`metric_schema_version=logical_extensions_v1`:
`raw_extension_candidates` counts TD-relation row entries before DAG
structural intersections, `generated_candidates`/`post_structural_candidates`
counts their output, and `accepted_extensions` counts recursive extensions.
Statistics-off outputs use null/absence, never a misleading zero.

Example:

```bash
./td_tree_gopher \
  ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42 \
  --label-mode stable-id \
  --count-only --selectivity paper --order reuse --memo dag --threads 8 \
  --cache-mb 256 --statistics off --result-digest off
```

For a skewed count-only workload:

```bash
./td_tree_gopher DATA QUERY K 42 \
  --count-only --order reuse --threads 32 --parallel prefix \
  --prefix-depth 0 --tasks-per-thread 8 --statistics off
```

`--early-prune off` is a controlled exact ablation of the cumulative
`I(phi) intersection S` empty-branch check. It does not disable local
edge-support checks, removal of intervals shorter than `k`, or final
durability validation. An empty cumulative support is carried to completion
and rejected there, which can deliberately cause a large search expansion.

`--verify-dag` recomputes every requested structural and temporal DAG root
with direct non-DAG logic and aborts on a mismatch. It is intended for
correctness runs, not timing.

## Parallel execution and measurement

`--parallel roots` preserves the original scheduler: an atomic index assigns
complete TD-tree root candidates to workers. It has minimal scheduling
overhead, but one expensive root cannot be shared after its DFS begins.

`--parallel prefix` adds bounded dynamic subtree sharing. Root candidates are
still claimed lazily, so even millions of roots do not materialize millions
of search states. During DFS, a worker may copy a query-sized prefix state
into a shared queue. Waiting workers then execute those disjoint subtrees.
The queue is bounded by `threads * tasks-per-thread`; it never holds a
data-graph-sized frontier. `--prefix-depth 0` uses the safe automatic depth
of two bound query vertices (the root plus one vertex). Larger explicit
depths expose deeper imbalance at the cost of more scheduling and state-copy
overhead; depth one is exactly root-only subdivision. A one-thread prefix
request uses the `roots-single-worker` effective mode and does not create
queue tasks, preserving the single-thread control without scheduler overhead.

Both schedulers share the thread-safe memo tables. Counts and the optional
order-independent digest are reduced exactly. Always-on scheduling metrics
record queue claims/peak, spawned prefixes, workers with work, and per-worker
task/DFS/active-time min, max, total, coefficient of variation, and
utilization. The peak accounted bytes of queued mapping/interval states are
also reported. These remain valid with `--statistics off`; general hot-path
search counters are zero and must be treated as disabled in that profile.

Full match rows are streamed in deterministic root/DFS order with one worker
to avoid unbounded per-thread output buffers. `--threads` therefore applies to
`--count-only`, which is the required performance mode.

Use two separate profiles:

Core runtime comparison:

```text
--count-only --statistics off --result-digest off
```

Mechanism/reuse diagnostics:

```text
--count-only --statistics on --result-digest off
```

Digest/correctness reconciliation:

```text
--count-only --statistics on --result-digest on --verify-dag
```

Detailed atomics and per-operation timers are intentionally optional because
they materially affect small/fast workloads. The selected settings are
recorded in the result file, timing file, stdout, and metrics JSON.

Every run writes:

- `matching_results_<dataset>.txt`;
- `timing_results_<dataset>.txt`; and
- `metrics_results_<dataset>.json` unless `--metrics` overrides it.

Stage timings use nanoseconds in the machine-readable outputs. Search,
preprocessing, TD-tree construction, DAG compilation, cache memory, peak RSS,
pruning, logical/physical operations, and reuse statistics remain separate.

## Input and semantics

Data rows are directed temporal occurrences:

```text
source target dense_snapshot_index
```

Query rows are directed arcs. Plain `A B` tokens use the label as identity.
Use `id:label` when distinct query vertices share a label:

```text
left:A center:B
center:B right:A
```

The temporal snapshot index must be dense: the preprocessing rule detects
`t` followed by `t + 1`. The implementation merges duplicate occurrences,
compresses sorted activity into intervals, and retains an ordered edge only
if at least one consecutive pair exists. Therefore the CLI requires `k >= 2`.

### Selectivity formula audit

The `tdtree`, `paper`, and `intended` names are necessary because the source papers do
not agree. TD-Tree uses `|V_L|/deg_Q` with `arg min`, whereas
`260408_cdh.pdf` prints the reciprocal structural ratio but retains
`arg min`. Its prose still says that fewer candidates and higher degree are
preferred. `intended` restores the TD-Tree numerator/denominator and keeps the
PDF's lifespan factor in the denominator. Neither interpretation is presented
as if the ambiguity did not exist; evaluations must label the mode.

## Tests

```bash
./run_tests.sh
STRICT=1 ./run_tests.sh
```

The script builds in a temporary directory and removes all test binaries.
Tests cover the inherited `ours` regression suite plus:

- independent per-snapshot brute-force oracle parity;
- exact sorted mapping-set parity, not count alone;
- legacy TD-tree/Gopher count parity;
- paper/reuse, all memo modes, early-prune, cache-zero, and thread parity;
- reciprocal arcs and self-loops;
- canonical DAG nodes and minimal relevant runtime bindings;
- irrelevant-binding hits and relevant-binding misses;
- bounded/max-entry admission and concurrent duplicate insert stress;
- single-root dynamic-prefix count/digest parity, queue bounds, depth control,
  and load-balance instrumentation with statistics both on and off;
- full/count-only parity and deterministic digest; and
- strict CLI and JSON-output validation.

The inherited regression suite also locks the stable-ID mixer to golden
labels and loads full/subset fixtures to verify external-ID label invariance.

## CPU/GPU boundary

This directory remains the CPU implementation. The separate
`ours_gopher_gpu` prototype uses ROCm and a fused device-resident frontier
executor; it does not reproduce these dependency-key runtime memo tables on
the GPU. GPU results therefore use both a CPU no-memo reference and this full
CPU Gopher implementation as denominators. Toolchain-only compilation is not
accepted as GPU evidence: the reported device must pass an actual
host-to-device, kernel, device-to-host execution test.
