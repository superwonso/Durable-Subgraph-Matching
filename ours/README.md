# Ours implementation

This directory implements the method described in `260408_cdh.pdf`:

- merge temporal snapshots and remove edges that never occur in two consecutive snapshots;
- store edge activity as compressed, sorted intervals;
- compute a recorded temporal-selectivity mode and choose its minimum as the
  DFS root (the PDF's printed equation remains the default);
- build one parent-keyed TD-tree block per parent candidate;
- verify tree and non-tree edges exactly during injective DFS matching; and
- report a match only when every mapped edge has a common interval of at least `k` consecutive snapshots.

Temporal and query edges are directed: every input row `u v t` means `u -> v` at snapshot `t`, and every query row `A B` means `A -> B`. Reciprocal edges retain separate time histories. Data vertex IDs are compacted internally and restored in result files.
Consecutive self-loops are retained by the same Algorithm 3.1 rule and a
query self-loop is verified exactly once in the common temporal intersection.

The temporal datasets do not contain vertex labels. All raw vertex IDs are sorted before temporal filtering and receive a uniformly random label from `A` through `E` using `std::mt19937`. The default seed is `42`, so experiments are reproducible and input-line order does not change labels. An optional fourth CLI argument selects another seed.

When Ours reads a supplied `_filtered` file while Original reads its raw
counterpart, use `--label-mode stable-id` for both. This opt-in mode derives a
label only from the 32-bit seed and non-negative external vertex ID using the
shared fixed 64-bit SplitMix finalizer and modulo five, so removing unrelated
vertices cannot shift labels. `--label-mode sequential-rng` remains the
backward-compatible default. The selected mode and seed are written to timing
output.

The temporal input's third integer must be a dense snapshot index (for example, the output of the repository's `timeinstance` converters), because consecutive snapshots are detected as `t` and `t + 1`. Raw Unix timestamps must be converted first.

The existing query format uses labels directly as vertex identities (`A C`). To represent different query vertices that share a label, use `id:label`, for example `left:A center:B` and `center:B right:A`.

## Selectivity formula audit

The 2022 TD-Tree paper defines
`S_topo(u) = |V_L(u)| / deg_Q(u)` and minimizes it. In contrast,
`260408_cdh.pdf` prints
`S_temp(u) = deg_Q(u) / (|V_L(u)| * A_dur(u))`, also with `arg min`.
That reversal makes fewer candidates and higher degree increase the printed
score, contradicting the PDF's surrounding explanation that those properties
should be preferred.

The CLI therefore exposes the ambiguity explicitly:

- `--selectivity tdtree` uses the 2022 TD-Tree formula unchanged:
  `|V_L(u)| / deg_Q(u)`;
- `--selectivity paper` (default) uses the PDF exactly as printed:
  `deg_Q(u) / (|V_L(u)| * A_dur(u))`;
- `--selectivity intended` uses the interpretation consistent with the
  original TD-Tree ratio and the PDF prose:
  `|V_L(u)| / (deg_Q(u) * A_dur(u))`.

All three modes minimize their score. The selected mode, every query
vertex's score components, root ID, full DFS order, and root score
are written to stdout, the matching-result file, and the timing file. They
must be reported as separate ablations rather than silently pooled.

## Build and run

```powershell
./build.ps1
./td_tree.exe ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42
./td_tree.exe ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42 --count-only
./td_tree.exe ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42 `
    --count-only --selectivity paper --threads 32
```

On Linux/macOS:

```bash
./build.sh
./td_tree ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42 \
    --count-only --selectivity paper --threads 32
./td_tree ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42 \
    --label-mode stable-id --count-only --selectivity paper
```

The PDF's fixed consecutive-pair prefilter assumes `k >= 2`; the CLI rejects smaller values.
`--label-mode sequential-rng|stable-id` accepts both separate-value and
`--label-mode=value` spellings.
`--prefilter on|off` (default `on`) controls Algorithm 3.1 for the
contribution ablation. `off` retains every unique directed edge history; raw
vertex labels are assigned before this choice, so paired runs use identical
labels.
`--count-only` may appear before or after the optional seed. It performs the
same exact candidate enumeration and durability checks while omitting
individual `Match ...` rows, so output I/O does not dominate performance
measurements. Match counts saturate and report an explicit error instead of
wrapping if they exceed `uint64_t`.

`--threads N` enables true root-task parallel enumeration in count-only mode;
`N` must be from 1 through 1024. Workers keep independent mapping,
injectivity, interval, and checked-count state while sharing only immutable
graph/TD-tree data. Full mapping output remains serial even when `N > 1` to
preserve deterministic row ordering and match numbers. The result and timing
files record requested/used threads, workers that claimed roots, total root
tasks, and per-worker root-task bounds. All requested workers are launched in
count-only mode; if there are fewer roots, `workers_with_tasks` makes idle
workers explicit. Loading, temporal filtering, label
statistics, decomposition, and TD-tree construction remain serial, so a 32T
headline time is still cold-process end-to-end time.

Candidate reporting separates implementation-specific index size
(`candidate_relation_entries`) from normalized dynamic fields
`raw_extension_candidates`, `post_structural_candidates`, and
`accepted_extensions`. The root is reported separately and is excluded from
extension counts. `raw_extension_candidates` is the cross-engine comparison
unit; downstream acceptance/materialization stages are compared only within
the same backend. These fields use `metric_schema_version:
logical_extensions_v1`.

For the filtered evaluation datasets:

```powershell
./run_unique_filtered.ps1 -QueryGraph ../Dataset/Query5.txt -K 5 -LabelSeed 42
./run_unique_filtered.ps1 -QueryGraph ../Dataset/Query5.txt -K 5 -CountOnly
```

The batch script rebuilds the executable by default, preventing an older binary from being run accidentally. Pass `-SkipBuild` only when the executable is known to be current.
Its default pattern selects the repository's prefiltered files for a
same-input implementation check. To measure the proposed filter itself and to
keep random labels aligned with Original on the complete raw vertex set, run
both methods on the corresponding unfiltered unique files, for example with
`-DatasetPattern "*unique-1w.txt"` (and ensure the selected names do not also
contain `_filtered`).

Timing output reports `readTemporalGraph` for file parsing and `filterTemporalGraph` for all post-read preprocessing (sorting, deduplication, consecutive-edge filtering, random-label assignment, compact graph construction, and vertex statistics). `readAndFilterTemporalGraph` remains the measured total for compatibility.

## Tests

```powershell
./run_tests.ps1
```

or, on Linux/macOS:

```bash
./run_tests.sh
```

The tests cover interval intersection, directed merge-time filtering,
reciprocal-edge and self-loop handling, strict input parsing, sparse-ID
compaction, seeded random labels, weakly connected DFS decomposition,
direction-aware tree/non-tree verification, exact common-interval durability,
full/count-only parity, checked counter overflow, and deterministic random
directed-graph comparisons against a brute-force oracle. They also verify
1T/32T count parity for TD-Tree/paper/intended selectivity, prefilter
on/off label stability, exact worker-local candidate counters, 32-worker execution when
enough roots exist, and serial full-output fallback.
They additionally verify stable-ID golden labels and invariance when the input
contains only a subset of the original external vertex IDs.
