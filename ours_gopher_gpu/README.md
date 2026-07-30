# Ours + Gopher ROCm GPU executor

This directory implements a ROCm/HIP backend for the durable subgraph matcher
in `ours_gopher`. It targets the AMD Radeon 8060S (`gfx1151`) available through
WSL2 ROCDXG and `/dev/dxg`.

The implementation is intentionally named and reported as:

```text
GPU fused Candidate-DAG executor (no runtime memo)
```

It preserves the exact directed, injective, consecutive-duration semantics of
`ours_gopher`, but it does not claim to be a literal GPU port of Gopher's CPU
hash memo. The distinction is recorded in every result:

```text
memo_effective: device_fused_no_runtime_memo
fallback_used: false
```

## Architecture

CPU preprocessing remains unchanged:

1. parse and compact the directed temporal graph;
2. apply the 260408 consecutive-pair edge filter;
3. compute paper or intended selectivity;
4. build the TD-tree candidate relations;
5. choose the fixed paper or reuse-aware query plan; and
6. compile the durability-aware Candidate DAG.

`GpuMatcher` lowers that fixed DAG to one frontier stage per query-plan depth.
The following CPU DAG operations are fused into each HIP frontier kernel:

- TD-block lookup;
- directed outgoing/incoming structural checks;
- structural intersection;
- temporal edge-history lookup;
- multiway temporal interval intersection; and
- longest-consecutive-run checking.

The graph, temporal interval histories, TD relations, query arcs, and current
frontier remain in device memory. Two selectable execution variants are
available.

### Level-synchronous baseline

`--gpu-variant level` is the backward-compatible default. Each depth uses two
exact passes:

1. count valid children for every partial mapping and exclusive-scan the
   counts;
2. copy the resulting 64-bit frontier size to the host;
3. allocate the exact next-frontier size; and
4. validate again while materializing those mappings.

This avoids allocating space for every unfiltered Cartesian proposal, but it
repeats candidate validation, launches four device kernels per non-empty
depth, synchronizes twice, and performs one depth-count D2H round trip.

### Resident continuation

`--gpu-variant resident` uses a device-resident continuation pipeline:

- two fixed-capacity ping-pong frontiers are allocated within the same
  `--gpu-memory-mb` cap;
- device depth-count arrays feed one depth directly into the next, so there is
  no intermediate count D2H or host frontier-size decision;
- a 128-thread kernel validates and materializes each candidate once, using a
  block scan to reserve output ranges without a global atomic per candidate;
- blocks obtain parent states from a dynamic device work queue, reducing the
  tail caused by highly unequal TD-relation row widths; and
- `hipOccupancyMaxActiveBlocksPerMultiprocessor` selects the resident grid. On
  the tested `gfx1151`, HIP selected 16 blocks per WGP and a 320-block grid.

All depth kernels are queued in one ordered device sequence and synchronized
once at its end. Frontier output order may differ, but count and the
order-independent `(xor, sum)` digest are exact. If the fixed frontier
capacity is exceeded, a device overflow flag causes an explicit error after
the sequence; execution never falls back to CPU.

### Exact temporal test

A GPU thread never materializes a per-state timestamp bitmap or a variable
length interval vector. Instead, it keeps one cursor per bound directed query
arc and performs a multiway sweep over the sorted interval lists. A branch is
accepted iff the lists share an interval of at least `k` consecutive snapshot
indices.

When `--early-prune on`, all currently bound arcs participate at every depth.
When it is off, each non-final depth checks only the newly introduced local
support, while the final depth checks every query arc. This matches the CPU
ablation's result semantics.

### Why runtime memo is absent

The CPU implementation shares immutable DAG results using dependency-binding
hash keys. That design maps poorly to thousands of GPU threads because a
concurrent variable-length hash cache adds irregular allocation, locking, and
pointer chasing. The GPU executor instead shares the immutable CSR data and
executes a whole DAG depth as a fused bulk operation. Static DAG planning and
durability pruning are retained; dependency-key runtime memo is not.

Accordingly, CPU `ours_gopher --memo dag` and this GPU backend must be shown as
different execution variants in performance tables. `--memo` is accepted for
benchmark CLI compatibility and recorded as `memo_requested`, but it does not
change GPU execution.

## Build

ROCm 7.2.4 and `hipcc` are required. The default target is `gfx1151`.

```bash
cd ours_gopher_gpu
./build.sh
```

Override the target if necessary:

```bash
GPU_ARCH=gfx1151 ./build.sh
```

The resulting executable is:

```text
ours_gopher_gpu/td_tree_gopher_gpu
```

The CMake build intentionally invokes `hipcc` directly. The installed ROCDXG
toolchain can compile HIP programs, but its `hip-lang` CMake package does not
ship the native-Linux clang runtime layout expected by CMake's HIP-language
probe.

## Run

```bash
./td_tree_gopher_gpu \
  ../Dataset/wiki-talk-temporal-timeinstance-unique-1w_filtered.txt \
  ../benchmark/queries/wiki_talk/path3_k3.qry \
  3 42 \
  --count-only \
  --label-mode stable-id \
  --order reuse \
  --selectivity paper \
  --statistics off \
  --result-digest off \
  --gpu-device 0 \
  --gpu-variant resident \
  --gpu-memory-mb 1024
```

The positional arguments and common flags match `ours_gopher`:

```text
td_tree_gopher_gpu DATA QUERY K [LABEL_SEED] [options]

--label-mode sequential-rng|stable-id
--order paper|reuse
--selectivity tdtree|paper|intended
--memo none|structural|temporal|dag
--threads N
--early-prune on|off
--statistics on|off
--result-digest on|off
--count-only
--output-dir DIR
--metrics PATH
```

`--label-mode sequential-rng` is the backward-compatible default.
`--label-mode stable-id` uses the same seed-plus-external-ID 64-bit SplitMix
finalizer as Original, Ours, and Ours+Gopher CPU, ensuring that a vertex shared
by raw and `_filtered` inputs receives the same A-E label. The selected
`label_mode` and `label_seed` are recorded in timing output, stdout, and
metrics JSON.

GPU-specific flags are:

```text
--gpu-device N
--gpu-variant level|resident
--gpu-memory-mb N
--verify-cpu
--backend gpu
```

`--threads` is accepted and recorded for a common benchmark interface. It does
not control GPU worker count; CPU preprocessing is single-threaded and the GPU
uses hardware scheduling. The level variant uses 256-thread workgroups; the
resident variant uses occupancy-selected grids of 128-thread workgroups.

`--verify-cpu` runs the one-thread CPU Gopher engine with memo disabled and
requires both the exact match count and an order-independent `(xor, sum)`
64-bit mapping digest to match. Do not enable it in timed performance runs.

## Tests

Compile-only:

```bash
./run_tests.sh
```

Compile plus device correctness:

```bash
RUN_GPU_TESTS=1 ./run_tests.sh
```

The runtime test covers:

- paper selectivity and paper order;
- intended selectivity and reuse-aware order;
- level-synchronous and resident-continuation execution;
- CPU/GPU count parity;
- CPU/GPU two-part mapping-digest parity; and
- JSON/result-file validation;
- stable-ID CLI/metrics propagation (the shared CPU regression suite verifies
  full/subset label invariance and golden mixer values); and
- explicit resident-capacity failure with no CPU fallback.

The deterministic layered fixture has 36 durable matches, so the smoke test
executes every frontier stage rather than passing vacuously with zero roots.

## Measurement fields

Every run writes the same three artifact classes used by `ours_gopher`:

- `matching_results_<dataset>.txt`;
- `timing_results_<dataset>.txt`; and
- `metrics_results_<dataset>.json`.

The JSON separates:

- `hostPreprocess`: load, filter, decomposition, TD-tree, plan, and DAG;
- `flattenGpuData`: host CSR flattening;
- `gpuAllocation`: HIP device-buffer allocation time;
- `gpuH2D` and `gpuD2H`: synchronous transfer time and bytes;
- `gpuKernel`: HIP-event time for validation, scans/materialization, resident
  device-control operations, and digest;
- `gpuSearch`: host-observed device search after upload, excluding separately
  measured D2H copies but including required synchronization/launch overhead;
- `gpuReadyToResult`: device allocation + H2D + search + D2H; and
- `endToEnd`: cold one-shot process algorithm time.

All entries under `timings_nanoseconds` are integer nanoseconds. For direct
spreadsheet ingestion, the JSON also emits top-level floating-point aliases
whose units are explicit:

```text
host_preprocess_ms
flatten_gpu_data_ms
gpu_h2d_ms
gpu_allocation_ms
gpu_kernel_ms
gpu_launch_sync_overhead_ms
gpu_depth_round_trip_ms
gpu_d2h_ms
gpu_search_ms
gpu_ready_to_result_ms
end_to_end_ms
```

Primary CPU/GPU comparisons should use cold one-shot `endToEnd` and report
`gpuReadyToResult` as a secondary decomposition. Kernel-only time is a
diagnostic, not an end-to-end speedup denominator.

The `gpu` block records kernel launches, device synchronizations, depth-count
round trips and bytes, repeated validation/materialization passes,
launch-and-sync overhead, resident capacity, occupancy, and grid size. The
`depths` array records input/output states and the host round-trip at each
depth. With `--statistics on`, it also records generated candidates, maximum
row width, mean row width, and their ratio. With statistics off those workload
counters are JSON `null` with `workload_metrics_available: false`; zero is not
used to imply that no work occurred.

Here, `depth_host_round_trips` and `depth_count_d2h_bytes` count intermediate
depth-control transfers that stall continuation. Resident mode still copies
the completed depth-count vector once after the queued sequence for the final
count and audit metadata; those bytes and time are included in the ordinary
`d2h_bytes`/`gpuD2H` totals. `device_synchronizations` counts explicit
HIP-event synchronization points around timed device sequences. Synchronous
H2D/D2H API time is reported separately rather than double-counted as an
additional event synchronization.

Detailed global counters include generated/accepted candidates, injectivity,
structural and temporal rejections, interval-sweep steps, directed lookups,
frontier peak, device bytes, device name, and architecture. The device block
also records the raw HIP runtime and driver version integers. Use
`--statistics off` for primary timing and `--statistics on` for mechanism
analysis.

The same counters are exposed under
`metric_schema_version=logical_extensions_v1`:
`raw_extension_candidates` is the TD-relation row entries validated on the
device, `post_structural_candidates` is equal to that raw count for the fused
GPU validator, and `accepted_extensions` is the output-frontier extension
count. Statistics-off runs emit null rather than zero.

`device.compute_units` is retained for report compatibility, but its source is
`hipDeviceProp_t.multiProcessorCount`. On this `gfx1151` device HIP reports 20
work-group processors (WGPs), whereas `rocminfo` expands the dual-CU WGPs and
reports 40 compute units. The JSON therefore also emits the unambiguous
`device.hip_multiprocessor_count` and `device.compute_units_source` fields.

## gfx1151 audit and smoke results

The implementation was audited on the AMD Radeon 8060S (`gfx1151`) through
ROCDXG. The controlled performance smoke used cold independent processes,
three interleaved repetitions, `--statistics off --result-digest off`, and
medians. The comparison binary was the preserved pre-change level-frontier
executor at `benchmark/baselines/td_tree_gopher_gpu_level_frontier`.

| Workload | Variant | Kernel ms | GPU-ready ms | End-to-end ms | H2D ms |
|---|---|---:|---:|---:|---:|
| skewed, 16,857,200 matches | level | 134.851 | 359.683 | 486.887 | 178.123 |
| skewed, 16,857,200 matches | resident | 47.968 | 308.745 | 435.653 | 180.569 |
| heavy skew, 84,939,655 matches | level | 380.149 | 674.554 | 794.536 | 176.815 |
| heavy skew, 84,939,655 matches | resident | 146.968 | 599.507 | 705.518 | 175.701 |
| balanced scaling, 16,959,872 matches | level | 739.696 | 969.303 | 1,784.697 | 179.471 |
| balanced scaling, 16,959,872 matches | resident | 288.614 | 534.064 | 1,399.242 | 175.609 |

Resident kernel speedups were 2.81x, 2.59x, and 2.56x respectively. The
end-to-end speedups were smaller—1.12x, 1.13x, and 1.28x—because cold H2D and
ROCDXG initialization remained about 176–181 ms, and the fixed resident
workspace cost more to allocate on the two skew workloads.

The statistics-on audit found the intended imbalance: at depth 2, maximum
candidate-row width divided by the mean was 26.73x for `reuse_star_skewed` and
34.41x for `reuse_star_skewed_heavy`. For four-depth queries, level execution
used 16 kernel launches, four depth-count D2H round trips, eight device
synchronizations, and eight validation passes. Resident execution used four
kernel launches, no depth-count D2H, one device synchronization, and four
validation passes.

Exact count and digest parity against the preserved level executor was checked
on all three workloads:

```text
skewed          16857200  7bbf9e99d3f20ef6:5104d29a43f86872
heavy skew      84939655  a5fdc21937f79af6:34cd5b4fb4214388
balanced        16959872  c94b48b897791c12:52b95b157880142a
```

These are implementation smoke results, not substitutes for the repository's
full repeated benchmark matrix. H2D is deliberately still reported as an
unresolved cold-start bottleneck; this change does not claim pinned or
asynchronous upload.

## Limits and failure behavior

- Count-only output is supported; GPU match-row streaming is not.
- At most 32 unique directed query arcs are supported.
- The whole flattened graph and TD relations must fit device-accessible memory.
- Frontier workspace is bounded by `--gpu-memory-mb`.
- Resident mode reserves two ping-pong frontiers within that cap. A
  conservative upper bound is used when it fits; otherwise the full cap is
  used and actual overflow is detected on device.
- A budget or allocation failure is explicit. There is no silent CPU fallback,
  because mixing fallback runs into GPU timings would invalidate comparisons.
- The current implementation is optimized for small query graphs and
  sufficiently large candidate frontiers. Small workloads are normally slower
  than CPU due to ROCDXG initialization and transfer cost.
- WSL2 compute uses `/dev/dxg`; `/dev/kfd` and `/dev/dri` are not required for
  this ROCDXG build.
