# Published measurements and fixed queries

This directory publishes the recorded observations and numerical tables behind
the v8 manuscript evaluation, the v6 design-alternative table, and the v9
follow-up experiments. These are distinct campaigns: do not pool their samples
or silently use an old campaign as one side of a newly measured comparison.
No new performance experiment was run to prepare this public export.

## Files to use

| Evidence | Public location |
|---|---|
| v8 baseline comparisons and runtime breakdown | `results/v8/baseline_comparison.csv`, `e2e_breakdown.csv` |
| v8 five-step optimization ladder | `results/v8/ladder_heavy.csv`, `ladder_small.csv` |
| v8 per-cell statistics | `results/v8/summary.csv` |
| v9 width-controlled erosion experiment | `results/v9/e1_width.csv` |
| Cardinality versus consecutive predicate | `results/v9/e3_card_vs_con.csv` |
| Weekly/monthly/daily raw and F2 graph statistics | `results/v9/e4_graph_stats.csv` |
| Bitcoin monthly measurements | `results/v9/e4a_bitcoin_monthly.csv` |
| StackOverflow monthly selection outcome | `results/v9/e4b_selection.json` |
| TD-Tree construction diagnostic | `results/v9/e5_build_profile.csv`, `e5_profile.json` |
| Block-union versus final index comparison | `results/v9/e6_union.csv`, `e6_union_counters.csv` |
| Edge activity, longest runs, concentration and CCDF | `results/v9/e7_activity_*.csv` |
| v9 shape/panel totals and geometric means | `results/v9/group_summary.csv`, `totals.csv` |
| Historical v6 Table VIII | `results/v6/table_viii_runs.csv`, `table_viii_cells.csv`, `table_viii_summary.csv` |
| Public per-run observations | `results/v8/observations/index.json`, `results/v9/observations/index.json` |
| Frozen query files and label projections | `queries/manifest.json`, `queries/weekly/`, `queries/monthly/` |
| Manuscript query identifiers | `queries/manuscript_query_metadata.csv` |

JSONL observation shards are listed in their `index.json`, with record counts
and SHA-256 hashes. There are 5,730 v8 records (including preflights and explicit
non-executed slots), 2,567 v9 records (2,525 attempted measurements plus 42
60-second-rule skips), and 2,016 v6 Table VIII measurements. The public records
retain timings, counts, status, memory measurements, available counters and
case/arm/repeat identity. Repeated full case objects, local paths and unrelated
inactive planner diagnostics are omitted. An absent/unmeasured counter is not
a measured zero. `source_record` is a historical journal locator, not a path
that the public runner must open. Source CSV/journal checksums are retained in
`results/source_manifest.json`.

## Units and timing boundaries

- `*_s` values are seconds; `*_ns` values are nanoseconds. Peak RSS is MiB
  (the process wrapper's KiB value divided by 1024).
- `wall_s` is the fresh-process elapsed time, including startup, input reads,
  construction, output and shutdown. It is not the search-only metric.
- v8 `enum_plus_build_s` and v9 `search_s` are enumeration plus the measured
  query-specific runtime preparation. These are the within-system ladder and
  new-arm search metrics; they exclude input-file loading and offline stores.
- v9 `query_s` adds planning (with separately measured W-store loading removed)
  and candidate-relation/tree construction to `search_s`. TD-Tree comparisons
  in the monthly table use `td_f2_query_*` versus `final_query_*`, not only
  TD-Tree enumeration versus the full proposed query pipeline.
- v8 `online_s` is the archived primary enumeration timer. It must not be
  relabelled as fresh-process end-to-end time. The v8 tables retain their
  explicit `metric` and `boundary` columns. In particular, the separately
  named historical `e2e_excl_read_filter_s` subtracts both measured parsing and
  fused filtering from wall time; it is not equivalent to excluding reading
  alone. The phase tables retain these components so the scope is inspectable.
- `raw`, `prepared`, and `raw_f2` input boundaries are separate cohorts. The
  prepared graph has already undergone F2. Offline graph/store creation is
  not folded into a prepared-input search comparison.

Means, medians, extrema, sample coefficient of variation and R7 quartiles are
provided without bootstrap intervals. Geometric means are computed from
per-query median ratios. A clear speed difference requires both mean and median
speedups of at least 1.20, non-overlapping IQRs, and at least six valid samples
on each side. "Within variation" does not establish statistical equivalence.
Ratios labelled baseline/new exceed one when the new arm is faster; the v6
Table VIII ratio is instead explicitly new/win_s and is faster below one.
Table VIII's `slower_median_count` counts any median increase, not the
clear-effect criterion.

## Recorded outcomes and exceptions

- **E1:** 486 successful performance runs; 18 unexecuted slots reflect the
  predetermined first-wall-over-60-seconds reduction from twelve to six
  repetitions. For the eleven k>=4 queries, width-controlled erosion has a
  1.235x geometric-mean median ratio; five comparisons meet the clear-effect
  rule. The three same-width controls have a padded/unpadded ratio of 0.989x.
- **E6:** 1,215 successful performance runs and 24 scheduled-repeat reductions.
  The heavy-panel Window/Union geometric-mean ratio is 1.013x with no clear
  gain; the small-panel ratio is 0.970x. The union check is not presented as a
  demonstrated heavy-query acceleration. Union/Final results are separate.
- **E3:** five of eight cardinality queries completed. C1, C3 and T1 each
  reached the fixed 1,800-second limit. The separate consecutive-predicate
  recount also completed. These are count observations, not repeated timing
  comparisons. CardDur was evaluated on raw input: F2 is not answer-preserving
  for the cardinality predicate. Successful CardDur counts reject 61.20–81.61%
  of matches when the consecutive condition is imposed.
- **E4a:** all 24 original heavy query files were screened on Bitcoin monthly
  data. T3 timed out at 600 seconds; P10 completed in 144.36 seconds but exceeded
  the predeclared 120-second screening cutoff. The remaining 22 queries were
  measured with five arms and six repeats: **660/660 successful, with matching
  counts**. Window/Final has a 1.403x geometric-mean median ratio. The DFS/Bitset
  comparison also adds P4, because this experiment has no separate probe arm;
  it must not be described as the isolated effect of bitsets.
- **E4b:** the frozen 48-slot M4 acquisition attempted 48 queries and produced
  44; four acquisition failures were not replaced. All 44 generated queries
  completed screening, but none reached the fixed 20ms search threshold.
  Accordingly, the predeclared minimum-four-heavy-queries rule stopped the
  repeated comparison. `e4b_so_monthly.csv` is intentionally empty apart from
  its header; it does not contain zero runtimes. This does not provide a
  successful second heavy dataset.
- **E5:** three separately instrumented TD-Tree construction profiles completed
  with correct counts. These are diagnostic timings, excluded from performance
  comparisons. The evaluated executable does not expose the requested three
  output modes, so no output-inclusive timing experiment was invented.
- **E7:** singleton edges plus multi-snapshot edges with no consecutive pair
  account exactly for F2 removals. The top 20% of weekly edges contribute only
  26.65–40.52% of occurrences; an 80/20 Pareto claim is not supported.
- **Collection recovery:** one initial E4b screening observation finished in
  the matcher but its wrapper metadata was not journaled because an optional
  W-store JSON value was null. After repairing the collector, that single
  observation was explicitly rerun. It was not selected by runtime and no
  completed observation was rerun. The failed collection is not a usable timing
  sample; no missing wall/swap values were estimated.

The v9 correctness checks include the fixed 200 small random graphs and a
positive word-width boundary fixture, followed by the 35 actual weekly
heavy/small cases for each new arm. Published successful runs have no recorded
count mismatch. Failed and ineligible cases remain in the observations and
selection records rather than being assigned substitute values.

## Fixed query and label metadata

There are 39 weekly cases (the union of heavy24, small12 and system4) and 68
monthly cases (24 Bitcoin and 44 generated StackOverflow). Shared queries are
intentionally present in multiple case directories when k or the input differs.
Query and projection bytes are exact copies of the recorded inputs and retain
their SHA-256 values. The two Bitcoin prepared-input SHAs are not merged.

Large graphs, per-vertex label maps and binary W-stores are not distributed in
this directory. Their original size/SHA and role are present in the query
manifest for regeneration and verification. A label projection must be used
with its corresponding global map, and an offline store only with the matching
input-file SHA. Historical source locators in the manuscript metadata are
retained as provenance; executable public query paths are in `queries/manifest.json`.
Explicit-label weekly cases also provide `raw_label_inputs` and
`raw_query_projection.json` for the raw/raw-F2 baselines and E3. These must not
be replaced by the prepared-graph projection, whose bound map-file SHA differs.

## Recompute the summary statistics without rerunning experiments

```sh
python3 scripts/summarize_results.py --check
python3 scripts/summarize_results.py --output-dir /tmp/icde2027-recomputed
```

The standalone script reads only the public observations and reference tables;
it launches no native executable. It recomputes 168 v6 design-alternative cells,
1,380 v8 cells, and 370 v9 metric cells, including panel-specific repeat prefixes
and missing-value handling. Export validation checked 23,775 scalar values
against the archived tables. Reference results are never overwritten. The
optional output directory must not contain files with the same output names.

This arithmetic check establishes that the exported measurements reproduce the
listed numerical summaries; it does not guarantee matching runtimes on another
machine or replace the documented experiment limitations.
