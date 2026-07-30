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
printf 'A B\n' >"$temp_dir/query.txt"
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
expect_success "equals-form default label mode" \
    "${required[@]}" --label-mode=sequential-rng
expect_success "intended selectivity" \
    "${required[@]}" --count-only --selectivity intended
grep -q '^selectivity_mode: intended$' matching_results_data.txt
grep -q '^selectivity_mode: intended$' timing_results_data.txt
expect_success "equals-form paper selectivity" \
    "${required[@]}" --selectivity=paper
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
popd >/dev/null

printf 'Ours CLI tests passed.\n'
