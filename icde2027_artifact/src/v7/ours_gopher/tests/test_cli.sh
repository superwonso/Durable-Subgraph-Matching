#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <executable>" >&2
    exit 2
fi

executable="$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")"
temp_dir="$(mktemp -d "${TMPDIR:-/tmp}/ours_gopher_cli.XXXXXX")"
trap 'rm -rf -- "$temp_dir"' EXIT

data="$temp_dir/data.txt"
query="$temp_dir/query.txt"
printf '10 20 1\n10 20 2\n10 20 3\n' > "$data"
# mt19937(seed=42) assigns the two sorted data vertices labels B and D.
printf 'B D\n' > "$query"

run_success() {
    local label="$1"
    shift
    if ! "$executable" "$@" >/dev/null 2>&1; then
        echo "CLI success case failed: $label" >&2
        exit 1
    fi
}

run_failure() {
    local label="$1"
    shift
    if "$executable" "$@" >/dev/null 2>&1; then
        echo "CLI failure case unexpectedly succeeded: $label" >&2
        exit 1
    fi
}

run_success "help" --help
run_success "default full" \
    "$data" "$query" 2 42 --output-dir "$temp_dir/default"
grep -q '^mode: full$' \
    "$temp_dir/default/matching_results_data.txt"

fixture_dir="$(cd -- "$(dirname -- "$0")/fixtures" && pwd)"
projection_data="$fixture_dir/label_projection_generic_c.txt"
projection_query="$fixture_dir/label_projection_generic_c.qry"
projection_map="$fixture_dir/label_projection_global20.json"
projection_spec="$fixture_dir/label_projection_generic_c.json"
run_success "explicit query label projection" \
    "$projection_data" "$projection_query" 3 --count-only \
    --memo none --global-label-map "$projection_map" \
    --query-label-projection "$projection_spec" \
    --output-dir "$temp_dir/explicit_projection"
projection_timing="$temp_dir/explicit_projection/timing_results_label_projection_generic_c.txt"
projection_metrics="$temp_dir/explicit_projection/metrics_results_label_projection_generic_c.json"
grep -q '^label_mode: explicit-projection$' "$projection_timing"
grep -q '^label_projection_validation: validated$' "$projection_timing"
grep -q '^selected_global_classes_in_A_to_E_order: 1,2,3$' "$projection_timing"
grep -q '^projected_invalid_vertex_count: 2$' "$projection_timing"
grep -q '"match_count": 6' "$projection_metrics"
grep -q '"validation": "validated"' "$projection_metrics"
grep -q '"selected_global_classes_in_A_to_E_order": \[1, 2, 3\]' \
    "$projection_metrics"

run_success "all Gopher options" \
    "$data" "$query" 2 --count-only 42 \
    --label-mode stable-id \
    --engine gopher --selectivity intended --order reuse --memo dag \
    --memo-policy structural-v2 \
    --cache-mb 1 --cache-entry-mb 1 --cache-min-work 0 \
    --no-cost-aware --threads 4 --parallel prefix \
    --prefix-depth 0 --tasks-per-thread 2 --early-prune off \
    --result-digest on --statistics off \
    --verify-dag --output-dir "$temp_dir/gopher" \
    --metrics "$temp_dir/gopher/custom.json"
grep -q '^mode: count-only$' \
    "$temp_dir/gopher/matching_results_data.txt"
grep -q '^early_prune: off$' \
    "$temp_dir/gopher/timing_results_data.txt"
grep -q '"schema_version": 2' \
    "$temp_dir/gopher/custom.json"
grep -q '"dag_operator_statistics_schema_version": "dag_operator_summary_v1"' \
    "$temp_dir/gopher/custom.json"
grep -q '"search_statistics_hooks_erased_at_build": false' \
    "$temp_dir/gopher/custom.json"
grep -q '"search_statistics_build_profile": "product"' \
    "$temp_dir/gopher/custom.json"
grep -q '"gopher_build_identity_bound": true' \
    "$temp_dir/gopher/custom.json"
grep -q '"off_overhead_comparison_eligible": true' \
    "$temp_dir/gopher/custom.json"
grep -q '"selectivity_mode": "intended"' \
    "$temp_dir/gopher/custom.json"
grep -q '"label_mode": "stable-id"' \
    "$temp_dir/gopher/custom.json"
grep -q '"memo_policy": "structural-v2"' \
    "$temp_dir/gopher/custom.json"
grep -q '^memo_policy: structural-v2$' \
    "$temp_dir/gopher/timing_results_data.txt"
grep -q '^label_mode: stable-id$' \
    "$temp_dir/gopher/timing_results_data.txt"
grep -Eq '^temporal_graph_content_fingerprint_fnv1a64: [0-9]+$' \
    "$temp_dir/gopher/timing_results_data.txt"
grep -Eq '"temporal_graph_content_fingerprint_fnv1a64": [0-9]+' \
    "$temp_dir/gopher/custom.json"
grep -q '"parallel_mode_requested": "prefix"' \
    "$temp_dir/gopher/custom.json"
grep -q '"parallel_statistics": {' \
    "$temp_dir/gopher/custom.json"
grep -q '"dag_topology": {' \
    "$temp_dir/gopher/custom.json"
grep -Eq '"nodes_materialized": [1-9][0-9]*' \
    "$temp_dir/gopher/custom.json"
grep -q '"op_node_counts": {' \
    "$temp_dir/gopher/custom.json"
grep -q '"parent": \[' \
    "$temp_dir/gopher/custom.json"
grep -Eq '"query_fingerprint": [1-9][0-9]*' \
    "$temp_dir/gopher/custom.json"
grep -q '"worker_dfs_states_total":' \
    "$temp_dir/gopher/custom.json"
grep -q '"raw_extension_candidates": null' \
    "$temp_dir/gopher/custom.json"
grep -q '"dag_operator_invocations_td_block_lookup": null' \
    "$temp_dir/gopher/custom.json"
grep -q '"dag_operator_reconciliation_passed": null' \
    "$temp_dir/gopher/custom.json"
grep -q '"diagnostics_enabled": false' \
    "$temp_dir/gopher/custom.json"
grep -q '"worker_active_nanoseconds_total": null' \
    "$temp_dir/gopher/custom.json"
grep -q '"structural_v2_eligible_nodes": null' \
    "$temp_dir/gopher/custom.json"
grep -q '^statistics_collected: false$' \
    "$temp_dir/gopher/timing_results_data.txt"
grep -q '^parallel_statistics_collected: false$' \
    "$temp_dir/gopher/timing_results_data.txt"
grep -q '^parallel_statistics_collected: false$' \
    "$temp_dir/gopher/matching_results_data.txt"
if grep -q '^parallel_worker_active_ns_total:' \
    "$temp_dir/gopher/timing_results_data.txt"; then
    echo "statistics-off timing receipt exposed worker diagnostics" >&2
    exit 1
fi
if grep -q '^parallel_root_tasks_claimed:' \
    "$temp_dir/gopher/matching_results_data.txt"; then
    echo "statistics-off result receipt exposed scheduler diagnostics" >&2
    exit 1
fi
if grep -q '^Match ' \
    "$temp_dir/gopher/matching_results_data.txt"; then
    echo "count-only output contains match rows" >&2
    exit 1
fi

for memo in none structural temporal dag; do
    run_success "memo $memo" \
        "$data" "$query" 2 --count-only \
        --memo "$memo" --output-dir "$temp_dir/memo_$memo"
done

for policy in legacy structural-v2; do
    run_success "memo policy $policy" \
        "$data" "$query" 2 --count-only \
        --memo-policy "$policy" \
        --output-dir "$temp_dir/policy_$policy"
    grep -q "^memo_policy: $policy$" \
        "$temp_dir/policy_$policy/timing_results_data.txt"
done

run_success "single-thread paper mode" \
    "$data" "$query" 2 --count-only \
    --selectivity paper --threads 1 \
    --output-dir "$temp_dir/single"
grep -q '^selectivity_mode: paper$' \
    "$temp_dir/single/timing_results_data.txt"
grep -q '^selectivity_formula_id: label_population_over_product_query_degree_mean_vertex_activity$' \
    "$temp_dir/single/timing_results_data.txt"
grep -q '^threads_requested: 1$' \
    "$temp_dir/single/timing_results_data.txt"
grep -q '^threads_used: 1$' \
    "$temp_dir/single/timing_results_data.txt"
grep -q '^parallel_mode_requested: roots$' \
    "$temp_dir/single/timing_results_data.txt"

run_success "TD-Tree selectivity mode" \
    "$data" "$query" 2 --count-only \
    --selectivity tdtree --threads 1 \
    --output-dir "$temp_dir/tdtree"
grep -q '^selectivity_mode: tdtree$' \
    "$temp_dir/tdtree/timing_results_data.txt"

for selectivity in structural survival temporal-cost; do
    run_success "$selectivity selectivity mode" \
        "$data" "$query" 2 --count-only \
        --selectivity "$selectivity" --threads 1 \
        --output-dir "$temp_dir/$selectivity"
    grep -q "^selectivity_mode: $selectivity$" \
        "$temp_dir/$selectivity/timing_results_data.txt"
done
grep -q '^tcc_beam_width: 64$' \
    "$temp_dir/temporal-cost/timing_results_data.txt"
grep -q '^tcc_root_plans_evaluated: 2$' \
    "$temp_dir/temporal-cost/timing_results_data.txt"
grep -q '"tcc_sample_cap": 4096' \
    "$temp_dir/temporal-cost/metrics_results_data.json"

run_success "temporal-cost-v2 budgeted handoff" \
    "$data" "$query" 2 --count-only \
    --selectivity temporal-cost-v2 --tcc-max-state-expansions 0 \
    --output-dir "$temp_dir/temporal-cost-v2"
v2_timing="$temp_dir/temporal-cost-v2/timing_results_data.txt"
v2_metrics="$temp_dir/temporal-cost-v2/metrics_results_data.json"
grep -q '^selectivity_mode: temporal-cost-v2$' "$v2_timing"
grep -q '^tcc_v2_fallback_reason: state-expansion-budget$' "$v2_timing"
grep -q '^tcc_v2_incumbent_selectivity_formula_id: label_population_over_product_query_degree_mean_vertex_activity$' \
    "$v2_timing"
grep -q '^tcc_v2_selected_tree_handoff: true$' "$v2_timing"
grep -q '^tcc_v2_fixed_plan_scoring_method: deterministic-capped-fixed-plan$' \
    "$v2_timing"
grep -q '^buildTDTree_ns: 0$' "$v2_timing"
if grep -q '^queryDecomposition_ns:' "$v2_timing"; then
    echo "temporal-cost-v2 emitted overlapping queryDecomposition timing" >&2
    exit 1
fi
grep -q '"temporalCostPlanning":' "$v2_metrics"
grep -q '"controllableQueryProcessing":' "$v2_metrics"

run_success "temporal-cost-v2 gate-off development arm" \
    "$data" "$query" 2 --count-only \
    --selectivity temporal-cost-v2 --tcc-beam-width 2 \
    --tcc-sample-cap 8 --tcc-max-catalog-probes 1000 \
    --tcc-max-state-expansions 100 --tcc-max-extension-probes 1000 \
    --tcc-max-interval-intersections 1000 \
    --tcc-pilot-extension-probes 4 --tcc-pilot-prefix-cap 4 \
    --tcc-rescore-top-l 2 --tcc-max-candidate-trees 2 \
    --tcc-opportunity-margin 0 --tcc-catalog-probe-cost-weight 0 \
    --tcc-cost-gate off --output-dir "$temp_dir/temporal-cost-v2-off"
grep -q '^tcc_v2_cost_gate_enabled: false$' \
    "$temp_dir/temporal-cost-v2-off/timing_results_data.txt"
grep -q '^tcc_v2_cost_gate_bypassed: true$' \
    "$temp_dir/temporal-cost-v2-off/timing_results_data.txt"
grep -q '"tcc_v2_cost_gate_bypassed": true' \
    "$temp_dir/temporal-cost-v2-off/metrics_results_data.json"
grep -q '"tcc_v2_fixed_plan_scores": \[' \
    "$temp_dir/temporal-cost-v2-off/metrics_results_data.json"
grep -q '^tcc_v2_max_interval_intersections: 1000$' \
    "$temp_dir/temporal-cost-v2-off/timing_results_data.txt"

run_success "legacy engine" \
    "$data" "$query" 2 --count-only \
    --engine legacy --output-dir "$temp_dir/legacy"

run_failure "duplicate count-only" \
    "$data" "$query" 2 --count-only --count-only
run_failure "unknown option" \
    "$data" "$query" 2 --unknown
run_failure "invalid memo" \
    "$data" "$query" 2 --memo invalid
run_failure "invalid memo policy" \
    "$data" "$query" 2 --memo-policy invalid
run_failure "missing memo policy" \
    "$data" "$query" 2 --memo-policy
run_failure "duplicate memo policy" \
    "$data" "$query" 2 --memo-policy legacy \
    --memo-policy structural-v2
run_failure "invalid selectivity" \
    "$data" "$query" 2 --selectivity invalid
run_failure "invalid label mode" \
    "$data" "$query" 2 --label-mode unstable
run_failure "global label map without query projection" \
    "$projection_data" "$projection_query" 3 \
    --global-label-map "$projection_map"
run_failure "query projection without global label map" \
    "$projection_data" "$projection_query" 3 \
    --query-label-projection "$projection_spec"
run_failure "explicit projection with label mode" \
    "$projection_data" "$projection_query" 3 \
    --global-label-map "$projection_map" \
    --query-label-projection "$projection_spec" \
    --label-mode stable-id
run_failure "explicit projection with positional seed" \
    "$projection_data" "$projection_query" 3 42 \
    --global-label-map "$projection_map" \
    --query-label-projection "$projection_spec"
run_failure "missing label mode" \
    "$data" "$query" 2 --label-mode
run_failure "duplicate label mode" \
    "$data" "$query" 2 --label-mode stable-id \
    --label-mode sequential-rng
run_failure "missing selectivity" \
    "$data" "$query" 2 --selectivity
run_failure "duplicate selectivity" \
    "$data" "$query" 2 --selectivity paper \
    --selectivity intended
run_failure "zero threads" \
    "$data" "$query" 2 --threads 0
run_failure "invalid parallel mode" \
    "$data" "$query" 2 --parallel invalid
run_failure "duplicate parallel mode" \
    "$data" "$query" 2 --parallel roots --parallel prefix
run_failure "invalid prefix depth" \
    "$data" "$query" 2 --prefix-depth -1
run_failure "zero tasks per thread" \
    "$data" "$query" 2 --tasks-per-thread 0
run_failure "invalid early prune" \
    "$data" "$query" 2 --early-prune maybe
run_failure "invalid result digest" \
    "$data" "$query" 2 --result-digest maybe
run_failure "invalid statistics" \
    "$data" "$query" 2 --statistics maybe
run_failure "duplicate seed" \
    "$data" "$query" 2 42 43
run_failure "non-integer k" \
    "$data" "$query" 2junk
run_failure "k below preprocessing contract" \
    "$data" "$query" 1
run_failure "invalid v2 cost gate" \
    "$data" "$query" 2 --selectivity temporal-cost-v2 \
    --tcc-cost-gate maybe
run_failure "duplicate v2 cost gate" \
    "$data" "$query" 2 --selectivity temporal-cost-v2 \
    --tcc-cost-gate on --tcc-cost-gate off
run_failure "v2 cost gate on legacy TCC" \
    "$data" "$query" 2 --selectivity temporal-cost \
    --tcc-cost-gate off
run_failure "zero v2 top-L" \
    "$data" "$query" 2 --selectivity temporal-cost-v2 \
    --tcc-rescore-top-l 0
run_failure "duplicate v2 tree budget" \
    "$data" "$query" 2 --selectivity temporal-cost-v2 \
    --tcc-max-candidate-trees 0 --tcc-max-candidate-trees 1
run_failure "duplicate v2 interval budget" \
    "$data" "$query" 2 --selectivity temporal-cost-v2 \
    --tcc-max-interval-intersections 0 \
    --tcc-max-interval-intersections 1

echo "Ours_gopher CLI tests passed."
