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
printf 'A B\n' > "$query"

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

run_success "all Gopher options" \
    "$data" "$query" 2 --count-only 42 \
    --label-mode stable-id \
    --engine gopher --selectivity intended --order reuse --memo dag \
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
grep -q '"selectivity_mode": "intended"' \
    "$temp_dir/gopher/custom.json"
grep -q '"label_mode": "stable-id"' \
    "$temp_dir/gopher/custom.json"
grep -q '^label_mode: stable-id$' \
    "$temp_dir/gopher/timing_results_data.txt"
grep -q '"parallel_mode_requested": "prefix"' \
    "$temp_dir/gopher/custom.json"
grep -q '"parallel_statistics": {' \
    "$temp_dir/gopher/custom.json"
grep -q '"worker_dfs_states_total":' \
    "$temp_dir/gopher/custom.json"
grep -q '"raw_extension_candidates": null' \
    "$temp_dir/gopher/custom.json"
grep -q '^statistics_collected: false$' \
    "$temp_dir/gopher/timing_results_data.txt"
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

run_success "single-thread paper mode" \
    "$data" "$query" 2 --count-only \
    --selectivity paper --threads 1 \
    --output-dir "$temp_dir/single"
grep -q '^selectivity_mode: paper$' \
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

run_success "legacy engine" \
    "$data" "$query" 2 --count-only \
    --engine legacy --output-dir "$temp_dir/legacy"

run_failure "duplicate count-only" \
    "$data" "$query" 2 --count-only --count-only
run_failure "unknown option" \
    "$data" "$query" 2 --unknown
run_failure "invalid memo" \
    "$data" "$query" 2 --memo invalid
run_failure "invalid selectivity" \
    "$data" "$query" 2 --selectivity invalid
run_failure "invalid label mode" \
    "$data" "$query" 2 --label-mode unstable
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

echo "Ours_gopher CLI tests passed."
