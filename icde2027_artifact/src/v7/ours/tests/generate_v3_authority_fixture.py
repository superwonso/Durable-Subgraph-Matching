#!/usr/bin/env python3
"""Generate synthetic-only Patch-6 C++ authority fixtures.

The helper imports the Python authority model but never opens a declared
data/query path and never starts a matcher.  Every output is under the caller's
temporary directory.
"""

from __future__ import annotations

import argparse
import copy
from pathlib import Path

from benchmark.icde2027_v3.canonical_artifact import (
    canonical_json_bytes,
    seal_artifact,
    sha256_bytes,
)
from benchmark.icde2027_v3.run_contract_model import (
    AUTHORIZATION_ARTIFACT_PATH,
    EXECUTION_OUTPUT_ROOT,
    PROTOCOL_ARTIFACT_PATH,
    build_run_contract,
)
from benchmark.icde2027_v3.test_run_contract_model import (
    authorization,
    executable_protocol,
)


def build_valid() -> tuple[dict, dict, dict]:
    protocol = executable_protocol()
    auth = authorization(protocol)
    protocol_bytes = canonical_json_bytes(protocol)
    auth_bytes = canonical_json_bytes(auth)
    contract = build_run_contract(
        protocol,
        auth,
        protocol_file_sha256=sha256_bytes(protocol_bytes),
        authorization_file_sha256=sha256_bytes(auth_bytes),
        protocol_file_bytes=protocol_bytes,
        authorization_file_bytes=auth_bytes,
        ordinal=1,
        output_root=EXECUTION_OUTPUT_ROOT,
    )
    return protocol, auth, contract


def mutate(
    mode: str, protocol: dict, auth: dict, contract: dict
) -> tuple[dict, dict, dict, bytes | None]:
    raw_protocol: bytes | None = None
    if mode == "valid":
        return protocol, auth, contract, raw_protocol
    if mode == "protocol_schedule_drift":
        protocol = copy.deepcopy(protocol)
        protocol["schedule"]["positions"][0]["arm_id"] = (
            "v3dev_cover_lazy_gate_on_16t"
        )
        protocol = seal_artifact(protocol)
        auth = authorization(protocol)
    elif mode == "authorization_crossbind":
        protocol = copy.deepcopy(protocol)
        protocol["readiness"][
            "patch_5_independent_review_receipt_sha256"
        ] = "e" * 64
        protocol = seal_artifact(protocol)
        auth = authorization(protocol)
    elif mode == "authorization_failure":
        auth = copy.deepcopy(auth)
        auth["independent_reviews"]["patch_5_independent_review"]["status"] = "fail"
        auth = seal_artifact(auth)
    elif mode == "contract_tamper_resealed":
        contract = copy.deepcopy(contract)
        contract["identity"]["arm_position"] = 2
        contract = seal_artifact(contract)
    elif mode == "contract_wrong_ordinal":
        contract = copy.deepcopy(contract)
        contract["identity"]["ordinal"] = 2
        contract = seal_artifact(contract)
    elif mode == "contract_wrong_case":
        contract = copy.deepcopy(contract)
        contract["identity"]["case_id"] = "D02_btc_cycle"
        contract = seal_artifact(contract)
    elif mode == "contract_wrong_arm":
        contract = copy.deepcopy(contract)
        contract["identity"]["arm_id"] = "v3dev_cover_lazy_gate_on_16t"
        contract = seal_artifact(contract)
    elif mode == "contract_formula_drift":
        contract = copy.deepcopy(contract)
        contract["algorithm"]["formula_id"] = "wrong_formula_generation"
        contract = seal_artifact(contract)
    elif mode == "contract_filtered":
        contract = copy.deepcopy(contract)
        contract["input"]["data_path"] = "Dataset/raw_filtered.txt"
        contract = seal_artifact(contract)
    elif mode == "null_cap":
        protocol = copy.deepcopy(protocol)
        protocol["numeric_caps"]["lazy"]["adjacency_probes"] = None
        protocol = seal_artifact(protocol)
        auth = authorization(protocol)
    elif mode == "bool_cap":
        protocol = copy.deepcopy(protocol)
        protocol["numeric_caps"]["lazy"]["adjacency_probes"] = True
        protocol = seal_artifact(protocol)
        auth = authorization(protocol)
    elif mode == "protocol_tamper":
        raw_protocol = canonical_json_bytes(protocol).replace(
            b"cpu16t-v2-fixture", b"cpu16t-v2-tampered", 1
        )
    elif mode == "protocol_duplicate":
        raw_protocol = canonical_json_bytes(protocol).replace(
            b'{"arms":', b'{"arms":[],"arms":', 1
        )
    elif mode == "protocol_float":
        raw_protocol = canonical_json_bytes(protocol).replace(
            b'"schema_version":2', b'"schema_version":2.0', 1
        )
    elif mode == "protocol_negative":
        raw_protocol = canonical_json_bytes(protocol).replace(
            b'"schema_version":2', b'"schema_version":-1', 1
        )
    elif mode == "protocol_overflow":
        raw_protocol = canonical_json_bytes(protocol).replace(
            b'"schema_version":2',
            b'"schema_version":18446744073709551616',
            1,
        )
    elif mode == "protocol_noncanonical":
        raw_protocol = b" " + canonical_json_bytes(protocol)
    elif mode == "protocol_bad_utf8":
        raw_protocol = canonical_json_bytes(protocol).replace(
            b"cpu16t-v2-fixture", b"cpu16t-v2-\xff", 1
        )
    else:
        raise ValueError(f"unknown fixture mode: {mode}")
    return protocol, auth, contract, raw_protocol


def write_fixture(root: Path, mode: str) -> None:
    protocol, auth, contract = build_valid()
    protocol, auth, contract, raw_protocol = mutate(
        mode, protocol, auth, contract
    )
    protocol_path = root / PROTOCOL_ARTIFACT_PATH
    authorization_path = root / AUTHORIZATION_ARTIFACT_PATH
    contract_path = root / EXECUTION_OUTPUT_ROOT / "0001" / "run_contract.json"
    for path in (protocol_path, authorization_path, contract_path):
        path.parent.mkdir(parents=True, exist_ok=True)
    protocol_path.write_bytes(
        raw_protocol if raw_protocol is not None else canonical_json_bytes(protocol)
    )
    authorization_path.write_bytes(canonical_json_bytes(auth))
    contract_path.write_bytes(canonical_json_bytes(contract))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("mode")
    args = parser.parse_args()
    write_fixture(args.root, args.mode)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
