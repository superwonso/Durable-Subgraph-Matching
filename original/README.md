# Original implementation

This directory contains the corrected Original baseline. It shares the exact
directed TD-tree matching semantics with `ours`, but deliberately preserves the
baseline's two algorithmic differences:

- it retains every directed edge history and does **not** apply Algorithm 3.1's
  consecutive-pair prefilter; and
- it chooses the DFS root by minimizing
  `S_topo(u) = |V_L(u)| / (out_degree_Q(u) + in_degree_Q(u))`.

Every data row `u v t` means `u -> v` at snapshot `t`. Repeated timestamps for
an ordered pair are deduplicated and its full history is stored as compressed
intervals. Reciprocal arcs and self-loops remain separate directed histories.
Sparse external vertex IDs are compacted internally and restored in output.

The datasets have no vertex labels, so sorted external IDs receive uniformly
random labels from `A` through `E` using `std::mt19937`. The default seed is
`42`; an optional positional seed selects another seed. The same
seed gives the same labels even if input rows are reordered.

Query rows are also directed. `A B` means query arc `A -> B`. When multiple
query vertices share a label, give them distinct identities with `id:label`,
for example `left:A center:B` and `center:B right:A`.

The timestamp column must be a dense snapshot index when durability means
consecutive `t, t+1, ...` snapshots. Unlike `ours`, `k=1` is valid because no
consecutive-pair prefilter is applied.

## Build and run

```powershell
./build.ps1
./td_tree.exe ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42
./td_tree.exe ../Dataset/testdata.txt ../Dataset/Query3.txt 3 42 --count-only
```

`--count-only` may appear before or after the optional seed. Exact candidate
enumeration and durability checks still run, but individual `Match ...` rows
are omitted. Candidate summaries and the final exact count remain in the result
file. Unknown options, duplicate `--count-only`, and multiple positional seeds
are rejected with a nonzero exit code.

The executable writes dataset-specific `matching_results_*.txt` and
`timing_results_*.txt` files in the current directory. Timing separates raw
file parsing (`readTemporalGraph`) from sorting, deduplication, interval
compression, label assignment, compact graph construction, and statistics
(`preprocessTemporalGraph`). It also reports TD-tree build, exact enumeration,
and end-to-end time.
Both result and timing files record `mode: full` or `mode: count-only`.

Match counts use `uint64_t`. If the exact count would exceed its maximum, the
counter saturates instead of wrapping, enumeration stops, result/timing files
record `count_overflow: true`, the result count is shown as `OVERFLOW`, and the
process exits nonzero.

`td_tree.o` is a stale ignored binary from the old implementation. It is not a
supported executable; `build.ps1` always creates `td_tree.exe` from current
sources.

To run all repository `*unique*_filtered.txt` datasets:

```powershell
./run_unique_filtered.ps1 -QueryGraph ../Dataset/Query5.txt -K 5 -LabelSeed 42
./run_unique_filtered.ps1 -QueryGraph ../Dataset/Query5.txt -K 5 -CountOnly
```

The batch runner rebuilds by default and copies each dataset's stdout, matching
results, and timings to `batch_results_original`. This default pattern is for
a same-input comparison with the other implementations. To measure Original's
no-prefilter behavior on raw inputs, select an appropriate raw-file pattern,
for example `-DatasetPattern "*unique-1w.txt"`; avoid a pattern that also
selects `_filtered` files.

## Tests

```powershell
./run_tests.ps1
```

The suite checks no-prefilter history retention, sparse-ID compaction,
reproducible random labels, `S_topo`, directed and reciprocal arcs, required
non-tree arcs, injective exact matching, common consecutive duration,
self-loops, and random directed graphs against a brute-force oracle.
It also covers full/count-only output parity, match-row suppression, checked
counter overflow, positional seed compatibility, and invalid/duplicate CLI
options.
