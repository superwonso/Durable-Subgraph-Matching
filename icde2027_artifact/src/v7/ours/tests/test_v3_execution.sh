#!/usr/bin/env bash
set -euo pipefail

if (($# != 1)); then
    printf 'Usage: %s TEST_EXECUTABLE\n' "$0" >&2
    exit 2
fi

executable=$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
fixture_abs=$(mktemp -d "$script_dir/v3exec.XXXXXX")
fixture_rel=${fixture_abs#"$repo_root/"}

cleanup() {
    rm -rf -- "$fixture_abs"
}
trap cleanup EXIT

(cd -- "$repo_root" && "$executable" "$fixture_rel")

PYTHONPATH="$repo_root" python3 - "$fixture_abs" <<'PY'
import json
import sys
from pathlib import Path

from benchmark.icde2027_v3.canonical_artifact import canonical_json_bytes, sha256_bytes
from benchmark.icde2027_v3.matcher_receipt_model import validate_matcher_receipt

root = Path(sys.argv[1])
run_dirs = sorted(root.glob("run_*"))
if len(run_dirs) != 3:
    raise SystemExit(f"expected three I/O/F fixture runs, got {len(run_dirs)}")
for run_dir in run_dirs:
    contract = json.loads((run_dir / "run_contract.json").read_text())
    receipt_path = run_dir / "matcher_receipt.json"
    raw = receipt_path.read_bytes()
    receipt = json.loads(raw)
    if raw != canonical_json_bytes(receipt):
        raise SystemExit(f"noncanonical matcher receipt: {receipt_path}")
    correctness_errors = validate_matcher_receipt(
        receipt,
        contract,
        development_mode=True,
        include_timing=False,
    )
    if correctness_errors:
        raise SystemExit(
            f"Python correctness receipt validation failed for {receipt_path}: "
            f"{correctness_errors}"
        )
    timing_errors = validate_matcher_receipt(
        receipt,
        contract,
        development_mode=True,
        include_timing=True,
    )
    expected_timing_error = (
        "timing evidence lacks formula-derived post-filter numeric-cap binding"
    )
    if timing_errors != [expected_timing_error]:
        raise SystemExit(
            f"synthetic static-cap receipt was not rejected only at timing "
            f"authority for {receipt_path}: {timing_errors}"
        )
    binding = {
        "active_vertices": receipt["input"]["active_vertices"],
        "backend": receipt["execution"]["backend"],
        "data_logical_fingerprint": receipt["snapshot"]["data_logical_fingerprint"],
        "executor_semantics_id": "tdtree_exact_durable_injective_count_v1",
        "formula_id": receipt["build"]["formula_id"],
        "mapping_digest_id": "external_id_query_vertex_order_splitmix64_xor_sum_v1",
        "match_output": receipt["input"]["match_output"],
        "minimum_duration": receipt["input"]["minimum_duration"],
        "query_logical_fingerprint": receipt["snapshot"]["query_logical_fingerprint"],
        "retained_unique_directed_edges": receipt["input"]["retained_unique_directed_edges"],
        "selected_order": receipt["plan"]["selected_order"],
        "selected_parent": receipt["plan"]["selected_parent"],
        "selected_plan_fingerprint": receipt["plan"]["selected_plan_fingerprint"],
        "selected_root": receipt["plan"]["selected_root"],
        "selected_tree_fingerprint": receipt["plan"]["selected_tree_fingerprint"],
        "schema": "icde2027_v3_tdtree_execution_binding",
        "schema_version": 1,
    }
    if sha256_bytes(canonical_json_bytes(binding)) != receipt["execution"][
        "execution_binding_sha256"
    ]:
        raise SystemExit(f"execution binding recomputation failed: {receipt_path}")
print(
    "All C++ -> Python V3 correctness round trips passed; "
    "static-cap timing evidence rejected."
)
PY
