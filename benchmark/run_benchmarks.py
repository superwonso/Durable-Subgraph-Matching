#!/usr/bin/env python3
"""Reproducible benchmark runner for the DSM CPU and GPU implementations.

The runner intentionally uses only Python's standard library.  It executes
every run in an isolated directory so programs that emit fixed result filenames
cannot overwrite another measurement.
"""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = Path(__file__).with_name("experiment_manifest.json")
REPORT_SIDECARS = (
    "environment.json",
    "dataset_inventory.csv",
    "methodology.csv",
)
_SHA256_CACHE: dict[tuple[str, int, int], str] = {}


def expand_path(value: str) -> str:
    return value.replace("{root}", str(ROOT))


def safe_id(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("_")


def sha256_file(path: Path) -> str:
    """Hash an immutable benchmark input once per runner process."""

    stat = path.stat()
    key = (str(path.resolve()), stat.st_size, stat.st_mtime_ns)
    cached = _SHA256_CACHE.get(key)
    if cached is not None:
        return cached
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    value = digest.hexdigest()
    _SHA256_CACHE[key] = value
    return value


def declared_threads(algorithm: dict[str, Any]) -> int | None:
    """Return and validate the optional manifest-level thread declaration."""

    if "threads" not in algorithm:
        return None
    explicit = algorithm["threads"]
    if (
        not isinstance(explicit, int)
        or isinstance(explicit, bool)
        or explicit <= 0
    ):
        raise ValueError("algorithm threads must be a positive integer")
    return explicit


def argv_threads(command: list[str]) -> int | None:
    """Read the effective application thread count from resolved argv."""

    value = command_option(command, "--threads")
    if value is None:
        return None
    if not re.fullmatch(r"\d+", value) or int(value) <= 0:
        raise ValueError("--threads must resolve to a positive integer")
    return int(value)


def configured_threads(
    algorithm: dict[str, Any],
    command: list[str] | None = None,
) -> int | None:
    """Return the effective application thread count.

    A resolved command is authoritative because case-level and
    algorithm-specific case flags are appended to the manifest algorithm
    flags.  When a manifest also declares ``threads``, the declaration is an
    assertion and a mismatch is rejected instead of silently mislabelling a
    run.  The declaration remains a compatibility fallback for implementations
    that do not expose a ``--threads`` option.
    """

    declared = declared_threads(algorithm)
    if command is None:
        flags = [expand_path(str(flag)) for flag in algorithm.get("flags", [])]
        command = flags
    resolved = argv_threads(command)
    if declared is not None and resolved is not None and declared != resolved:
        raise ValueError(
            "declared threads "
            f"({declared}) do not match resolved --threads ({resolved})"
        )
    return resolved if resolved is not None else declared


def command_option(command: list[str], option: str) -> str | None:
    """Return the last effective value of a CLI option.

    Case-level flags are appended after algorithm flags, so scanning the full
    resolved argv and retaining the last occurrence mirrors the matchers'
    duplicate-option rejection while still making failed runs auditable.
    """

    value: str | None = None
    for index, argument in enumerate(command):
        if argument == option and index + 1 < len(command):
            value = command[index + 1]
        elif argument.startswith(f"{option}="):
            value = argument.split("=", 1)[1]
    return value


def parse_scalar(text: str) -> Any:
    value = text.strip()
    lowered = value.lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    if lowered == "null":
        return None
    if lowered in {"nan", "+nan", "-nan"}:
        return math.nan
    if re.fullmatch(r"[-+]?\d+", value):
        try:
            return int(value)
        except ValueError:
            pass
    if re.fullmatch(
        r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?", value
    ):
        try:
            return float(value)
        except ValueError:
            pass
    return value


def parse_key_value_file(path: Path, suffix: str = "") -> dict[str, Any]:
    values: dict[str, Any] = {}
    if not path.exists():
        return values
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if ":" in raw_line:
            key, value = raw_line.split(":", 1)
        elif "=" in raw_line:
            key, value = raw_line.split("=", 1)
        else:
            continue
        key = safe_id(key.strip())
        value = re.sub(r"\s+ms$", "", value.strip())
        if key:
            values[f"{key}{suffix}"] = parse_scalar(value)
    return values


def parse_timing_file(path: Path) -> dict[str, Any]:
    """Parse timing metadata without corrupting explicit unit suffixes.

    Stage timings printed as ``value ms`` receive an ``_ms`` column suffix.
    Keys that already encode another unit (for example ``cache_lookup_ns`` or
    ``cache_memory_bytes``) are preserved verbatim.
    """
    values: dict[str, Any] = {}
    if not path.exists():
        return values
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if ":" in raw_line:
            key, value = raw_line.split(":", 1)
        elif "=" in raw_line:
            key, value = raw_line.split("=", 1)
        else:
            continue
        clean_key = safe_id(key.strip())
        raw_value = value.strip()
        if raw_value.endswith(" ms"):
            raw_value = raw_value[:-3].strip()
            if not clean_key.lower().endswith("_ms"):
                clean_key += "_ms"
        if clean_key:
            values[clean_key] = parse_scalar(raw_value)
    return values


def normalize_plan_order(value: Any) -> str | None:
    """Normalize a plan order from text or JSON to a space-separated string."""

    if isinstance(value, list):
        if all(
            isinstance(item, int) and not isinstance(item, bool)
            for item in value
        ):
            return " ".join(str(item) for item in value)
        return None
    if not isinstance(value, str):
        return None
    stripped = value.strip()
    if not stripped:
        return ""
    try:
        decoded = json.loads(stripped)
    except json.JSONDecodeError:
        decoded = None
    if isinstance(decoded, list):
        return normalize_plan_order(decoded)
    tokens = re.findall(r"-?\d+", stripped)
    if tokens and re.fullmatch(r"[\s,\[\]\d+-]+", stripped):
        return " ".join(tokens)
    return " ".join(stripped.split())


def nested_json_value(
    payload: dict[str, Any], *path: str
) -> tuple[bool, Any]:
    current: Any = payload
    for key in path:
        if not isinstance(current, dict) or key not in current:
            return False, None
        current = current[key]
    return True, current


def merge_json_canonical_metrics(
    metrics: dict[str, Any], payload: dict[str, Any]
) -> None:
    """Expose cross-engine metric names in addition to legacy gopher_* names."""

    def assign(
        canonical: str,
        paths: tuple[tuple[str, ...], ...],
        *,
        transform: Any = None,
    ) -> None:
        for path in paths:
            present, value = nested_json_value(payload, *path)
            if not present:
                continue
            if transform is not None:
                value = transform(value)
                if value is None:
                    continue
            metrics[canonical] = value
            return

    assign("metric_schema_version", (("metric_schema_version",),))
    assign(
        "root_candidate_count",
        (("root_candidate_count",),),
    )
    assign(
        "candidate_relation_entries",
        (("candidate_relation_entries",),),
    )
    assign(
        "raw_extension_candidates",
        (
            ("statistics", "raw_extension_candidates"),
            ("raw_extension_candidates",),
        ),
    )
    assign(
        "post_structural_candidates",
        (
            ("statistics", "post_structural_candidates"),
            ("statistics", "generated_candidates"),
            ("post_structural_candidates",),
        ),
    )
    assign(
        "accepted_extensions",
        (
            ("statistics", "accepted_extensions"),
            ("statistics", "accepted_candidates"),
            ("accepted_extensions",),
        ),
    )
    assign(
        "query_root",
        (
            ("query_root",),
            ("plan", "root"),
        ),
    )
    assign(
        "plan_order",
        (
            ("plan_order",),
            ("plan", "order"),
        ),
        transform=normalize_plan_order,
    )
    assign(
        "selectivity",
        (
            ("selectivity",),
            ("selectivity_mode",),
        ),
    )
    assign("prefilter", (("prefilter",),))
    assign("early_prune", (("early_prune",),))
    assign("statistics_enabled", (("statistics_enabled",),))
    assign(
        "gpu_frontier_budget_bytes",
        (("gpu", "frontier_budget_bytes"),),
    )

    statistics = payload.get("statistics")
    statistics_enabled = metrics.get("statistics_enabled")
    if statistics_enabled is False:
        # Older CPU Gopher metrics serialized zero-filled counters while
        # explicitly declaring statistics off.  The enable flag is the
        # authority; those zeros are missing observations, not measurements.
        metrics["statistics_collected"] = False
        for key in list(metrics):
            if key.startswith("gopher_statistics_"):
                metrics[key] = None
        for key in (
            "raw_extension_candidates",
            "post_structural_candidates",
            "accepted_extensions",
        ):
            if key in metrics:
                metrics[key] = None
    elif isinstance(statistics, dict) and statistics:
        values = list(statistics.values())
        metrics["statistics_collected"] = any(
            value is not None for value in values
        )
    elif isinstance(statistics_enabled, bool):
        metrics["statistics_collected"] = statistics_enabled

    budget = metrics.get("gpu_frontier_budget_bytes")
    if (
        isinstance(budget, (int, float))
        and not isinstance(budget, bool)
        and math.isfinite(float(budget))
    ):
        metrics["gpu_memory_mb_effective"] = (
            float(budget) / (1024.0 * 1024.0)
        )

def normalize_output_metrics(metrics: dict[str, Any]) -> None:
    """Fill canonical names from timing/result aliases without coercing null."""

    alias_groups = {
        "metric_schema_version": (
            "result_metric_schema_version",
            "gopher_metric_schema_version",
        ),
        "root_candidate_count": (
            "result_root_candidate_count",
            "gopher_root_candidate_count",
        ),
        "candidate_relation_entries": (
            "result_candidate_relation_entries",
            "gopher_candidate_relation_entries",
            "candidate_relation_entries_stdout",
        ),
        "raw_extension_candidates": (
            "result_raw_extension_candidates",
            "gopher_statistics_raw_extension_candidates",
        ),
        "post_structural_candidates": (
            "result_post_structural_candidates",
            "gopher_statistics_post_structural_candidates",
            "gopher_statistics_generated_candidates",
        ),
        "accepted_extensions": (
            "result_accepted_extensions",
            "gopher_statistics_accepted_extensions",
            "gopher_statistics_accepted_candidates",
        ),
        "query_root": (
            "result_query_root",
            "gopher_query_root",
            "gopher_plan_root",
        ),
        "selectivity": (
            "selectivity_mode",
            "result_selectivity",
            "result_selectivity_mode",
            "gopher_selectivity",
            "gopher_selectivity_mode",
        ),
        "prefilter": (
            "result_prefilter",
            "gopher_prefilter",
        ),
        "early_prune": (
            "result_early_prune",
            "gopher_early_prune",
        ),
        "statistics_enabled": (
            "result_statistics_enabled",
            "gopher_statistics_enabled",
        ),
        "statistics_collected": (
            "result_statistics_collected",
            "gopher_statistics_collected",
        ),
        "gpu_frontier_budget_bytes": (
            "gopher_gpu_frontier_budget_bytes",
        ),
    }
    for canonical, aliases in alias_groups.items():
        if canonical in metrics:
            continue
        for alias in aliases:
            if alias in metrics:
                metrics[canonical] = metrics[alias]
                break

    if "plan_order" not in metrics:
        for alias in (
            "result_plan_order",
            "gopher_plan_order",
        ):
            if alias in metrics:
                normalized = normalize_plan_order(metrics[alias])
                if normalized is not None:
                    metrics["plan_order"] = normalized
                    break
    else:
        normalized = normalize_plan_order(metrics["plan_order"])
        if normalized is not None:
            metrics["plan_order"] = normalized

    budget = metrics.get("gpu_frontier_budget_bytes")
    if (
        "gpu_memory_mb_effective" not in metrics
        and isinstance(budget, (int, float))
        and not isinstance(budget, bool)
        and math.isfinite(float(budget))
    ):
        metrics["gpu_memory_mb_effective"] = (
            float(budget) / (1024.0 * 1024.0)
        )

    if "statistics_collected" not in metrics:
        statistic_keys = (
            "raw_extension_candidates",
            "post_structural_candidates",
            "accepted_extensions",
        )
        present_values = [
            metrics[key] for key in statistic_keys if key in metrics
        ]
        if present_values:
            metrics["statistics_collected"] = any(
                value is not None for value in present_values
            )
        else:
            enabled = metrics.get("statistics_enabled")
            if isinstance(enabled, bool):
                metrics["statistics_collected"] = enabled
            elif isinstance(enabled, str) and enabled.lower() in {
                "on",
                "off",
            }:
                metrics["statistics_collected"] = (
                    enabled.lower() == "on"
                )


def parse_program_outputs(run_dir: Path, stdout: str) -> dict[str, Any]:
    metrics: dict[str, Any] = {}

    graph_match = re.search(
        r"Temporal graph:\s*(\d+) occurrences,\s*(\d+) unique directed edges,"
        r"\s*(\d+) (?:retained edges|filtered edges),\s*(\d+) active vertices",
        stdout,
    )
    if graph_match:
        metrics.update(
            {
                "input_occurrences": int(graph_match.group(1)),
                "input_unique_edges": int(graph_match.group(2)),
                "retained_edges": int(graph_match.group(3)),
                "active_vertices": int(graph_match.group(4)),
            }
        )

    root_match = re.search(r"Query root:\s*q(\d+).*?\|\s*DFS order:([^|\n]+)", stdout)
    if root_match:
        metrics["query_root"] = int(root_match.group(1))
        metrics["dfs_order"] = " ".join(root_match.group(2).split())

    final_match = re.search(
        r"Final (?:durable )?matches:\s*(OVERFLOW|\d+)", stdout
    )
    if final_match:
        if final_match.group(1) == "OVERFLOW":
            metrics["match_count_overflow"] = True
        else:
            metrics["match_count"] = int(final_match.group(1))

    relation_entries = [
        int(value) for value in re.findall(r"relation_entries=(\d+)", stdout)
    ]
    if relation_entries:
        metrics["candidate_relation_entries_stdout"] = sum(relation_entries)

    # Accept both "key: value" and "key=value" statistics printed to stdout.
    known_stat_prefixes = (
        "recursive",
        "partial",
        "generated",
        "raw_extension",
        "post_structural",
        "accepted_extension",
        "structural",
        "temporal",
        "pruned",
        "completed",
        "root_candidate",
        "candidate_relation",
        "metric_schema",
        "cache",
        "memo",
        "dag",
        "result_digest",
        "thread",
        "tasks",
        "count_overflow",
    )
    known_configuration_keys = {
        "selectivity",
        "selectivity_mode",
        "prefilter",
        "early_prune",
        "statistics_enabled",
        "statistics_collected",
        "gpu_frontier_budget_bytes",
        "plan_order",
    }
    for line in stdout.splitlines():
        match = re.fullmatch(
            r"\s*([A-Za-z][A-Za-z0-9_. -]*?)\s*[:=]\s*(.+?)\s*", line
        )
        if not match:
            continue
        key = safe_id(match.group(1)).lower()
        if (
            key in known_configuration_keys
            or key.startswith(known_stat_prefixes)
        ):
            metrics[key] = parse_scalar(match.group(2))

    timing_files = sorted(run_dir.glob("timing_results_*.txt"))
    if timing_files:
        metrics.update(parse_timing_file(timing_files[-1]))

    result_files = sorted(run_dir.glob("matching_results_*.txt"))
    if result_files:
        result_text = result_files[-1].read_text(
            encoding="utf-8", errors="replace"
        )
        count_match = re.search(r"^Count:\s*(OVERFLOW|\d+)", result_text, re.MULTILINE)
        if count_match:
            if count_match.group(1) == "OVERFLOW":
                metrics["match_count_overflow"] = True
            else:
                metrics["match_count"] = int(count_match.group(1))
        metrics.update(
            {
                f"result_{key.lower()}": value
                for key, value in parse_key_value_text(result_text).items()
            }
        )

    metrics_files = sorted(run_dir.glob("metrics_results_*.json"))
    metrics_files.extend(sorted(run_dir.glob("gopher_metrics*.json")))
    if metrics_files:
        try:
            payload = json.loads(metrics_files[-1].read_text(encoding="utf-8"))

            def add_json_values(value: Any, prefix: str) -> None:
                if isinstance(value, dict):
                    for child_key, child_value in value.items():
                        clean = safe_id(str(child_key)).lower()
                        add_json_values(
                            child_value,
                            f"{prefix}_{clean}" if prefix else clean,
                        )
                elif isinstance(value, list):
                    metrics[f"gopher_{prefix}"] = json.dumps(
                        value, ensure_ascii=False, sort_keys=True
                    )
                else:
                    metrics[f"gopher_{prefix}"] = value

            add_json_values(payload, "")
            if isinstance(payload, dict):
                merge_json_canonical_metrics(metrics, payload)
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            metrics["gopher_metrics_parse_error"] = str(error)
    normalize_output_metrics(metrics)
    return metrics


def parse_key_value_text(text: str) -> dict[str, Any]:
    values: dict[str, Any] = {}
    for line in text.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        clean_key = safe_id(key.strip())
        if clean_key:
            values[clean_key] = parse_scalar(value.strip())
    return values


def read_time_metrics(path: Path) -> dict[str, Any]:
    values = parse_key_value_file(path)
    renamed: dict[str, Any] = {}
    for key, value in values.items():
        renamed[f"resource_{key}"] = value
    return renamed


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    columns = sorted({key for row in rows for key in row})
    preferred = [
        "case_id",
        "algorithm",
        "semantic_group",
        "repeat",
        "status",
        "exit_code",
        "wall_s",
        "match_count",
    ]
    ordered = [column for column in preferred if column in columns]
    ordered.extend(column for column in columns if column not in ordered)
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=ordered)
        writer.writeheader()
        writer.writerows(rows)


def factor_fields(algorithm: dict[str, Any]) -> dict[str, Any]:
    """Flatten algorithm ablation factors into stable ``factor_*`` columns."""

    raw_factors = algorithm.get("factors", {})
    if raw_factors is None:
        return {}
    if not isinstance(raw_factors, dict):
        raise ValueError("algorithm factors must be an object")
    flattened: dict[str, Any] = {}
    for raw_key, raw_value in raw_factors.items():
        normalized_key = safe_id(str(raw_key)).lower()
        normalized_key = re.sub(r"[.-]+", "_", normalized_key)
        key = f"factor_{normalized_key}"
        if key == "factor_":
            raise ValueError("algorithm factor names must not be empty")
        if key in flattened:
            raise ValueError(
                f"algorithm factors collide after normalization: {raw_key}"
            )
        if isinstance(raw_value, (dict, list)):
            flattened[key] = json.dumps(
                raw_value, ensure_ascii=False, sort_keys=True
            )
        else:
            flattened[key] = raw_value
    return flattened


def parse_summary_section(
    text: str, section_name: str
) -> list[dict[str, Any]]:
    """Parse ``qN key=value`` rows from one matcher result section."""

    rows: list[dict[str, Any]] = []
    in_section = False
    section_header = f"[{section_name}]"
    for raw_line in text.splitlines():
        stripped = raw_line.strip()
        if stripped == section_header:
            in_section = True
            continue
        if in_section and re.fullmatch(r"\[[^\]]+\]", stripped):
            break
        if not in_section or not stripped:
            continue
        vertex_match = re.match(r"^q(\d+)(?:\s+|$)(.*)$", stripped)
        if not vertex_match:
            continue
        parsed: dict[str, Any] = {
            "query_vertex": int(vertex_match.group(1))
        }
        remainder = vertex_match.group(2)
        for match in re.finditer(
            r"([A-Za-z][A-Za-z0-9_.-]*)=([^\s]*)",
            remainder,
        ):
            key = safe_id(match.group(1)).lower()
            parsed[key] = parse_scalar(match.group(2))
        rows.append(parsed)
    return rows


def plan_positions(value: Any) -> dict[int, int]:
    normalized = normalize_plan_order(value)
    if normalized is None:
        return {}
    positions: dict[int, int] = {}
    for position, token in enumerate(normalized.split()):
        if re.fullmatch(r"-?\d+", token):
            positions[int(token)] = position
    return positions


def detail_row_context(row: dict[str, Any]) -> dict[str, Any]:
    """Copy audit identity/configuration fields to normalized detail rows."""

    fixed_fields = (
        "case_id",
        "algorithm",
        "semantic_group",
        "repeat",
        "status",
        "data_sha256",
        "query_sha256",
        "k",
        "label_seed",
        "backend",
        "device",
        "threads",
        "threads_declared",
        "threads_configured_argv",
        "suite",
        "dataset_id",
        "input_variant",
        "query_shape",
        "query_edges",
        "order_config",
        "selectivity_config",
        "prefilter_config",
        "early_prune_config",
        "statistics_config",
        "gpu_memory_mb_config",
        "gpu_memory_bytes_config",
        "metric_schema_version",
        "query_root",
        "plan_order",
        "selectivity",
        "prefilter",
        "early_prune",
        "statistics_enabled",
        "statistics_collected",
        "run_dir",
    )
    context = {
        field: row.get(field)
        for field in fixed_fields
        if field in row
    }
    context.update(
        {
            key: value
            for key, value in row.items()
            if key.startswith("factor_")
        }
    )
    return context


def result_detail_rows(
    rows: Iterable[dict[str, Any]],
    section_name: str,
) -> list[dict[str, Any]]:
    """Read and normalize one q-level section from every completed run."""

    detailed: list[dict[str, Any]] = []
    for row in rows:
        raw_run_dir = row.get("run_dir")
        if not isinstance(raw_run_dir, str) or not raw_run_dir:
            continue
        run_dir = Path(raw_run_dir)
        result_files = sorted(run_dir.glob("matching_results_*.txt"))
        if not result_files:
            continue
        try:
            text = result_files[-1].read_text(
                encoding="utf-8", errors="replace"
            )
        except OSError:
            continue
        result_values = {
            key.lower(): value
            for key, value in parse_key_value_text(text).items()
        }
        plan_order = row.get("plan_order")
        if plan_order is None:
            plan_order = result_values.get("plan_order")
        positions = plan_positions(plan_order)
        query_root = row.get("query_root")
        if query_root is None:
            query_root = result_values.get("query_root")
        context = detail_row_context(row)
        if "plan_order" not in context and plan_order is not None:
            context["plan_order"] = normalize_plan_order(plan_order)
        if "query_root" not in context and query_root is not None:
            context["query_root"] = query_root
        if "selectivity" not in context:
            result_selectivity = result_values.get(
                "selectivity",
                result_values.get("selectivity_mode"),
            )
            if result_selectivity is not None:
                context["selectivity"] = result_selectivity
        for parsed in parse_summary_section(text, section_name):
            query_vertex = parsed["query_vertex"]
            normalized = {**context, **parsed}
            if section_name == "Candidate Summary":
                if "candidates" in parsed:
                    normalized["unique_candidates"] = parsed["candidates"]
                if "relation_entries" in parsed:
                    normalized["candidate_relation_entries_q"] = parsed[
                        "relation_entries"
                    ]
                if isinstance(query_root, int) and not isinstance(
                    query_root, bool
                ):
                    normalized["is_root"] = query_vertex == query_root
                if query_vertex in positions:
                    normalized["order_position"] = positions[query_vertex]
            elif section_name == "Selectivity Summary":
                if "candidate_count" in parsed:
                    normalized["label_candidate_count"] = parsed[
                        "candidate_count"
                    ]
                if "is_root" not in normalized and isinstance(
                    query_root, int
                ) and not isinstance(query_root, bool):
                    normalized["is_root"] = query_vertex == query_root
                if "order_position" not in normalized and query_vertex in positions:
                    normalized["order_position"] = positions[query_vertex]
            detailed.append(normalized)
    return detailed


def candidate_rows(
    rows: Iterable[dict[str, Any]]
) -> list[dict[str, Any]]:
    return result_detail_rows(rows, "Candidate Summary")


def selectivity_rows(
    rows: Iterable[dict[str, Any]]
) -> list[dict[str, Any]]:
    return result_detail_rows(rows, "Selectivity Summary")


def gpu_depth_rows(rows: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    """Explode GPU per-depth JSON into typed, auditable rows."""

    exploded: list[dict[str, Any]] = []
    for row in rows:
        raw_depths = row.get("gopher_depths")
        if not isinstance(raw_depths, str) or not raw_depths.strip():
            continue
        try:
            depths = json.loads(raw_depths)
        except json.JSONDecodeError:
            continue
        if not isinstance(depths, list):
            continue
        for depth in depths:
            if not isinstance(depth, dict):
                continue
            exploded.append(
                {
                    "case_id": row.get("case_id"),
                    "algorithm": row.get("algorithm"),
                    "repeat": row.get("repeat"),
                    "status": row.get("status"),
                    "semantic_group": row.get("semantic_group"),
                    "data_sha256": row.get("data_sha256"),
                    "query_sha256": row.get("query_sha256"),
                    "k": row.get("k"),
                    "label_seed": row.get("label_seed"),
                    "gpu_variant": (
                        row.get("gopher_gpu_variant")
                        or row.get("gpu_variant_config")
                    ),
                    "statistics_enabled": row.get(
                        "statistics_enabled",
                        row.get("gopher_statistics_enabled"),
                    ),
                    "statistics_collected": row.get(
                        "statistics_collected"
                    ),
                    "query_root": row.get("query_root"),
                    "plan_order": row.get("plan_order"),
                    "selectivity": row.get("selectivity"),
                    "prefilter": row.get("prefilter"),
                    "early_prune": row.get("early_prune"),
                    "gpu_memory_mb_config": row.get(
                        "gpu_memory_mb_config"
                    ),
                    "gpu_frontier_budget_bytes": row.get(
                        "gpu_frontier_budget_bytes"
                    ),
                    **{
                        key: value
                        for key, value in row.items()
                        if key.startswith("factor_")
                    },
                    **depth,
                }
            )
    return exploded


def numeric_values(rows: Iterable[dict[str, Any]], key: str) -> list[float]:
    values: list[float] = []
    for row in rows:
        value = row.get(key)
        if isinstance(value, bool):
            continue
        if isinstance(value, (int, float)) and math.isfinite(float(value)):
            values.append(float(value))
    return values


def summarize(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[str, str, str], list[dict[str, Any]]] = {}
    for row in rows:
        key = (
            str(row["case_id"]),
            str(row["algorithm"]),
            str(row.get("semantic_group", "")),
        )
        groups.setdefault(key, []).append(row)

    summaries: list[dict[str, Any]] = []
    for (case_id, algorithm, semantics), group in sorted(groups.items()):
        summary: dict[str, Any] = {
            "case_id": case_id,
            "algorithm": algorithm,
            "semantic_group": semantics,
            "runs": len(group),
            "successful_runs": sum(row.get("status") == "ok" for row in group),
            "timeouts": sum(row.get("status") == "timeout" for row in group),
            "errors": sum(row.get("status") == "error" for row in group),
        }
        metric_keys = sorted({key for row in group for key in row})
        successful = [row for row in group if row.get("status") == "ok"]
        for metric in metric_keys:
            values = numeric_values(
                successful, metric
            )
            if not values:
                continue
            summary[f"{metric}_median"] = statistics.median(values)
            summary[f"{metric}_min"] = min(values)
            summary[f"{metric}_max"] = max(values)
            if len(values) >= 2:
                summary[f"{metric}_mean"] = statistics.fmean(values)
                summary[f"{metric}_stdev"] = statistics.stdev(values)
                quartiles = statistics.quantiles(
                    values, n=4, method="inclusive"
                )
                summary[f"{metric}_q1"] = quartiles[0]
                summary[f"{metric}_q3"] = quartiles[2]
                summary[f"{metric}_iqr"] = quartiles[2] - quartiles[0]
        stable_metric_tokens = (
            "fallback",
            "verification",
            "digest",
            "statistics_enabled",
            "parallel_mode",
            "memo_effective",
            "gpu_variant",
            "device_name",
            "architecture",
            "selectivity",
            "order_mode",
            "result_order",
            "gopher_order",
            "order_config",
            "memo_config",
            "statistics_config",
            "selectivity_config",
            "parallel_config",
            "implementation_generation",
            "comparison_family",
            "cpu_affinity",
            "factor_",
        )
        for metric in metric_keys:
            if not any(token in metric.lower() for token in stable_metric_tokens):
                continue
            values = [
                row.get(metric)
                for row in successful
                if isinstance(row.get(metric), (str, bool))
            ]
            if len(values) == len(successful) and values:
                canonical = {
                    json.dumps(value, ensure_ascii=False, sort_keys=True)
                    for value in values
                }
                if len(canonical) == 1:
                    summary[f"{metric}_value"] = values[0]
        counted = [
            row
            for row in successful
            if isinstance(row.get("match_count"), int)
            and not isinstance(row.get("match_count"), bool)
        ]
        match_counts = {row["match_count"] for row in counted}
        summary["match_count_runs"] = len(counted)
        summary["repeat_match_count_consistent"] = (
            bool(successful)
            and len(counted) == len(successful)
            and len(match_counts) <= 1
        )
        if len(counted) == len(successful) and len(match_counts) == 1:
            summary["match_count"] = next(iter(match_counts))

        digested = [
            row
            for row in successful
            if isinstance(row.get("result_digest"), str)
            and str(row["result_digest"]).strip().lower()
            not in {"", "disabled", "off", "none"}
        ]
        result_digests = {str(row["result_digest"]) for row in digested}
        summary["result_digest_runs"] = len(digested)
        summary["repeat_result_digest_consistent"] = (
            bool(successful)
            and len(digested) == len(successful)
            and len(result_digests) == 1
        )
        if len(digested) == len(successful) and len(result_digests) == 1:
            summary["result_digest"] = next(iter(result_digests))
        summaries.append(summary)
    return summaries


def correctness_rows(
    summaries: list[dict[str, Any]],
    selected_algorithms: dict[str, dict[str, Any]],
    selected_cases: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    by_case_and_semantics: dict[tuple[str, str], dict[str, dict[str, Any]]] = {}
    for row in summaries:
        key = (str(row["case_id"]), str(row.get("semantic_group", "")))
        by_case_and_semantics.setdefault(key, {})[str(row["algorithm"])] = row

    expected: dict[tuple[str, str], set[str]] = {}
    digest_required_by_case: dict[str, bool] = {}
    expected_count_by_case: dict[str, int | None] = {}
    for case in selected_cases:
        case_id = str(case["id"])
        digest_required_by_case[case_id] = bool(
            case.get("require_digest", False)
        )
        expected_count = case.get("expected_match_count")
        expected_count_by_case[case_id] = (
            int(expected_count)
            if isinstance(expected_count, int)
            and not isinstance(expected_count, bool)
            and expected_count >= 0
            else None
        )
        allowed = set(case.get("algorithms", selected_algorithms))
        for algorithm_name, algorithm in selected_algorithms.items():
            if algorithm_name not in allowed:
                continue
            semantics = str(algorithm.get("semantic_group", ""))
            expected.setdefault((case_id, semantics), set()).add(algorithm_name)

    checks: list[dict[str, Any]] = []
    for (case_id, semantics), expected_names in sorted(expected.items()):
        rows_by_algorithm = by_case_and_semantics.get((case_id, semantics), {})
        missing_algorithms = sorted(expected_names - rows_by_algorithm.keys())
        incomplete_algorithms: list[str] = []
        counts: dict[str, Any] = {}
        digests: dict[str, str] = {}
        digest_required = digest_required_by_case.get(case_id, False)
        incomplete_digest_algorithms: list[str] = []
        for algorithm_name in sorted(expected_names):
            row = rows_by_algorithm.get(algorithm_name)
            if row is None:
                continue
            all_runs_succeeded = (
                int(row.get("runs", 0)) > 0
                and int(row.get("successful_runs", 0)) == int(row.get("runs", 0))
            )
            repeats_consistent = bool(
                row.get("repeat_match_count_consistent", False)
            )
            count = row.get("match_count")
            if (
                not all_runs_succeeded
                or not repeats_consistent
                or not isinstance(count, int)
            ):
                incomplete_algorithms.append(algorithm_name)
                continue
            counts[algorithm_name] = count
            if digest_required:
                digest = row.get("result_digest")
                digest_consistent = bool(
                    row.get("repeat_result_digest_consistent", False)
                )
                if (
                    not digest_consistent
                    or not isinstance(digest, str)
                    or digest.strip().lower()
                    in {"", "disabled", "off", "none"}
                ):
                    incomplete_digest_algorithms.append(algorithm_name)
                else:
                    digests[algorithm_name] = digest

        concrete = set(counts.values())
        concrete_digests = set(digests.values())
        expected_count = expected_count_by_case.get(case_id)
        oracle_matches = (
            expected_count is None
            or (len(concrete) == 1 and next(iter(concrete)) == expected_count)
        )
        if len(concrete) > 1 or not oracle_matches:
            status = "fail"
        elif digest_required and len(concrete_digests) > 1:
            status = "fail"
        elif (
            missing_algorithms
            or incomplete_algorithms
            or incomplete_digest_algorithms
            or len(counts) != len(expected_names)
            or (digest_required and len(digests) != len(expected_names))
        ):
            status = "missing"
        elif len(concrete) == 1:
            status = "pass"
        else:
            status = "missing"

        checks.append(
            {
                "case_id": case_id,
                "semantic_group": semantics,
                "algorithms": ",".join(sorted(expected_names)),
                "expected_algorithms": ",".join(sorted(expected_names)),
                "completed_algorithms": ",".join(sorted(counts)),
                "missing_algorithms": ",".join(missing_algorithms),
                "incomplete_algorithms": ",".join(incomplete_algorithms),
                "counts": json.dumps(counts, sort_keys=True),
                "expected_match_count": expected_count,
                "oracle_status": (
                    "not_required"
                    if expected_count is None
                    else ("pass" if oracle_matches else "fail")
                ),
                "digest_required": digest_required,
                "digests": json.dumps(digests, sort_keys=True),
                "incomplete_digest_algorithms": ",".join(
                    incomplete_digest_algorithms
                ),
                "status": status,
            }
        )
    return checks


def resolve_run_command(
    algorithm: dict[str, Any],
    case: dict[str, Any],
    algorithm_name: str,
) -> tuple[Path, Path, Path, list[str]]:
    executable = Path(
        expand_path(str(algorithm["executable"]))
    ).resolve()
    data_value = case_algorithm_value(case, "data", algorithm_name)
    if data_value is None:
        raise ValueError(
            f"case is missing data for algorithm {algorithm_name}"
        )
    data_path = Path(expand_path(str(data_value))).resolve()
    query_path = Path(expand_path(str(case["query"]))).resolve()
    flags = [
        expand_path(str(flag))
        for flag in algorithm.get("flags", [])
    ]
    flags.extend(
        expand_path(str(flag))
        for flag in case.get("extra_flags", [])
    )
    algorithm_flags = case.get("algorithm_flags", {})
    flags.extend(
        expand_path(str(flag))
        for flag in algorithm_flags.get(algorithm_name, [])
    )
    command = [
        str(executable),
        str(data_path),
        str(query_path),
        str(case["k"]),
        str(case.get("label_seed", 42)),
        *flags,
    ]
    return executable, data_path, query_path, command


def case_algorithm_value(
    case: dict[str, Any],
    key: str,
    algorithm_name: str,
) -> Any:
    """Resolve an optional per-algorithm case field override.

    A benchmark case normally shares one input across every implementation.
    Paper-style filtered-graph comparisons are an intentional exception:
    baselines consume the raw graph while proposed methods consume the
    prefiltered graph.  ``<key>_by_algorithm`` makes that distinction explicit
    without splitting one correctness cohort into unrelated case IDs.
    """

    overrides = case.get(f"{key}_by_algorithm", {})
    if overrides is None:
        overrides = {}
    if not isinstance(overrides, dict):
        raise ValueError(f"{key}_by_algorithm must be an object")
    return overrides.get(algorithm_name, case.get(key))


def run_signature(
    algorithm_name: str,
    algorithm: dict[str, Any],
    case: dict[str, Any],
    repeat: int,
) -> str:
    executable, data_path, query_path, command = resolve_run_command(
        algorithm, case, algorithm_name
    )
    payload = {
        "algorithm_name": algorithm_name,
        "algorithm_spec": algorithm,
        "case_spec": case,
        "repeat": repeat,
        "command": command,
        "executable_sha256": sha256_file(executable),
        "data_sha256": sha256_file(data_path),
        "query_sha256": sha256_file(query_path),
    }
    encoded = json.dumps(
        payload,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def run_one(
    algorithm_name: str,
    algorithm: dict[str, Any],
    case: dict[str, Any],
    repeat: int,
    execution_position: int,
    output_root: Path,
    default_timeout: int,
) -> dict[str, Any]:
    case_id = str(case["id"])
    run_id = safe_id(f"{case_id}__{algorithm_name}__r{repeat}")
    run_dir = output_root / "runs" / run_id
    if run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)

    executable, data_path, query_path, command = resolve_run_command(
        algorithm, case, algorithm_name
    )

    timeout_seconds = int(case.get("timeout_seconds", default_timeout))
    time_path = run_dir / "resource_time.txt"
    stdout_path = run_dir / "stdout.txt"
    stderr_path = run_dir / "stderr.txt"
    time_format = (
        "wall_s=%e\nuser_s=%U\nsystem_s=%S\nmax_rss_kb=%M\n"
        "major_faults=%F\nminor_faults=%R\nvoluntary_ctx=%w\n"
        "involuntary_ctx=%c\nexit_code=%x"
    )
    execution_command = list(command)
    cpu_affinity = str(algorithm.get("cpu_affinity", "")).strip()
    if cpu_affinity:
        execution_command = ["taskset", "-c", cpu_affinity, *execution_command]
    wrapped = [
        "/usr/bin/time",
        "-f",
        time_format,
        "-o",
        str(time_path),
        "timeout",
        "--signal=TERM",
        "--kill-after=5s",
        f"{timeout_seconds}s",
        *execution_command,
    ]

    started = time.perf_counter()
    started_at = datetime.now(timezone.utc)
    environment = os.environ.copy()
    thread_count = configured_threads(algorithm, command)
    thread_count_declared = declared_threads(algorithm)
    thread_count_argv = argv_threads(command)
    if thread_count is not None:
        # These variables do not control the implementation's std::thread pool,
        # but they prevent an indirectly linked numerical runtime from silently
        # adding worker threads to a declared single-thread run.
        environment.update(
            {
                "OMP_NUM_THREADS": str(thread_count),
                "OMP_DYNAMIC": "FALSE",
                "OPENBLAS_NUM_THREADS": str(thread_count),
                "MKL_NUM_THREADS": str(thread_count),
                "BLIS_NUM_THREADS": str(thread_count),
                "VECLIB_MAXIMUM_THREADS": str(thread_count),
                "NUMEXPR_NUM_THREADS": str(thread_count),
            }
        )
    environment.update(
        {str(key): expand_path(str(value)) for key, value in algorithm.get("env", {}).items()}
    )
    try:
        completed = subprocess.run(
            wrapped,
            cwd=run_dir,
            text=True,
            capture_output=True,
            env=environment,
            check=False,
        )
        exit_code = completed.returncode
        stdout = completed.stdout
        stderr = completed.stderr
    except OSError as error:
        exit_code = 127
        stdout = ""
        stderr = str(error)
    wall_observed = time.perf_counter() - started
    ended_at = datetime.now(timezone.utc)
    stdout_path.write_text(stdout, encoding="utf-8")
    stderr_path.write_text(stderr, encoding="utf-8")

    killed_after_timeout = (
        exit_code == 137 and wall_observed >= float(timeout_seconds)
    )
    status = (
        "ok"
        if exit_code == 0
        else "timeout"
        if exit_code == 124 or killed_after_timeout
        else "error"
    )
    record: dict[str, Any] = {
        "case_id": case_id,
        "algorithm": algorithm_name,
        "semantic_group": str(algorithm.get("semantic_group", "")),
        "repeat": repeat,
        "execution_position": execution_position,
        "status": status,
        "exit_code": exit_code,
        "timeout_seconds": timeout_seconds,
        "started_at_utc": started_at.isoformat(),
        "ended_at_utc": ended_at.isoformat(),
        "wall_observed_s": wall_observed,
        "wall_s": wall_observed,
        "data": str(data_path),
        "data_sha256": sha256_file(data_path),
        "query": str(query_path),
        "query_sha256": sha256_file(query_path),
        "executable_sha256": sha256_file(executable),
        "k": int(case["k"]),
        "label_seed": int(case.get("label_seed", 42)),
        "backend": str(algorithm.get("backend", "cpu")),
        "device": str(algorithm.get("device", "")),
        "threads": thread_count,
        "threads_declared": thread_count_declared,
        "threads_configured_argv": thread_count_argv,
        "threads_config_valid": True,
        "algorithm_role": str(algorithm.get("role", "")),
        "implementation_generation": str(
            algorithm.get("implementation_generation", "")
        ),
        "comparison_family": str(algorithm.get("comparison_family", "")),
        "cpu_affinity": cpu_affinity,
        "order_config": command_option(command, "--order"),
        "selectivity_config": command_option(command, "--selectivity"),
        "memo_config": command_option(command, "--memo"),
        "statistics_config": command_option(command, "--statistics"),
        "prefilter_config": command_option(command, "--prefilter"),
        "label_mode_config": command_option(command, "--label-mode"),
        "early_prune_config": command_option(command, "--early-prune"),
        "parallel_config": command_option(command, "--parallel"),
        "gpu_variant_config": command_option(command, "--gpu-variant"),
        "suite": str(case.get("suite", "")),
        "axes": json.dumps(case.get("axes", []), ensure_ascii=False),
        "dataset_id": str(case.get("dataset_id", "")),
        "input_variant": str(
            case_algorithm_value(
                case, "input_variant", algorithm_name
            )
            or ""
        ),
        "query_shape": str(case.get("query_shape", "")),
        "query_edges": case.get("query_edges"),
        "command": json.dumps(command),
        "run_signature": run_signature(
            algorithm_name, algorithm, case, repeat
        ),
        "run_dir": str(run_dir),
    }
    gpu_memory_mb = command_option(command, "--gpu-memory-mb")
    if gpu_memory_mb is not None:
        parsed_gpu_memory_mb = parse_scalar(gpu_memory_mb)
        record["gpu_memory_mb_config"] = parsed_gpu_memory_mb
        if (
            isinstance(parsed_gpu_memory_mb, int)
            and not isinstance(parsed_gpu_memory_mb, bool)
            and parsed_gpu_memory_mb > 0
        ):
            record["gpu_memory_bytes_config"] = (
                parsed_gpu_memory_mb * 1024 * 1024
            )
    record.update(factor_fields(algorithm))
    record.update(read_time_metrics(time_path))
    record.update(parse_program_outputs(run_dir, stdout))
    return record


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--algorithm", action="append", dest="algorithms")
    parser.add_argument("--case", action="append", dest="cases")
    parser.add_argument("--repeats", type=int)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument(
        "--list",
        action="store_true",
        help="validate and list the selected matrix without executing it",
    )
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if args.algorithms:
        unknown_algorithms = sorted(
            set(args.algorithms) - set(manifest["algorithms"])
        )
        if unknown_algorithms:
            parser.error(
                "unknown algorithm(s): " + ", ".join(unknown_algorithms)
            )
    if args.cases:
        known_cases = {str(case["id"]) for case in manifest["cases"]}
        unknown_cases = sorted(set(args.cases) - known_cases)
        if unknown_cases:
            parser.error("unknown case(s): " + ", ".join(unknown_cases))
    selected_algorithms = {
        name: spec
        for name, spec in manifest["algorithms"].items()
        if spec.get("enabled", True)
        and (not args.algorithms or name in set(args.algorithms))
    }
    selected_cases = [
        case
        for case in manifest["cases"]
        if case.get("enabled", True)
        and (not args.cases or str(case["id"]) in set(args.cases))
    ]
    default_repeats = int(
        args.repeats if args.repeats is not None else manifest.get("repeats", 3)
    )
    default_timeout = int(manifest.get("timeout_seconds", 600))
    if default_repeats < 1:
        parser.error("repeats must be at least 1")

    if args.list:
        problems: list[str] = []
        planned_runs = 0
        for algorithm_name, algorithm in selected_algorithms.items():
            executable = Path(expand_path(str(algorithm["executable"])))
            state = "ok" if executable.exists() else "missing"
            print(f"algorithm\t{algorithm_name}\t{state}\t{executable}")
            if state == "missing":
                problems.append(f"missing executable: {executable}")
            try:
                factor_fields(algorithm)
                declared_threads(algorithm)
            except ValueError as error:
                problems.append(f"{algorithm_name}: {error}")
        for case in selected_cases:
            allowed = set(case.get("algorithms", selected_algorithms))
            unknown = sorted(allowed - manifest["algorithms"].keys())
            if unknown:
                problems.append(
                    f"{case['id']}: unknown or disabled algorithms: {','.join(unknown)}"
                )
            case_algorithms = allowed & selected_algorithms.keys()
            for algorithm_name in sorted(case_algorithms):
                algorithm = selected_algorithms[algorithm_name]
                try:
                    _, _, _, command = resolve_run_command(
                        algorithm, case, algorithm_name
                    )
                    configured_threads(algorithm, command)
                except ValueError as error:
                    problems.append(
                        f"{case['id']}/{algorithm_name}: {error}"
                    )
            repeats = int(
                args.repeats
                if args.repeats is not None
                else case.get("repeats", default_repeats)
            )
            if repeats < 1:
                problems.append(f"{case['id']}: repeats must be at least 1")
            query_path = Path(expand_path(str(case["query"])))
            for algorithm_name in sorted(case_algorithms):
                try:
                    data_value = case_algorithm_value(
                        case, "data", algorithm_name
                    )
                except ValueError as error:
                    problems.append(
                        f"{case['id']}/{algorithm_name}: {error}"
                    )
                    continue
                if data_value is None:
                    problems.append(
                        f"{case['id']}/{algorithm_name}: missing data field"
                    )
                    continue
                data_path = Path(expand_path(str(data_value)))
                if not data_path.exists():
                    problems.append(
                        f"{case['id']}/{algorithm_name}: "
                        f"missing data: {data_path}"
                    )
            if not query_path.exists():
                problems.append(f"{case['id']}: missing query: {query_path}")
            planned_runs += repeats * len(case_algorithms)
            print(
                f"case\t{case['id']}\talgorithms={len(case_algorithms)}"
                f"\trepeats={repeats}\truns={repeats * len(case_algorithms)}"
            )
        print(
            f"Selected {len(selected_cases)} cases, "
            f"{len(selected_algorithms)} algorithms, {planned_runs} runs."
        )
        for problem in problems:
            print(f"ERROR\t{problem}", file=sys.stderr)
        return 2 if problems else 0

    if args.output is None:
        parser.error("--output is required unless --list is used")
    output_root = args.output.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    (output_root / "runs").mkdir(exist_ok=True)
    (output_root / "manifest.used.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    (output_root / "manifest.used.sha256").write_text(
        sha256_file(args.manifest.resolve()) + "\n", encoding="ascii"
    )
    for sidecar_name in REPORT_SIDECARS:
        source = ROOT / "evaluation" / sidecar_name
        destination = output_root / sidecar_name
        if source.exists() and source.resolve() != destination.resolve():
            shutil.copy2(source, destination)

    raw_path = output_root / "raw_runs.jsonl"
    existing: dict[tuple[str, str, int], dict[str, Any]] = {}
    if args.resume and raw_path.exists():
        case_by_id = {
            str(case["id"]): case for case in selected_cases
        }
        for line in raw_path.read_text(encoding="utf-8").splitlines():
            row = json.loads(line)
            identity = (
                str(row["case_id"]),
                str(row["algorithm"]),
                int(row["repeat"]),
            )
            case = case_by_id.get(identity[0])
            algorithm = selected_algorithms.get(identity[1])
            selected_identity = (
                case is not None
                and algorithm is not None
                and identity[1]
                in set(case.get("algorithms", selected_algorithms))
            )
            if selected_identity:
                expected_signature = run_signature(
                    identity[1], algorithm, case, identity[2]
                )
                if row.get("run_signature") != expected_signature:
                    print(
                        "Stale resume identity will be rerun: "
                        f"{identity[0]}/{identity[1]}/r{identity[2]}",
                        file=sys.stderr,
                    )
                    continue
            existing[identity] = row
        # Canonicalize the resume journal so stale or duplicate identities
        # cannot survive into validation after their replacements are appended.
        raw_path.write_text(
            "".join(
                json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n"
                for _, row in sorted(existing.items())
            ),
            encoding="utf-8",
        )

    rows = list(existing.values())
    with raw_path.open("a" if args.resume else "w", encoding="utf-8") as raw_output:
        for case in selected_cases:
            allowed = set(case.get("algorithms", selected_algorithms))
            repeats = int(
                args.repeats
                if args.repeats is not None
                else case.get("repeats", default_repeats)
            )
            case_algorithms = [
                (name, spec)
                for name, spec in selected_algorithms.items()
                if name in allowed
            ]
            for algorithm_name, algorithm in case_algorithms:
                executable = Path(expand_path(str(algorithm["executable"])))
                if not executable.exists():
                    print(f"Missing executable: {executable}", file=sys.stderr)
                    return 2
                try:
                    _, _, _, command = resolve_run_command(
                        algorithm, case, algorithm_name
                    )
                    configured_threads(algorithm, command)
                    factor_fields(algorithm)
                except ValueError as error:
                    print(
                        f"Invalid configuration for "
                        f"{case['id']}/{algorithm_name}: {error}",
                        file=sys.stderr,
                    )
                    return 2

            # Interleave algorithms by repeat and rotate the first position.
            # This prevents every repeat of one implementation from running
            # before the others and spreads unavoidable first-read cache bias.
            if case_algorithms:
                stable_offset = (
                    int.from_bytes(
                        hashlib.sha256(str(case["id"]).encode("utf-8")).digest()[:8],
                        "big",
                    )
                    % len(case_algorithms)
                )
            else:
                stable_offset = 0
            for repeat in range(1, repeats + 1):
                rotation = (stable_offset + repeat - 1) % max(
                    1, len(case_algorithms)
                )
                ordered_algorithms = (
                    case_algorithms[rotation:] + case_algorithms[:rotation]
                )
                for execution_position, (algorithm_name, algorithm) in enumerate(
                    ordered_algorithms, start=1
                ):
                    identity = (str(case["id"]), algorithm_name, repeat)
                    if identity in existing:
                        continue
                    print(
                        f"[{case['id']}] {algorithm_name} repeat {repeat}/{repeats}",
                        flush=True,
                    )
                    row = run_one(
                        algorithm_name,
                        algorithm,
                        case,
                        repeat,
                        execution_position,
                        output_root,
                        default_timeout,
                    )
                    rows.append(row)
                    raw_output.write(
                        json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n"
                    )
                    raw_output.flush()

    rows.sort(key=lambda row: (str(row["case_id"]), str(row["algorithm"]), int(row["repeat"])))
    write_csv(output_root / "runs.csv", rows)
    summaries = summarize(rows)
    write_csv(output_root / "summary.csv", summaries)
    write_csv(output_root / "gpu_depths.csv", gpu_depth_rows(rows))
    write_csv(output_root / "candidate_rows.csv", candidate_rows(rows))
    write_csv(output_root / "selectivity_rows.csv", selectivity_rows(rows))
    checks = correctness_rows(summaries, selected_algorithms, selected_cases)
    write_csv(output_root / "correctness.csv", checks)

    failed_checks = [row for row in checks if row["status"] == "fail"]
    errors = [row for row in rows if row["status"] == "error"]
    print(
        f"Completed {len(rows)} runs; {len(errors)} errors, "
        f"{len(failed_checks)} correctness failures. Results: {output_root}"
    )
    return 3 if failed_checks else 0


if __name__ == "__main__":
    raise SystemExit(main())
