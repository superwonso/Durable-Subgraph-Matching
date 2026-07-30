#!/usr/bin/env python3
"""Build the strict selectivity/candidate/ablation evaluation workbook.

The workbook is intentionally manifest-driven.  ``runs.csv`` remains the
canonical measurement source; the spreadsheet only aggregates official
case/algorithm pairs selected by ``manifest.used.json``.  Candidate ratios
are emitted only when their logical metric schema, backend, and measurement
cohort are identical.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import statistics
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable, Sequence

try:
    from openpyxl import Workbook, load_workbook
    from openpyxl.chart import BarChart, Reference
    from openpyxl.chart.label import DataLabelList
    from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
    from openpyxl.utils import get_column_letter
    from openpyxl.worksheet.table import Table, TableStyleInfo
except ImportError as error:  # pragma: no cover - dependency gate
    raise SystemExit(
        "openpyxl is required. Install benchmark/requirements.txt first."
    ) from error


EXPECTED_SHEETS = [
    "Overview",
    "Configuration Matrix",
    "Selectivity Performance",
    "Candidate & Pruning",
    "Controlled Ablation",
    "GPU Evaluation",
    "Correctness",
    "Raw Runs",
    "Methodology & Limits",
    "Provenance",
]
DEFAULT_OUTPUT = Path(
    "evaluation/dsm_selectivity_candidate_ablation_20260729.xlsx"
)
MAIN_COMPARISON_WORKBOOK = (
    "evaluation/dsm_dataset_32t_protocol_corrected_20260729.xlsx"
)

TITLE_FILL = PatternFill("solid", fgColor="17365D")
HEADER_FILL = PatternFill("solid", fgColor="D9EAF7")
SUBHEADER_FILL = PatternFill("solid", fgColor="E2F0D9")
PASS_FILL = PatternFill("solid", fgColor="E2F0D9")
WARNING_FILL = PatternFill("solid", fgColor="FFF2CC")
FAIL_FILL = PatternFill("solid", fgColor="F4CCCC")
GREY_FILL = PatternFill("solid", fgColor="E7E6E6")
THIN_GREY = Side(style="thin", color="B7B7B7")
COLORS = ["4472C4", "ED7D31", "70AD47", "A5A5A5"]

NULL_TEXT = {"", "none", "null", "nan", "n/a", "na"}
STATISTICS_GATED_METRICS = {
    "raw_extension_candidates",
    "accepted_extensions",
    "generated_candidates",
    "accepted_candidates",
    "dfs_states",
    "completed_matches",
    "pruned_injectivity",
    "pruned_temporal_support",
    "pruned_active_support",
    "pruned_structural",
    "pruned_temporal",
}
CANDIDATE_METRICS = {
    "root_candidate_count",
    "raw_extension_candidates",
    "accepted_extensions",
    "generated_candidates",
    "accepted_candidates",
    "dfs_states",
    "completed_matches",
    "candidate_relation_entries",
    "pruned_injectivity",
    "pruned_temporal_support",
    "pruned_active_support",
    "pruned_structural",
    "pruned_temporal",
}
CROSS_ENGINE_LOGICAL_CANDIDATE_METRICS = {
    "root_candidate_count",
    "raw_extension_candidates",
    "completed_matches",
}

ALIASES: dict[str, tuple[str, ...]] = {
    "wall_s": ("wall_s", "wall_observed_s", "resource_wall_s"),
    "match_count": ("match_count", "result_count", "gopher_match_count"),
    "root_candidate_count": (
        "root_candidate_count",
        "result_root_candidate_count",
        "gopher_root_candidate_count",
    ),
    "raw_extension_candidates": (
        "raw_extension_candidates",
        "result_raw_extension_candidates",
        "gopher_statistics_raw_extension_candidates",
    ),
    "accepted_extensions": (
        "accepted_extensions",
        "result_accepted_extensions",
        "gopher_statistics_accepted_extensions",
    ),
    "generated_candidates": (
        "generated_candidates",
        "result_generated_candidates",
        "gopher_statistics_generated_candidates",
    ),
    "accepted_candidates": (
        "accepted_candidates",
        "result_accepted_candidates",
        "gopher_statistics_accepted_candidates",
    ),
    "dfs_states": (
        "dfs_states",
        "result_dfs_states",
        "gopher_statistics_dfs_states",
    ),
    "completed_matches": (
        "completed_matches",
        "result_completed_matches",
        "gopher_statistics_completed_matches",
    ),
    "candidate_relation_entries": (
        "candidate_relation_entries",
        "result_candidate_relation_entries",
        "candidate_relation_entries_stdout",
        "gopher_candidate_relation_entries",
    ),
    "pruned_injectivity": (
        "pruned_injectivity",
        "result_pruned_injectivity",
        "gopher_statistics_pruned_injectivity",
    ),
    "pruned_temporal_support": (
        "pruned_temporal_support",
        "result_pruned_temporal_support",
        "gopher_statistics_pruned_temporal_support",
    ),
    "pruned_active_support": (
        "pruned_active_support",
        "result_pruned_active_support",
        "gopher_statistics_pruned_active_support",
    ),
    "pruned_structural": (
        "pruned_structural",
        "result_pruned_structural",
        "gopher_statistics_pruned_structural",
    ),
    "pruned_temporal": (
        "pruned_temporal",
        "result_pruned_temporal",
        "gopher_statistics_pruned_temporal",
    ),
    "retained_edges": (
        "retained_edges",
        "result_retained_edges",
        "gopher_input_retained_edges",
    ),
    "root_selectivity_score": (
        "root_selectivity_score",
        "result_root_selectivity_score",
        "gopher_root_selectivity_score",
        "gopher_plan_root_selectivity_score",
    ),
    "root_query_vertex": (
        "query_root",
        "result_query_root",
        "gopher_query_root",
        "gopher_plan_root",
    ),
    "gpu_kernel_ms": ("gpuKernel_ms", "gopher_gpu_kernel_ms"),
    "gpu_ready_ms": (
        "gpuReadyToResult_ms",
        "gopher_gpu_ready_to_result_ms",
    ),
    "gpu_search_ms": ("gpuSearch_ms", "gopher_gpu_search_ms"),
    "gpu_peak_frontier_states": (
        "gpu_peak_frontier_states",
        "gopher_gpu_peak_frontier_states",
    ),
    "gpu_peak_frontier_device_bytes": (
        "gpu_peak_frontier_device_bytes",
        "gopher_gpu_peak_frontier_device_bytes",
    ),
    "resource_max_rss_kb": ("resource_max_rss_kb",),
    "enumerateMatches_ms": (
        "enumerateMatches_ms",
        "result_enumeration_ms",
        "gopher_enumerateMatches_ms",
    ),
    "buildTDTree_ms": (
        "buildTDTree_ms",
        "result_build_td_tree_ms",
        "gopher_buildTDTree_ms",
    ),
}


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create or validate the strict selectivity-ablation workbook."
    )
    parser.add_argument("--input-dir", type=Path)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--validate-only", type=Path)
    return parser.parse_args(argv)


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def text_or_none(value: Any) -> str | None:
    if value is None:
        return None
    text = str(value).strip()
    return None if text.lower() in NULL_TEXT else text


def number(value: Any) -> float | None:
    text = text_or_none(value)
    if text is None:
        return None
    try:
        result = float(text)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def integer(value: Any) -> int | None:
    result = number(value)
    if result is None or not result.is_integer():
        return None
    return int(result)


def boolean(value: Any) -> bool | None:
    if isinstance(value, bool):
        return value
    text = text_or_none(value)
    if text is None:
        return None
    lowered = text.lower()
    if lowered in {"true", "yes", "on", "1"}:
        return True
    if lowered in {"false", "no", "off", "0"}:
        return False
    return None


def metric(row: dict[str, Any], name: str) -> float | None:
    for key in ALIASES.get(name, (name,)):
        result = number(row.get(key))
        if result is not None:
            return result
    return None


def first_text(row: dict[str, Any], keys: Iterable[str]) -> str | None:
    for key in keys:
        result = text_or_none(row.get(key))
        if result is not None:
            return result
    return None


def percentile(values: Sequence[float], fraction: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    index = (len(ordered) - 1) * fraction
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return ordered[lower]
    weight = index - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def median(values: Iterable[float | None]) -> float | None:
    present = [float(value) for value in values if value is not None]
    return statistics.median(present) if present else None


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def flatten_json(value: Any, prefix: str = "") -> list[tuple[str, Any]]:
    rows: list[tuple[str, Any]] = []
    if isinstance(value, dict):
        for key, child in value.items():
            child_prefix = f"{prefix}.{key}" if prefix else str(key)
            rows.extend(flatten_json(child, child_prefix))
    elif isinstance(value, list):
        rows.append((prefix, json.dumps(value, ensure_ascii=False)))
    else:
        rows.append((prefix, value))
    return rows


def flags_map(flags: Sequence[Any]) -> dict[str, str]:
    parsed: dict[str, str] = {}
    index = 0
    while index < len(flags):
        token = str(flags[index])
        if not token.startswith("--"):
            index += 1
            continue
        key = token[2:].replace("-", "_")
        if index + 1 < len(flags) and not str(flags[index + 1]).startswith("--"):
            parsed[key] = str(flags[index + 1])
            index += 2
        else:
            parsed[key] = "true"
            index += 1
    return parsed


def algorithm_flags(
    manifest: dict[str, Any], case: dict[str, Any], algorithm_id: str
) -> dict[str, str]:
    algorithm = manifest.get("algorithms", {}).get(algorithm_id, {})
    flags: list[Any] = list(algorithm.get("flags", []))
    flags.extend(case.get("extra_flags", []))
    flags.extend(case.get("algorithm_flags", {}).get(algorithm_id, []))
    return flags_map(flags)


def algorithm_label(algorithm_id: str, algorithm: dict[str, Any]) -> str:
    explicit = text_or_none(algorithm.get("label"))
    if explicit:
        return explicit
    lowered = algorithm_id.lower()
    if lowered.startswith("original") or lowered.startswith("tdtree"):
        return "Original (TD-Tree)"
    if "gopher" in lowered and "gpu" in lowered:
        return "Ours+Gopher-GPU"
    if "gopher" in lowered:
        return "Ours+Gopher"
    if lowered.startswith("ours"):
        return "Ours"
    return algorithm_id


def engine_family(algorithm_id: str) -> str:
    lowered = algorithm_id.lower()
    if lowered.startswith("original") or lowered.startswith("tdtree"):
        return "original_tdtree"
    if "gopher" in lowered:
        return "ours_gopher"
    if lowered.startswith("ours"):
        return "ours"
    return lowered


def infer_backend(
    row: dict[str, Any],
    algorithm_id: str,
    algorithm: dict[str, Any],
    flags: dict[str, str],
) -> str:
    direct = first_text(
        row,
        ("backend", "result_backend", "gopher_backend", "device"),
    )
    if direct and direct.lower() in {"cpu", "gpu"}:
        return direct.upper()
    if flags.get("backend", "").lower() == "gpu":
        return "GPU"
    executable = str(algorithm.get("executable", "")).lower()
    return "GPU" if "gpu" in algorithm_id.lower() or "gpu" in executable else "CPU"


def infer_statistics(
    row: dict[str, Any], flags: dict[str, str]
) -> bool | None:
    for key in (
        "statistics_enabled",
        "gopher_statistics_enabled",
        "result_statistics_enabled",
        "statistics_config",
    ):
        result = boolean(row.get(key))
        if result is not None:
            return result
    return boolean(flags.get("statistics"))


def infer_cohort(
    row: dict[str, Any],
    case: dict[str, Any],
    algorithm: dict[str, Any],
    statistics_enabled: bool | None,
) -> str:
    explicit = first_text(
        row, ("measurement_cohort", "cohort", "benchmark_cohort")
    ) or text_or_none(algorithm.get("cohort")) or text_or_none(case.get("cohort"))
    if explicit:
        return explicit.lower()
    suite = str(case.get("suite", row.get("suite", ""))).lower()
    if "correct" in suite:
        return "correctness"
    if "diagnostic" in suite or statistics_enabled is True:
        return "diagnostic"
    return "performance"


def decorate_run(
    row: dict[str, str],
    manifest: dict[str, Any],
    cases: dict[str, dict[str, Any]],
    algorithms: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    result: dict[str, Any] = dict(row)
    case_id = str(row.get("case_id", ""))
    algorithm_id = str(row.get("algorithm", ""))
    case = cases.get(case_id, {})
    algorithm = algorithms.get(algorithm_id, {})
    flags = algorithm_flags(manifest, case, algorithm_id)
    statistics_enabled = infer_statistics(row, flags)
    backend = infer_backend(row, algorithm_id, algorithm, flags)

    result["_case"] = case
    result["_algorithm"] = algorithm
    result["_flags"] = flags
    result["_algorithm_label"] = algorithm_label(algorithm_id, algorithm)
    result["_backend"] = backend
    result["_statistics_enabled"] = statistics_enabled
    result["_cohort"] = infer_cohort(
        row, case, algorithm, statistics_enabled
    )
    result["_suite"] = str(case.get("suite", row.get("suite", "")))
    result["_dataset_id"] = str(
        case.get("dataset_id", row.get("dataset_id", ""))
    )
    result["_query_shape"] = str(
        case.get("query_shape", row.get("query_shape", ""))
    )
    result["_k"] = (
        integer(row.get("k"))
        or integer(row.get("minimum_duration_k"))
        or integer(case.get("k"))
    )
    factors = algorithm.get("factors", {})
    result["_selectivity"] = (
        first_text(
            row,
            (
                "selectivity_config",
                "selectivity_mode",
                "gopher_selectivity_mode",
                "gopher_selectivity",
                "result_selectivity_mode",
                "selectivity",
            ),
        )
        or flags.get("selectivity")
        or text_or_none(factors.get("selectivity"))
        or ("n/a" if algorithm_id.lower().startswith(("original", "tdtree")) else "paper")
    )
    result["_prefilter"] = (
        first_text(row, ("prefilter_config", "prefilter", "filter_config"))
        or flags.get("prefilter")
        or text_or_none(factors.get("prefilter"))
        or ("n/a" if algorithm_id.lower().startswith(("original", "tdtree")) else "on")
    )
    result["_early_prune"] = (
        first_text(row, ("early_prune", "result_early_prune", "gopher_early_prune"))
        or flags.get("early_prune")
        or text_or_none(factors.get("early_prune"))
        or "n/a"
    )
    result["_order"] = (
        first_text(row, ("order_config", "order", "result_order", "gopher_order"))
        or flags.get("order")
        or text_or_none(factors.get("order"))
        or "n/a"
    )
    result["_memo"] = (
        first_text(
            row,
            ("memo_config", "memo", "result_memo", "gopher_memo"),
        )
        or flags.get("memo")
        or text_or_none(factors.get("memo"))
        or "n/a"
    )
    result["_parallel"] = (
        first_text(
            row,
            (
                "parallel_config",
                "parallel_mode_effective",
                "result_parallel_mode_effective",
            ),
        )
        or flags.get("parallel")
        or text_or_none(factors.get("parallel"))
        or "n/a"
    )
    result["_gpu_variant"] = (
        first_text(row, ("gpu_variant_config", "gpu_variant", "gopher_gpu_variant"))
        or flags.get("gpu_variant")
        or text_or_none(factors.get("gpu_variant"))
        or "n/a"
    )
    result["_gpu_memory_mb"] = (
        number(row.get("gpu_memory_mb_config"))
        or number(row.get("gpu_memory_mb"))
        or number(flags.get("gpu_memory_mb"))
        or number(factors.get("gpu_memory_mb"))
    )
    result["_metric_schema_version"] = (
        first_text(
            row,
            (
                "metric_schema_version",
                "result_metric_schema_version",
                "gopher_metric_schema_version",
                "gopher_schema_version",
            ),
        )
        or ""
    )
    return result


def official_pairs(
    manifest: dict[str, Any], runs: Sequence[dict[str, str]]
) -> tuple[
    dict[str, dict[str, Any]],
    dict[str, dict[str, Any]],
    list[tuple[str, str, int]],
]:
    cases = {
        str(case.get("id", "")): case
        for case in manifest.get("cases", [])
        if case.get("id")
    }
    algorithms = {
        str(key): value
        for key, value in manifest.get("algorithms", {}).items()
    }
    pairs: list[tuple[str, str, int]] = []
    if cases:
        default_repeats = int(manifest.get("repeats", 1))
        for case_id, case in cases.items():
            selected = case.get("algorithms") or list(algorithms)
            repeats = int(case.get("repeats", default_repeats))
            for algorithm_id in selected:
                if algorithm_id not in algorithms:
                    raise ValueError(
                        f"{case_id}: unknown manifest algorithm {algorithm_id}"
                    )
                pairs.append((case_id, str(algorithm_id), repeats))
    else:
        seen = sorted(
            {
                (str(row.get("case_id", "")), str(row.get("algorithm", "")))
                for row in runs
            }
        )
        cases = {case_id: {"id": case_id} for case_id, _ in seen}
        algorithms = {
            algorithm_id: {}
            for _, algorithm_id in seen
        }
        for case_id, algorithm_id in seen:
            repeats = len(
                [
                    row
                    for row in runs
                    if row.get("case_id") == case_id
                    and row.get("algorithm") == algorithm_id
                ]
            )
            pairs.append((case_id, algorithm_id, repeats))
    return cases, algorithms, pairs


def _summary_pair(row: dict[str, str]) -> tuple[str, str]:
    return str(row.get("case_id", "")), str(row.get("algorithm", ""))


def validate_official_inputs(
    runs: Sequence[dict[str, str]],
    summary: Sequence[dict[str, str]],
    correctness: Sequence[dict[str, str]],
    manifest: dict[str, Any],
) -> tuple[
    list[dict[str, Any]],
    dict[str, dict[str, Any]],
    dict[str, dict[str, Any]],
]:
    cases, algorithms, pairs = official_pairs(manifest, runs)
    grouped: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    for row in runs:
        grouped[_summary_pair(row)].append(row)
    summary_pairs = {_summary_pair(row) for row in summary}

    selected: list[dict[str, Any]] = []
    for case_id, algorithm_id, expected_repeats in pairs:
        group = grouped.get((case_id, algorithm_id), [])
        if len(group) != expected_repeats:
            raise ValueError(
                f"{case_id}/{algorithm_id}: expected {expected_repeats} official "
                f"runs, found {len(group)}"
            )
        if any(str(row.get("status", "")).lower() != "ok" for row in group):
            raise ValueError(f"{case_id}/{algorithm_id}: non-ok official run")
        repeats = [str(row.get("repeat", "")) for row in group]
        if len(set(repeats)) != len(repeats):
            raise ValueError(f"{case_id}/{algorithm_id}: duplicate repeat IDs")
        if (case_id, algorithm_id) not in summary_pairs:
            raise ValueError(
                f"{case_id}/{algorithm_id}: missing summary.csv row"
            )
        decorated_group = [
            decorate_run(row, manifest, cases, algorithms) for row in group
        ]
        walls = [metric(row, "wall_s") for row in decorated_group]
        if any(value is None or value <= 0 for value in walls):
            raise ValueError(f"{case_id}/{algorithm_id}: invalid wall_s")
        match_counts = {
            integer(metric(row, "match_count"))
            for row in decorated_group
        }
        if None in match_counts or len(match_counts) != 1:
            raise ValueError(
                f"{case_id}/{algorithm_id}: missing or unstable match count"
            )
        expected_match_count = integer(
            cases.get(case_id, {}).get("expected_match_count")
        )
        if (
            expected_match_count is not None
            and next(iter(match_counts)) != expected_match_count
        ):
            raise ValueError(
                f"{case_id}/{algorithm_id}: match count does not match oracle"
            )
        for key in (
            "_backend",
            "_cohort",
            "_selectivity",
            "_prefilter",
            "_early_prune",
            "_order",
            "_memo",
            "_statistics_enabled",
            "_metric_schema_version",
        ):
            values = {row[key] for row in decorated_group}
            if len(values) != 1:
                raise ValueError(
                    f"{case_id}/{algorithm_id}: unstable {key[1:]}"
                )
        if decorated_group[0]["_backend"] == "GPU":
            if any(
                boolean(
                    first_text(
                        row,
                        (
                            "fallback_used",
                            "result_fallback_used",
                            "gopher_fallback_used",
                        ),
                    )
                )
                is True
                for row in decorated_group
            ):
                raise ValueError(
                    f"{case_id}/{algorithm_id}: GPU CPU-fallback observed"
                )
        selected.extend(decorated_group)

    selected_cases = {case_id for case_id, _, _ in pairs}
    correctness_by_case: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in correctness:
        correctness_by_case[str(row.get("case_id", ""))].append(row)
    for case_id in selected_cases:
        case_rows = correctness_by_case.get(case_id, [])
        if not case_rows:
            raise ValueError(f"{case_id}: missing correctness.csv row")
        if not any(str(row.get("status", "")).lower() == "pass" for row in case_rows):
            raise ValueError(f"{case_id}: correctness did not pass")
        if any(
            str(row.get("status", "")).lower() not in {"pass", ""}
            for row in case_rows
        ):
            raise ValueError(f"{case_id}: non-pass correctness row")

    for row in selected:
        if row["_statistics_enabled"] is not False:
            continue
        populated: list[str] = []
        for name in STATISTICS_GATED_METRICS:
            for key in ALIASES.get(name, (name,)):
                if text_or_none(row.get(key)) is not None:
                    populated.append(key)
        if populated:
            raise ValueError(
                f"{row.get('case_id')}/{row.get('algorithm')}/r"
                f"{row.get('repeat')}: statistics=off must serialize gated "
                f"metrics as null/blank, not zero/value ({sorted(set(populated))})"
            )
    return selected, cases, algorithms


def normalize_comparisons(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    raw = manifest.get("comparisons")
    if raw is None:
        raw = manifest.get("comparison_metadata", [])
    if isinstance(raw, dict):
        if isinstance(raw.get("definitions"), list):
            raw = raw["definitions"]
        else:
            raw = [
                {"id": key, **(value if isinstance(value, dict) else {"notes": value})}
                for key, value in raw.items()
            ]
    if not isinstance(raw, list):
        return []
    result: list[dict[str, Any]] = []
    for index, item in enumerate(raw, start=1):
        if not isinstance(item, dict):
            continue
        normalized = dict(item)
        normalized.setdefault("id", f"comparison_{index}")
        result.append(normalized)
    return result


def group_aggregate(rows: Sequence[dict[str, Any]]) -> dict[str, Any]:
    walls = [metric(row, "wall_s") for row in rows]
    present_walls = [float(value) for value in walls if value is not None]
    counts = {integer(metric(row, "match_count")) for row in rows}
    counts.discard(None)
    return {
        "runs": len(rows),
        "wall_median": statistics.median(present_walls) if present_walls else None,
        "wall_q1": percentile(present_walls, 0.25) if present_walls else None,
        "wall_q3": percentile(present_walls, 0.75) if present_walls else None,
        "match_count": next(iter(counts)) if len(counts) == 1 else None,
        **{
            name: median(metric(row, name) for row in rows)
            for name in CANDIDATE_METRICS
        },
        "retained_edges": median(metric(row, "retained_edges") for row in rows),
        "root_selectivity_score": median(
            metric(row, "root_selectivity_score") for row in rows
        ),
        "root_query_vertex": median(
            metric(row, "root_query_vertex") for row in rows
        ),
        "gpu_kernel_ms": median(metric(row, "gpu_kernel_ms") for row in rows),
        "gpu_ready_ms": median(metric(row, "gpu_ready_ms") for row in rows),
        "gpu_search_ms": median(metric(row, "gpu_search_ms") for row in rows),
        "gpu_peak_frontier_states": median(
            metric(row, "gpu_peak_frontier_states") for row in rows
        ),
        "gpu_peak_frontier_device_bytes": median(
            metric(row, "gpu_peak_frontier_device_bytes") for row in rows
        ),
        "resource_max_rss_kb": median(
            metric(row, "resource_max_rss_kb") for row in rows
        ),
        "enumerateMatches_ms": median(
            metric(row, "enumerateMatches_ms") for row in rows
        ),
        "buildTDTree_ms": median(
            metric(row, "buildTDTree_ms") for row in rows
        ),
    }


def grouped_runs(
    rows: Sequence[dict[str, Any]]
) -> dict[tuple[Any, ...], tuple[list[dict[str, Any]], dict[str, Any]]]:
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        key = (
            str(row.get("case_id", "")),
            str(row.get("algorithm", "")),
            row["_backend"],
            row["_cohort"],
            row["_selectivity"],
            row["_prefilter"],
            row["_early_prune"],
            row["_order"],
            row["_memo"],
            row["_parallel"],
            row["_gpu_variant"],
            row["_gpu_memory_mb"],
            row["_metric_schema_version"],
        )
        groups[key].append(row)
    return {
        key: (group, group_aggregate(group))
        for key, group in groups.items()
    }


def style_title(ws: Any, title: str, subtitle: str | None = None) -> int:
    ws.merge_cells(start_row=1, start_column=1, end_row=1, end_column=14)
    cell = ws.cell(1, 1, title)
    cell.fill = TITLE_FILL
    cell.font = Font(color="FFFFFF", bold=True, size=16)
    cell.alignment = Alignment(horizontal="left", vertical="center")
    ws.row_dimensions[1].height = 28
    if subtitle:
        ws.merge_cells(start_row=2, start_column=1, end_row=2, end_column=14)
        cell = ws.cell(2, 1, subtitle)
        cell.font = Font(color="666666", italic=True)
        cell.alignment = Alignment(wrap_text=True, vertical="top")
        ws.row_dimensions[2].height = 34
        return 3
    return 2


def style_header(ws: Any, row: int, start: int, end: int) -> None:
    for column in range(start, end + 1):
        cell = ws.cell(row, column)
        cell.fill = HEADER_FILL
        cell.font = Font(bold=True)
        cell.alignment = Alignment(
            horizontal="center", vertical="center", wrap_text=True
        )
        cell.border = Border(bottom=THIN_GREY)


def add_table(
    ws: Any, name: str, header_row: int, last_row: int, last_column: int
) -> None:
    if last_row <= header_row:
        return
    table = Table(
        displayName=name,
        ref=(
            f"A{header_row}:"
            f"{get_column_letter(last_column)}{last_row}"
        ),
    )
    table.tableStyleInfo = TableStyleInfo(
        name="TableStyleMedium2",
        showFirstColumn=False,
        showLastColumn=False,
        showRowStripes=True,
        showColumnStripes=False,
    )
    ws.add_table(table)


def autosize(ws: Any, maximum: int = 42) -> None:
    for column_cells in ws.columns:
        column_index = column_cells[0].column
        length = 0
        for cell in column_cells:
            if cell.value is not None:
                length = max(length, len(str(cell.value)))
        ws.column_dimensions[get_column_letter(column_index)].width = min(
            maximum, max(10, length + 2)
        )


def set_numeric_formats(ws: Any, header_row: int) -> None:
    headers = {
        str(ws.cell(header_row, column).value): column
        for column in range(1, ws.max_column + 1)
    }
    for name, column in headers.items():
        lowered = name.lower()
        if any(
            token in lowered
            for token in ("wall", "speedup", "ratio", "rate", "score", "(ms)")
        ):
            for row in range(header_row + 1, ws.max_row + 1):
                ws.cell(row, column).number_format = "0.0000"
        elif any(
            token in lowered
            for token in ("candidate", "match", "states", "bytes", "edges", "rss")
        ):
            for row in range(header_row + 1, ws.max_row + 1):
                ws.cell(row, column).number_format = "#,##0"


def unique_table_name(prefix: str, index: int = 0) -> str:
    clean = "".join(character for character in prefix if character.isalnum())
    return f"{clean[:245]}{index}" if index else clean[:250]


def comparison_case_ids(
    comparison: dict[str, Any], cases: dict[str, dict[str, Any]]
) -> list[str]:
    raw = (
        comparison.get("case_ids")
        or comparison.get("cases")
        or ([comparison["case_id"]] if comparison.get("case_id") else None)
    )
    if raw:
        return [str(item) for item in raw if str(item) in cases]
    suites = comparison.get("suites") or (
        [comparison["suite"]] if comparison.get("suite") else []
    )
    if suites:
        wanted = {str(item) for item in suites}
        return [
            case_id
            for case_id, case in cases.items()
            if str(case.get("suite", "")) in wanted
        ]
    return list(cases)


def comparison_pairs(
    comparison: dict[str, Any]
) -> list[tuple[str, str]]:
    explicit = comparison.get("pairs")
    if isinstance(explicit, list):
        pairs: list[tuple[str, str]] = []
        for item in explicit:
            if not isinstance(item, dict):
                continue
            baseline = item.get("baseline_algorithm") or item.get("baseline")
            variant = item.get("variant_algorithm") or item.get("variant")
            if baseline and variant:
                pairs.append((str(baseline), str(variant)))
        if pairs:
            return pairs
    baseline = comparison.get("baseline_algorithm") or comparison.get("baseline")
    variants = (
        comparison.get("variant_algorithms")
        or comparison.get("variants")
        or comparison.get("compare_algorithms")
        or []
    )
    if isinstance(variants, str):
        variants = [variants]
    if baseline:
        return [(str(baseline), str(variant)) for variant in variants]
    algorithms = comparison.get("algorithms", [])
    if isinstance(algorithms, list) and len(algorithms) >= 2:
        return [
            (str(algorithms[0]), str(variant))
            for variant in algorithms[1:]
        ]
    return []


def matching_group(
    groups: dict[tuple[Any, ...], tuple[list[dict[str, Any]], dict[str, Any]]],
    case_id: str,
    algorithm_id: str,
    cohort: str | None,
) -> tuple[list[dict[str, Any]], dict[str, Any]] | None:
    matches = [
        value
        for key, value in groups.items()
        if key[0] == case_id
        and key[1] == algorithm_id
        and (cohort is None or key[3] == cohort)
    ]
    if len(matches) == 1:
        return matches[0]
    if cohort is not None:
        # Manifest comparison ``cohort`` is often a human-readable controlled
        # condition (for example "CPU32, statistics off"), whereas run rows
        # carry a stable family ID.  Fall back only when the case/algorithm
        # resolves to exactly one official group; eligibility still checks the
        # two groups' actual cohort IDs below.
        fallback = [
            value
            for key, value in groups.items()
            if key[0] == case_id and key[1] == algorithm_id
        ]
        if len(fallback) == 1:
            return fallback[0]
    return None


def controlled_comparison_rows(
    comparisons: Sequence[dict[str, Any]],
    groups: dict[tuple[Any, ...], tuple[list[dict[str, Any]], dict[str, Any]]],
    cases: dict[str, dict[str, Any]],
) -> list[list[Any]]:
    result: list[list[Any]] = []
    for comparison in comparisons:
        comparison_id = str(comparison.get("id", "comparison"))
        requested_metrics = [
            item
            for item in (
                comparison.get("metric") or "wall_s",
                comparison.get("candidate_metric"),
            )
            if item
        ]
        requested_metrics = list(
            dict.fromkeys(str(item) for item in requested_metrics)
        )
        requested_cohort = text_or_none(comparison.get("cohort"))
        if requested_cohort:
            requested_cohort = requested_cohort.lower()
        notes = str(
            comparison.get("description")
            or comparison.get("notes")
            or ""
        )
        for requested_metric in requested_metrics:
            metric_name = {
                "median_wall_s": "wall_s",
                "runtime": "wall_s",
                "end_to_end": "wall_s",
            }.get(requested_metric, requested_metric)
            for case_id in comparison_case_ids(comparison, cases):
                for baseline_id, variant_id in comparison_pairs(comparison):
                    baseline = matching_group(
                        groups, case_id, baseline_id, requested_cohort
                    )
                    variant = matching_group(
                        groups, case_id, variant_id, requested_cohort
                    )
                    prefix = [
                        comparison_id,
                        comparison.get("category", ""),
                        comparison.get("sheet", ""),
                        case_id,
                        baseline_id,
                        variant_id,
                    ]
                    if baseline is None or variant is None:
                        result.append(
                            prefix
                            + [
                                requested_cohort or "",
                                "",
                                metric_name,
                                None,
                                None,
                                False,
                                None,
                                "missing or ambiguous official group",
                                notes,
                            ]
                        )
                        continue
                    baseline_rows, baseline_agg = baseline
                    variant_rows, variant_agg = variant
                    baseline_row = baseline_rows[0]
                    variant_row = variant_rows[0]
                    same_backend = (
                        baseline_row["_backend"] == variant_row["_backend"]
                    )
                    same_cohort = (
                        baseline_row["_cohort"] == variant_row["_cohort"]
                    )
                    same_statistics_state = (
                        baseline_row["_statistics_enabled"]
                        == variant_row["_statistics_enabled"]
                    )
                    candidate_metric = metric_name in CANDIDATE_METRICS
                    same_engine_family = (
                        engine_family(baseline_id)
                        == engine_family(variant_id)
                    )
                    same_schema = (
                        bool(baseline_row["_metric_schema_version"])
                        and baseline_row["_metric_schema_version"]
                        == variant_row["_metric_schema_version"]
                    )
                    reasons: list[str] = []
                    if not same_backend:
                        reasons.append("CPU/GPU ratio prohibited")
                    if not same_cohort or not same_statistics_state:
                        reasons.append("performance/diagnostic ratio prohibited")
                    if candidate_metric and not same_schema:
                        reasons.append(
                            "candidate metric schema missing/mismatch"
                        )
                    if (
                        candidate_metric
                        and not same_engine_family
                        and metric_name
                        not in CROSS_ENGINE_LOGICAL_CANDIDATE_METRICS
                    ):
                        reasons.append(
                            "candidate metric is engine-specific"
                        )
                    baseline_value = (
                        baseline_agg["wall_median"]
                        if metric_name == "wall_s"
                        else baseline_agg.get(metric_name)
                    )
                    variant_value = (
                        variant_agg["wall_median"]
                        if metric_name == "wall_s"
                        else variant_agg.get(metric_name)
                    )
                    if baseline_value is None or variant_value in {None, 0}:
                        reasons.append("metric missing or zero denominator")
                    eligible = not reasons
                    ratio = (
                        float(baseline_value) / float(variant_value)
                        if eligible
                        else None
                    )
                    result.append(
                        prefix
                        + [
                            baseline_row["_cohort"],
                            baseline_row["_backend"],
                            metric_name,
                            baseline_value,
                            variant_value,
                            eligible,
                            ratio,
                            "; ".join(reasons),
                            notes,
                        ]
                    )
    return result


def load_inputs(input_dir: Path) -> dict[str, Any]:
    required = [
        input_dir / "runs.csv",
        input_dir / "summary.csv",
        input_dir / "correctness.csv",
    ]
    for path in required:
        if not path.exists():
            raise ValueError(f"Missing required input: {path}")
    runs = read_csv(required[0])
    summary = read_csv(required[1])
    correctness = read_csv(required[2])
    manifest_path = input_dir / "manifest.used.json"
    manifest = (
        json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest_path.exists()
        else {}
    )
    environment_path = input_dir / "environment.json"
    environment = (
        json.loads(environment_path.read_text(encoding="utf-8"))
        if environment_path.exists()
        else {}
    )
    selected, cases, algorithms = validate_official_inputs(
        runs, summary, correctness, manifest
    )
    optional_rows = {}
    for name in ("candidate_rows.csv", "selectivity_rows.csv"):
        path = input_dir / name
        optional_rows[name] = read_csv(path) if path.exists() else []
    return {
        "runs": runs,
        "summary": summary,
        "correctness": correctness,
        "manifest": manifest,
        "environment": environment,
        "selected": selected,
        "cases": cases,
        "algorithms": algorithms,
        "comparisons": normalize_comparisons(manifest),
        **optional_rows,
    }


def derive_key_findings(
    data: dict[str, Any],
    groups: dict[tuple[Any, ...], tuple[list[dict[str, Any]], dict[str, Any]]],
) -> list[tuple[str, str, str]]:
    """Return compact, auditable findings from manifest-controlled contrasts."""

    comparisons = controlled_comparison_rows(
        data["comparisons"], groups, data["cases"]
    )

    def ratios(comparison_ids: set[str], metric_name: str) -> list[float]:
        return [
            float(row[12])
            for row in comparisons
            if row[0] in comparison_ids
            and row[8] == metric_name
            and row[11] is True
            and isinstance(row[12], (int, float))
        ]

    findings: list[tuple[str, str, str]] = []
    case_counts: dict[str, int] = {}
    for _, (rows, aggregate) in groups.items():
        if aggregate["match_count"] is not None:
            case_counts.setdefault(
                str(rows[0].get("case_id", "")),
                int(aggregate["match_count"]),
            )
    if case_counts:
        nonzero = sum(value > 0 for value in case_counts.values())
        findings.append(
            (
                "Matches / 매칭 결과",
                f"{nonzero}/{len(case_counts)} official cases are nonzero",
                "Count-only disabled mapping materialization, not counting.",
            )
        )

    prefilter_ids = {
        "prefilter_under_tdtree_selectivity",
        "prefilter_under_paper_selectivity",
        "prefilter_under_intended_selectivity",
    }
    prefilter_speedups = ratios(prefilter_ids, "wall_s")
    edge_reductions: list[float] = []
    by_case_mode: dict[tuple[str, str, str], float] = {}
    for _, (rows, aggregate) in groups.items():
        row = rows[0]
        if (
            row["_suite"] == "selectivity_prefilter_cpu32"
            and str(row.get("algorithm", "")).startswith("ours_")
            and aggregate["retained_edges"] not in {None, 0}
        ):
            by_case_mode[
                (
                    str(row.get("case_id", "")),
                    str(row["_selectivity"]),
                    str(row["_prefilter"]),
                )
            ] = float(aggregate["retained_edges"])
    for (case_id, selectivity, prefilter), off_edges in by_case_mode.items():
        if prefilter != "off":
            continue
        on_edges = by_case_mode.get((case_id, selectivity, "on"))
        if on_edges is not None:
            edge_reductions.append(1.0 - on_edges / off_edges)
    if prefilter_speedups:
        evidence = (
            f"{min(prefilter_speedups):.2f}–"
            f"{max(prefilter_speedups):.2f}× median wall-time speedup"
        )
        if edge_reductions:
            evidence += (
                f"; retained edges −{min(edge_reductions):.1%}"
                f" to −{max(edge_reductions):.1%}"
            )
        findings.append(
            (
                "Consecutive-pair prefilter / 전처리 필터",
                evidence,
                "Largest and most consistent contribution in the controlled Ours factorial.",
            )
        )

    primary_selectivity_id = (
        "full_raw_selectivity_candidates"
        if any(
            row[0] == "full_raw_selectivity_candidates"
            for row in comparisons
        )
        else "selectivity_with_prefilter"
    )
    paper_candidate_factors = [
        float(row[10]) / float(row[9])
        for row in comparisons
        if row[0] == primary_selectivity_id
        and row[8] == "raw_extension_candidates"
        and "paper" in str(row[5])
        and row[9] not in {None, 0}
        and row[10] is not None
        and row[11] is True
    ]
    paper_runtime_factors = [
        float(row[10]) / float(row[9])
        for row in comparisons
        if row[0] == primary_selectivity_id
        and row[8] == "wall_s"
        and "paper" in str(row[5])
        and row[9] not in {None, 0}
        and row[10] is not None
        and row[11] is True
    ]
    intended_candidate_factors = [
        float(row[10]) / float(row[9])
        for row in comparisons
        if row[0] == primary_selectivity_id
        and row[8] == "raw_extension_candidates"
        and "intended" in str(row[5])
        and row[9] not in {None, 0}
        and row[10] is not None
        and row[11] is True
    ]
    if paper_candidate_factors:
        evidence = (
            f"paper raw work is {min(paper_candidate_factors):.2f}–"
            f"{max(paper_candidate_factors):.2f}× TD-Tree selectivity"
        )
        if paper_runtime_factors:
            evidence += (
                f"; wall time {min(paper_runtime_factors):.2f}–"
                f"{max(paper_runtime_factors):.2f}×"
            )
        if intended_candidate_factors and all(
            abs(value - 1.0) < 1e-12
            for value in intended_candidate_factors
        ):
            evidence += "; intended raw work equals TD-Tree on all cases"
        findings.append(
            (
                "Selectivity candidate volume / 선택도별 후보 수",
                evidence,
                (
                    "Primary cohort uses the identical supplied full raw path "
                    "and changes only Selectivity."
                    if primary_selectivity_id
                    == "full_raw_selectivity_candidates"
                    else
                    "The printed-paper root score can materially change "
                    "candidate work even when end-to-end time moves less."
                ),
            )
        )

    cpu_prune = ratios({"gopher_cpu32_early_prune_runtime"}, "wall_s")
    gpu_prune = ratios({"gopher_gpu_early_prune_runtime"}, "wall_s")
    raw_prune = ratios(
        {
            "gopher_cpu1_early_prune_candidates",
            "gopher_gpu_early_prune_candidates",
        },
        "raw_extension_candidates",
    )
    if raw_prune:
        reduction_low = 1.0 - 1.0 / min(raw_prune)
        reduction_high = 1.0 - 1.0 / max(raw_prune)
        evidence = (
            f"raw extensions −{reduction_low:.1%} to −{reduction_high:.1%}"
        )
        if cpu_prune:
            evidence += (
                f"; CPU32 {min(cpu_prune):.2f}–{max(cpu_prune):.2f}×"
            )
        if gpu_prune:
            evidence += (
                f"; GPU {min(gpu_prune):.2f}–{max(gpu_prune):.2f}×"
            )
        findings.append(
            (
                "Gopher early prune / 조기 가지치기",
                evidence,
                "Candidate reduction is consistent; runtime benefit is larger on CPU.",
            )
        )

    memory = ratios({"gpu_resident_memory_cap"}, "wall_s")
    if memory:
        findings.append(
            (
                "GPU memory cap / GPU 메모리 상한",
                f"32–1024 MiB variants are {min(memory):.2f}–{max(memory):.2f}× "
                "vs 4096 MiB",
                "All official caps completed without CPU fallback; small differences should be read with run spread.",
            )
        )
    return findings


def _write_overview(
    workbook: Workbook,
    data: dict[str, Any],
    groups: dict[tuple[Any, ...], tuple[list[dict[str, Any]], dict[str, Any]]],
) -> None:
    ws = workbook.create_sheet("Overview")
    header_row = style_title(
        ws,
        "Selectivity · Candidate · Ablation Evaluation / 선택도·후보·요인 평가",
        "Official manifest-selected runs only. Runtime and diagnostic cohorts "
        "are kept separate; cross-backend ratios are intentionally suppressed.",
    )
    schemas = sorted(
        {
            row["_metric_schema_version"]
            for row in data["selected"]
            if row["_metric_schema_version"]
        }
    )
    gpu_rows = [row for row in data["selected"] if row["_backend"] == "GPU"]
    overview = [
        ("Validation / 검증", "PASS", "All official runs OK; correctness PASS."),
        ("Official runs / 공식 실행", len(data["selected"]), "Manifest-selected repetitions"),
        ("Cases / 케이스", len(data["cases"]), "Controlled workloads"),
        ("Algorithms / 설정", len(data["algorithms"]), "Configuration IDs, not only engines"),
        ("Aggregated groups / 집계 그룹", len(groups), "No cohort/backend merging"),
        ("GPU runs / GPU 실행", len(gpu_rows), "CPU fallback is shown separately"),
        ("Candidate schema / 후보 스키마", ", ".join(schemas) or "MISSING", "Ratios require exact equality"),
        ("Comparisons / 비교 정의", len(data["comparisons"]), "Manifest-controlled when present"),
        ("Primary runtime / 실행시간", "median wall_s", "Cold-process end-to-end"),
        ("Original label / 기준 명칭", "Original (TD-Tree)", "tdtree is an Ours selectivity ablation mode"),
        (
            "Main comparison / 주 성능평가",
            MAIN_COMPARISON_WORKBOOK,
            "This workbook is additive: focused selectivity, candidate, pruning, and GPU ablations.",
        ),
    ]
    ws.append(["Metric / 항목", "Value / 값", "Audit interpretation / 해석"])
    style_header(ws, header_row, 1, 3)
    for item in overview:
        ws.append(item)
    main_row = header_row + 1 + next(
        index
        for index, item in enumerate(overview)
        if item[0] == "Main comparison / 주 성능평가"
    )
    ws.cell(main_row, 2).hyperlink = (
        "dsm_dataset_32t_protocol_corrected_20260729.xlsx"
    )
    ws.cell(main_row, 2).font = Font(color="0563C1", underline="single")
    ws.cell(header_row + 1, 2).fill = PASS_FILL
    ws.cell(header_row + 1, 2).font = Font(color="006100", bold=True)
    if not schemas:
        ws.cell(header_row + 7, 2).fill = WARNING_FILL
    ws.freeze_panes = f"A{header_row + 1}"
    add_table(ws, "OverviewAuditTable", header_row, ws.max_row, 3)
    findings = derive_key_findings(data, groups)
    if findings:
        row = ws.max_row + 3
        ws.cell(row, 1, "Key findings / 핵심 결과")
        ws.cell(row, 1).fill = SUBHEADER_FILL
        ws.cell(row, 1).font = Font(bold=True)
        findings_header = row + 1
        ws.append(
            [
                "Finding / 결과",
                "Evidence / 근거",
                "Interpretation / 해석",
            ]
        )
        style_header(ws, findings_header, 1, 3)
        for finding in findings:
            ws.append(finding)
        add_table(
            ws,
            "OverviewFindingsTable",
            findings_header,
            ws.max_row,
            3,
        )
        for data_row in range(findings_header + 1, ws.max_row + 1):
            for column in range(1, 4):
                ws.cell(data_row, column).alignment = Alignment(
                    wrap_text=True, vertical="top"
                )
    autosize(ws)


def _write_configuration(workbook: Workbook, data: dict[str, Any]) -> None:
    ws = workbook.create_sheet("Configuration Matrix")
    header_row = style_title(
        ws,
        "Configuration Matrix / 구성 행렬",
        "Algorithm IDs are exact executable configurations. Original is the "
        "TD-Tree implementation; tdtree is used only as an Ours selectivity mode.",
    )
    headers = [
        "Algorithm ID",
        "Display method",
        "Backend",
        "Cohort",
        "Role",
        "Threads",
        "Affinity",
        "Selectivity",
        "Prefilter",
        "Early prune",
        "Order",
        "Memo",
        "Parallel",
        "GPU variant",
        "GPU memory MiB",
        "Executable",
        "Flags",
    ]
    ws.append(headers)
    style_header(ws, header_row, 1, len(headers))
    for algorithm_id, algorithm in data["algorithms"].items():
        matching = next(
            (
                row
                for row in data["selected"]
                if row.get("algorithm") == algorithm_id
            ),
            None,
        )
        flags = flags_map(algorithm.get("flags", []))
        ws.append(
            [
                algorithm_id,
                algorithm_label(algorithm_id, algorithm),
                matching["_backend"] if matching else "",
                matching["_cohort"] if matching else algorithm.get("cohort", ""),
                algorithm.get("role", ""),
                algorithm.get("threads", flags.get("threads", "")),
                algorithm.get("cpu_affinity", ""),
                matching["_selectivity"] if matching else flags.get("selectivity", ""),
                matching["_prefilter"] if matching else flags.get("prefilter", ""),
                matching["_early_prune"] if matching else flags.get("early_prune", ""),
                matching["_order"] if matching else flags.get("order", ""),
                matching["_memo"] if matching else flags.get("memo", ""),
                matching["_parallel"] if matching else flags.get("parallel", ""),
                matching["_gpu_variant"] if matching else flags.get("gpu_variant", ""),
                matching["_gpu_memory_mb"] if matching else flags.get("gpu_memory_mb", ""),
                algorithm.get("executable", ""),
                " ".join(str(item) for item in algorithm.get("flags", [])),
            ]
        )
    algorithm_last = ws.max_row
    add_table(ws, "ConfigurationAlgorithmsTable", header_row, algorithm_last, len(headers))

    row = ws.max_row + 3
    ws.cell(row, 1, "Official cases / 공식 케이스")
    ws.cell(row, 1).fill = SUBHEADER_FILL
    ws.cell(row, 1).font = Font(bold=True)
    case_header = row + 1
    case_headers = [
        "Case ID",
        "Suite",
        "Dataset",
        "Input variant",
        "Query shape",
        "Query edges",
        "k",
        "Repeats",
        "Expected match count",
        "Algorithms",
        "Data",
        "Query",
    ]
    ws.append(case_headers)
    style_header(ws, case_header, 1, len(case_headers))
    default_repeats = data["manifest"].get("repeats", "")
    for case_id, case in data["cases"].items():
        ws.append(
            [
                case_id,
                case.get("suite", ""),
                case.get("dataset_id", ""),
                case.get("input_variant", ""),
                case.get("query_shape", ""),
                case.get("query_edges", ""),
                case.get("k", ""),
                case.get("repeats", default_repeats),
                case.get("expected_match_count", ""),
                ", ".join(case.get("algorithms", [])),
                case.get("data", ""),
                case.get("query", ""),
            ]
        )
    add_table(
        ws,
        "ConfigurationCasesTable",
        case_header,
        ws.max_row,
        len(case_headers),
    )
    ws.freeze_panes = f"A{header_row + 1}"
    ws.auto_filter.ref = f"A{header_row}:Q{algorithm_last}"
    autosize(ws, 55)


def _write_selectivity(
    workbook: Workbook,
    data: dict[str, Any],
    groups: dict[tuple[Any, ...], tuple[list[dict[str, Any]], dict[str, Any]]],
) -> None:
    ws = workbook.create_sheet("Selectivity Performance")
    header_row = style_title(
        ws,
        "Selectivity Performance / 선택도별 실행시간",
        "Median cold-process wall time. No speedup is computed across CPU/GPU "
        "or across performance/diagnostic cohorts.",
    )
    headers = [
        "Case ID",
        "Dataset",
        "Algorithm ID",
        "Method",
        "Backend",
        "Cohort",
        "Selectivity",
        "Prefilter",
        "k",
        "Root q-vertex",
        "Root selectivity score",
        "Retained edges",
        "Median wall (s)",
        "Q1 wall (s)",
        "Q3 wall (s)",
        "IQR wall (s)",
        "Match count",
        "Runs",
    ]
    ws.append(headers)
    style_header(ws, header_row, 1, len(headers))
    for key, (rows, aggregate) in sorted(groups.items()):
        row = rows[0]
        if row["_cohort"] == "correctness":
            continue
        ws.append(
            [
                row.get("case_id", ""),
                row["_dataset_id"],
                row.get("algorithm", ""),
                row["_algorithm_label"],
                row["_backend"],
                row["_cohort"],
                row["_selectivity"],
                row["_prefilter"],
                row["_k"],
                aggregate["root_query_vertex"],
                aggregate["root_selectivity_score"],
                aggregate["retained_edges"],
                aggregate["wall_median"],
                aggregate["wall_q1"],
                aggregate["wall_q3"],
                (
                    aggregate["wall_q3"] - aggregate["wall_q1"]
                    if aggregate["wall_q1"] is not None
                    and aggregate["wall_q3"] is not None
                    else None
                ),
                aggregate["match_count"],
                aggregate["runs"],
            ]
        )
    summary_last = ws.max_row
    add_table(ws, "SelectivityPerformanceTable", header_row, summary_last, len(headers))
    set_numeric_formats(ws, header_row)
    ws.freeze_panes = f"A{header_row + 1}"

    detail_rows = data["selectivity_rows.csv"]
    if detail_rows:
        row = ws.max_row + 3
        ws.cell(row, 1, "Per-query-vertex selectivity audit / 질의 정점별 선택도")
        ws.cell(row, 1).fill = SUBHEADER_FILL
        ws.cell(row, 1).font = Font(bold=True)
        detail_header = row + 1
        detail_headers = list(detail_rows[0])
        ws.append(detail_headers)
        style_header(ws, detail_header, 1, len(detail_headers))
        for detail in detail_rows:
            ws.append([detail.get(name, "") for name in detail_headers])
        add_table(
            ws,
            "SelectivityVertexDetailTable",
            detail_header,
            ws.max_row,
            len(detail_headers),
        )

    chart_rows = min(summary_last, header_row + 24)
    if chart_rows > header_row:
        chart = BarChart()
        chart.type = "bar"
        chart.style = 10
        chart.title = "Median runtime by official configuration"
        chart.y_axis.title = "Case / method"
        chart.x_axis.title = "Wall time (s)"
        chart.add_data(
            Reference(
                ws,
                min_col=headers.index("Median wall (s)") + 1,
                min_row=header_row,
                max_row=chart_rows,
            ),
            titles_from_data=True,
        )
        chart.set_categories(
            Reference(
                ws,
                min_col=headers.index("Algorithm ID") + 1,
                min_row=header_row + 1,
                max_row=chart_rows,
            )
        )
        chart.height = 8
        chart.width = 16
        chart.legend = None
        ws.add_chart(chart, f"T{header_row}")
    autosize(ws, 38)


def _write_candidate(
    workbook: Workbook,
    data: dict[str, Any],
    groups: dict[tuple[Any, ...], tuple[list[dict[str, Any]], dict[str, Any]]],
) -> None:
    ws = workbook.create_sheet("Candidate & Pruning")
    header_row = style_title(
        ws,
        "Candidate & Pruning / 후보 수와 가지치기",
        "raw_extension_candidates is the cross-engine common work unit. "
        "accepted_extensions is a same-backend/stage diagnostic. Ratios also "
        "require the exact same metric_schema_version and cohort.",
    )
    headers = [
        "Case ID",
        "Algorithm ID",
        "Method",
        "Backend",
        "Cohort",
        "Statistics",
        "Selectivity",
        "Prefilter",
        "Early prune",
        "Metric schema",
        "Root candidates",
        "Raw extension candidates",
        "Accepted extensions",
        "Acceptance rate",
        "DFS states",
        "Completed matches",
        "Pruned injectivity",
        "Pruned temporal support",
        "Pruned active support",
        "Pruned structural",
        "Pruned temporal",
        "Candidate relation entries*",
        "Runs",
    ]
    ws.append(headers)
    style_header(ws, header_row, 1, len(headers))
    for _, (rows, aggregate) in sorted(groups.items()):
        row = rows[0]
        raw = aggregate["raw_extension_candidates"]
        accepted = aggregate["accepted_extensions"]
        if not any(
            aggregate.get(name) is not None
            for name in CANDIDATE_METRICS
        ):
            continue
        ws.append(
            [
                row.get("case_id", ""),
                row.get("algorithm", ""),
                row["_algorithm_label"],
                row["_backend"],
                row["_cohort"],
                (
                    "on"
                    if row["_statistics_enabled"] is True
                    else "off"
                    if row["_statistics_enabled"] is False
                    else "always-on/unspecified"
                ),
                row["_selectivity"],
                row["_prefilter"],
                row["_early_prune"],
                row["_metric_schema_version"],
                aggregate["root_candidate_count"],
                raw,
                accepted,
                (
                    accepted / raw
                    if raw not in {None, 0} and accepted is not None
                    else None
                ),
                aggregate["dfs_states"],
                aggregate["completed_matches"],
                aggregate["pruned_injectivity"],
                aggregate["pruned_temporal_support"],
                aggregate["pruned_active_support"],
                aggregate["pruned_structural"],
                aggregate["pruned_temporal"],
                aggregate["candidate_relation_entries"],
                aggregate["runs"],
            ]
        )
    summary_last = ws.max_row
    add_table(ws, "CandidatePruningTable", header_row, summary_last, len(headers))
    set_numeric_formats(ws, header_row)
    for row in range(header_row + 1, summary_last + 1):
        ws.cell(row, headers.index("Acceptance rate") + 1).number_format = "0.00%"

    detail_rows = data["candidate_rows.csv"]
    if detail_rows:
        row = ws.max_row + 3
        ws.cell(row, 1, "Per-query-vertex candidate detail / 질의 정점별 후보")
        ws.cell(row, 1).fill = SUBHEADER_FILL
        ws.cell(row, 1).font = Font(bold=True)
        detail_header = row + 1
        detail_headers = list(detail_rows[0])
        ws.append(detail_headers)
        style_header(ws, detail_header, 1, len(detail_headers))
        for detail in detail_rows:
            ws.append([detail.get(name, "") for name in detail_headers])
        add_table(
            ws,
            "CandidateVertexDetailTable",
            detail_header,
            ws.max_row,
            len(detail_headers),
        )

    chart_rows = min(summary_last, header_row + 20)
    if chart_rows > header_row:
        chart = BarChart()
        chart.type = "col"
        chart.style = 10
        chart.title = "Raw vs accepted logical extensions"
        chart.y_axis.title = "Candidates"
        chart.x_axis.title = "Configuration"
        for column_name in ("Raw extension candidates", "Accepted extensions"):
            chart.add_data(
                Reference(
                    ws,
                    min_col=headers.index(column_name) + 1,
                    min_row=header_row,
                    max_row=chart_rows,
                ),
                titles_from_data=True,
            )
        chart.set_categories(
            Reference(
                ws,
                min_col=headers.index("Algorithm ID") + 1,
                min_row=header_row + 1,
                max_row=chart_rows,
            )
        )
        chart.height = 8
        chart.width = 17
        ws.add_chart(chart, f"Y{header_row}")
    ws.freeze_panes = f"A{header_row + 1}"
    autosize(ws, 36)


def _write_controlled_ablation(
    workbook: Workbook,
    data: dict[str, Any],
    groups: dict[tuple[Any, ...], tuple[list[dict[str, Any]], dict[str, Any]]],
) -> None:
    ws = workbook.create_sheet("Controlled Ablation")
    header_row = style_title(
        ws,
        "Controlled Ablation / 통제 요인 실험",
        "Ratios are baseline / variant. A blank ratio is deliberate whenever "
        "backend, cohort, or candidate metric schema is not controlled.",
    )
    headers = [
        "Comparison ID",
        "Category",
        "Manifest target",
        "Case ID",
        "Baseline algorithm",
        "Variant algorithm",
        "Cohort",
        "Backend",
        "Metric",
        "Baseline value",
        "Variant value",
        "Ratio eligible",
        "Baseline / variant",
        "Suppression reason",
        "Manifest notes",
    ]
    ws.append(headers)
    style_header(ws, header_row, 1, len(headers))
    rows = controlled_comparison_rows(
        data["comparisons"], groups, data["cases"]
    )
    if rows:
        for item in rows:
            ws.append(item)
    else:
        ws.append(
            [
                "No explicit comparisons",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                False,
                None,
                "Add top-level manifest.comparisons for controlled ratios.",
                "",
            ]
        )
    for row in range(header_row + 1, ws.max_row + 1):
        eligible = ws.cell(row, headers.index("Ratio eligible") + 1).value
        cell = ws.cell(row, headers.index("Ratio eligible") + 1)
        cell.fill = PASS_FILL if eligible else WARNING_FILL
        ws.cell(row, headers.index("Baseline / variant") + 1).number_format = "0.0000"
    add_table(ws, "ControlledAblationTable", header_row, ws.max_row, len(headers))
    ws.freeze_panes = f"A{header_row + 1}"
    autosize(ws, 48)


def _write_gpu(
    workbook: Workbook,
    groups: dict[tuple[Any, ...], tuple[list[dict[str, Any]], dict[str, Any]]],
) -> None:
    ws = workbook.create_sheet("GPU Evaluation")
    header_row = style_title(
        ws,
        "GPU Evaluation / ROCm GPU 평가",
        "GPU configurations are compared only with other GPU configurations "
        "in the same cohort. End-to-end wall time is primary; kernel and "
        "GPU-ready times are diagnostics.",
    )
    headers = [
        "Case ID",
        "Algorithm ID",
        "Cohort",
        "Selectivity",
        "Early prune",
        "Order",
        "GPU variant",
        "GPU memory MiB",
        "Median wall (s)",
        "GPU kernel (ms)",
        "GPU ready-to-result (ms)",
        "GPU search (ms)",
        "Peak frontier states",
        "Peak frontier bytes",
        "Root candidates",
        "Raw extension candidates",
        "Accepted extensions",
        "Match count",
        "Fallback observed",
        "Runs",
    ]
    ws.append(headers)
    style_header(ws, header_row, 1, len(headers))
    for _, (rows, aggregate) in sorted(groups.items()):
        row = rows[0]
        if row["_backend"] != "GPU":
            continue
        fallback = any(
            boolean(
                first_text(
                    item,
                    (
                        "fallback_used",
                        "result_fallback_used",
                        "gopher_fallback_used",
                    ),
                )
            )
            is True
            for item in rows
        )
        ws.append(
            [
                row.get("case_id", ""),
                row.get("algorithm", ""),
                row["_cohort"],
                row["_selectivity"],
                row["_early_prune"],
                row["_order"],
                row["_gpu_variant"],
                row["_gpu_memory_mb"],
                aggregate["wall_median"],
                aggregate["gpu_kernel_ms"],
                aggregate["gpu_ready_ms"],
                aggregate["gpu_search_ms"],
                aggregate["gpu_peak_frontier_states"],
                aggregate["gpu_peak_frontier_device_bytes"],
                aggregate["root_candidate_count"],
                aggregate["raw_extension_candidates"],
                aggregate["accepted_extensions"],
                aggregate["match_count"],
                fallback,
                aggregate["runs"],
            ]
        )
    if ws.max_row == header_row:
        ws.append(["No official GPU rows"] + [""] * (len(headers) - 1))
    last_row = ws.max_row
    add_table(ws, "GpuEvaluationTable", header_row, last_row, len(headers))
    set_numeric_formats(ws, header_row)
    fallback_column = headers.index("Fallback observed") + 1
    for row in range(header_row + 1, last_row + 1):
        ws.cell(row, fallback_column).fill = (
            FAIL_FILL if ws.cell(row, fallback_column).value is True else PASS_FILL
        )

    chart_rows = min(last_row, header_row + 20)
    if chart_rows > header_row and ws.cell(header_row + 1, 9).value is not None:
        chart = BarChart()
        chart.type = "bar"
        chart.style = 11
        chart.title = "GPU end-to-end runtime"
        chart.x_axis.title = "Wall time (s)"
        chart.add_data(
            Reference(ws, min_col=9, min_row=header_row, max_row=chart_rows),
            titles_from_data=True,
        )
        chart.set_categories(
            Reference(ws, min_col=2, min_row=header_row + 1, max_row=chart_rows)
        )
        chart.height = 7
        chart.width = 15
        chart.legend = None
        ws.add_chart(chart, f"V{header_row}")
    ws.freeze_panes = f"A{header_row + 1}"
    autosize(ws, 36)


def _write_correctness(workbook: Workbook, data: dict[str, Any]) -> None:
    ws = workbook.create_sheet("Correctness")
    header_row = style_title(
        ws,
        "Correctness / 정확성",
        "Every selected case must have correctness status PASS before this "
        "workbook is created.",
    )
    rows = [
        row
        for row in data["correctness"]
        if row.get("case_id") in data["cases"]
    ]
    headers = list(rows[0]) if rows else ["case_id", "status"]
    ws.append(headers)
    style_header(ws, header_row, 1, len(headers))
    for item in rows:
        ws.append([item.get(name, "") for name in headers])
    status_column = headers.index("status") + 1 if "status" in headers else None
    if status_column:
        for row in range(header_row + 1, ws.max_row + 1):
            cell = ws.cell(row, status_column)
            cell.fill = (
                PASS_FILL if str(cell.value).lower() == "pass" else FAIL_FILL
            )
            cell.font = Font(bold=True)
    add_table(ws, "CorrectnessTable", header_row, ws.max_row, len(headers))
    ws.freeze_panes = f"A{header_row + 1}"
    autosize(ws, 50)


def _write_raw_runs(workbook: Workbook, data: dict[str, Any]) -> None:
    ws = workbook.create_sheet("Raw Runs")
    header_row = style_title(
        ws,
        "Official Raw Runs / 공식 원시 실행",
        "Compact audit view. runs.csv in Provenance is the complete canonical "
        "source and retains every parsed field.",
    )
    preferred = [
        "case_id",
        "algorithm",
        "repeat",
        "status",
        "wall_s",
        "match_count",
        "backend",
        "threads",
        "threads_used",
        "cpu_affinity",
        "selectivity_config",
        "prefilter_config",
        "early_prune",
        "statistics_enabled",
        "metric_schema_version",
        "root_candidate_count",
        "raw_extension_candidates",
        "accepted_extensions",
        "dfs_states",
        "completed_matches",
        "pruned_injectivity",
        "pruned_temporal_support",
        "candidate_relation_entries",
        "retained_edges",
        "gpu_variant",
        "gpu_memory_mb_config",
        "gpuKernel_ms",
        "gpuReadyToResult_ms",
        "fallback_used",
        "resource_max_rss_kb",
        "run_dir",
        "run_signature",
        "executable_sha256",
        "data_sha256",
        "query_sha256",
        "command",
    ]
    source_headers = list(data["runs"][0]) if data["runs"] else []
    alias_keys = {
        alias
        for name in CANDIDATE_METRICS
        for alias in ALIASES.get(name, ())
    }
    useful_extra = [
        name
        for name in source_headers
        if name not in preferred
        and (
            name in alias_keys
            or name.startswith("result_metric_schema")
            or name.startswith("gopher_metric_schema")
        )
    ]
    headers = [name for name in preferred if name in source_headers]
    headers.extend(name for name in useful_extra if name not in headers)
    ws.append(headers)
    style_header(ws, header_row, 1, len(headers))
    selected_ids = {
        (
            str(row.get("case_id", "")),
            str(row.get("algorithm", "")),
            str(row.get("repeat", "")),
        )
        for row in data["selected"]
    }
    for item in data["runs"]:
        identity = (
            str(item.get("case_id", "")),
            str(item.get("algorithm", "")),
            str(item.get("repeat", "")),
        )
        if identity in selected_ids:
            ws.append([item.get(name, "") for name in headers])
    add_table(ws, "OfficialRawRunsTable", header_row, ws.max_row, len(headers))
    set_numeric_formats(ws, header_row)
    ws.freeze_panes = f"A{header_row + 1}"
    autosize(ws, 32)


def _write_methodology(workbook: Workbook, data: dict[str, Any]) -> None:
    ws = workbook.create_sheet("Methodology & Limits")
    header_row = style_title(
        ws,
        "Methodology & Limits / 방법론과 한계",
        "Definitions and comparison boundaries required to interpret the "
        "workbook without conflating incompatible counters or timings.",
    )
    ws.append(["Topic / 주제", "Definition / 정의", "Consequence / 해석"])
    style_header(ws, header_row, 1, 3)
    fixed = [
        (
            "Additive scope",
            f"This focused workbook supplements {MAIN_COMPARISON_WORKBOOK}.",
            "Use the main comparison workbook for the requested four-method dataset headline.",
        ),
        (
            "Original",
            "Original (TD-Tree) is the 2022 TD-Tree implementation.",
            "The Ours selectivity value `tdtree` is an ablation formula, not a new engine.",
        ),
        (
            "Runtime",
            "wall_s median over isolated official repetitions.",
            "Primary end-to-end metric; GPU kernel time is diagnostic only.",
        ),
        (
            "Selectivity modes",
            "tdtree=|V_L|/deg; paper=deg/(|V_L|·A_dur); "
            "intended=|V_L|/(deg·A_dur).",
            "They may select different roots and therefore different candidate work.",
        ),
        (
            "Logical candidate schema",
            "raw_extension_candidates counts TD-relation row entries inspected.",
            "Cross-engine candidate ratios require exact metric_schema_version equality.",
        ),
        (
            "Accepted extensions",
            "accepted_extensions is measured at an engine execution-stage boundary.",
            "With early-prune off, CPU counts a final extension before base-case "
            "empty-support rejection while fused GPU avoids materializing that state; "
            "therefore use it only within the same backend/stage.",
        ),
        (
            "Legacy candidate counters",
            "generated_candidates and accepted_candidates retain engine-local meanings.",
            "Only the normalized logical counters are eligible for cross-engine candidate ratios.",
        ),
        (
            "Root candidates",
            "Number of root data candidates before DFS expansion.",
            "Reported separately from non-root logical extension candidates.",
        ),
        (
            "Candidate relation entries*",
            "Materialized engine-specific relation size.",
            "Original TD-cell and Candidate-DAG layouts differ; do not use for cross-engine ratios.",
        ),
        (
            "Statistics off",
            "Statistics-gated candidate/pruning fields must be null/blank.",
            "A serialized zero is rejected because it is indistinguishable from a measured zero.",
        ),
        (
            "Cohort boundary",
            "performance and diagnostic runs are never combined in ratios.",
            "Instrumentation overhead cannot contaminate headline timing ratios.",
        ),
        (
            "Backend boundary",
            "CPU/GPU values may be shown side by side but not divided.",
            "GPU evaluation uses within-GPU comparisons only.",
        ),
        (
            "Count-only",
            "Suppresses match materialization, not match counting.",
            "A nonzero match_count remains a real cardinality result.",
        ),
    ]
    for item in fixed:
        ws.append(item)
    for key, value in data["manifest"].get("methodology", {}).items():
        ws.append((f"Manifest: {key}", str(value), "Declared experimental boundary"))
    add_table(ws, "MethodologyLimitsTable", header_row, ws.max_row, 3)
    for row in range(header_row + 1, ws.max_row + 1):
        for column in range(1, 4):
            ws.cell(row, column).alignment = Alignment(wrap_text=True, vertical="top")
    ws.freeze_panes = f"A{header_row + 1}"
    autosize(ws, 72)


def _write_provenance(
    workbook: Workbook, input_dir: Path, data: dict[str, Any]
) -> None:
    ws = workbook.create_sheet("Provenance")
    header_row = style_title(
        ws,
        "Provenance / 재현 출처",
        "Hashes bind this workbook to its source tables, used manifest, and "
        "environment snapshot.",
    )
    ws.append(["Kind", "Key / file", "Value / SHA-256"])
    style_header(ws, header_row, 1, 3)
    for name in (
        "runs.csv",
        "summary.csv",
        "correctness.csv",
        "candidate_rows.csv",
        "selectivity_rows.csv",
        "manifest.used.json",
        "environment.json",
        "manifest.used.sha256",
        "validation_receipt.json",
    ):
        path = input_dir / name
        if path.exists():
            ws.append(("file", str(path.resolve()), sha256(path)))
    for key, value in flatten_json(data["manifest"]):
        ws.append(("manifest", key, value))
    for key, value in flatten_json(data["environment"]):
        ws.append(("environment", key, value))
    add_table(ws, "ProvenanceTable", header_row, ws.max_row, 3)
    ws.freeze_panes = f"A{header_row + 1}"
    autosize(ws, 80)


def build_workbook(input_dir: Path, output: Path) -> dict[str, Any]:
    data = load_inputs(input_dir)
    groups = grouped_runs(data["selected"])
    workbook = Workbook()
    workbook.remove(workbook.active)
    workbook.properties.title = "Selectivity and Candidate Ablation Evaluation"
    workbook.properties.subject = (
        "Original (TD-Tree), Ours, Ours+Gopher CPU and ROCm GPU evaluation"
    )
    workbook.properties.creator = "DSM benchmark strict workbook generator"

    _write_overview(workbook, data, groups)
    _write_configuration(workbook, data)
    _write_selectivity(workbook, data, groups)
    _write_candidate(workbook, data, groups)
    _write_controlled_ablation(workbook, data, groups)
    _write_gpu(workbook, groups)
    _write_correctness(workbook, data)
    _write_raw_runs(workbook, data)
    _write_methodology(workbook, data)
    _write_provenance(workbook, input_dir, data)

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        prefix=f".{output.stem}.",
        suffix=".xlsx",
        dir=output.parent,
        delete=False,
    ) as temporary:
        temporary_path = Path(temporary.name)
    try:
        workbook.save(temporary_path)
        validation = validate_workbook(temporary_path)
        os.replace(temporary_path, output)
    finally:
        if temporary_path.exists():
            temporary_path.unlink()
    validation.update(
        {
            "output": str(output.resolve()),
            "sha256": sha256(output),
            "official_runs": len(data["selected"]),
            "official_groups": len(groups),
            "manifest_comparisons": len(data["comparisons"]),
            "metric_schema_versions": sorted(
                {
                    row["_metric_schema_version"]
                    for row in data["selected"]
                    if row["_metric_schema_version"]
                }
            ),
        }
    )
    return validation


def validate_workbook(path: Path) -> dict[str, Any]:
    workbook = load_workbook(path, data_only=False, read_only=False)
    if workbook.sheetnames != EXPECTED_SHEETS:
        raise ValueError(
            f"Unexpected sheet order: expected {EXPECTED_SHEETS}, "
            f"found {workbook.sheetnames}"
        )
    overview = workbook["Overview"]
    validation_cells = [
        cell.value
        for row in overview.iter_rows()
        for cell in row
        if cell.value == "PASS"
    ]
    if not validation_cells:
        raise ValueError("Overview validation PASS marker is missing")
    charts = sum(len(sheet._charts) for sheet in workbook.worksheets)
    if charts < 2:
        raise ValueError("Expected at least two focused charts")

    controlled = workbook["Controlled Ablation"]
    header_row = next(
        (
            row
            for row in range(1, controlled.max_row + 1)
            if controlled.cell(row, 1).value == "Comparison ID"
        ),
        None,
    )
    if header_row is None:
        raise ValueError("Controlled Ablation header is missing")
    headers = {
        controlled.cell(header_row, column).value: column
        for column in range(1, controlled.max_column + 1)
    }
    for row in range(header_row + 1, controlled.max_row + 1):
        eligible = controlled.cell(row, headers["Ratio eligible"]).value
        ratio = controlled.cell(row, headers["Baseline / variant"]).value
        reason = controlled.cell(row, headers["Suppression reason"]).value
        if eligible is False and ratio not in {None, ""}:
            raise ValueError("Suppressed comparison contains a ratio")
        if eligible is False and not reason:
            raise ValueError("Suppressed comparison lacks an audit reason")
    if any(sheet.sheet_state != "visible" for sheet in workbook.worksheets):
        raise ValueError("All audit sheets must remain visible")
    return {
        "validation": "passed",
        "sheets": len(workbook.sheetnames),
        "charts": charts,
        "tables": sum(len(sheet.tables) for sheet in workbook.worksheets),
    }


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.validate_only:
        print(json.dumps(validate_workbook(args.validate_only), indent=2))
        return 0
    if not args.input_dir:
        raise SystemExit("--input-dir is required")
    print(json.dumps(build_workbook(args.input_dir, args.output), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
