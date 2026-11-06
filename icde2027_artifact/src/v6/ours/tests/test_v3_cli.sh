#!/usr/bin/env bash
set -euo pipefail

if (($# != 1)); then
    printf 'Usage: %s <td_tree executable>\n' "$0" >&2
    exit 2
fi

executable=$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
generator="$script_dir/generate_v3_authority_fixture.py"
contract='evaluation/icde2027_v3_dev_16t_20260812/runs/0001/run_contract.json'
authorization='paper/icde2027/v3_protocol/execution_authorization_receipt.json'
receipt='evaluation/icde2027_v3_dev_16t_20260812/runs/0001/matcher_receipt.json'
fixture_parent=$(mktemp -d)

cleanup() {
    rm -rf -- "$fixture_parent"
}
trap cleanup EXIT

make_fixture() {
    local mode=$1
    local root
    root=$(mktemp -d "$fixture_parent/${mode}.XXXXXX")
    PYTHONPATH="$repo_root" python3 "$generator" "$root" "$mode"
    printf '%s\n' "$root"
}

run_code() {
    local root=$1
    shift
    local code=0
    (cd -- "$root" && "$executable" "$@" >/dev/null 2>&1) || code=$?
    printf '%s\n' "$code"
}

valid_root=$(make_fixture valid)
if [[ $(run_code "$valid_root" \
    --check-v3-run-contract "$contract" \
    --v3-authorization "$authorization") -ne 0 ]]; then
    printf 'valid read-only V3 authority check failed\n' >&2
    exit 1
fi

# The production executor is connected, but this authority generation carries
# static concrete caps rather than formula-derived caps materialized from the
# fresh process's post-filter n_f/m_f/q.  It must therefore fail with authority
# exit 11 before environment checks, input I/O, or receipt publication.
run_status=$(run_code "$valid_root" \
    --v3-run-contract "$contract" \
    --v3-authorization "$authorization" \
    --v3-receipt "$receipt")
if [[ $run_status -ne 11 ]]; then
    printf 'V3 run mode did not reject static numeric-cap authority: %s\n' \
        "$run_status" >&2
    exit 1
fi
if [[ -e "$valid_root/$receipt" ]]; then
    printf 'fail-closed V3 run unexpectedly created a matcher receipt\n' >&2
    exit 1
fi

expect_cli_10() {
    if [[ $(run_code "$valid_root" "$@") -ne 10 ]]; then
        printf 'malformed/mixed V3 CLI did not return exit 10: %s\n' "$*" >&2
        exit 1
    fi
}
expect_cli_10 --check-v3-run-contract "$contract"
expect_cli_10 --check-v3-run-contract "$contract" \
    --v3-authorization "$authorization" --v3-authorization "$authorization"
expect_cli_10 --v3-run-contract="$contract" \
    --v3-authorization "$authorization" --v3-receipt "$receipt"
expect_cli_10 missing-data missing-query 2 \
    --v3-authorization "$authorization"
expect_cli_10 --v3-run-contract "$contract" \
    --v3-authorization "$authorization" --v3-receipt "$receipt" --threads 16
expect_cli_10 --check-v3-run-contract \
    benchmark/queries/icde2027_v2_holdout/forbidden.json \
    --v3-authorization "$authorization"
expect_cli_10 --check-v3-run-contract arbitrary/readable.json \
    --v3-authorization "$authorization"

for mode in \
    protocol_schedule_drift authorization_crossbind authorization_failure \
    contract_tamper_resealed contract_wrong_ordinal contract_wrong_case \
    contract_wrong_arm contract_formula_drift contract_filtered null_cap bool_cap; do
    root=$(make_fixture "$mode")
    if [[ $(run_code "$root" \
        --check-v3-run-contract "$contract" \
        --v3-authorization "$authorization") -ne 11 ]]; then
        printf 'semantic authority mutation did not return exit 11: %s\n' "$mode" >&2
        exit 1
    fi
done

root=$(make_fixture protocol_tamper)
if [[ $(run_code "$root" \
    --check-v3-run-contract "$contract" \
    --v3-authorization "$authorization") -ne 11 ]]; then
    printf 'integrity tamper did not return exit 11\n' >&2
    exit 1
fi

for mode in \
    protocol_duplicate protocol_float protocol_negative protocol_overflow \
    protocol_noncanonical protocol_bad_utf8; do
    root=$(make_fixture "$mode")
    if [[ $(run_code "$root" \
        --check-v3-run-contract "$contract" \
        --v3-authorization "$authorization") -ne 10 ]]; then
        printf 'canonical/schema mutation did not return exit 10: %s\n' "$mode" >&2
        exit 1
    fi
done

touch "$valid_root/$receipt"
if [[ $(run_code "$valid_root" \
    --v3-run-contract "$contract" \
    --v3-authorization "$authorization" \
    --v3-receipt "$receipt") -ne 17 ]]; then
    printf 'pre-existing matcher receipt target did not return exit 17\n' >&2
    exit 1
fi

printf 'All closed V3 CLI process tests passed.\n'
