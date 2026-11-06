#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <erased-executable>" >&2
    exit 2
fi

executable="$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")"
temp_dir="$(mktemp -d "${TMPDIR:-/tmp}/ours_gopher_erased.XXXXXX")"
trap 'rm -rf -- "$temp_dir"' EXIT

data="$temp_dir/data.txt"
query="$temp_dir/query.txt"
printf '10 20 1\n10 20 2\n10 20 3\n' > "$data"
printf 'B D\n' > "$query"

output="$temp_dir/control"
"$executable" "$data" "$query" 2 42 \
    --experiment-variant dag-no-memo --threads 2 \
    --output-dir "$output" >"$temp_dir/control.stdout"
timing="$output/timing_results_data.txt"
metrics="$output/metrics_results_data.json"

python3 -m json.tool "$metrics" >/dev/null
grep -q '^search_statistics_hooks_erased_at_build: true$' "$timing"
grep -q '^search_statistics_build_profile: erased-control$' "$timing"
grep -q '^gopher_build_identity_bound: true$' "$timing"
grep -q '^statistics_enabled: off$' "$timing"
grep -q '^off_overhead_comparison_eligible: true$' "$timing"
grep -q '^primary_timing_eligible: false$' "$timing"
grep -q '^parallel_statistics_enabled: off$' "$timing"
grep -q '^parallel_statistics_collected: false$' "$timing"
if grep -q '^parallel_worker_active_ns_total:' "$timing"; then
    echo "erased-control receipt exposed worker diagnostics" >&2
    exit 1
fi
grep -q '"search_statistics_hooks_erased_at_build": true' "$metrics"
grep -q '"search_statistics_build_profile": "erased-control"' "$metrics"
grep -q '"gopher_build_identity_bound": true' "$metrics"
grep -Eq '"gopher_source_identity": "(test-source-identity|[0-9a-f]{64})"' \
    "$metrics"
grep -Eq '"gopher_common_flags_identity": "(test-flags-identity|[0-9a-f]{64})"' \
    "$metrics"
grep -q '"diagnostics_enabled": false' "$metrics"
grep -q '"worker_active_nanoseconds_total": null' "$metrics"
grep -q '"dag_operator_invocations_td_block_lookup": null' "$metrics"
grep -q '"dag_operator_reconciliation_passed": null' "$metrics"

preflight_output="$temp_dir/preflight"
"$executable" "$data" "$query" 2 42 \
    --experiment-variant dag-no-memo --threads 2 \
    --correctness-preflight --result-digest on --statistics off \
    --output-dir "$preflight_output" >"$temp_dir/preflight.stdout"
preflight_timing="$preflight_output/timing_results_data.txt"
preflight_metrics="$preflight_output/metrics_results_data.json"
grep -q '^correctness_preflight: true$' "$preflight_timing"
grep -q '^statistics_enabled: off$' "$preflight_timing"
grep -q '^order: paper$' "$preflight_timing"
grep -q '^parallel_mode_requested: roots$' "$preflight_timing"
grep -q '^cost_aware_admission: false$' "$preflight_timing"
grep -q '^cache_minimum_work: 0$' "$preflight_timing"
grep -Eq '^result_digest: [0-9a-f]{16}:[0-9a-f]{16}$' \
    "$preflight_timing"
grep -q '^off_overhead_comparison_eligible: false$' \
    "$preflight_timing"
grep -q '^primary_timing_eligible: false$' "$preflight_timing"
python3 -m json.tool "$preflight_metrics" >/dev/null
grep -q '"correctness_preflight": true' "$preflight_metrics"
grep -q '"off_overhead_comparison_eligible": false' \
    "$preflight_metrics"

if "$executable" "$data" "$query" 2 42 \
    --experiment-variant dag-no-memo --statistics on \
    --output-dir "$temp_dir/bad-statistics" >/dev/null 2>&1; then
    echo "erased control accepted --statistics on" >&2
    exit 1
fi

if "$executable" "$data" "$query" 2 42 \
    --experiment-variant dag-no-memo --correctness-preflight \
    --result-digest on --statistics on \
    --output-dir "$temp_dir/bad-preflight-statistics" \
    >/dev/null 2>&1; then
    echo "erased preflight accepted --statistics on" >&2
    exit 1
fi

if "$executable" "$data" "$query" 2 42 \
    --experiment-variant dag-no-memo --statistics on \
    --runtime-key-diagnostics exact --exact-runtime-key-cap 1000 \
    --output-dir "$temp_dir/bad-exact" >/dev/null 2>&1; then
    echo "erased control accepted exact diagnostics" >&2
    exit 1
fi

echo "Erased statistics-control CLI tests passed."
