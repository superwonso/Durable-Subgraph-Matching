# Ours implementation

This directory implements the method described in `260408_cdh.pdf`:

- merge temporal snapshots and remove edges that never occur in two consecutive snapshots;
- store edge activity as compressed, sorted intervals;
- compute `S_temp(u) = deg_Q(u) / (|V_L(u)| * A_dur(u))` and choose its minimum as the DFS root;
- build one parent-keyed TD-tree block per parent candidate;
- verify tree and non-tree edges exactly during injective DFS matching; and
- report a match only when every mapped edge has a common interval of at least `k` consecutive snapshots.

Temporal and query edges are directed: every input row `u v t` means `u -> v` at snapshot `t`, and every query row `A B` means `A -> B`. Reciprocal edges retain separate time histories. Data vertex IDs are compacted internally and restored in result files.

The temporal datasets do not contain vertex labels. All raw vertex IDs are sorted before temporal filtering and receive a uniformly random label from `A` through `E` using `std::mt19937`. The default seed is `42`, so experiments are reproducible and input-line order does not change labels. An optional fourth CLI argument selects another seed.

The temporal input's third integer must be a dense snapshot index (for example, the output of the repository's `timeinstance` converters), because consecutive snapshots are detected as `t` and `t + 1`. Raw Unix timestamps must be converted first.

The existing query format uses labels directly as vertex identities (`A C`). To represent different query vertices that share a label, use `id:label`, for example `left:A center:B` and `center:B right:A`.

## Build and run

```powershell
./build.ps1
./td_tree.exe ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42
```

The PDF's fixed consecutive-pair prefilter assumes `k >= 2`; the CLI rejects smaller values.

For the filtered evaluation datasets:

```powershell
./run_unique_filtered.ps1 -QueryGraph ../Dataset/Query5.txt -K 5 -LabelSeed 42
```

The batch script rebuilds the executable by default, preventing an older binary from being run accidentally. Pass `-SkipBuild` only when the executable is known to be current.

Timing output reports `readTemporalGraph` for file parsing and `filterTemporalGraph` for all post-read preprocessing (sorting, deduplication, consecutive-edge filtering, random-label assignment, compact graph construction, and vertex statistics). `readAndFilterTemporalGraph` remains the measured total for compatibility.

## Tests

```powershell
./run_tests.ps1
```

The tests cover interval intersection, directed merge-time filtering, reciprocal-edge separation, sparse-ID compaction, seeded random labels, weakly connected DFS decomposition, direction-aware tree/non-tree verification, exact common-interval durability, and deterministic random directed-graph comparisons against a brute-force oracle.
