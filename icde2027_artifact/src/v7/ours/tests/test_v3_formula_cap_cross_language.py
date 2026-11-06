#!/usr/bin/env python3
"""Python/C++ byte-level differential checks for the pure V3 cap oracle.

This helper performs no real-data, matcher, GPU, or performance work.  Build
``v3_formula_cap_oracle_cli.cpp`` against the pure oracle, then pass that binary
with ``--binary``.  Every valid case is compared as raw canonical JSON bytes;
hostile cases must fail before emitting an accepted materialization.
"""

from __future__ import annotations

import argparse
import copy
import itertools
import json
import random
import subprocess
from pathlib import Path
from typing import Any

from benchmark.icde2027_v3.canonical_artifact import (
    UINT64_MAX,
    canonical_json_bytes,
    sha256_bytes,
)
from benchmark.icde2027_v3.cap_policy_model import (
    ALPHA_B_VALUES,
    ALPHA_L_VALUES,
    COMPLETE_PROPOSAL_VALUES,
    LAZY_CACHE_ENTRY_VALUES,
    LAZY_EXTENSION_VALUES,
    LAZY_INTERVAL_VALUES,
    PAYLOAD_CEILING_VALUES,
    PILOT_EXTENSION_VALUES,
    PREFIX_VALUES,
    PROPOSAL_STATE_VALUES,
    CapPolicyError,
    materialize_caps,
)
from benchmark.icde2027_v3.test_cap_policy_model import policy as base_policy


H = "a" * 64


def request(**overrides: Any) -> dict[str, Any]:
    value: dict[str, Any] = {
        "filtered_vertices": 100,
        "filtered_edges": 1001,
        "interval_records": 1500,
        "query_vertices": 6,
        "query_edges": 7,
        "graph_storage_quote": {
            "accounting_model_sha256": H,
            "snapshot_storage_bytes": 1_000_000,
            "temporary_validation_bytes": 200_000,
            "seal_peak_live_bytes": 2_000_000,
        },
    }
    value.update(overrides)
    return value


def python_materialize(policy: dict[str, Any], req: dict[str, Any]) -> dict[str, Any]:
    return materialize_caps(policy, **req)


def add_case(
    cases: list[tuple[dict[str, Any], dict[str, Any]]],
    policy: dict[str, Any],
    req: dict[str, Any] | None = None,
) -> None:
    cases.append((policy, request() if req is None else req))


def valid_cases() -> list[tuple[dict[str, Any], dict[str, Any]]]:
    cases: list[tuple[dict[str, Any], dict[str, Any]]] = []
    add_case(cases, base_policy())

    # Every preregistered value is exercised on its own axis.
    for numerator, denominator in sorted(ALPHA_B_VALUES):
        value = base_policy()
        value["candidate_build"]["adjacency_multiplier"] = {
            "numerator": numerator,
            "denominator": denominator,
        }
        add_case(cases, value)
    for ceiling in sorted(PAYLOAD_CEILING_VALUES):
        value = base_policy()
        value["candidate_build"]["payload_ceiling_bytes"] = ceiling
        add_case(cases, value)
    for complete in sorted(COMPLETE_PROPOSAL_VALUES):
        value = base_policy()
        value["planner"]["complete_proposal_cap"] = complete
        add_case(cases, value)
    for states in sorted(PROPOSAL_STATE_VALUES):
        value = base_policy()
        value["planner"]["state_expansions"] = states
        add_case(cases, value)
    for prefix in sorted(PREFIX_VALUES):
        value = base_policy()
        value["planner"]["prefix_samples_per_plan"] = prefix
        add_case(cases, value)
    for numerator, denominator in sorted(ALPHA_L_VALUES):
        value = base_policy()
        value["lazy"]["adjacency_fraction"] = {
            "numerator": numerator,
            "denominator": denominator,
        }
        add_case(cases, value)
    for entries in sorted(LAZY_CACHE_ENTRY_VALUES):
        value = base_policy()
        value["lazy"]["cache_entries"] = entries
        add_case(cases, value)
    for probes in sorted(LAZY_EXTENSION_VALUES):
        value = base_policy()
        value["lazy"]["extension_probes"] = probes
        add_case(cases, value)
    for intersections in sorted(LAZY_INTERVAL_VALUES):
        value = base_policy()
        value["lazy"]["interval_intersections"] = intersections
        add_case(cases, value)
    for prefix in sorted(PREFIX_VALUES):
        value = base_policy()
        value["rescore"]["prefix_samples_per_plan"] = prefix
        add_case(cases, value)
    for probes in sorted(PILOT_EXTENSION_VALUES):
        value = base_policy()
        value["rescore"]["extension_probes"] = probes
        add_case(cases, value)

    # Deterministic interaction coverage over the Cartesian authority without
    # materializing the impractical full 419,904-vector product.
    dimensions = [
        sorted(ALPHA_B_VALUES),
        sorted(PAYLOAD_CEILING_VALUES),
        sorted(COMPLETE_PROPOSAL_VALUES),
        sorted(PROPOSAL_STATE_VALUES),
        sorted(PREFIX_VALUES),
        sorted(ALPHA_L_VALUES),
        sorted(LAZY_CACHE_ENTRY_VALUES),
        sorted(LAZY_EXTENSION_VALUES),
        sorted(LAZY_INTERVAL_VALUES),
        sorted(PREFIX_VALUES),
        sorted(PILOT_EXTENSION_VALUES),
    ]
    product = itertools.product(*dimensions)
    for index, combination in enumerate(product):
        if index % 4093 != 0:
            continue
        (
            alpha_b,
            payload,
            complete,
            states,
            planner_prefix,
            alpha_l,
            cache_entries,
            lazy_extensions,
            lazy_intervals,
            rescore_prefix,
            rescore_extensions,
        ) = combination
        value = base_policy()
        value["candidate_build"]["adjacency_multiplier"] = {
            "numerator": alpha_b[0],
            "denominator": alpha_b[1],
        }
        value["candidate_build"]["payload_ceiling_bytes"] = payload
        value["planner"].update(
            {
                "complete_proposal_cap": complete,
                "state_expansions": states,
                "prefix_samples_per_plan": planner_prefix,
            }
        )
        value["lazy"].update(
            {
                "adjacency_fraction": {
                    "numerator": alpha_l[0],
                    "denominator": alpha_l[1],
                },
                "cache_entries": cache_entries,
                "extension_probes": lazy_extensions,
                "interval_intersections": lazy_intervals,
            }
        )
        value["rescore"].update(
            {
                "prefix_samples_per_plan": rescore_prefix,
                "extension_probes": rescore_extensions,
            }
        )
        add_case(cases, value)
        if len(cases) >= 150:
            break

    # Exact empty-filter, ceil threshold, lazy clamp, and structural boundaries.
    boundaries = [
        request(
            filtered_vertices=0,
            filtered_edges=0,
            interval_records=0,
            graph_storage_quote={
                "accounting_model_sha256": H,
                "snapshot_storage_bytes": 1,
                "temporary_validation_bytes": 0,
                "seal_peak_live_bytes": 1,
            },
        ),
        request(filtered_vertices=1, filtered_edges=0, interval_records=0, query_vertices=2, query_edges=1),
        request(filtered_vertices=1, filtered_edges=1, interval_records=1, query_vertices=2, query_edges=1),
        request(filtered_edges=4),
        request(filtered_edges=5),
        request(filtered_edges=4096 * 64),
        request(filtered_edges=(1 << 24) * 64),
        request(filtered_edges=1 << 40),
        request(query_vertices=64, query_edges=128),
    ]
    for boundary in boundaries:
        add_case(cases, base_policy(), boundary)

    rng = random.Random(0xC0FFEE)
    alpha_b_values = sorted(ALPHA_B_VALUES)
    alpha_l_values = sorted(ALPHA_L_VALUES)
    for _ in range(128):
        value = base_policy()
        alpha_b = rng.choice(alpha_b_values)
        alpha_l = rng.choice(alpha_l_values)
        value["candidate_build"]["adjacency_multiplier"] = {
            "numerator": alpha_b[0],
            "denominator": alpha_b[1],
        }
        value["candidate_build"]["payload_ceiling_bytes"] = rng.choice(
            sorted(PAYLOAD_CEILING_VALUES)
        )
        value["planner"].update(
            {
                "complete_proposal_cap": rng.choice(sorted(COMPLETE_PROPOSAL_VALUES)),
                "state_expansions": rng.choice(sorted(PROPOSAL_STATE_VALUES)),
                "prefix_samples_per_plan": rng.choice(sorted(PREFIX_VALUES)),
            }
        )
        value["lazy"].update(
            {
                "adjacency_fraction": {
                    "numerator": alpha_l[0],
                    "denominator": alpha_l[1],
                },
                "cache_entries": rng.choice(sorted(LAZY_CACHE_ENTRY_VALUES)),
                "extension_probes": rng.choice(sorted(LAZY_EXTENSION_VALUES)),
                "interval_intersections": rng.choice(sorted(LAZY_INTERVAL_VALUES)),
            }
        )
        value["rescore"].update(
            {
                "prefix_samples_per_plan": rng.choice(sorted(PREFIX_VALUES)),
                "extension_probes": rng.choice(sorted(PILOT_EXTENSION_VALUES)),
            }
        )
        add_case(
            cases,
            value,
            request(
                filtered_vertices=rng.randrange(0, 100_001),
                filtered_edges=rng.randrange(0, 1_000_001),
                interval_records=rng.randrange(0, 2_000_001),
                query_vertices=rng.randrange(2, 17),
                query_edges=rng.randrange(1, 65),
                graph_storage_quote={
                    "accounting_model_sha256": H,
                    "snapshot_storage_bytes": rng.randrange(1, 10_000_001),
                    "temporary_validation_bytes": rng.randrange(0, 5_000_001),
                    "seal_peak_live_bytes": rng.randrange(1, 20_000_001),
                },
            ),
        )
    return cases


def run_valid_differential(binary: Path) -> int:
    cases = valid_cases()
    input_bytes = b"".join(
        canonical_json_bytes({"policy": policy, "request": req})
        for policy, req in cases
    )
    expected_bytes = b"".join(
        canonical_json_bytes(python_materialize(policy, req))
        for policy, req in cases
    )
    completed = subprocess.run(
        [str(binary)],
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"C++ valid differential failed ({completed.returncode}): "
            f"{completed.stderr.decode(errors='replace')}"
        )
    if completed.stdout != expected_bytes:
        raise AssertionError(
            "Python/C++ canonical materialized bytes differ: "
            f"python_sha={sha256_bytes(expected_bytes)} "
            f"cpp_sha={sha256_bytes(completed.stdout)}"
        )
    golden = python_materialize(base_policy(), request())
    if sha256_bytes(canonical_json_bytes(base_policy())) != (
        "8ae14b8a48723a0e99c077034f5af8844b1013d2e456698974644b1cad5a7fde"
    ):
        raise AssertionError("shared policy golden SHA drift")
    if sha256_bytes(canonical_json_bytes(golden)) != (
        "20d5d6f9536b1123e7dd40472f0c888d1f2ef3f0b5606dbda93b20f827485156"
    ):
        raise AssertionError("shared materialized golden SHA drift")
    return len(cases)


def assert_cpp_rejects(binary: Path, policy: dict[str, Any], req: dict[str, Any]) -> None:
    completed = subprocess.run(
        [str(binary)],
        input=canonical_json_bytes({"policy": policy, "request": req}),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode == 0:
        raise AssertionError("C++ accepted hostile formula-cap input")


def run_hostile_rejections(binary: Path) -> int:
    cases: list[tuple[dict[str, Any], dict[str, Any]]] = []

    value = base_policy()
    value["planner"]["complete_proposal_cap"] = True
    cases.append((value, request()))
    value = base_policy()
    value["unexpected"] = 1
    cases.append((value, request()))
    value = base_policy()
    del value["planner"]["state_expansions"]
    cases.append((value, request()))
    value = base_policy()
    value["candidate_build"]["adjacency_multiplier"] = {
        "numerator": 3,
        "denominator": 4,
    }
    cases.append((value, request()))
    value = base_policy()
    value["candidate_build"]["adjacency_multiplier"] = {
        "numerator": 1,
        "denominator": 0,
    }
    cases.append((value, request()))
    value = base_policy()
    value["candidate_build"]["adjacency_multiplier"] = {
        "numerator": 0,
        "denominator": 1,
    }
    cases.append((value, request()))
    value = base_policy()
    value["fixed"]["sample_seed"] = 41
    cases.append((value, request()))
    value = base_policy()
    value["accounting"]["candidate_value_bytes"] = 8
    cases.append((value, request()))
    value = base_policy()
    value["cost_coefficients"]["weights"]["adjacency_probe"] = 999_999
    cases.append((value, request()))
    value = base_policy()
    value["graph_validation"]["accounting_model_sha256"] = "A" * 64
    cases.append((value, request()))

    hostile_request = request()
    hostile_request["interval_records"] = True
    cases.append((base_policy(), hostile_request))
    hostile_request = request()
    hostile_request["unexpected"] = 1
    cases.append((base_policy(), hostile_request))
    hostile_request = request(query_vertices=1)
    cases.append((base_policy(), hostile_request))
    hostile_request = request(query_edges=0)
    cases.append((base_policy(), hostile_request))
    hostile_request = request()
    hostile_request["graph_storage_quote"]["temporary_validation_bytes"] = False
    cases.append((base_policy(), hostile_request))
    hostile_request = request()
    hostile_request["graph_storage_quote"]["unexpected"] = 1
    cases.append((base_policy(), hostile_request))
    hostile_request = request()
    hostile_request["graph_storage_quote"]["snapshot_storage_bytes"] = 0
    cases.append((base_policy(), hostile_request))
    hostile_request = request()
    hostile_request["graph_storage_quote"]["accounting_model_sha256"] = "b" * 64
    cases.append((base_policy(), hostile_request))

    value = base_policy()
    value["candidate_build"]["adjacency_multiplier"] = {
        "numerator": 2,
        "denominator": 1,
    }
    cases.append((value, request(filtered_edges=UINT64_MAX // 2 + 1)))
    cases.append((base_policy(), request(filtered_edges=UINT64_MAX)))
    cases.append((base_policy(), request(interval_records=UINT64_MAX)))
    cases.append(
        (
            base_policy(),
            request(
                filtered_vertices=UINT64_MAX,
                filtered_edges=0,
                interval_records=0,
            ),
        )
    )
    cases.append(
        (
            base_policy(),
            request(filtered_vertices=2, query_vertices=UINT64_MAX),
        )
    )
    value = base_policy()
    value["accounting"]["lazy_cache_entry_bytes"] = UINT64_MAX
    cases.append((value, request()))
    cases.append((base_policy(), request(query_edges=UINT64_MAX)))

    for policy, req in cases:
        try:
            python_materialize(policy, req)
        except (CapPolicyError, TypeError):
            pass
        else:
            raise AssertionError("Python authority unexpectedly accepted hostile case")
        assert_cpp_rejects(binary, policy, req)

    # Parser-level uint64, negative, duplicate-key, and helper-root drift.
    raw_cases = [
        b'{"policy":{},"request":{"filtered_edges":-1}}\n',
        b'{"policy":{},"request":{"filtered_edges":18446744073709551616}}\n',
        b'{"policy":{},"policy":{},"request":{}}\n',
        canonical_json_bytes({"policy": base_policy(), "request": request(), "extra": 1}),
    ]
    for raw in raw_cases:
        completed = subprocess.run(
            [str(binary)],
            input=raw,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if completed.returncode == 0:
            raise AssertionError("C++ parser/helper accepted hostile raw JSON")
    return len(cases) + len(raw_cases)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        raise SystemExit(f"oracle binary does not exist: {binary}")
    valid_count = run_valid_differential(binary)
    hostile_count = run_hostile_rejections(binary)
    print(
        "v3 formula-cap Python/C++ differential passed: "
        f"valid={valid_count}, hostile={hostile_count}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
