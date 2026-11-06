# WinDur research artifact

Code, frozen queries, experiment definitions, and recorded measurements for
**Window Algebra and Support-Guided Candidate Generation for Consecutive
Durable Subgraph Matching**. This artifact accompanies the v8 manuscript
evaluation and the completed v9 follow-up experiments. It is a research
artifact, not a claim that the paper has been accepted or published.

The current method is exact direct DFS with consecutive-pair filtering (F2),
edge probing (P4), eroded-window histories (W-store), and a support-guided
candidate-block index. Historical directory and executable names containing
`gopher` or `dag` do not change the selected direct execution mode. The
repository's older implementations remain available outside this directory;
their results must not be mixed with these campaigns.

## What is included

- [`src/`](src/): versioned implementation snapshots, TD-Tree/TiNLA reference
  adapters, and the separate diagnostic build used for TD-Tree profiling.
- [`SOURCES.md`](SOURCES.md): build targets and source provenance, including
  any packaging-only path changes.
- [`queries/`](queries/) and [`configs/`](configs/): frozen query files and
  experiment parameters. Do not replace queries after observing runtimes.
- [`results/`](results/) and [`RESULTS.md`](RESULTS.md): recorded observations,
  summaries, timeout/exclusion decisions, and metric definitions.
- [`DATASETS.md`](DATASETS.md) and [`REPRODUCE.md`](REPRODUCE.md): data acquisition,
  input preparation, labels, stores, build and execution instructions.
- [`scripts/`](scripts/): build, small smoke checks, preparation, execution,
  and result summarization utilities.

Start with **[REPRODUCE.md](REPRODUCE.md)**. Large benchmark campaigns are
explicit actions, not part of the default smoke check. No large experiments
were rerun just to publish this artifact.

For a dataset-free build and small CLI check, run from this directory:

```sh
bash scripts/build.sh --jobs 4
python3 -B scripts/smoke_test.py
python3 -B scripts/summarize_results.py --check
```

The last command recomputes archived summary statistics; it does not execute
the benchmark. See [VALIDATION.md](VALIDATION.md) for the checks performed on
this publication package.

## Reading the results

Use measurements from the same campaign, input boundary, metric, and thread
setting. Search time, per-query construction-inclusive time, fresh-process
wall time, and preprocessing are different quantities. In particular, source
files and stores are loaded before the online search interval. The metadata
and reproduction instructions describe what each metric includes; `wall_s`
is retained as a separate observation rather than relabeled as online time.

The evaluated candidate-relation and TD-Tree construction are serial. The
16-thread configuration applies to the subsequent parallel search. Runtime
ratios are not CPU-utilization measurements.

### Completed v9 follow-ups and limitations

- E1: fixed-width erosion comparison. All scheduled successful comparisons
  preserve the expected answer counts.
- E2: implementation, baseline, and environment provenance.
- E3: raw-graph cardinality versus consecutive counts. Five of eight
  cardinality queries completed; three reached the fixed 1,800-second limit.
  A separate consecutive reference count was also checked. Missing counts
  are not estimated.
- E4a: 24 Bitcoin monthly queries were screened. One timed out and one
  successful screening run exceeded the fixed 120-second eligibility limit.
  The remaining 22 queries completed five arms times six repetitions: 660
  observations, all with matching answers.
- E4b: all 44 generated StackOverflow monthly queries were screened; none
  reached the fixed 20-ms heavy-query threshold. Repeated measurements were
  therefore omitted under the prespecified minimum-four rule. Four other
  fixed query-generation attempts failed and were not replaced.
- E5: three TD-Tree construction profiles completed. These instrumented
  observations are diagnostics, not additional performance repetitions.
  Output-materialization comparisons were not implemented because the
  evaluated CLI did not expose the required modes.
- E6: block-union and candidate-index comparisons on the heavy/small panels.
- E7: directed-edge activity, longest runs, F2 removals, and concentration
  statistics. The data do **not** support an 80/20 occurrence-concentration
  claim.

A collector error interrupted the first E4b screening observation before
wrapper metadata was saved. It was repaired and that single observation
was replayed with explicit approval. Completed measurements and the E3
timeouts were not retried. The incomplete first collection is not counted as
a valid timing sample. Results disclose this exception.

## Publication boundary

This release contains reproducibility materials only. It intentionally
excludes unpublished manuscript PDF/LaTeX, third-party reference papers,
private review/discussion records, credentials, user-specific machine paths,
compiled binaries, multi-gigabyte graph files, and generated stores. Data
sources and preparation instructions are provided instead. An input that
cannot be reconstructed exactly must be reported as such, not silently
substituted while claiming the recorded experiment was reproduced.

The existing repository's MIT license applies to this project's code; see
[`LICENSE`](LICENSE) and [`THIRD_PARTY.md`](THIRD_PARTY.md). Dataset rights are
separate. See [`CITATION.cff`](CITATION.cff) for software citation metadata.
