#!/usr/bin/env bash
set -euo pipefail

if (($# != 1)); then
    printf 'Usage: %s <executable>\n' "$0" >&2
    exit 2
fi

executable=$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
temp_dir=$(mktemp -d)
cleanup() {
    rm -rf -- "$temp_dir"
}
trap cleanup EXIT

printf '10 20 1\n10 20 2\n' >"$temp_dir/data.txt"
# mt19937(seed=42) assigns the two sorted data vertices labels B and D.
printf 'B D\n' >"$temp_dir/query.txt"
required=("$temp_dir/data.txt" "$temp_dir/query.txt" "2")

expect_success() {
    local label=$1
    shift
    if ! "$executable" "$@" >/dev/null 2>&1; then
        printf 'CLI success case failed: %s\n' "$label" >&2
        exit 1
    fi
}

expect_failure() {
    local label=$1
    shift
    if "$executable" "$@" >/dev/null 2>&1; then
        printf 'CLI failure case unexpectedly succeeded: %s\n' "$label" >&2
        exit 1
    fi
}

pushd "$temp_dir" >/dev/null
expect_success "legacy positional CLI" "${required[@]}"
grep -q '^mode: full$' matching_results_data.txt

expect_success "count-only without seed" "${required[@]}" --count-only
grep -q '^mode: count-only$' matching_results_data.txt
grep -q '^mode: count-only$' timing_results_data.txt
if grep -q '^Match ' matching_results_data.txt; then
    printf 'count-only mode emitted match rows\n' >&2
    exit 1
fi

expect_success "seed before count-only" "${required[@]}" 42 --count-only
expect_success "count-only before seed" "${required[@]}" --count-only 42
expect_success "stable external-ID labels" \
    "${required[@]}" --count-only --label-mode stable-id
grep -q '^label_mode: stable-id$' timing_results_data.txt
grep -q '^label_seed: 42$' timing_results_data.txt
grep -Eq '^temporal_graph_content_fingerprint_fnv1a64: [0-9]+$' \
    timing_results_data.txt
expect_success "equals-form default label mode" \
    "${required[@]}" --label-mode=sequential-rng
expect_success "intended selectivity" \
    "${required[@]}" --count-only --selectivity intended
grep -q '^selectivity_mode: intended$' matching_results_data.txt
grep -q '^selectivity_mode: intended$' timing_results_data.txt
expect_success "equals-form paper selectivity" \
    "${required[@]}" --selectivity=paper
grep -q '^selectivity_formula_id: label_population_over_product_query_degree_mean_vertex_activity$' \
    timing_results_data.txt
expect_success "durability-survival selectivity" \
    "${required[@]}" --count-only --selectivity survival
grep -q '^selectivity_mode: survival$' matching_results_data.txt
grep -q '^root_structural_candidate_count: ' \
    timing_results_data.txt
expect_success "structural control selectivity" \
    "${required[@]}" --count-only --selectivity structural
grep -q '^selectivity_mode: structural$' matching_results_data.txt
expect_success "temporal cost criterion selectivity" \
    "${required[@]}" --count-only --selectivity temporal-cost
grep -q '^selectivity_mode: temporal-cost$' matching_results_data.txt
grep -q '^temporal_cost_estimated_total_candidates: ' \
    timing_results_data.txt
grep -q '^temporal_cost_beam_width: 64$' timing_results_data.txt
grep -q '^temporal_cost_sample_cap: 4096$' timing_results_data.txt
grep -Eq '^plan_parent: (-1 0|1 -1)$' matching_results_data.txt
grep -Eq '^plan_parent: (-1 0|1 -1)$' timing_results_data.txt
matching_plan_parent=$(sed -n 's/^plan_parent://p' matching_results_data.txt)
timing_plan_parent=$(sed -n 's/^plan_parent://p' timing_results_data.txt)
if [[ "$matching_plan_parent" != "$timing_plan_parent" ]]; then
    printf 'matching/timing plan_parent mismatch\n' >&2
    exit 1
fi
expect_success "temporal cost criterion overrides" \
    "${required[@]}" --count-only --selectivity temporal-cost \
    --tcc-beam-width 1 --tcc-sample-cap=17
grep -q '^temporal_cost_beam_width: 1$' timing_results_data.txt
grep -q '^temporal_cost_sample_cap: 17$' timing_results_data.txt
expect_success "temporal cost v2 KCI budget fallback" \
    "${required[@]}" --count-only --selectivity temporal-cost-v2 \
    --tcc-max-state-expansions 0
grep -q '^selectivity_mode: temporal-cost-v2$' matching_results_data.txt
grep -q '^tcc_v2_decision: kci-fallback$' timing_results_data.txt
grep -q '^tcc_v2_incumbent_selectivity_formula_id: label_population_over_product_query_degree_mean_vertex_activity$' \
    timing_results_data.txt
grep -q '^tcc_v2_fallback_reason: state-expansion-budget$' \
    timing_results_data.txt
grep -q '^tcc_v2_selected_tree_handoff: true$' timing_results_data.txt
grep -q '^tcc_v2_fixed_plan_scoring_method: deterministic-capped-fixed-plan$' \
    timing_results_data.txt
grep -q '^temporal_cost_planning_ns: ' timing_results_data.txt
grep -q '^controllableQueryProcessing_ns: ' timing_results_data.txt
grep -q '^buildTDTree: 0 ms$' timing_results_data.txt
if grep -q '^queryDecomposition: ' timing_results_data.txt; then
    printf 'temporal-cost-v2 emitted overlapping queryDecomposition timing\n' >&2
    exit 1
fi
expect_success "temporal cost v2 all controls" \
    "${required[@]}" --count-only --selectivity=tcc-v2 \
    --tcc-beam-width=2 --tcc-sample-cap 8 \
    --tcc-max-catalog-probes=1000 \
    --tcc-max-state-expansions 100 \
    --tcc-max-extension-probes=1000 \
    --tcc-max-interval-intersections 1000 \
    --tcc-pilot-extension-probes 4 \
    --tcc-pilot-prefix-cap=4 --tcc-rescore-top-l 2 \
    --tcc-max-candidate-trees=2 --tcc-opportunity-margin 0 \
    --tcc-catalog-probe-cost-weight=0 --tcc-cost-gate=off
grep -q '^tcc_v2_max_catalog_probes: 1000$' timing_results_data.txt
grep -q '^tcc_v2_max_state_expansions: 100$' timing_results_data.txt
grep -q '^tcc_v2_max_extension_probes: 1000$' timing_results_data.txt
grep -q '^tcc_v2_max_interval_intersections: 1000$' \
    timing_results_data.txt
grep -q '^tcc_v2_max_candidate_trees_built: 2$' timing_results_data.txt
grep -q '^tcc_v2_rescore_top_l: 2$' timing_results_data.txt
grep -q '^tcc_v2_cost_gate_enabled: false$' timing_results_data.txt
grep -q '^tcc_v2_cost_gate_bypassed: true$' timing_results_data.txt
if [[ $(grep -c '^tcc_v2_cost_gate_bypassed:' timing_results_data.txt) -ne 1 ]]; then
    printf 'cost-gate bypass telemetry must be one canonical boolean row\n' >&2
    exit 1
fi
expect_success "TD-Tree selectivity and prefilter off" \
    "${required[@]}" --count-only --selectivity tdtree \
    --prefilter off
grep -q '^selectivity_mode: tdtree$' timing_results_data.txt
grep -q '^prefilter: off$' timing_results_data.txt
grep -q '^metric_schema_version: logical_extensions_v1$' \
    timing_results_data.txt
expect_success "equals-form prefilter on" \
    "${required[@]}" --count-only --prefilter=on
expect_success "count-only threads" \
    "${required[@]}" --count-only --threads 32
grep -q '^threads_requested: 32$' timing_results_data.txt
grep -q '^threads_used: 32$' timing_results_data.txt
expect_success "equals-form threads" \
    "${required[@]}" --count-only --threads=2

expect_failure "duplicate count-only" \
    "${required[@]}" --count-only --count-only
expect_failure "invalid selectivity" \
    "${required[@]}" --selectivity invalid
expect_failure "missing selectivity" \
    "${required[@]}" --selectivity
expect_failure "duplicate selectivity" \
    "${required[@]}" --selectivity paper --selectivity=intended
expect_failure "invalid prefilter" \
    "${required[@]}" --prefilter maybe
expect_failure "missing prefilter" \
    "${required[@]}" --prefilter
expect_failure "duplicate prefilter" \
    "${required[@]}" --prefilter on --prefilter=off
expect_failure "unknown option" "${required[@]}" --unknown
expect_failure "malformed count-only" "${required[@]}" --count-only=true
expect_failure "duplicate seed" "${required[@]}" 42 43
expect_failure "invalid label mode" \
    "${required[@]}" --label-mode unstable
expect_failure "missing label mode" "${required[@]}" --label-mode
expect_failure "duplicate label mode" \
    "${required[@]}" --label-mode stable-id \
    --label-mode=sequential-rng
expect_failure "non-integer suffix in k" \
    "$temp_dir/data.txt" "$temp_dir/query.txt" 2junk
expect_failure "k below prefilter contract" \
    "$temp_dir/data.txt" "$temp_dir/query.txt" 1
expect_failure "zero threads" "${required[@]}" --threads 0
expect_failure "negative threads" "${required[@]}" --threads -1
expect_failure "non-integer threads" "${required[@]}" --threads many
expect_failure "missing threads" "${required[@]}" --threads
expect_failure "duplicate threads" \
    "${required[@]}" --threads 2 --threads=3
expect_failure "excessive threads" "${required[@]}" --threads 1025
expect_failure "missing TCC beam width" \
    "${required[@]}" --selectivity temporal-cost --tcc-beam-width
expect_failure "zero TCC beam width" \
    "${required[@]}" --selectivity temporal-cost --tcc-beam-width 0
expect_failure "negative TCC beam width" \
    "${required[@]}" --selectivity temporal-cost --tcc-beam-width -1
expect_failure "malformed TCC beam width" \
    "${required[@]}" --selectivity temporal-cost --tcc-beam-width many
expect_failure "duplicate TCC beam width" \
    "${required[@]}" --selectivity temporal-cost \
    --tcc-beam-width 1 --tcc-beam-width=2
expect_failure "missing TCC sample cap" \
    "${required[@]}" --selectivity temporal-cost --tcc-sample-cap
expect_failure "zero TCC sample cap" \
    "${required[@]}" --selectivity temporal-cost --tcc-sample-cap 0
expect_failure "negative TCC sample cap" \
    "${required[@]}" --selectivity temporal-cost --tcc-sample-cap -1
expect_failure "malformed TCC sample cap" \
    "${required[@]}" --selectivity temporal-cost --tcc-sample-cap many
expect_failure "duplicate TCC sample cap" \
    "${required[@]}" --selectivity temporal-cost \
    --tcc-sample-cap 17 --tcc-sample-cap=18
expect_failure "TCC controls without temporal-cost mode" \
    "${required[@]}" --selectivity paper --tcc-beam-width 1
expect_failure "v2 controls on legacy TCC" \
    "${required[@]}" --selectivity temporal-cost \
    --tcc-max-catalog-probes 1
expect_failure "duplicate v2 catalogue budget" \
    "${required[@]}" --selectivity temporal-cost-v2 \
    --tcc-max-catalog-probes 1 --tcc-max-catalog-probes=2
expect_failure "negative v2 extension budget" \
    "${required[@]}" --selectivity temporal-cost-v2 \
    --tcc-max-extension-probes -1
expect_failure "duplicate v2 interval budget" \
    "${required[@]}" --selectivity temporal-cost-v2 \
    --tcc-max-interval-intersections 0 \
    --tcc-max-interval-intersections=1
expect_failure "negative v2 interval budget" \
    "${required[@]}" --selectivity temporal-cost-v2 \
    --tcc-max-interval-intersections -1
expect_failure "malformed v2 pilot probe cap" \
    "${required[@]}" --selectivity temporal-cost-v2 \
    --tcc-pilot-extension-probes many
expect_failure "zero v2 pilot prefix cap" \
    "${required[@]}" --selectivity temporal-cost-v2 \
    --tcc-pilot-prefix-cap 0
expect_failure "zero v2 rescore top-L" \
    "${required[@]}" --selectivity temporal-cost-v2 \
    --tcc-rescore-top-l 0
expect_failure "duplicate v2 candidate tree budget" \
    "${required[@]}" --selectivity temporal-cost-v2 \
    --tcc-max-candidate-trees 0 --tcc-max-candidate-trees=1
expect_failure "negative v2 opportunity margin" \
    "${required[@]}" --selectivity temporal-cost-v2 \
    --tcc-opportunity-margin -0.1
expect_failure "non-finite v2 catalog weight" \
    "${required[@]}" --selectivity temporal-cost-v2 \
    --tcc-catalog-probe-cost-weight nan
expect_failure "invalid v2 cost gate" \
    "${required[@]}" --selectivity temporal-cost-v2 \
    --tcc-cost-gate maybe
expect_failure "duplicate v2 cost gate" \
    "${required[@]}" --selectivity temporal-cost-v2 \
    --tcc-cost-gate on --tcc-cost-gate=off
expect_failure "v2 cost gate on legacy TCC" \
    "${required[@]}" --selectivity temporal-cost \
    --tcc-cost-gate off
popd >/dev/null

printf 'Ours CLI tests passed.\n'
