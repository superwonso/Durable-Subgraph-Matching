# Measured source snapshots and portable build

The paper's final system is Direct DFS with P4 edge probes, an offline W-store,
and candidate-block indexing. The source still contains earlier Candidate-DAG
experiments for provenance; their presence does not make them part of the final
method. Versioned source trees are intentionally retained because published
measurements came from different binaries. Do not pool measurements across
versions or silently substitute a new snapshot for a historical binary.

## Build

Requirements: Linux or WSL2, a C++17 compiler, CMake 3.16 or newer, POSIX threads,
Bash, and Python 3. The release was built with GCC 13.3.0 and CMake Release
settings (`-O3 -DNDEBUG` for the matching snapshots).

From `icde2027_artifact/`:

```sh
bash scripts/build.sh --jobs 4
python3 scripts/smoke_test.py
```

Both commands accept `--build-root DIR`. The build additionally accepts
`--snapshot v6|v7|v8|v9|baselines|profile_v9|all`. Builds do **not** run the
historical large correctness campaigns or any performance experiment. CTest is
disabled by the default wrapper, while original test sources are retained.

The smoke test uses one included eight-vertex, directed triangle fixture with
six expected mappings, explicit labels, k=3 and two threads. It builds two tiny
stores and checks ten executable configurations against a small independent
Python oracle. It is a portability/CLI test, not new performance evidence and
not a replacement for the archived correctness tests. Use `--output NEW_DIR`
to retain its commands and receipts; otherwise its temporary files are removed.

## Snapshot mapping

| Released source | Original snapshot | Binary under the selected build root | Use |
| --- | --- | --- | --- |
| `src/v6` | `adaptive_opt_v6/source` | `v6/td_tree_direct_v6` | Table VIII design alternatives |
| `src/v7` | `adaptive_opt_v7/source` | `v7/td_tree_direct_v7` | DFS, probe, Window and final system in the v8 main campaign; monthly E4 |
| `src/v8` | `adaptive_opt_v8/source` | `v8/td_tree_bitset_v8` | Raw-bitset control in v8 and monthly E4 |
| `src/v9` | `followup_v9/source` | `v9/td_tree_followup_v9` | E1 width and E6 block-union follow-ups; all comparison arms within those schedules use this binary |
| `src/baselines` | `adaptive_opt_v8/baseline_source` | `baselines/td_tree_baseline_v8`, `baselines/tinla_baseline_v8` | Repository TD-Tree/TiNLA adaptations |
| `src/profile_v9` | `followup_v9/profile_source` | `profile_v9/td_tree_profile_v9` | Instrumented E5 TD-Tree construction diagnosis only |

`v6/build_wstore` and `v7/build_wstore` build eroded k-window stores. The
`v8/build_raw_bitset_store_v8` and `v9/build_raw_bitset_store_v8` tools build raw
history bitsets and require `--ks 1`. Their command name does not mean that the
graph itself is unfiltered: the store builders consume the prepared F2 graph.
Each store binds to the exact input bytes by SHA-256; different Bitcoin input
files must keep separate stores.

The final configuration uses `--experiment-variant durable-dfs --dag-passes
none --direct-p4 on --window-algebra on --window-build eager --bindex always
--bindex-block-size 16 --bindex-min-size 64 --bindex-c 1 --bindex-alpha 0.5`
and an explicitly loaded eroded W-store. The alpha parameter is retained for
CLI consistency and does not select an adaptive policy in always-index mode.
Use the released command/configuration manifests for other arms, particularly
v6 alternatives: do not replace their policies with this final configuration.

TD-Tree and TiNLA here are the repository's corrected paper-derived and
clean-room implementations/adaptations, **not verified author-released
artifacts**. Their directory name `original` does not establish authorship.
Consecutive and cardinality durability are different predicates; use
`--duration-mode consecutive` for same-semantics comparisons. Raw-graph
cardinality measurements must not apply F2.

## Source preservation and portability changes

The v6/v7/v8/v9 source snapshots are byte-preserved, apart from exclusion of
Windows `:Zone.Identifier` download metadata. The adjacent `ours` and
`original` trees are included because matching sources include their headers
and canonical JSON/time-planning implementation files.

The baseline and profiling build definitions were made self-contained:

- Their shared `E3E4AdapterCommonV1.cpp` dependency is included locally instead
  of reaching into a private workspace's `benchmark/` directory.
- The legacy typed-plan adapter's canonical-repository-root literal is replaced
  with `DSM_ARTIFACT_REPOSITORY_ROOT`, configured to this artifact directory.
  This affects a legacy path-validation facility, not the matching/search
  kernels used by these baseline commands.
- The baseline CMake target list builds the two measured adapted baselines;
  historical pre-adapter control executables are not recreated by this wrapper.

The per-file original and released hashes in `src/SOURCE_MANIFEST.csv` identify
these limited changes and the exact source snapshot origins. No compiler,
operating system or path-independent binary identity is claimed: rebuilt
binary hashes can differ from the recorded experimental hashes. Existing
measurement files are not rewritten to pretend they came from this build.

Historical low-level tests may target old internal modes that are not part of
this release's supported reproduction interface. The supported entry points
are `scripts/build.sh`, the bounded smoke test, the public experiment runner,
and the documented executable CLIs—not the old private-workspace supervisors.
