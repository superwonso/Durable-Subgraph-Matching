#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <executable>" >&2
    exit 2
fi

executable="$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")"
temp_dir="$(mktemp -d "${TMPDIR:-/tmp}/ours_gopher_experiment_cli.XXXXXX")"
trap 'rm -rf -- "$temp_dir"' EXIT

data="$temp_dir/data.txt"
query="$temp_dir/query.txt"
printf '10 20 1\n10 20 2\n10 20 3\n' > "$data"
printf 'B D\n' > "$query"

variants=(
    durable-dfs
    dag-no-memo
    dag-full-prefix-memo
    dag-dep-key-memo
    dag-no-intern-no-memo
    dag-intern-no-memo
    dag-intern-full-prefix-memo
    dag-intern-dep-key-memo
)
expected_names=(
    DurableDFS
    DAG-NoMemo
    DAG-FullPrefixMemo
    DAG-DepKeyMemo
    DAG-NoIntern-NoMemo
    DAG-Intern-NoMemo
    DAG-Intern-FullPrefixMemo
    DAG-Intern-DepKeyMemo
)

reference_count=
for index in "${!variants[@]}"; do
    variant=${variants[$index]}
    output="$temp_dir/$variant"
    control_args=(--threads 2 --cache-mb 1 --cache-entry-mb 1)
    if [[ $variant == dag-no-intern-no-memo || $variant == dag-intern-* ]]; then
        control_args=(--threads 1 --cache-mb 256 --cache-entry-mb 8)
    fi
    "$executable" "$data" "$query" 2 42 \
        --experiment-variant "$variant" "${control_args[@]}" \
        --output-dir "$output" \
        >"$output.stdout"
    timing="$output/timing_results_data.txt"
    metrics="$output/metrics_results_data.json"

    grep -q "^experiment_variant: ${expected_names[$index]}$" "$timing"
    grep -q '^mode: count-only$' "$timing"
    grep -q '^order: paper$' "$timing"
    grep -q '^parallel_mode_requested: roots$' "$timing"
    grep -q '^memo_policy: legacy$' "$timing"
    grep -q '^cost_aware_admission: false$' "$timing"
    grep -q '^cache_minimum_work: 0$' "$timing"
    grep -q '^phase_schema_version: prepared_query_phases_v1$' "$timing"
    grep -q '^phase_sum_matches_total: true$' "$timing"
    grep -q '^runtime_key_diagnostics: off$' "$timing"
    grep -q '^runtime_key_diagnostic_only: false$' "$timing"
    grep -q '^correctness_preflight: false$' "$timing"
    grep -q '^off_overhead_comparison_eligible: true$' "$timing"
    grep -q '^primary_timing_eligible: true$' "$timing"
    if [[ $variant == dag-no-intern-no-memo ]]; then
        grep -q '^experiment_contract_version: candidate_dag_static_canonicalization_v1$' "$timing"
        grep -q '^dag_interning: no-intern$' "$timing"
        grep -q '"dag_interning": "no-intern"' "$metrics"
    elif [[ $variant == dag-intern-* ]]; then
        grep -q '^experiment_contract_version: candidate_dag_static_canonicalization_v1$' "$timing"
        grep -q '^dag_interning: hash-cons$' "$timing"
        grep -q '"dag_interning": "hash-cons"' "$metrics"
    else
        grep -q '^experiment_contract_version: candidate_dag_fair_v1$' "$timing"
    fi
    if [[ $variant == dag-no-intern-no-memo || $variant == dag-intern-* ]]; then
        grep -q '^threads_requested: 1$' "$timing"
        grep -q '^early_prune: on$' "$timing"
        grep -q '^cache_budget_bytes: 268435456$' "$timing"
        grep -q '^cache_max_entry_bytes: 8388608$' "$timing"
        grep -q '^cache_shards: 64$' "$timing"
    fi

    planning=$(awk -F': ' '$1 == "phase_planning_ns" {print $2}' "$timing")
    td=$(awk -F': ' '$1 == "phase_td_construction_ns" {print $2}' "$timing")
    dag=$(awk -F': ' '$1 == "phase_dag_compilation_ns" {print $2}' "$timing")
    enumeration=$(awk -F': ' '$1 == "phase_enumeration_ns" {print $2}' "$timing")
    finalization=$(awk -F': ' '$1 == "phase_output_finalization_ns" {print $2}' "$timing")
    total=$(awk -F': ' '$1 == "phase_prepared_query_total_ns" {print $2}' "$timing")
    controllable=$(awk -F': ' '$1 == "controllableQueryProcessing_ns" {print $2}' "$timing")
    [[ $((planning + td + dag + enumeration + finalization)) -eq $total ]]
    [[ $total -eq $controllable ]]

    if [[ $variant == durable-dfs ]]; then
        grep -q '^dag_compilation_applicable: false$' "$timing"
        [[ $dag -eq 0 ]]
    else
        grep -q '^dag_compilation_applicable: true$' "$timing"
    fi

    count=$(awk -F': ' '$1 == "match_count" {print $2}' "$output.stdout")
    if [[ -z $reference_count ]]; then
        reference_count=$count
    else
        [[ $count == "$reference_count" ]]
    fi
    /usr/bin/python3 -X pycache_prefix=/dev/null -S -B -m json.tool "$metrics" >/dev/null
    grep -q '"correctness_preflight": false' "$metrics"
done

preflight_output="$temp_dir/dag-no-memo-preflight-off"
"$executable" "$data" "$query" 2 42 \
    --experiment-variant dag-no-memo --threads 2 \
    --correctness-preflight --result-digest on --statistics off \
    --output-dir "$preflight_output" >"$preflight_output.stdout"
preflight_timing="$preflight_output/timing_results_data.txt"
preflight_metrics="$preflight_output/metrics_results_data.json"
grep -q '^correctness_preflight: true$' "$preflight_timing"
grep -q '^result_digest_enabled: on$' "$preflight_timing"
grep -q '^order: paper$' "$preflight_timing"
grep -q '^parallel_mode_requested: roots$' "$preflight_timing"
grep -q '^cost_aware_admission: false$' "$preflight_timing"
grep -q '^cache_minimum_work: 0$' "$preflight_timing"
grep -Eq '^result_digest: [0-9a-f]{16}:[0-9a-f]{16}$' \
    "$preflight_timing"
grep -q '^off_overhead_comparison_eligible: false$' \
    "$preflight_timing"
grep -q '^primary_timing_eligible: false$' "$preflight_timing"
grep -q '^correctness_preflight: true$' "$preflight_output.stdout"
grep -Eq '^result_digest: [0-9a-f]{16}:[0-9a-f]{16}$' \
    "$preflight_output.stdout"
/usr/bin/python3 -X pycache_prefix=/dev/null -S -B -m json.tool "$preflight_metrics" >/dev/null
grep -q '"correctness_preflight": true' "$preflight_metrics"
grep -q '"result_digest_enabled": true' "$preflight_metrics"
grep -q '"off_overhead_comparison_eligible": false' \
    "$preflight_metrics"
grep -q '"primary_timing_eligible": false' "$preflight_metrics"

e2_preflight_output="$temp_dir/dag-no-intern-preflight"
"$executable" "$data" "$query" 2 42 \
    --experiment-variant dag-no-intern-no-memo --threads 1 \
    --correctness-preflight --result-digest on --statistics off \
    --output-dir "$e2_preflight_output" >"$e2_preflight_output.stdout"
e2_preflight_metrics="$e2_preflight_output/metrics_results_data.json"
/usr/bin/python3 -X pycache_prefix=/dev/null -S -B -m json.tool "$e2_preflight_metrics" >/dev/null
grep -q '"schema_version": "candidate_dag_compilation_audit_v1"' \
    "$e2_preflight_metrics"
grep -q '"interning_mode": "no-intern"' "$e2_preflight_metrics"
grep -q '"nodes_merged": 0' "$e2_preflight_metrics"
/usr/bin/python3 -X pycache_prefix=/dev/null -S -B - "$e2_preflight_metrics" <<'PY'
import json
import sys

metrics = json.load(open(sys.argv[1], encoding="utf-8"))
topology = metrics["dag_topology"]
assert topology["nodes_materialized"] == topology["nodes_attempted"]
assert sum(topology["op_node_attempt_counts"].values()) == topology["nodes_attempted"]
assert topology["fanout_consumer_scope"] == (
    "internal parent-edge consumers only; structural, temporal, and "
    "longest-run root-array references are excluded"
)
assert topology["reachable_topology_scope"].startswith(
    "nodes reachable from structural, temporal, and longest-run root arrays"
)
audit = metrics["dag_compilation_audit"]
assert audit["attempt_count"] == topology["nodes_attempted"]
assert len(audit["attempts"]) == topology["nodes_attempted"]
assert len(audit["materialized_nodes"]) == topology["nodes_materialized"]
assert audit["canonical_parent_deduplicated_node_ids_scope"] == (
    "diagnostic-local materialized node IDs only; never compared across "
    "interning modes"
)
PY

preflight_statistics_output="$temp_dir/dag-no-memo-preflight-on"
"$executable" "$data" "$query" 2 42 \
    --experiment-variant dag-no-memo --threads 1 \
    --correctness-preflight --result-digest on --statistics on \
    --output-dir "$preflight_statistics_output" \
    >"$preflight_statistics_output.stdout"
preflight_statistics_timing="$preflight_statistics_output/timing_results_data.txt"
grep -q '^correctness_preflight: true$' \
    "$preflight_statistics_timing"
grep -q '^statistics_enabled: on$' "$preflight_statistics_timing"
grep -q '^off_overhead_comparison_eligible: false$' \
    "$preflight_statistics_timing"
grep -q '^primary_timing_eligible: false$' \
    "$preflight_statistics_timing"
preflight_digest=$(awk \
    '$1 == "result_digest:" {sub(/^result_digest: /, ""); print}' \
    "$preflight_timing")
preflight_statistics_digest=$(awk \
    '$1 == "result_digest:" {sub(/^result_digest: /, ""); print}' \
    "$preflight_statistics_timing")
[[ $preflight_digest == "$preflight_statistics_digest" ]]

diagnostic_output="$temp_dir/dag-no-memo-exact-diagnostic"
"$executable" "$data" "$query" 2 42 \
    --experiment-variant dag-no-memo --threads 1 \
    --runtime-key-diagnostics exact --exact-runtime-key-cap 1000 \
    --statistics on --output-dir "$diagnostic_output" \
    >"$diagnostic_output.stdout"
diagnostic_timing="$diagnostic_output/timing_results_data.txt"
diagnostic_metrics="$diagnostic_output/metrics_results_data.json"
grep -q '^runtime_key_diagnostics: exact$' "$diagnostic_timing"
grep -q '^exact_runtime_key_cap: 1000$' "$diagnostic_timing"
grep -q '^runtime_key_diagnostic_only: true$' "$diagnostic_timing"
grep -q '^primary_timing_eligible: false$' "$diagnostic_timing"
grep -q '^exact_runtime_key_diagnostics: on$' "$diagnostic_timing"
grep -Eq '^exact_runtime_key_invocations: [1-9][0-9]*$' \
    "$diagnostic_timing"
grep -Eq '^exact_runtime_key_distinct: [1-9][0-9]*$' \
    "$diagnostic_timing"
grep -q '^exact_runtime_key_invocations_td_block_lookup:' \
    "$diagnostic_timing"
grep -q '^dag_operator_statistics_schema_version: dag_operator_summary_v1$' \
    "$diagnostic_timing"
grep -q '^dag_operator_reconciliation_passed: true$' \
    "$diagnostic_timing"
grep -q '^parallel_statistics_collected: true$' \
    "$diagnostic_timing"
for operation in \
    td_block_lookup out_neighbor_lookup in_neighbor_lookup \
    structural_intersection edge_time_lookup temporal_intersection \
    longest_consecutive_run; do
    grep -Eq "^dag_operator_invocations_${operation}: [0-9]+$" \
        "$diagnostic_timing"
    grep -Eq "^dag_operator_result_observations_${operation}: [0-9]+$" \
        "$diagnostic_timing"
    grep -q "^dag_operator_cache_hits_${operation}: 0$" \
        "$diagnostic_timing"
    grep -q "^dag_operator_cache_misses_${operation}: 0$" \
        "$diagnostic_timing"
done
grep -Eq '^longest_run_returned_length_observations: [0-9]+$' \
    "$diagnostic_timing"
/usr/bin/python3 -X pycache_prefix=/dev/null -S -B -m json.tool "$diagnostic_metrics" >/dev/null
grep -q '"runtime_key_diagnostics": "exact"' "$diagnostic_metrics"
grep -q '"runtime_key_diagnostic_only": true' "$diagnostic_metrics"
grep -q '"primary_timing_eligible": false' "$diagnostic_metrics"
grep -Eq '"exact_runtime_key_invocations": [1-9][0-9]*' \
    "$diagnostic_metrics"
grep -q '"exact_runtime_key_distinct_td_block_lookup":' \
    "$diagnostic_metrics"
grep -q '"dag_operator_reconciliation_passed": true' \
    "$diagnostic_metrics"
grep -q '"dag_operator_result_units_mean_longest_consecutive_run":' \
    "$diagnostic_metrics"
grep -q '"longest_run_returned_length_mean":' \
    "$diagnostic_metrics"

statistics_output="$temp_dir/dag-no-memo-statistics-on"
"$executable" "$data" "$query" 2 42 \
    --experiment-variant dag-no-memo --threads 1 \
    --statistics on --output-dir "$statistics_output" \
    >"$statistics_output.stdout"
statistics_timing="$statistics_output/timing_results_data.txt"
statistics_metrics="$statistics_output/metrics_results_data.json"
grep -q '^runtime_key_diagnostics: off$' "$statistics_timing"
grep -q '^statistics_enabled: on$' "$statistics_timing"
grep -q '^primary_timing_eligible: false$' "$statistics_timing"
grep -q '"primary_timing_eligible": false' "$statistics_metrics"

if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-dep-key-memo --parallel prefix \
    --output-dir "$temp_dir/bad-prefix" >/dev/null 2>&1; then
    echo "experiment contract accepted prefix scheduling" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-no-memo --result-digest on \
    --output-dir "$temp_dir/bad-primary-digest" >/dev/null 2>&1; then
    echo "primary experiment contract accepted a result digest" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-no-memo --correctness-preflight \
    --output-dir "$temp_dir/bad-preflight-no-digest" >/dev/null 2>&1; then
    echo "correctness preflight accepted a missing result digest" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --correctness-preflight --result-digest on \
    --output-dir "$temp_dir/bad-preflight-no-variant" >/dev/null 2>&1; then
    echo "correctness preflight accepted a non-experiment run" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-no-memo \
    --runtime-key-diagnostics exact --statistics on \
    --output-dir "$temp_dir/bad-exact-no-cap" >/dev/null 2>&1; then
    echo "exact diagnostics accepted a missing cap" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-no-memo \
    --runtime-key-diagnostics exact --exact-runtime-key-cap 1000 \
    --output-dir "$temp_dir/bad-exact-statistics-off" >/dev/null 2>&1; then
    echo "exact diagnostics entered a closed arm without statistics on" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant durable-dfs --statistics on \
    --runtime-key-diagnostics exact --exact-runtime-key-cap 1000 \
    --output-dir "$temp_dir/bad-exact-direct" >/dev/null 2>&1; then
    echo "exact DAG-key diagnostics accepted DurableDFS" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-no-memo --exact-runtime-key-cap 1000 \
    --output-dir "$temp_dir/bad-cap-without-exact" >/dev/null 2>&1; then
    echo "exact-key cap was accepted without exact diagnostics" >&2
    exit 1
fi
cap_error="$temp_dir/exact-cap-error.txt"
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-no-memo --statistics on \
    --runtime-key-diagnostics exact --exact-runtime-key-cap 1 \
    --output-dir "$temp_dir/exact-cap-breach" \
    >/dev/null 2>"$cap_error"; then
    echo "exact diagnostics silently continued after its key cap" >&2
    exit 1
fi
grep -q 'exact runtime-key distinct-key cap exceeded' "$cap_error"
if "$executable" "$data" "$query" 2 \
    --runtime-key-diagnostics approximate \
    --output-dir "$temp_dir/bad-approximate" >/dev/null 2>&1; then
    echo "production CLI exposed approximate key diagnostics" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-dep-key-memo --order reuse \
    --output-dir "$temp_dir/bad-order" >/dev/null 2>&1; then
    echo "experiment contract accepted reuse-aware order" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-dep-key-memo --memo-key required-bindings \
    --output-dir "$temp_dir/bad-key-override" >/dev/null 2>&1; then
    echo "experiment contract accepted a key override" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-no-intern-no-memo --memo dag \
    --output-dir "$temp_dir/bad-nointern-memo" >/dev/null 2>&1; then
    echo "NoIntern experiment arm accepted a memo override" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-no-intern-no-memo --threads 2 \
    --output-dir "$temp_dir/bad-e2-threads" >/dev/null 2>&1; then
    echo "E2 experiment contract accepted two threads" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-intern-no-memo --selectivity structural \
    --output-dir "$temp_dir/bad-e2-selectivity" >/dev/null 2>&1; then
    echo "E2 experiment contract accepted non-paper selectivity" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-intern-full-prefix-memo --early-prune off \
    --output-dir "$temp_dir/bad-e2-pruning" >/dev/null 2>&1; then
    echo "E2 experiment contract accepted disabled early pruning" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-intern-dep-key-memo --cache-mb 1 \
    --output-dir "$temp_dir/bad-e2-cache" >/dev/null 2>&1; then
    echo "E2 experiment contract accepted a noncanonical cache budget" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-intern-dep-key-memo --cache-entry-mb 1 \
    --output-dir "$temp_dir/bad-e2-entry" >/dev/null 2>&1; then
    echo "E2 experiment contract accepted a noncanonical entry cap" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --experiment-variant dag-dep-key-memo --cache-min-work 999 \
    --output-dir "$temp_dir/bad-work-threshold" >/dev/null 2>&1; then
    echo "experiment contract accepted a nonzero work threshold" >&2
    exit 1
fi
if "$executable" "$data" "$query" 2 \
    --memo-key static-node-only \
    --output-dir "$temp_dir/bad-static-key" >/dev/null 2>&1; then
    echo "production CLI exposed the unsound static-only key" >&2
    exit 1
fi

echo "Experiment CLI tests passed."
