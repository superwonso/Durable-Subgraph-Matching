#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ours_dir=$(cd -- "$script_dir/.." && pwd)
compiler=${CXX:-g++}
fixture=$(mktemp -d)
cleanup() {
    rm -rf -- "$fixture"
}
trap cleanup EXIT

python3 - "$fixture/protocol.json" <<'PY'
import json
import sys
from pathlib import Path

value = {
    "execution_contract": {"threads_requested": 16},
    "fixed_policy": {"candidate_tree_tokens": 1},
    "numeric_caps": {"planner": {"complete_proposal_cap": 7}},
    "resource_envelope": {"per_process_timeout_seconds": 60},
}
Path(sys.argv[1]).write_text(
    json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n",
    encoding="utf-8",
)
PY

python3 "$ours_dir/generate_v3_build_identity.py" \
    --ours "$ours_dir" \
    --compiler "$compiler" \
    --output "$fixture/v3_build_identity_generated.h" \
    --source-manifest-output "$fixture/source_bundle.json" \
    --source-copy-output "$fixture/pinned_ours" \
    --compiler-manifest-output "$fixture/compiler_identity.json" \
    --parameter-protocol "$fixture/protocol.json" \
    --parameter-bundle-output "$fixture/parameter_bundle.json"

python3 - "$ours_dir" "$fixture" <<'PY'
import hashlib
import json
from pathlib import Path
import re
import sys

ours = Path(sys.argv[1])
fixture = Path(sys.argv[2])
header = (fixture / "v3_build_identity_generated.h").read_text()
macros = dict(re.findall(r'#define DSM_V3_([A-Z0-9_]+) "([0-9a-f]{64})"', header))
artifacts = {
    "SOURCE_BUNDLE_SHA256": fixture / "source_bundle.json",
    "COMPILER_IDENTITY_SHA256": fixture / "compiler_identity.json",
    "PARAMETER_BUNDLE_SHA256": fixture / "parameter_bundle.json",
}
for macro, path in artifacts.items():
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if macros.get(macro) != digest:
        raise SystemExit(f"embedded {macro} does not equal artifact file SHA-256")
    parsed = json.loads(path.read_bytes())
    canonical = (json.dumps(parsed, sort_keys=True, separators=(",", ":")) + "\n").encode()
    if canonical != path.read_bytes():
        raise SystemExit(f"noncanonical build identity artifact: {path}")

source = json.loads((fixture / "source_bundle.json").read_bytes())
observed = {entry["path"] for entry in source["files"]}
expected = {
    f"ours/{path.name}"
    for pattern in ("*.cpp", "*.h")
    for path in ours.glob(pattern)
}
expected.update({
    "ours/build.sh",
    "ours/generate_v3_build_identity.py",
    "ours/validate_v3_graph_accounting_binding.py",
})
if observed != expected:
    raise SystemExit(
        f"source bundle scope drift: missing={sorted(expected-observed)}, "
        f"extra={sorted(observed-expected)}"
    )
for entry in source["files"]:
    copied = fixture / "pinned_ours" / Path(entry["path"]).name
    if hashlib.sha256(copied.read_bytes()).hexdigest() != entry["sha256"]:
        raise SystemExit(f"pinned source differs from manifest: {copied}")
print("V3 build identity artifact/macro self-consistency passed.")
PY

# Exercise the production build entry point as well as the generator.  The
# build must persist each distinct canonical identity artifact next to the
# executable, and the exact artifact digests must be embedded in that binary.
"$ours_dir/build.sh" "$fixture/td_tree" "$fixture/protocol.json" >/dev/null

python3 - "$fixture" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

fixture = Path(sys.argv[1])
binary = (fixture / "td_tree").read_bytes()
artifacts = (
    fixture / "td_tree.source_bundle.json",
    fixture / "td_tree.compiler_identity.json",
    fixture / "td_tree.parameter_bundle.json",
)
for path in artifacts:
    raw = path.read_bytes()
    parsed = json.loads(raw)
    canonical = (json.dumps(parsed, sort_keys=True, separators=(",", ":")) + "\n").encode()
    if raw != canonical:
        raise SystemExit(f"production build emitted noncanonical identity: {path}")
    digest = hashlib.sha256(raw).hexdigest().encode("ascii")
    if digest not in binary:
        raise SystemExit(f"production binary does not embed artifact SHA-256: {path}")
graph_model_path = fixture / "td_tree.graph_accounting_model.json"
graph_raw = graph_model_path.read_bytes()
graph_model = json.loads(graph_raw)
graph_canonical = (
    json.dumps(graph_model, sort_keys=True, separators=(",", ":")) + "\n"
).encode()
if graph_raw != graph_canonical:
    raise SystemExit("production graph accounting model is noncanonical")
if graph_model.get("schema") != "icde2027_v3_graph_accounting_model":
    raise SystemExit("production graph accounting model schema drift")
source_sha = hashlib.sha256(
    (fixture / "td_tree.source_bundle.json").read_bytes()
).hexdigest()
compiler_sha = hashlib.sha256(
    (fixture / "td_tree.compiler_identity.json").read_bytes()
).hexdigest()
if graph_model.get("source_bundle_sha256") != source_sha:
    raise SystemExit("graph accounting model does not bind source bundle")
if graph_model.get("compiler_identity_sha256") != compiler_sha:
    raise SystemExit("graph accounting model does not bind compiler identity")
print("V3 production build artifacts are canonical and embedded in the binary.")
PY

# The graph binding consumes a full Formula Protocol V3, not a permissive
# fragment.  Its graph-related fields are independently shape/type checked and
# both graph hashes must equal the build-bound native artifact.
python3 - "$fixture" <<'PY'
import copy
import hashlib
import json
from pathlib import Path
import sys

fixture = Path(sys.argv[1])

def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()

def seal(value):
    result = copy.deepcopy(value)
    result.pop("integrity", None)
    digest = hashlib.sha256(canonical(result)).hexdigest()
    result["integrity"] = {
        "algorithm": "sha256",
        "canonical_payload_sha256": digest,
        "scope": "canonical_json_all_fields_except_integrity",
    }
    return result

model_sha = hashlib.sha256(
    (fixture / "td_tree.graph_accounting_model.json").read_bytes()
).hexdigest()
policy = {
    "accounting": {},
    "candidate_build": {},
    "cost_coefficients": {},
    "fixed": {},
    "formula_id": "tcc_v3_post_filter_checked_u64_caps_v1",
    "graph_validation": {
        "accounting_model_sha256": model_sha,
        "formula_id": "validated_snapshot_exact_structural_quote_v1",
    },
    "lazy": {},
    "planner": {},
    "policy_id": "synthetic-graph-binding-fixture",
    "rescore": {},
    "schema": "icde2027_v3_formula_cap_policy",
    "schema_version": 1,
}
contract = {
    "binding_mode": "post_filter_formula_derived_checked_u64_v1",
    "candidate_payload_accounting_model_sha256": "1" * 64,
    "checked_integer_semantics": "uint64_reject_bool_checked_add_mul_ceil_ratio_v1",
    "derivation_stage": "after_internal_filter_before_snapshot_allocation_v1",
    "existing_capacity_accounting_model_sha256": "2" * 64,
    "formula_id": "tcc_v3_post_filter_checked_u64_caps_v1",
    "graph_accounting_model_sha256": model_sha,
    "graph_quote_mode": "native_exact_preallocation_quote_reverified_at_seal_v1",
    "memory_formula_id": "existing_plus_candidate_plus_headroom_le_process_v1",
}
protocol = {
    "arms": [],
    "authorization": {"execution_authorized": True},
    "authorization_artifact_path": "paper/icde2027/v3_protocol/execution_authorization_receipt_v2.json",
    "build_provenance": {},
    "cap_materialization_contract": contract,
    "cap_policy": policy,
    "cases": [],
    "cost_objective": {},
    "datasets": [],
    "execution_contract": {},
    "execution_output_root": "evaluation/icde2027_v3_dev_16t_formula_caps_20260813/runs",
    "fixed_policy": {},
    "gate_c_thresholds": {},
    "path_policy": {},
    "protocol_artifact_path": "paper/icde2027/v3_protocol/development_protocol_v3.json",
    "protocol_id": "synthetic-graph-binding-fixture",
    "readiness": {},
    "resource_envelope": {},
    "schedule": {},
    "schema": "icde2027_v3_executable_protocol",
    "schema_version": 3,
    "status": "frozen_execution_authorized",
}
(fixture / "formula_authority.json").write_bytes(canonical(seal(protocol)))
PY
python3 "$ours_dir/validate_v3_graph_accounting_binding.py" \
    --authority "$fixture/formula_authority.json" \
    --graph-accounting-model "$fixture/td_tree.graph_accounting_model.json" \
    --expected-native-model-sha256 "$(sha256sum "$fixture/td_tree.graph_accounting_model.json" | cut -d' ' -f1)"

# Boolean versions, unknown fields, partial authorities, and a coherently
# rebound but wrong model must all fail closed rather than exploit ordinary
# Python equality or permissive dictionary access.
python3 - "$fixture" "$ours_dir/validate_v3_graph_accounting_binding.py" <<'PY'
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys

fixture = Path(sys.argv[1])
validator = Path(sys.argv[2])
base_model_path = fixture / "td_tree.graph_accounting_model.json"
base_model = json.loads(base_model_path.read_bytes())
base_protocol = json.loads((fixture / "formula_authority.json").read_bytes())

def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()

def seal(value):
    result = copy.deepcopy(value)
    result.pop("integrity", None)
    result["integrity"] = {
        "algorithm": "sha256",
        "canonical_payload_sha256": hashlib.sha256(canonical(result)).hexdigest(),
        "scope": "canonical_json_all_fields_except_integrity",
    }
    return result

def bind(protocol, digest):
    result = copy.deepcopy(protocol)
    result["cap_policy"]["graph_validation"]["accounting_model_sha256"] = digest
    result["cap_materialization_contract"]["graph_accounting_model_sha256"] = digest
    return seal(result)

base_model_sha = hashlib.sha256(base_model_path.read_bytes()).hexdigest()

def expect_reject(name, protocol, model=base_model, expected_sha=base_model_sha):
    authority_path = fixture / f"hostile-{name}-authority.json"
    model_path = fixture / f"hostile-{name}-model.json"
    authority_path.write_bytes(canonical(protocol))
    model_path.write_bytes(canonical(model))
    completed = subprocess.run(
        [
            sys.executable,
            str(validator),
            "--authority",
            str(authority_path),
            "--graph-accounting-model",
            str(model_path),
            "--expected-native-model-sha256",
            expected_sha,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if completed.returncode == 0:
        raise SystemExit(f"hostile graph binding unexpectedly accepted: {name}")

wrong = "a" * 64
if wrong == hashlib.sha256(canonical(base_model)).hexdigest():
    wrong = "b" * 64
expect_reject("wrong-model-binding", bind(base_protocol, wrong))

for name, mutate in (
    ("bool-model-version", lambda value: value.__setitem__("schema_version", True)),
    ("unknown-model-field", lambda value: value.__setitem__("unexpected", 1)),
    (
        "bool-native-width",
        lambda value: value["native_size_bytes"].__setitem__("edge", True),
    ),
    (
        "positive-native-width-drift",
        lambda value: value["native_size_bytes"].__setitem__(
            "edge", value["native_size_bytes"]["edge"] + 1
        ),
    ),
    (
        "unknown-native-width",
        lambda value: value["native_size_bytes"].__setitem__("unexpected", 1),
    ),
):
    model = copy.deepcopy(base_model)
    mutate(model)
    digest = hashlib.sha256(canonical(model)).hexdigest()
    # Protocol hashes are coherently rebound, but the independent native CLI
    # SHA remains fixed to the originally emitted model.
    expect_reject(name, bind(base_protocol, digest), model, base_model_sha)

for name, mutate in (
    ("bool-protocol-version", lambda value: value.__setitem__("schema_version", True)),
    ("unknown-protocol-field", lambda value: value.__setitem__("unexpected", 1)),
    (
        "bool-policy-version",
        lambda value: value["cap_policy"].__setitem__("schema_version", True),
    ),
    (
        "unknown-policy-graph-field",
        lambda value: value["cap_policy"]["graph_validation"].__setitem__("unexpected", 1),
    ),
    (
        "unknown-materialization-field",
        lambda value: value["cap_materialization_contract"].__setitem__("unexpected", 1),
    ),
):
    protocol = copy.deepcopy(base_protocol)
    protocol.pop("integrity", None)
    mutate(protocol)
    expect_reject(name, seal(protocol))

expect_reject(
    "partial-formula-authority",
    {"cap_policy": copy.deepcopy(base_protocol["cap_policy"])},
)
missing = copy.deepcopy(base_protocol)
missing.pop("integrity", None)
missing.pop("cap_policy")
missing.pop("cap_materialization_contract")
expect_reject("formula-protocol-missing-graph-authority", seal(missing))

print("V3 formula graph-accounting build binding and hostile checks passed.")
PY
