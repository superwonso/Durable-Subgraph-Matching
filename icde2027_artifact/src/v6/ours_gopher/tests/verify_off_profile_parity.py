#!/usr/bin/env python3
"""Untimed semantic parity/reconciliation probe for isolated OFF kernels."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path
from typing import Any


VARIANTS = (
    "dag-dep-key-memo",
    "dag-full-prefix-memo",
    "dag-no-memo",
    "durable-dfs",
)
OPERATIONS = (
    "td_block_lookup",
    "out_neighbor_lookup",
    "in_neighbor_lookup",
    "structural_intersection",
    "edge_time_lookup",
    "temporal_intersection",
    "longest_consecutive_run",
)


class ParityFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ParityFailure(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def execute(
    binary: Path,
    graph: Path,
    query: Path,
    minimum_duration: int,
    variant: str,
    statistics: str,
    output_dir: Path,
) -> dict[str, Any]:
    metrics = output_dir / "metrics.json"
    command = [
        str(binary),
        str(graph),
        str(query),
        str(minimum_duration),
        "20260819",
        "--label-mode", "stable-id",
        "--engine", "gopher",
        "--selectivity", "paper",
        "--order", "paper",
        "--parallel", "roots",
        "--count-only",
        "--experiment-variant", variant,
        "--threads", "1",
        "--cache-mb", "256",
        "--cache-entry-mb", "8",
        "--cache-min-work", "0",
        "--no-cost-aware",
        "--early-prune", "on",
        "--runtime-key-diagnostics", "off",
        "--statistics", statistics,
        "--result-digest", "on",
        "--output-dir", str(output_dir),
        "--metrics", str(metrics),
        "--correctness-preflight",
    ]
    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=120,
    )
    require(
        completed.returncode == 0,
        f"semantic probe failed ({completed.returncode}): {' '.join(command)}\n"
        f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
    )
    require(metrics.is_file(), f"semantic probe missed metrics: {metrics}")
    return json.loads(metrics.read_text(encoding="utf-8"))


def validate_common(metrics: dict[str, Any], variant: str) -> None:
    require(metrics["engine"] == "gopher", "engine drift")
    require(metrics["mode"] == "count-only", "mode drift")
    require(metrics["order"] == "paper", "order drift")
    require(metrics["selectivity_mode"] == "paper", "selectivity drift")
    require(metrics["threads_requested"] == 1, "thread-count drift")
    require(metrics["threads_used"] == 1, "effective thread-count drift")
    require(metrics["parallel_mode_requested"] == "roots",
            "scheduler drift")
    require(metrics["cost_aware_admission"] is False,
            "cost-aware admission was enabled")
    require(metrics["runtime_key_diagnostics"] == "off",
            "exact diagnostics were enabled")
    require(metrics["result_digest_enabled"] is True,
            "result digest was disabled")
    require(metrics["correctness_preflight"] is True,
            "semantic probe was not marked preflight")
    require(metrics["count_overflow"] is False, "match count overflow")
    contract = metrics["experiment_contract"]
    require(contract["scheduler"] == "roots", "contract scheduler drift")
    require(contract["order"] == "paper", "contract order drift")
    expected_admission = (
        "eager-on-first-eligible-touch"
        if variant in ("dag-dep-key-memo", "dag-full-prefix-memo")
        else "not-applicable"
    )
    require(contract["memo_admission"] == expected_admission,
            f"contract admission drift for {variant}")
    require(contract["correctness_preflight"] is True,
            "contract preflight drift")
    require(metrics["off_overhead_comparison_eligible"] is False,
            "semantic probe became timing-eligible")
    require(variant in VARIANTS, "unknown closed variant")


def validate_statistics_on(metrics: dict[str, Any], variant: str) -> None:
    require(metrics["statistics_enabled"] is True,
            "product ON statistics were disabled")
    require(metrics["search_statistics_hooks_erased_at_build"] is False,
            "product ON reported erased hooks")
    require(metrics["search_statistics_build_profile"] == "product",
            "product ON profile drift")
    stats = metrics["statistics"]
    for operation in OPERATIONS:
        invocations = stats[f"dag_operator_invocations_{operation}"]
        observations = stats[
            f"dag_operator_result_observations_{operation}"
        ]
        for field in (
            invocations,
            observations,
            stats[f"dag_operator_cache_hits_{operation}"],
            stats[f"dag_operator_cache_misses_{operation}"],
            stats[f"dag_operator_cache_admissions_{operation}"],
        ):
            require(isinstance(field, int) and field >= 0,
                    f"invalid seven-op statistic for {operation}")
        require(invocations == observations,
                f"result/invocation mismatch for {operation}")
    if variant != "durable-dfs":
        require(stats["dag_operator_reconciliation_passed"] is True,
                f"DAG reconciliation failed for {variant}")


def validate_statistics_off(
    metrics: dict[str, Any],
    expected_profile: str,
    expected_erased: bool,
) -> None:
    require(metrics["statistics_enabled"] is False,
            "statistics-OFF arm reported enabled counters")
    require(metrics["search_statistics_build_profile"] == expected_profile,
            "statistics-OFF build profile drift")
    require(metrics["search_statistics_hooks_erased_at_build"]
            is expected_erased, "statistics-OFF erased flag drift")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--product", required=True, type=Path)
    parser.add_argument("--erased", required=True, type=Path)
    parser.add_argument("--fixtures", required=True, type=Path)
    args = parser.parse_args()

    product = args.product.resolve(strict=True)
    erased = args.erased.resolve(strict=True)
    fixtures = args.fixtures.resolve(strict=True)
    require(product.is_file(), f"missing product binary: {product}")
    require(erased.is_file(), f"missing erased binary: {erased}")
    receipt_path = fixtures / "fixture_receipt.json"
    require(receipt_path.is_file(), f"missing fixture receipt: {receipt_path}")
    fixture_receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    fixture_rows = fixture_receipt.get("fixtures")
    require(isinstance(fixture_rows, list) and len(fixture_rows) == 3,
            "expected the three frozen M5 fixtures")

    cells: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="gopher_off_parity_") as temp:
        temp_root = Path(temp)
        for fixture in fixture_rows:
            fixture_id = fixture["fixture_id"]
            graph = fixtures / fixture["graph_path"]
            query = fixtures / fixture["query_path"]
            require(sha256(graph) == fixture["graph_sha256"],
                    f"graph hash drift: {fixture_id}")
            require(sha256(query) == fixture["query_sha256"],
                    f"query hash drift: {fixture_id}")
            for variant in VARIANTS:
                cell = f"{fixture_id}::{variant}"
                arms = {
                    "product_off": execute(
                        product, graph, query, fixture["minimum_duration"],
                        variant, "off", temp_root / f"{cell}-product-off",
                    ),
                    "product_on": execute(
                        product, graph, query, fixture["minimum_duration"],
                        variant, "on", temp_root / f"{cell}-product-on",
                    ),
                    "erased_off": execute(
                        erased, graph, query, fixture["minimum_duration"],
                        variant, "off", temp_root / f"{cell}-erased-off",
                    ),
                }
                for metrics in arms.values():
                    validate_common(metrics, variant)
                validate_statistics_off(arms["product_off"], "product", False)
                validate_statistics_on(arms["product_on"], variant)
                validate_statistics_off(
                    arms["erased_off"], "erased-control", True
                )
                outcomes = {
                    (metrics["match_count"], metrics["result_digest"])
                    for metrics in arms.values()
                }
                require(len(outcomes) == 1,
                        f"count/digest parity failed: {cell}")
                plans = {
                    json.dumps(metrics["plan"], sort_keys=True)
                    for metrics in arms.values()
                }
                require(len(plans) == 1, f"plan parity failed: {cell}")
                source_ids = {
                    metrics["gopher_source_identity"]
                    for metrics in arms.values()
                }
                common_ids = {
                    metrics["gopher_common_flags_identity"]
                    for metrics in arms.values()
                }
                require(len(source_ids) == 1, f"source identity drift: {cell}")
                require(len(common_ids) == 1, f"build identity drift: {cell}")
                count, digest = next(iter(outcomes))
                cells.append({
                    "cell": cell,
                    "match_count": count,
                    "result_digest": digest,
                    "source_identity": next(iter(source_ids)),
                    "common_flags_identity": next(iter(common_ids)),
                })

    receipt = {
        "schema": "gopher_off_profile_parity_v1",
        "status": "PASS",
        "performance_evidence_eligible": False,
        "processes_executed": len(cells) * 3,
        "cell_count": len(cells),
        "product_sha256": sha256(product),
        "erased_sha256": sha256(erased),
        "fixture_receipt_sha256": sha256(receipt_path),
        "cells_sha256": hashlib.sha256(json.dumps(
            cells, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")).hexdigest(),
    }
    print(json.dumps(receipt, sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ParityFailure, OSError, ValueError, KeyError,
            json.JSONDecodeError, subprocess.TimeoutExpired) as error:
        print(json.dumps({
            "schema": "gopher_off_profile_parity_v1",
            "status": "FAIL",
            "error": str(error),
        }, sort_keys=True))
        raise SystemExit(1)
