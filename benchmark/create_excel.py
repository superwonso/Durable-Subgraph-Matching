#!/usr/bin/env python3
"""Build and validate an Excel benchmark report from runner CSV outputs.

The script intentionally avoids pandas so that the only non-standard runtime
dependency is openpyxl.  Source CSV columns are retained verbatim in the raw
and summary worksheets; normalized, derived worksheets are built from a small
set of documented aliases.
"""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import statistics
import sys
from typing import Any, Iterable, Sequence

try:
    from openpyxl import Workbook, load_workbook
    from openpyxl.chart import BarChart, LineChart, Reference
    from openpyxl.formatting.rule import CellIsRule, ColorScaleRule, FormulaRule
    from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
    from openpyxl.utils import get_column_letter
except ImportError as error:  # pragma: no cover - exercised only without deps
    raise SystemExit(
        "openpyxl is required. Install benchmark/requirements.txt before "
        "running benchmark/create_excel.py."
    ) from error


SHEET_NAMES = (
    "Overview",
    "Environment & GPU",
    "Datasets",
    "Correctness",
    "Raw Runs",
    "Summary",
    "Speedup",
    "Query Size",
    "K Sensitivity",
    "Gopher Ablation",
    "CPU Scaling",
    "CPU Single Thread",
    "GPU Evaluation",
    "Parallel Architecture",
    "GPU Depths",
    "Methodology & Limits",
)

REQUIRED_INPUTS = ("runs.csv", "summary.csv", "correctness.csv")
OPTIONAL_INPUTS = (
    "environment.json",
    "dataset_inventory.csv",
    "methodology.csv",
    "gpu_depths.csv",
)

IDENTITY_HINTS = {
    "algorithm",
    "case_id",
    "command",
    "counts",
    "data",
    "dataset",
    "dfs_order",
    "digest",
    "id",
    "mode",
    "path",
    "query",
    "run_dir",
    "semantic_group",
    "status",
}

RUNTIME_SUMMARY_ALIASES = (
    "wall_s_median",
    "resource_wall_s_median",
    "wall_observed_s_median",
    "total_time_s_median",
    "total_time_ms_median",
    "total_runtime_s_median",
    "total_runtime_ms_median",
    "end_to_end_s_median",
    "end_to_end_ms_median",
    "runtime_s_median",
    "runtime_ms_median",
)

RUNTIME_RUN_ALIASES = (
    "wall_s",
    "resource_wall_s",
    "wall_observed_s",
    "total_time_s",
    "total_time_ms",
    "total_runtime_s",
    "total_runtime_ms",
    "end_to_end_s",
    "end_to_end_ms",
    "runtime_s",
    "runtime_ms",
)

BLUE = "2F5597"
BLUE_LIGHT = "D9EAF7"
BLUE_XLIGHT = "EDF4FB"
GOLD = "C69214"
ORANGE = "D97706"
INK = "243447"
GREY = "697386"
GRID = "D9E1E8"
GREEN_FILL = "E2F0D9"
GREEN_FONT = "375623"
RED_FILL = "FCE4D6"
RED_FONT = "9C0006"
AMBER_FILL = "FFF2CC"
AMBER_FONT = "7F6000"
WHITE = "FFFFFF"

HEADER_FILL = PatternFill("solid", fgColor=BLUE)
HEADER_FONT = Font(color=WHITE, bold=True)
TITLE_FONT = Font(color=INK, bold=True, size=18)
SECTION_FONT = Font(color=INK, bold=True, size=12)
THIN_BOTTOM = Border(bottom=Side(style="thin", color=GRID))


def normalized_name(value: Any) -> str:
    return re.sub(r"[^a-z0-9]+", "_", str(value).strip().lower()).strip("_")


def is_number(value: Any) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
    )


def parse_csv_value(header: str, raw: str | None) -> Any:
    if raw is None:
        return None
    value = raw.strip()
    if value == "":
        return None
    header_key = normalized_name(header)
    if any(
        header_key == hint
        or header_key.startswith(f"{hint}_")
        or header_key.endswith(f"_{hint}")
        or f"_{hint}_" in header_key
        for hint in IDENTITY_HINTS
    ):
        return value
    lowered = value.lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    if re.fullmatch(r"[-+]?\d+", value):
        try:
            return int(value)
        except ValueError:
            pass
    if re.fullmatch(
        r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?", value
    ):
        try:
            parsed = float(value)
            return parsed if math.isfinite(parsed) else value
        except ValueError:
            pass
    return value


def read_csv_file(path: Path) -> tuple[list[str], list[dict[str, Any]]]:
    with path.open(newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames:
            raise ValueError(f"{path} has no CSV header")
        headers = [str(header) for header in reader.fieldnames]
        rows = [
            {
                header: parse_csv_value(header, raw_row.get(header))
                for header in headers
            }
            for raw_row in reader
        ]
    return headers, rows


def flattened_json_rows(value: Any, prefix: str = "") -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if isinstance(value, dict):
        for key, child in value.items():
            child_prefix = f"{prefix}.{key}" if prefix else str(key)
            rows.extend(flattened_json_rows(child, child_prefix))
    elif isinstance(value, list):
        rows.append(
            {
                "Scope": classify_environment_scope(prefix),
                "Key": prefix,
                "Value": json.dumps(value, ensure_ascii=False, sort_keys=True),
            }
        )
    else:
        rows.append(
            {
                "Scope": classify_environment_scope(prefix),
                "Key": prefix,
                "Value": value,
            }
        )
    return rows


def classify_environment_scope(key: str) -> str:
    lowered = key.lower()
    if any(token in lowered for token in ("gpu", "rocm", "hip", "device", "gfx")):
        return "GPU / accelerator"
    if any(token in lowered for token in ("cpu", "processor", "thread", "core")):
        return "CPU"
    if any(token in lowered for token in ("compiler", "python", "os", "kernel", "git")):
        return "Software"
    return "Other"


def row_lookup(row: dict[str, Any]) -> dict[str, Any]:
    return {normalized_name(key): value for key, value in row.items()}


def pick(row: dict[str, Any], *aliases: str) -> Any:
    lookup = row_lookup(row)
    for alias in aliases:
        key = normalized_name(alias)
        if key in lookup and lookup[key] not in (None, ""):
            return lookup[key]
    return None


def pick_numeric(row: dict[str, Any], *aliases: str) -> float | int | None:
    value = pick(row, *aliases)
    return value if is_number(value) else None


def runtime_scale(header: str) -> float:
    key = normalized_name(header)
    if re.search(r"(?:^|_)ns(?:_|$)", key):
        return 1.0e-9
    if re.search(r"(?:^|_)us(?:_|$)", key):
        return 1.0e-6
    if re.search(r"(?:^|_)ms(?:_|$)", key):
        return 1.0e-3
    return 1.0


def runtime_seconds(
    row: dict[str, Any],
    *,
    summary: bool,
    requested_column: str | None = None,
) -> tuple[float | None, str | None]:
    lookup = row_lookup(row)
    if requested_column:
        requested = normalized_name(requested_column)
        requested_candidates = [requested]
        if summary and not requested.endswith("_median"):
            requested_candidates.insert(0, f"{requested}_median")
        if not summary and requested.endswith("_median"):
            requested_candidates.insert(0, requested.removesuffix("_median"))
        for candidate in requested_candidates:
            value = lookup.get(candidate)
            if is_number(value):
                return (
                    float(value) * runtime_scale(candidate),
                    candidate,
                )
        return None, requested_column

    aliases = RUNTIME_SUMMARY_ALIASES if summary else RUNTIME_RUN_ALIASES
    for alias in aliases:
        value = lookup.get(alias)
        if is_number(value):
            return float(value) * runtime_scale(alias), alias

    suffix = "_median" if summary else ""
    candidates: list[tuple[int, str, float]] = []
    for key, value in lookup.items():
        if not is_number(value) or (suffix and not key.endswith(suffix)):
            continue
        if not any(token in key for token in ("wall", "runtime", "total_time", "end_to_end")):
            continue
        priority = 0 if "wall" in key else 1
        candidates.append((priority, key, float(value) * runtime_scale(key)))
    if candidates:
        _, key, value = sorted(candidates)[0]
        return value, key
    return None, None


def basename_without_suffix(value: Any) -> str | None:
    if value in (None, ""):
        return None
    name = str(value).replace("\\", "/").rsplit("/", 1)[-1]
    while True:
        stem, suffix = os.path.splitext(name)
        if suffix.lower() in {".txt", ".csv", ".qry", ".json", ".graph"}:
            name = stem
        else:
            break
    return name or None


def infer_integer(text: str, patterns: Sequence[str]) -> int | None:
    for pattern in patterns:
        match = re.search(pattern, text, re.IGNORECASE)
        if match:
            return int(match.group(1))
    return None


def infer_query_edges(row: dict[str, Any], case_id: str, query: str) -> int | None:
    explicit = pick_numeric(
        row,
        "query_edges",
        "query_edge_count",
        "pattern_edges",
        "query_size",
        "num_query_edges",
    )
    if explicit is not None:
        return int(explicit)
    return infer_integer(
        f"{query} {case_id}",
        (
            r"(?:path|cycle|tailed|star|query|q)[_-]?(\d+)",
            r"(?:^|[_-])e(\d+)(?:[_-]|$)",
        ),
    )


def infer_k(row: dict[str, Any], case_id: str) -> int | None:
    explicit = pick_numeric(
        row,
        "k",
        "k_median",
        "duration_k",
        "minimum_duration",
        "minimum_duration_k",
        "durability",
    )
    if explicit is not None:
        return int(explicit)
    return infer_integer(case_id, (r"(?:^|[_-])k(\d+)(?:[_-]|$)",))


def infer_threads(row: dict[str, Any], algorithm: str, case_id: str) -> int | None:
    explicit = pick_numeric(
        row,
        "threads",
        "thread_count",
        "num_threads",
        "cpu_threads",
        "threads_requested",
        "gopher_threads_requested",
        "result_threads_requested",
        "threads_used",
        "gopher_threads_used",
        "result_threads_used",
        "threads_median",
        "thread_count_median",
        "threads_requested_median",
        "gopher_threads_requested_median",
        "result_threads_requested_median",
        "threads_used_median",
        "gopher_threads_used_median",
        "result_threads_used_median",
    )
    if explicit is not None:
        return int(explicit)
    command = str(pick(row, "command") or "")
    joined = f"{algorithm} {case_id} {command}"
    inferred = infer_integer(
        joined,
        (
            r"--threads(?:=|\s+)(\d+)",
            r"(?:^|[_-])threads?[_-]?(\d+)(?:[_-]|$)",
            r"(?:^|[_-])t(\d+)(?:[_-]|$)",
        ),
    )
    if inferred is not None:
        return inferred
    lowered = algorithm.lower()
    # The TD-Tree and non-Gopher ours executables have no worker-pool option
    # and execute the matcher on the calling thread.  Keep this compatibility
    # inference for benchmark CSVs produced before the runner emitted an
    # explicit threads column.
    if "gopher" not in lowered and (
        lowered == "ours"
        or "original" in lowered
        or "td-tree" in lowered
        or "td_tree" in lowered
        or "tdtree" in lowered
    ):
        return 1
    return None


def infer_backend(row: dict[str, Any], algorithm: str) -> str:
    explicit = pick(row, "backend", "execution_backend", "accelerator")
    if explicit not in (None, ""):
        return str(explicit).strip().lower()
    lowered = algorithm.lower()
    if "gpu" in lowered or "rocm" in lowered or "hip" in lowered:
        return "gpu"
    return "cpu"


def base_algorithm_for_scaling(algorithm: str) -> str:
    return re.sub(
        r"(?:[_-](?:threads?|t)[_-]?\d+)$",
        "",
        algorithm,
        flags=re.IGNORECASE,
    )


def infer_ablation(row: dict[str, Any], algorithm: str) -> str:
    explicit = pick(row, "ablation", "variant", "configuration")
    if explicit not in (None, ""):
        return str(explicit)
    lowered = algorithm.lower()
    for prefix in ("ours_gopher_", "ours-gopher-", "gopher_"):
        if lowered.startswith(prefix):
            suffix = algorithm[len(prefix) :]
            return suffix or "full"
    return "full" if "gopher" in lowered else ""


def case_metadata(row: dict[str, Any]) -> dict[str, Any]:
    case_id = str(pick(row, "case_id", "case") or "")
    algorithm = str(pick(row, "algorithm", "method") or "")
    data_value = pick(row, "dataset", "dataset_name", "data")
    query_value = pick(row, "query_name", "pattern", "query")
    dataset = (
        str(pick(row, "dataset", "dataset_name"))
        if pick(row, "dataset", "dataset_name") not in (None, "")
        else basename_without_suffix(data_value)
    )
    query = (
        str(pick(row, "query_name", "pattern"))
        if pick(row, "query_name", "pattern") not in (None, "")
        else basename_without_suffix(query_value)
    )
    query = query or case_id
    backend = infer_backend(row, algorithm)
    return {
        "case_id": case_id,
        "algorithm": algorithm,
        "semantic_group": str(pick(row, "semantic_group", "semantics") or ""),
        "dataset": dataset or "unknown",
        "query": query,
        "query_edges": infer_query_edges(row, case_id, query),
        "k": infer_k(row, case_id),
        "threads": infer_threads(row, algorithm, case_id),
        "backend": backend,
        "device": str(pick(row, "device", "gpu_device") or ""),
        "algorithm_role": str(pick(row, "algorithm_role", "role") or ""),
        "suite": str(pick(row, "suite") or ""),
        "input_variant": str(pick(row, "input_variant") or ""),
        "query_shape": str(pick(row, "query_shape") or ""),
        "ablation": infer_ablation(row, algorithm),
    }


def unique_hint(values: Iterable[Any]) -> Any:
    concrete = {value for value in values if value not in (None, "")}
    return next(iter(concrete)) if len(concrete) == 1 else None


def build_run_hints(
    runs: list[dict[str, Any]],
) -> dict[tuple[str, str, str], dict[str, Any]]:
    grouped: dict[tuple[str, str, str], list[dict[str, Any]]] = {}
    for row in runs:
        metadata = case_metadata(row)
        key = (
            metadata["case_id"],
            metadata["algorithm"],
            metadata["semantic_group"],
        )
        grouped.setdefault(key, []).append(metadata)
    hints: dict[tuple[str, str, str], dict[str, Any]] = {}
    for key, entries in grouped.items():
        hints[key] = {
            field: unique_hint(entry.get(field) for entry in entries)
            for field in (
                "dataset",
                "query",
                "query_edges",
                "k",
                "threads",
                "backend",
                "device",
                "algorithm_role",
                "suite",
                "input_variant",
                "query_shape",
                "ablation",
            )
        }
    return hints


def enrich_summaries(
    summaries: list[dict[str, Any]],
    runs: list[dict[str, Any]],
    runtime_column: str | None,
) -> list[dict[str, Any]]:
    hints = build_run_hints(runs)
    enriched: list[dict[str, Any]] = []
    for index, row in enumerate(summaries, start=2):
        metadata = case_metadata(row)
        key = (
            metadata["case_id"],
            metadata["algorithm"],
            metadata["semantic_group"],
        )
        hint = hints.get(key, {})
        for field, value in hint.items():
            if metadata.get(field) in (None, "", "unknown"):
                metadata[field] = value
        runtime, runtime_source = runtime_seconds(
            row, summary=True, requested_column=runtime_column
        )
        enriched.append(
            {
                **metadata,
                "runtime_s": runtime,
                "runtime_source": runtime_source,
                "successful_runs": pick_numeric(row, "successful_runs"),
                "match_count": pick_numeric(row, "match_count"),
                "repeat_match_count_consistent": pick(
                    row, "repeat_match_count_consistent"
                ),
                "summary_row": index,
                "source": row,
            }
        )
    return enriched


def union_headers(
    rows: list[dict[str, Any]], preferred: Sequence[str] = ()
) -> list[str]:
    seen: list[str] = []
    for row in rows:
        for key in row:
            if key not in seen:
                seen.append(key)
    return [key for key in preferred if key in seen] + [
        key for key in seen if key not in preferred
    ]


def safe_excel_text(value: str) -> str:
    value = value[:32767]
    if value and value[0] in ("=", "+", "-", "@"):
        return "'" + value
    return value


def write_value(cell: Any, value: Any, header: str) -> None:
    if isinstance(value, str):
        cell.value = safe_excel_text(value)
    elif isinstance(value, (int, float, bool)) or value is None:
        cell.value = value
    else:
        cell.value = safe_excel_text(
            json.dumps(value, ensure_ascii=False, sort_keys=True)
        )
    if is_number(value):
        cell.number_format = number_format_for(header, float(value))
        cell.alignment = Alignment(horizontal="right")
    else:
        cell.alignment = Alignment(vertical="top", wrap_text=True)


def number_format_for(header: str, value: float | None = None) -> str:
    key = normalized_name(header)
    if key in {"speedup", "speedup_x", "scaling"} or "speedup" in key:
        return '0.00"×"'
    if any(token in key for token in ("rate", "ratio", "percent", "pct", "reduction")):
        if value is not None and abs(value) > 1.0:
            return '0.0"%"'
        return "0.0%"
    if re.search(r"(?:^|_)s(?:_|$)", key) or key.endswith("_seconds"):
        return '0.000" s"'
    if re.search(r"(?:^|_)ms(?:_|$)", key):
        return '0.000" ms"'
    if re.search(r"(?:^|_)us(?:_|$)", key):
        return '0.000" µs"'
    if any(token in key for token in ("bytes", "_kb", "_mb", "_gb")):
        return "#,##0"
    if any(
        token in key
        for token in (
            "count",
            "runs",
            "timeout",
            "errors",
            "entries",
            "nodes",
            "edges",
            "vertices",
            "occurrences",
            "calls",
            "candidates",
            "hits",
            "misses",
            "threads",
        )
    ):
        return "#,##0"
    return "#,##0.000" if value is not None and value != int(value) else "#,##0"


def style_header(ws: Any, row: int, start_col: int, end_col: int) -> None:
    for column in range(start_col, end_col + 1):
        cell = ws.cell(row=row, column=column)
        cell.fill = HEADER_FILL
        cell.font = HEADER_FONT
        cell.alignment = Alignment(vertical="center", wrap_text=True)
        cell.border = THIN_BOTTOM
    ws.row_dimensions[row].height = 30


def adjust_widths(
    ws: Any, start_col: int = 1, end_col: int | None = None, max_width: int = 48
) -> None:
    end_col = end_col or ws.max_column
    for column in range(start_col, end_col + 1):
        letter = get_column_letter(column)
        width = 10
        for cell in ws[letter]:
            if cell.value is not None:
                width = max(width, min(max_width, len(str(cell.value)) + 2))
        ws.column_dimensions[letter].width = width


def write_table(
    ws: Any,
    headers: Sequence[str],
    rows: Sequence[dict[str, Any]],
    *,
    freeze: str = "B2",
    empty_note: str | None = None,
) -> None:
    for column, header in enumerate(headers, start=1):
        ws.cell(row=1, column=column, value=safe_excel_text(str(header)))
    style_header(ws, 1, 1, max(1, len(headers)))
    for row_index, row in enumerate(rows, start=2):
        for column, header in enumerate(headers, start=1):
            write_value(ws.cell(row=row_index, column=column), row.get(header), header)
    if not rows and empty_note:
        ws.cell(row=2, column=1, value=empty_note)
        ws.cell(row=2, column=1).font = Font(color=GREY, italic=True)
    last_row = max(1, 1 + len(rows))
    last_column = max(1, len(headers))
    ws.auto_filter.ref = (
        f"A1:{get_column_letter(last_column)}{last_row}"
    )
    ws.freeze_panes = freeze
    ws.sheet_view.showGridLines = False
    adjust_widths(ws, 1, last_column)


def header_column(headers: Sequence[str], *aliases: str) -> int | None:
    lookup = {normalized_name(header): index for index, header in enumerate(headers, 1)}
    for alias in aliases:
        column = lookup.get(normalized_name(alias))
        if column is not None:
            return column
    return None


def add_status_formatting(
    ws: Any, headers: Sequence[str], aliases: Sequence[str], last_row: int
) -> None:
    if last_row < 2:
        return
    for alias in aliases:
        column = header_column(headers, alias)
        if column is None:
            continue
        letter = get_column_letter(column)
        cell_range = f"{letter}2:{letter}{last_row}"
        ws.conditional_formatting.add(
            cell_range,
            FormulaRule(
                formula=[f'OR(LOWER(${letter}2)="pass",LOWER(${letter}2)="ok",LOWER(${letter}2)="valid")'],
                fill=PatternFill("solid", fgColor=GREEN_FILL),
                font=Font(color=GREEN_FONT),
            ),
        )
        ws.conditional_formatting.add(
            cell_range,
            FormulaRule(
                formula=[f'OR(LOWER(${letter}2)="fail",LOWER(${letter}2)="error",LOWER(${letter}2)="invalid")'],
                fill=PatternFill("solid", fgColor=RED_FILL),
                font=Font(color=RED_FONT),
            ),
        )
        ws.conditional_formatting.add(
            cell_range,
            FormulaRule(
                formula=[f'OR(LOWER(${letter}2)="missing",LOWER(${letter}2)="timeout",LOWER(${letter}2)="not comparable")'],
                fill=PatternFill("solid", fgColor=AMBER_FILL),
                font=Font(color=AMBER_FONT),
            ),
        )


def add_positive_number_warning(
    ws: Any, headers: Sequence[str], aliases: Sequence[str], last_row: int
) -> None:
    if last_row < 2:
        return
    for alias in aliases:
        column = header_column(headers, alias)
        if column is None:
            continue
        letter = get_column_letter(column)
        ws.conditional_formatting.add(
            f"{letter}2:{letter}{last_row}",
            CellIsRule(
                operator="greaterThan",
                formula=["0"],
                fill=PatternFill("solid", fgColor=RED_FILL),
                font=Font(color=RED_FONT),
            ),
        )


def add_false_warning(
    ws: Any, headers: Sequence[str], aliases: Sequence[str], last_row: int
) -> None:
    if last_row < 2:
        return
    for alias in aliases:
        column = header_column(headers, alias)
        if column is None:
            continue
        letter = get_column_letter(column)
        ws.conditional_formatting.add(
            f"{letter}2:{letter}{last_row}",
            FormulaRule(
                formula=[
                    f"AND(${letter}2<>\"\",OR(${letter}2=FALSE,"
                    f"LOWER(${letter}2&\"\")=\"false\"))"
                ],
                fill=PatternFill("solid", fgColor=RED_FILL),
                font=Font(color=RED_FONT),
            ),
        )


def add_true_warning(
    ws: Any, headers: Sequence[str], aliases: Sequence[str], last_row: int
) -> None:
    if last_row < 2:
        return
    for alias in aliases:
        column = header_column(headers, alias)
        if column is None:
            continue
        letter = get_column_letter(column)
        ws.conditional_formatting.add(
            f"{letter}2:{letter}{last_row}",
            FormulaRule(
                formula=[
                    f'OR(${letter}2=TRUE,LOWER(${letter}2&"")="true")'
                ],
                fill=PatternFill("solid", fgColor=RED_FILL),
                font=Font(color=RED_FONT),
            ),
        )


def prioritize_algorithms(algorithms: Iterable[str], limit: int = 8) -> list[str]:
    unique = sorted(set(algorithms))

    def priority(name: str) -> tuple[int, str]:
        lowered = name.lower()
        if "original" in lowered or "td_tree" in lowered or "td-tree" in lowered:
            return (0, lowered)
        if lowered == "ours":
            return (1, lowered)
        if lowered == "ours_gopher":
            return (2, lowered)
        if "gopher" in lowered:
            return (3, lowered)
        return (4, lowered)

    return sorted(unique, key=priority)[:limit]


def add_median_pivot(
    ws: Any,
    rows: Sequence[dict[str, Any]],
    *,
    x_key: str,
    series_key: str,
    value_key: str,
    section_title: str,
    x_title: str,
    y_title: str,
    start_col: int,
    start_row: int = 1,
    chart_anchor: str | None = None,
    force_bar: bool = False,
    series_limit: int = 8,
) -> int:
    usable = [
        row
        for row in rows
        if row.get(x_key) not in (None, "")
        and row.get(series_key) not in (None, "")
        and is_number(row.get(value_key))
    ]
    if not usable:
        ws.cell(row=start_row, column=start_col, value=section_title)
        ws.cell(row=start_row, column=start_col).font = SECTION_FONT
        ws.cell(
            row=start_row + 1,
            column=start_col,
            value="No chartable observations for this dimension.",
        ).font = Font(color=GREY, italic=True)
        return 0

    x_values = sorted(
        {row[x_key] for row in usable},
        key=lambda value: (not is_number(value), value),
    )
    series = prioritize_algorithms(
        (str(row[series_key]) for row in usable),
        limit=series_limit,
    )
    groups: dict[tuple[Any, str], list[float]] = {}
    for row in usable:
        key = (row[x_key], str(row[series_key]))
        groups.setdefault(key, []).append(float(row[value_key]))

    ws.cell(row=start_row, column=start_col, value=section_title)
    ws.cell(row=start_row, column=start_col).font = SECTION_FONT
    header_row = start_row + 1
    ws.cell(row=header_row, column=start_col, value=x_title)
    for offset, name in enumerate(series, start=1):
        ws.cell(row=header_row, column=start_col + offset, value=name)
    style_header(ws, header_row, start_col, start_col + len(series))
    for row_offset, x_value in enumerate(x_values, start=1):
        row_number = header_row + row_offset
        write_value(ws.cell(row=row_number, column=start_col), x_value, x_title)
        for series_offset, name in enumerate(series, start=1):
            values = groups.get((x_value, name))
            if values:
                cell = ws.cell(row=row_number, column=start_col + series_offset)
                cell.value = statistics.median(values)
                cell.number_format = number_format_for(y_title, cell.value)

    use_bar = force_bar or len(x_values) < 2
    chart = BarChart() if use_bar else LineChart()
    if use_bar:
        chart.type = "col"
    else:
        for chart_series in chart.series:
            chart_series.marker.symbol = "circle"
    chart.style = 10
    chart.title = section_title
    chart.x_axis.title = x_title
    chart.y_axis.title = y_title
    chart.height = 7.2
    chart.width = 13.5
    chart.legend.position = "b"
    data = Reference(
        ws,
        min_col=start_col + 1,
        max_col=start_col + len(series),
        min_row=header_row,
        max_row=header_row + len(x_values),
    )
    categories = Reference(
        ws,
        min_col=start_col,
        min_row=header_row + 1,
        max_row=header_row + len(x_values),
    )
    chart.add_data(data, titles_from_data=True)
    chart.set_categories(categories)
    if not use_bar:
        for chart_series in chart.series:
            chart_series.marker.symbol = "circle"
            chart_series.graphicalProperties.line.width = 22000
    if chart_anchor is None:
        chart_anchor = (
            f"{get_column_letter(start_col)}{header_row + len(x_values) + 2}"
        )
    ws.add_chart(chart, chart_anchor)
    adjust_widths(ws, start_col, start_col + len(series), max_width=24)
    return 1


def correctness_index(
    correctness: Sequence[dict[str, Any]],
) -> dict[tuple[str, str], str]:
    result: dict[tuple[str, str], str] = {}
    for row in correctness:
        key = (
            str(pick(row, "case_id", "case") or ""),
            str(pick(row, "semantic_group", "semantics") or ""),
        )
        result[key] = str(pick(row, "status") or "missing").lower()
    return result


def choose_baseline(
    entries: Sequence[dict[str, Any]], requested: str | None
) -> dict[str, Any] | None:
    if requested:
        for entry in entries:
            if entry["algorithm"].lower() == requested.lower():
                return entry
        return None

    def priority(entry: dict[str, Any]) -> tuple[int, str]:
        name = entry["algorithm"].lower()
        if name == "original_consecutive":
            return (0, name)
        if "original" in name and "consecutive" in name:
            return (1, name)
        if name in {"original", "td-tree", "td_tree", "tdtree"}:
            return (2, name)
        if "td-tree" in name or "td_tree" in name:
            return (3, name)
        if "original" in name:
            return (4, name)
        return (99, name)

    candidates = sorted(entries, key=priority)
    return candidates[0] if candidates and priority(candidates[0])[0] < 99 else None


def build_speedup_rows(
    summaries: Sequence[dict[str, Any]],
    correctness: Sequence[dict[str, Any]],
    requested_baseline: str | None,
) -> list[dict[str, Any]]:
    checks = correctness_index(correctness)
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for row in summaries:
        grouped.setdefault(
            (row["case_id"], row["semantic_group"]), []
        ).append(row)

    result: list[dict[str, Any]] = []
    for (case_id, semantics), entries in sorted(grouped.items()):
        baseline = choose_baseline(entries, requested_baseline)
        if baseline is None:
            for entry in entries:
                result.append(
                    {
                        "Dataset": entry["dataset"],
                        "Case ID": case_id,
                        "Semantics": semantics,
                        "Baseline": requested_baseline or "(no TD-Tree baseline)",
                        "Algorithm": entry["algorithm"],
                        "Baseline runtime (s)": None,
                        "Algorithm runtime (s)": entry["runtime_s"],
                        "Correctness": checks.get((case_id, semantics), "missing"),
                        "Repeat consistency": "missing",
                        "Comparison status": "not comparable",
                        "Speedup (×)": None,
                        "_speedup_numeric": None,
                    }
                )
            continue
        baseline_runtime = baseline.get("runtime_s")
        for entry in entries:
            correctness_status = checks.get((case_id, semantics), "missing")
            runtime = entry.get("runtime_s")
            successful = entry.get("successful_runs")
            baseline_successful = baseline.get("successful_runs")
            repeat_consistent = (
                entry.get("repeat_match_count_consistent")
                not in (False, 0, "false", "False")
                and baseline.get("repeat_match_count_consistent")
                not in (False, 0, "false", "False")
            )
            valid = (
                correctness_status == "pass"
                and is_number(baseline_runtime)
                and is_number(runtime)
                and float(runtime) > 0
                and (successful is None or successful > 0)
                and (baseline_successful is None or baseline_successful > 0)
                and repeat_consistent
            )
            speedup = (
                float(baseline_runtime) / float(runtime)
                if valid
                else None
            )
            result.append(
                {
                    "Dataset": entry["dataset"],
                    "Case ID": case_id,
                    "Semantics": semantics,
                    "Baseline": baseline["algorithm"],
                    "Algorithm": entry["algorithm"],
                    "Baseline runtime (s)": baseline_runtime,
                    "Algorithm runtime (s)": runtime,
                    "Correctness": correctness_status,
                    "Repeat consistency": "pass" if repeat_consistent else "fail",
                    "Comparison status": "valid" if valid else "not comparable",
                    "Speedup (×)": None,
                    "_speedup_numeric": speedup,
                }
            )
    return result


def normalized_metric(
    source: dict[str, Any], aliases: Sequence[str], contains: Sequence[str] = ()
) -> float | int | None:
    lookup = row_lookup(source)
    for alias in aliases:
        value = lookup.get(normalized_name(alias))
        if is_number(value):
            return value
    if not contains:
        return None
    candidates = [
        (key, value)
        for key, value in lookup.items()
        if is_number(value) and all(token in key for token in contains)
    ]
    return sorted(candidates)[0][1] if candidates else None


def summed_metrics(
    source: dict[str, Any], aliases: Sequence[str]
) -> float | int | None:
    values = [
        normalized_metric(source, (alias,))
        for alias in aliases
    ]
    concrete = [value for value in values if is_number(value)]
    return sum(concrete) if concrete else None


def build_gopher_rows(
    summaries: Sequence[dict[str, Any]],
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    ablation_cases = {
        entry["case_id"]
        for entry in summaries
        if "ablation" in entry["case_id"].lower()
    }
    for entry in summaries:
        if ablation_cases:
            if entry["case_id"] not in ablation_cases:
                continue
        elif "gopher" not in entry["algorithm"].lower():
            continue
        source = entry["source"]
        statistics_enabled = optional_bool(
            pick(
                source,
                "gopher_statistics_enabled_value",
                "statistics_enabled_value",
                "gopher_statistics_enabled",
                "statistics_enabled",
            )
        )
        cache_hits = (
            summed_metrics(
                source,
                (
                    "structural_cache_hits_median",
                    "temporal_cache_hits_median",
                    "longest_cache_hits_median",
                ),
            )
            if statistics_enabled is not False
            else None
        )
        cache_misses = (
            summed_metrics(
                source,
                (
                    "structural_cache_misses_median",
                    "temporal_cache_misses_median",
                    "longest_cache_misses_median",
                ),
            )
            if statistics_enabled is not False
            else None
        )
        hit_rate = None
        if (
            is_number(cache_hits)
            and is_number(cache_misses)
            and float(cache_hits) + float(cache_misses) > 0
        ):
            hit_rate = float(cache_hits) / (
                float(cache_hits) + float(cache_misses)
            )
        rows.append(
            {
                "Dataset": entry["dataset"],
                "Case ID": entry["case_id"],
                "Semantics": entry["semantic_group"],
                "Algorithm": entry["algorithm"],
                "Variant / ablation": entry["ablation"] or entry["algorithm"],
                "Runtime (s)": entry["runtime_s"],
                "Cache hits": cache_hits,
                "Cache misses": cache_misses,
                "Cache hit rate": hit_rate,
                "Cache lookup (s)": (
                    float(cache_lookup_ns) * 1.0e-9
                    if statistics_enabled is not False
                    and is_number(
                        cache_lookup_ns := normalized_metric(
                            source,
                            ("cache_lookup_ns_median",),
                        )
                    )
                    else None
                ),
                "Cache memory (MiB)": (
                    float(cache_memory_bytes) / (1024.0 * 1024.0)
                    if is_number(
                        cache_memory_bytes := normalized_metric(
                            source,
                            ("gopher_memory_bytes_cache_median",),
                        )
                    )
                    else None
                ),
                "DAG nodes": normalized_metric(
                    source,
                    ("dag_nodes_materialized_median",),
                    ("dag", "nodes", "materialized"),
                ),
                "DFS states": (
                    normalized_metric(
                        source,
                        ("dfs_states_median",),
                        ("dfs", "states"),
                    )
                    if statistics_enabled is not False
                    else None
                ),
                "Generated candidates": (
                    normalized_metric(
                        source,
                        ("generated_candidates_median",),
                        ("generated", "candidates"),
                    )
                    if statistics_enabled is not False
                    else None
                ),
                "Active-support prunes": (
                    normalized_metric(
                        source,
                        ("pruned_active_support_median",),
                        ("pruned", "active", "support"),
                    )
                    if statistics_enabled is not False
                    else None
                ),
                "Temporal-support prunes": (
                    normalized_metric(
                        source,
                        ("pruned_temporal_support_median",),
                        ("pruned", "temporal", "support"),
                    )
                    if statistics_enabled is not False
                    else None
                ),
                "Structural prunes": (
                    normalized_metric(
                        source,
                        ("pruned_structural_median",),
                        ("pruned", "structural"),
                    )
                    if statistics_enabled is not False
                    else None
                ),
                "Injectivity prunes": (
                    normalized_metric(
                        source,
                        ("pruned_injectivity_median",),
                        ("pruned", "injectivity"),
                    )
                    if statistics_enabled is not False
                    else None
                ),
            }
        )
    return rows


def build_scaling_rows(
    runs: Sequence[dict[str, Any]], runtime_column: str | None
) -> list[dict[str, Any]]:
    grouped: dict[
        tuple[str, str, str, str, int, int | None, int | None], list[float]
    ] = {}
    for row in runs:
        if str(pick(row, "status") or "").lower() != "ok":
            continue
        metadata = case_metadata(row)
        threads = metadata["threads"]
        runtime, _ = runtime_seconds(
            row, summary=False, requested_column=runtime_column
        )
        if threads is None or runtime is None:
            continue
        key = (
            metadata["dataset"],
            metadata["case_id"],
            metadata["semantic_group"],
            base_algorithm_for_scaling(metadata["algorithm"]),
            int(threads),
            metadata["query_edges"],
            metadata["k"],
        )
        grouped.setdefault(key, []).append(runtime)
    rows: list[dict[str, Any]] = []
    thread_sets: dict[
        tuple[str, str, str, str, int | None, int | None], set[int]
    ] = {}
    for key in grouped:
        dataset, case_id, semantics, algorithm, threads, query_edges, k = key
        base_key = (dataset, case_id, semantics, algorithm, query_edges, k)
        thread_sets.setdefault(base_key, set()).add(threads)
    for key, values in sorted(grouped.items()):
        dataset, case_id, semantics, algorithm, threads, query_edges, k = key
        base_key = (dataset, case_id, semantics, algorithm, query_edges, k)
        if len(thread_sets[base_key]) < 2:
            continue
        rows.append(
            {
                "Dataset": dataset,
                "Case ID": case_id,
                "Semantics": semantics,
                "Algorithm": algorithm,
                "Scaling series": f"{case_id} | {algorithm}",
                "Threads": threads,
                "Query edges": query_edges,
                "k": k,
                "Runs": len(values),
                "Median runtime (s)": statistics.median(values),
                "Min runtime (s)": min(values),
                "Max runtime (s)": max(values),
            }
        )
    return rows


def is_gpu_backend(value: Any) -> bool:
    lowered = str(value or "").strip().lower()
    return lowered in {"gpu", "rocm", "hip", "dxg", "rocdxg"} or any(
        token in lowered for token in ("gpu", "rocm", "hip")
    )


def is_primary_runtime_entry(entry: dict[str, Any]) -> bool:
    """Exclude instrumented mechanism variants from headline comparisons."""

    name = str(entry.get("algorithm", "")).lower()
    role = str(entry.get("algorithm_role", "")).lower()
    excluded_tokens = (
        "stats",
        "diagnostic",
        "cache0",
        "cache64",
        "cache1024",
        "no_cost",
        "no_prune",
        "structural",
        "temporal_paper",
        "none_paper",
    )
    return not any(token in name or token in role for token in excluded_tokens)


def build_single_thread_rows(
    summaries: Sequence[dict[str, Any]],
    correctness: Sequence[dict[str, Any]],
    requested_baseline: str | None,
) -> list[dict[str, Any]]:
    checks = correctness_index(correctness)
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for entry in summaries:
        if is_gpu_backend(entry.get("backend")) or entry.get("threads") != 1:
            continue
        grouped.setdefault(
            (entry["case_id"], entry["semantic_group"]), []
        ).append(entry)

    rows: list[dict[str, Any]] = []
    for (case_id, semantics), entries in sorted(grouped.items()):
        baseline = choose_baseline(entries, requested_baseline)
        baseline_runtime = baseline.get("runtime_s") if baseline else None
        correctness_status = checks.get((case_id, semantics), "missing")
        for entry in sorted(entries, key=lambda value: value["algorithm"]):
            runtime = entry.get("runtime_s")
            repeat_consistent = entry.get("repeat_match_count_consistent") not in (
                False,
                0,
                "false",
                "False",
            )
            comparable = (
                baseline is not None
                and correctness_status == "pass"
                and repeat_consistent
                and is_number(baseline_runtime)
                and is_number(runtime)
                and float(runtime) > 0
            )
            speedup = (
                float(baseline_runtime) / float(runtime)
                if comparable
                else None
            )
            rows.append(
                {
                    "Dataset": entry["dataset"],
                    "Case ID": case_id,
                    "Suite": entry.get("suite"),
                    "Query": entry["query"],
                    "Query edges": entry["query_edges"],
                    "k": entry["k"],
                    "Semantics": semantics,
                    "Algorithm": entry["algorithm"],
                    "Role": entry.get("algorithm_role"),
                    "Threads": 1,
                    "Median runtime (s)": runtime,
                    "Match count": entry.get("match_count"),
                    "Successful runs": entry.get("successful_runs"),
                    "Repeat consistency": (
                        "pass" if repeat_consistent else "fail"
                    ),
                    "Correctness": correctness_status,
                    "Baseline": baseline["algorithm"] if baseline else None,
                    "Speedup vs baseline (×)": speedup,
                    "Headline eligible": is_primary_runtime_entry(entry),
                }
            )
    return rows


def first_numeric_metric(
    source: dict[str, Any], aliases: Sequence[str]
) -> float | None:
    value = normalized_metric(source, aliases)
    return float(value) if is_number(value) else None


def metric_milliseconds_as_seconds(
    source: dict[str, Any], aliases: Sequence[str]
) -> float | None:
    value = first_numeric_metric(source, aliases)
    return value * 1.0e-3 if value is not None else None


def optional_bool(value: Any) -> bool | None:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)) and value in (0, 1):
        return bool(value)
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered in {"true", "yes", "on", "pass", "passed", "1"}:
            return True
        if lowered in {"false", "no", "off", "fail", "failed", "0"}:
            return False
    return None


def choose_cpu_reference(
    entries: Sequence[dict[str, Any]], *, single_thread: bool
) -> dict[str, Any] | None:
    candidates = [
        entry
        for entry in entries
        if not is_gpu_backend(entry.get("backend"))
        and (not single_thread or entry.get("threads") == 1)
        and is_number(entry.get("runtime_s"))
        and is_primary_runtime_entry(entry)
    ]
    if single_thread:
        gopher_candidates = [
            entry
            for entry in candidates
            if "gopher" in entry["algorithm"].lower()
        ]
        if gopher_candidates:
            candidates = gopher_candidates
    if not candidates:
        return None
    return min(candidates, key=lambda entry: float(entry["runtime_s"]))


def choose_gpu_logical_cpu_reference(
    entries: Sequence[dict[str, Any]],
    target_gpu: dict[str, Any] | None = None,
) -> dict[str, Any] | None:
    """Choose the 1T CPU DAG path that likewise has no runtime memo tables."""

    def config_value(
        entry: dict[str, Any],
        *aliases: str,
    ) -> Any:
        return pick(entry.get("source", {}), *aliases)

    target_order = (
        config_value(
            target_gpu,
            "order_config_value",
            "gopher_order_value",
            "result_order_value",
        )
        if target_gpu is not None
        else None
    )
    target_selectivity = (
        config_value(
            target_gpu,
            "selectivity_config_value",
            "gopher_selectivity_mode_value",
            "gopher_selectivity_value",
        )
        if target_gpu is not None
        else None
    )
    target_statistics = (
        optional_bool(
            config_value(
                target_gpu,
                "statistics_config_value",
                "gopher_statistics_enabled_value",
            )
        )
        if target_gpu is not None
        else None
    )

    candidates = []
    for entry in entries:
        name = str(entry.get("algorithm", "")).lower()
        role = str(entry.get("algorithm_role", "")).lower()
        if (
            is_gpu_backend(entry.get("backend"))
            or entry.get("threads") != 1
            or not is_number(entry.get("runtime_s"))
        ):
            continue
        explicit = any(
            token in role
            for token in (
                "gpu_closest_cpu_reference",
                "no_runtime_memo",
                "memo_none",
            )
        )
        inferred = "gopher" in name and any(
            token in name for token in ("_none_", "_no_memo", "_nomemo")
        )
        memo = config_value(
            entry,
            "memo_config_value",
            "gopher_memo_value",
            "result_memo_value",
            "gopher_memo_effective_value",
        )
        no_memo = (
            explicit
            or inferred
            or (
                memo not in (None, "")
                and str(memo).strip().lower()
                in {"none", "off", "disabled", "no_runtime_memo"}
            )
        )
        if not no_memo:
            continue

        candidate_order = config_value(
            entry,
            "order_config_value",
            "gopher_order_value",
            "result_order_value",
        )
        candidate_selectivity = config_value(
            entry,
            "selectivity_config_value",
            "gopher_selectivity_mode_value",
            "gopher_selectivity_value",
        )
        candidate_statistics = optional_bool(
            config_value(
                entry,
                "statistics_config_value",
                "gopher_statistics_enabled_value",
            )
        )
        if (
            target_order not in (None, "")
            and candidate_order not in (None, "")
            and str(candidate_order) != str(target_order)
        ):
            continue
        if (
            target_selectivity not in (None, "")
            and candidate_selectivity not in (None, "")
            and str(candidate_selectivity) != str(target_selectivity)
        ):
            continue
        if (
            target_statistics is not None
            and candidate_statistics is not None
            and candidate_statistics != target_statistics
        ):
            continue
        candidates.append(entry)
    if not candidates:
        return None
    return min(candidates, key=lambda entry: float(entry["runtime_s"]))


def build_gpu_rows(
    summaries: Sequence[dict[str, Any]],
    correctness: Sequence[dict[str, Any]],
) -> list[dict[str, Any]]:
    checks = correctness_index(correctness)
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for entry in summaries:
        grouped.setdefault(
            (entry["case_id"], entry["semantic_group"]), []
        ).append(entry)

    rows: list[dict[str, Any]] = []
    for (case_id, semantics), entries in sorted(grouped.items()):
        gpu_entries = [
            entry for entry in entries if is_gpu_backend(entry.get("backend"))
        ]
        if not gpu_entries:
            continue
        cpu_1t = choose_cpu_reference(entries, single_thread=True)
        cpu_best = choose_cpu_reference(entries, single_thread=False)
        original = choose_baseline(entries, None)
        ours = next(
            (
                entry
                for entry in entries
                if entry["algorithm"].lower() == "ours"
                and not is_gpu_backend(entry.get("backend"))
            ),
            None,
        )
        correctness_status = checks.get((case_id, semantics), "missing")

        for entry in sorted(gpu_entries, key=lambda value: value["algorithm"]):
            source = entry["source"]
            logical_cpu = choose_gpu_logical_cpu_reference(
                entries, target_gpu=entry
            )
            gpu_runtime = entry.get("runtime_s")
            statistics_enabled = optional_bool(
                pick(
                    source,
                    "gopher_statistics_enabled_value",
                    "statistics_enabled_value",
                    "gopher_statistics_enabled",
                    "statistics_enabled",
                )
            )
            fallback_used = optional_bool(
                pick(
                    source,
                    "gopher_fallback_used_value",
                    "fallback_used_value",
                    "gopher_fallback_used",
                    "fallback_used",
                )
            )
            verification_passed = optional_bool(
                pick(
                    source,
                    "gopher_verification_passed_value",
                    "verification_passed_value",
                    "gopher_verification_passed",
                    "verification_passed",
                )
            )
            verification_requested = optional_bool(
                pick(
                    source,
                    "gopher_verification_requested_value",
                    "verification_requested_value",
                    "gopher_verification_requested",
                    "verification_requested",
                )
            )
            execution_eligible = (
                fallback_used is not True
                and (
                    verification_requested is not True
                    or verification_passed is True
                )
            )
            cpu_1t_runtime = cpu_1t.get("runtime_s") if cpu_1t else None
            cpu_best_runtime = cpu_best.get("runtime_s") if cpu_best else None
            logical_cpu_runtime = (
                logical_cpu.get("runtime_s") if logical_cpu else None
            )
            original_runtime = original.get("runtime_s") if original else None
            ours_runtime = ours.get("runtime_s") if ours else None

            def speedup(reference: Any) -> float | None:
                if (
                    correctness_status == "pass"
                    and execution_eligible
                    and is_number(reference)
                    and is_number(gpu_runtime)
                    and float(gpu_runtime) > 0
                ):
                    return float(reference) / float(gpu_runtime)
                return None

            rows.append(
                {
                    "Dataset": entry["dataset"],
                    "Case ID": case_id,
                    "Suite": entry.get("suite"),
                    "Query": entry["query"],
                    "Query edges": entry["query_edges"],
                    "k": entry["k"],
                    "Semantics": semantics,
                    "GPU algorithm": entry["algorithm"],
                    "GPU device": entry.get("device"),
                    "GPU end-to-end (s)": gpu_runtime,
                    "Host preprocess (s)": metric_milliseconds_as_seconds(
                        source,
                        (
                            "gopher_host_preprocess_ms_median",
                            "host_preprocess_ms_median",
                        ),
                    ),
                    "GPU data flatten (s)": metric_milliseconds_as_seconds(
                        source,
                        (
                            "gopher_flatten_gpu_data_ms_median",
                            "flatten_gpu_data_ms_median",
                        ),
                    ),
                    "GPU kernel (s)": metric_milliseconds_as_seconds(
                        source,
                        (
                            "gopher_gpu_kernel_ms_median",
                            "gpu_kernel_ms_median",
                            "kernel_ms_median",
                        ),
                    ),
                    "Host-to-device (s)": metric_milliseconds_as_seconds(
                        source,
                        (
                            "gopher_gpu_h2d_ms_median",
                            "gopher_gpu_host_to_device_ms_median",
                            "gpu_h2d_ms_median",
                            "h2d_ms_median",
                        ),
                    ),
                    "Device-to-host (s)": metric_milliseconds_as_seconds(
                        source,
                        (
                            "gopher_gpu_d2h_ms_median",
                            "gopher_gpu_device_to_host_ms_median",
                            "gpu_d2h_ms_median",
                            "d2h_ms_median",
                        ),
                    ),
                    "GPU search (s)": metric_milliseconds_as_seconds(
                        source,
                        (
                            "gopher_gpu_search_ms_median",
                            "gpu_search_ms_median",
                        ),
                    ),
                    "GPU ready-to-result (s)": (
                        metric_milliseconds_as_seconds(
                            source,
                            (
                                "gopher_gpu_ready_to_result_ms_median",
                                "gpu_ready_to_result_ms_median",
                            ),
                        )
                    ),
                    "GPU batches": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_batches_median",
                            "gpu_batches_median",
                            "gopher_gpu_kernel_launches_median",
                            "gpu_kernel_launches_median",
                            "gopher_gpu_frontier_levels_median",
                        ),
                    ),
                    "GPU work items": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_work_items_median",
                            "gopher_gpu_candidates_median",
                            "gpu_work_items_median",
                            "gopher_statistics_generated_candidates_median",
                            "generated_candidates_median",
                        ),
                    )
                    if statistics_enabled is not False
                    else None,
                    "Accepted GPU candidates": (
                        first_numeric_metric(
                            source,
                            (
                                "gopher_statistics_accepted_candidates_median",
                                "accepted_candidates_median",
                            ),
                        )
                        if statistics_enabled is not False
                        else None
                    ),
                    "Peak frontier states": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_peak_frontier_states_median",
                            "gpu_peak_frontier_states_median",
                        ),
                    ),
                    "Peak frontier bytes": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_peak_frontier_device_bytes_median",
                            "gpu_peak_frontier_device_bytes_median",
                        ),
                    ),
                    "Persistent device bytes": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_persistent_device_bytes_median",
                            "gpu_persistent_device_bytes_median",
                        ),
                    ),
                    "H2D bytes": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_h2d_bytes_median",
                            "gpu_h2d_bytes_median",
                        ),
                    ),
                    "D2H bytes": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_d2h_bytes_median",
                            "gpu_d2h_bytes_median",
                        ),
                    ),
                    "Fallback used": fallback_used,
                    "CPU verification requested": verification_requested,
                    "CPU verification passed": verification_passed,
                    "Execution eligible": execution_eligible,
                    "Closest CPU no-memo reference": (
                        logical_cpu["algorithm"] if logical_cpu else None
                    ),
                    "Closest CPU no-memo (s)": logical_cpu_runtime,
                    "Speedup vs closest CPU no-memo (×)": speedup(
                        logical_cpu_runtime
                    ),
                    "CPU 1T reference": (
                        cpu_1t["algorithm"] if cpu_1t else None
                    ),
                    "CPU 1T end-to-end (s)": cpu_1t_runtime,
                    "Speedup vs CPU 1T (×)": speedup(cpu_1t_runtime),
                    "Best CPU reference": (
                        cpu_best["algorithm"] if cpu_best else None
                    ),
                    "Best CPU end-to-end (s)": cpu_best_runtime,
                    "Speedup vs best CPU (×)": speedup(cpu_best_runtime),
                    "Original end-to-end (s)": original_runtime,
                    "Speedup vs original (×)": speedup(original_runtime),
                    "Ours end-to-end (s)": ours_runtime,
                    "Speedup vs ours (×)": speedup(ours_runtime),
                    "Match count": entry.get("match_count"),
                    "Successful runs": entry.get("successful_runs"),
                    "Correctness": correctness_status,
                }
            )
    return rows


def parallel_structure_label(entry: dict[str, Any]) -> str:
    source = entry["source"]
    explicit = pick(
        source,
        "gopher_parallel_mode_effective_value",
        "parallel_mode_effective_value",
        "gopher_parallel_mode_effective",
        "parallel_mode_effective",
        "gopher_gpu_variant_value",
        "gpu_variant_value",
        "gopher_gpu_variant",
        "gpu_variant",
    )
    if explicit not in (None, ""):
        explicit_text = str(explicit)
        # With one requested worker the prefix scheduler intentionally takes
        # the serial root path. Keep it in the prefix series so its 1T runtime
        # is the correct denominator for scheduler scaling.
        if explicit_text == "roots-single-worker":
            algorithm_text = " ".join(
                (
                    str(entry.get("algorithm", "")),
                    str(entry.get("algorithm_role", "")),
                )
            ).lower()
            if "prefix" in algorithm_text:
                return "prefix"
        return explicit_text

    text = " ".join(
        (
            str(entry.get("algorithm", "")),
            str(entry.get("algorithm_role", "")),
        )
    ).lower()
    if "prefix" in text:
        return "dynamic_prefix"
    if "resident" in text:
        return "resident_continuation"
    if "level" in text and "frontier" in text:
        return "level_frontier"
    if "root" in text:
        return "root_tasks"
    return "unspecified"


def metric_duration_seconds(
    source: dict[str, Any],
    *,
    millisecond_aliases: Sequence[str] = (),
    nanosecond_aliases: Sequence[str] = (),
) -> float | None:
    milliseconds = first_numeric_metric(source, millisecond_aliases)
    if milliseconds is not None:
        return milliseconds * 1.0e-3
    nanoseconds = first_numeric_metric(source, nanosecond_aliases)
    if nanoseconds is not None:
        return nanoseconds * 1.0e-9
    return None


def build_parallel_architecture_rows(
    summaries: Sequence[dict[str, Any]],
    correctness: Sequence[dict[str, Any]],
) -> list[dict[str, Any]]:
    checks = correctness_index(correctness)
    candidates: list[dict[str, Any]] = []
    for entry in summaries:
        structure = parallel_structure_label(entry)
        signal = " ".join(
            (
                str(entry.get("case_id", "")),
                str(entry.get("suite", "")),
                str(entry.get("algorithm", "")),
                str(entry.get("algorithm_role", "")),
                structure,
            )
        ).lower()
        if not any(
            token in signal
            for token in (
                "parallel_v2",
                "dynamic_prefix",
                "prefix_scheduler",
                "root_tasks_frozen",
                "resident_continuation",
                "level_frontier_frozen",
            )
        ):
            continue
        candidates.append({**entry, "_parallel_structure": structure})

    grouped: dict[tuple[str, str, int | None], list[dict[str, Any]]] = {}
    for entry in candidates:
        backend = "gpu" if is_gpu_backend(entry.get("backend")) else "cpu"
        threads = entry.get("threads") if backend == "cpu" else None
        grouped.setdefault(
            (entry["case_id"], backend, threads), []
        ).append(entry)

    rows: list[dict[str, Any]] = []
    for (case_id, backend, _threads), entries in sorted(grouped.items()):
        def baseline_rank(entry: dict[str, Any]) -> tuple[int, str]:
            text = " ".join(
                (
                    str(entry.get("algorithm", "")),
                    str(entry.get("algorithm_role", "")),
                    str(entry.get("_parallel_structure", "")),
                )
            ).lower()
            if "frozen" in text or "baseline" in text:
                return (0, str(entry["algorithm"]))
            if backend == "cpu" and "root" in text:
                return (1, str(entry["algorithm"]))
            if backend == "gpu" and "level" in text:
                return (1, str(entry["algorithm"]))
            return (9, str(entry["algorithm"]))

        baseline_entry = min(entries, key=baseline_rank)
        if baseline_rank(baseline_entry)[0] >= 9:
            baseline_entry = None
        baseline_runtime = (
            baseline_entry.get("runtime_s")
            if baseline_entry is not None
            else None
        )

        def current_control_rank(
            entry: dict[str, Any],
        ) -> tuple[int, str]:
            structure = str(entry.get("_parallel_structure", ""))
            text = " ".join(
                (
                    str(entry.get("algorithm", "")),
                    str(entry.get("algorithm_role", "")),
                )
            ).lower()
            if "frozen" in text:
                return (9, str(entry["algorithm"]))
            if backend == "cpu" and structure == "roots":
                return (0, str(entry["algorithm"]))
            if backend == "gpu" and structure == "level_synchronous":
                return (0, str(entry["algorithm"]))
            return (9, str(entry["algorithm"]))

        current_control_entry = min(entries, key=current_control_rank)
        if current_control_rank(current_control_entry)[0] >= 9:
            current_control_entry = baseline_entry
        current_control_runtime = (
            current_control_entry.get("runtime_s")
            if current_control_entry is not None
            else None
        )
        correctness_status = checks.get(
            (case_id, entries[0]["semantic_group"]), "missing"
        )

        for entry in sorted(entries, key=lambda value: value["algorithm"]):
            source = entry["source"]
            runtime = entry.get("runtime_s")
            fallback = optional_bool(
                pick(
                    source,
                    "gopher_fallback_used_value",
                    "fallback_used_value",
                    "gopher_fallback_used",
                    "fallback_used",
                )
            )
            comparable = (
                correctness_status == "pass"
                and fallback is not True
                and is_number(baseline_runtime)
                and is_number(runtime)
                and float(runtime) > 0
            )
            speedup = (
                float(baseline_runtime) / float(runtime)
                if comparable
                else None
            )
            current_control_speedup = (
                float(current_control_runtime) / float(runtime)
                if (
                    correctness_status == "pass"
                    and fallback is not True
                    and is_number(current_control_runtime)
                    and is_number(runtime)
                    and float(runtime) > 0
                )
                else None
            )
            structure = str(entry["_parallel_structure"])
            threads = entry.get("threads")
            selectivity = pick(
                source,
                "gopher_selectivity_mode_value",
                "gopher_selectivity_value",
                "selectivity_config_value",
                "result_selectivity_mode_value",
                "result_selectivity_value",
                "gopher_selectivity_mode",
                "gopher_selectivity",
                "result_selectivity_mode",
                "result_selectivity",
            )
            order = pick(
                source,
                "gopher_order_value",
                "order_config_value",
                "result_order_value",
                "gopher_order_mode_value",
                "result_order_mode_value",
                "gopher_order",
                "result_order",
                "gopher_order_mode",
                "result_order_mode",
            )
            statistics_enabled = optional_bool(
                pick(
                    source,
                    "gopher_statistics_enabled_value",
                    "statistics_config_value",
                    "statistics_enabled_value",
                    "gopher_statistics_enabled",
                    "statistics_enabled",
                )
            )
            row = {
                    "Dataset": entry["dataset"],
                    "Case ID": case_id,
                    "Backend": backend,
                    "Algorithm": entry["algorithm"],
                    "Structure": structure,
                    "Selectivity": selectivity,
                    "Order": order,
                    "Statistics enabled": statistics_enabled,
                    "Threads": threads,
                    "Runs": entry.get("successful_runs"),
                    "Median end-to-end (s)": runtime,
                    "Search / GPU-ready (s)": (
                        metric_duration_seconds(
                            source,
                            millisecond_aliases=(
                                "gopher_gpu_ready_to_result_ms_median",
                                "gpu_ready_to_result_ms_median",
                                "enumerateMatches_ms_median",
                            ),
                            nanosecond_aliases=(
                                "enumerateMatches_ns_median",
                                "enumerate_matches_ns_median",
                                "search_ns_median",
                            ),
                        )
                    ),
                    "Baseline algorithm": (
                        baseline_entry["algorithm"]
                        if baseline_entry is not None
                        else None
                    ),
                    "Baseline end-to-end (s)": baseline_runtime,
                    "Speedup vs same-backend old structure (×)": speedup,
                    "Current control algorithm": (
                        current_control_entry["algorithm"]
                        if current_control_entry is not None
                        else None
                    ),
                    "Current control end-to-end (s)": current_control_runtime,
                    "Speedup vs current control (×)": current_control_speedup,
                    "Match count": entry.get("match_count"),
                    "Correctness": correctness_status,
                    "Repeat consistency": (
                        "pass"
                        if entry.get("repeat_match_count_consistent")
                        not in (False, 0, "false", "False")
                        else "fail"
                    ),
                    "Prefix tasks spawned": first_numeric_metric(
                        source,
                        (
                            "gopher_parallel_prefix_tasks_spawned_median",
                            "gopher_parallel_statistics_prefix_tasks_spawned_median",
                            "parallel_prefix_tasks_spawned_median",
                        ),
                    ),
                    "Prefix split attempts": first_numeric_metric(
                        source,
                        (
                            "gopher_parallel_prefix_split_attempts_median",
                            "gopher_parallel_statistics_prefix_split_attempts_median",
                            "parallel_prefix_split_attempts_median",
                        ),
                    ),
                    "Prefix split rejected": first_numeric_metric(
                        source,
                        (
                            "gopher_parallel_prefix_split_rejected_median",
                            "gopher_parallel_statistics_prefix_split_rejected_median",
                            "parallel_prefix_split_rejected_median",
                        ),
                    ),
                    "Queue peak": first_numeric_metric(
                        source,
                        (
                            "gopher_parallel_queue_peak_median",
                            "gopher_parallel_statistics_queue_peak_median",
                            "parallel_queue_peak_median",
                        ),
                    ),
                    "Queue capacity": first_numeric_metric(
                        source,
                        (
                            "gopher_parallel_task_queue_capacity_median",
                            "gopher_parallel_statistics_task_queue_capacity_median",
                            "parallel_task_queue_capacity_median",
                        ),
                    ),
                    "Workers with tasks": first_numeric_metric(
                        source,
                        (
                            "gopher_parallel_workers_with_tasks_median",
                            "gopher_parallel_statistics_workers_with_tasks_median",
                            "parallel_workers_with_tasks_median",
                        ),
                    ),
                    "Worker DFS states min": first_numeric_metric(
                        source,
                        (
                            "gopher_parallel_worker_dfs_states_min_median",
                            "gopher_parallel_statistics_worker_dfs_states_min_median",
                            "parallel_worker_dfs_states_min_median",
                        ),
                    ),
                    "Worker DFS states max": first_numeric_metric(
                        source,
                        (
                            "gopher_parallel_worker_dfs_states_max_median",
                            "gopher_parallel_statistics_worker_dfs_states_max_median",
                            "parallel_worker_dfs_states_max_median",
                        ),
                    ),
                    "Worker DFS states total": first_numeric_metric(
                        source,
                        (
                            "gopher_parallel_worker_dfs_states_total_median",
                            "gopher_parallel_statistics_worker_dfs_states_total_median",
                            "parallel_worker_dfs_states_total_median",
                        ),
                    ),
                    "Worker DFS-state CV": first_numeric_metric(
                        source,
                        (
                            "gopher_parallel_worker_dfs_states_cv_median",
                            "gopher_parallel_statistics_worker_dfs_states_cv_median",
                            "parallel_worker_dfs_states_cv_median",
                        ),
                    ),
                    "Worker active-time CV": first_numeric_metric(
                        source,
                        (
                            "gopher_parallel_worker_active_nanoseconds_cv_median",
                            "gopher_parallel_statistics_worker_active_nanoseconds_cv_median",
                            "parallel_worker_active_nanoseconds_cv_median",
                            "parallel_worker_active_ns_cv_median",
                        ),
                    ),
                    "Worker utilization": first_numeric_metric(
                        source,
                        (
                            "gopher_parallel_worker_utilization_median",
                            "gopher_parallel_statistics_worker_utilization_median",
                            "parallel_worker_utilization_median",
                        ),
                    ),
                    "GPU kernel launches": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_kernel_launches_median",
                            "gpu_kernel_launches_median",
                        ),
                    ),
                    "GPU depth host round trips": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_depth_host_round_trips_median",
                            "gpu_depth_host_round_trips_median",
                        ),
                    ),
                    "GPU device synchronizations": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_device_synchronizations_median",
                            "gpu_device_synchronizations_median",
                        ),
                    ),
                    "GPU validation passes": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_frontier_validation_passes_median",
                            "gpu_frontier_validation_passes_median",
                        ),
                    ),
                    "GPU materialization passes": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_frontier_materialization_passes_median",
                            "gpu_frontier_materialization_passes_median",
                        ),
                    ),
                    "GPU launch/sync overhead (s)": (
                        metric_duration_seconds(
                            source,
                            nanosecond_aliases=(
                                "gopher_gpu_launch_and_sync_overhead_nanoseconds_median",
                                "gpu_launch_and_sync_overhead_nanoseconds_median",
                            ),
                        )
                    ),
                    "GPU depth round-trip (s)": metric_duration_seconds(
                        source,
                        nanosecond_aliases=(
                            "gopher_gpu_depth_host_round_trip_nanoseconds_median",
                            "gpu_depth_host_round_trip_nanoseconds_median",
                        ),
                    ),
                    "GPU kernel (s)": metric_duration_seconds(
                        source,
                        millisecond_aliases=(
                            "gopher_gpu_kernel_ms_median",
                            "gpu_kernel_ms_median",
                        ),
                    ),
                    "Resident capacity states": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_resident_capacity_states_median",
                            "gpu_resident_capacity_states_median",
                        ),
                    ),
                    "Resident grid blocks": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_resident_grid_blocks_median",
                            "gpu_resident_grid_blocks_median",
                        ),
                    ),
                    "Occupancy blocks per multiprocessor": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_occupancy_blocks_per_multiprocessor_median",
                            "gpu_occupancy_blocks_per_multiprocessor_median",
                        ),
                    ),
                    "Peak frontier states": first_numeric_metric(
                        source,
                        (
                            "gopher_gpu_peak_frontier_states_median",
                            "gpu_peak_frontier_states_median",
                        ),
                    ),
                    "Fallback used": fallback,
                    "Prefix split rejection rate": None,
                    "Queue peak / capacity": None,
                    "Workers-with-tasks ratio": None,
                    "Worker DFS max / mean": None,
                    "CPU structure speedup vs 1T (×)": None,
                    "CPU parallel efficiency": None,
                    "CPU Karp-Flatt fraction": None,
                }
            if backend == "gpu":
                for key in (
                    "Prefix tasks spawned",
                    "Prefix split attempts",
                    "Prefix split rejected",
                    "Queue peak",
                    "Queue capacity",
                    "Workers with tasks",
                    "Worker DFS states min",
                    "Worker DFS states max",
                    "Worker DFS states total",
                    "Worker DFS-state CV",
                    "Worker active-time CV",
                    "Worker utilization",
                ):
                    row[key] = None
            else:
                for key in (
                    "GPU kernel launches",
                    "GPU depth host round trips",
                    "GPU device synchronizations",
                    "GPU validation passes",
                    "GPU materialization passes",
                    "GPU launch/sync overhead (s)",
                    "GPU depth round-trip (s)",
                    "GPU kernel (s)",
                    "Resident capacity states",
                    "Resident grid blocks",
                    "Occupancy blocks per multiprocessor",
                    "Peak frontier states",
                    "Fallback used",
                ):
                    row[key] = None

                attempts = row["Prefix split attempts"]
                rejected = row["Prefix split rejected"]
                if (
                    is_number(attempts)
                    and float(attempts) > 0
                    and is_number(rejected)
                ):
                    row["Prefix split rejection rate"] = (
                        float(rejected) / float(attempts)
                    )
                queue_peak = row["Queue peak"]
                queue_capacity = row["Queue capacity"]
                if (
                    is_number(queue_capacity)
                    and float(queue_capacity) > 0
                    and is_number(queue_peak)
                ):
                    row["Queue peak / capacity"] = (
                        float(queue_peak) / float(queue_capacity)
                    )
                workers = row["Workers with tasks"]
                if (
                    isinstance(threads, int)
                    and threads > 0
                    and is_number(workers)
                ):
                    row["Workers-with-tasks ratio"] = (
                        float(workers) / threads
                    )
                dfs_total = row["Worker DFS states total"]
                dfs_max = row["Worker DFS states max"]
                if (
                    isinstance(threads, int)
                    and threads > 0
                    and is_number(dfs_total)
                    and float(dfs_total) > 0
                    and is_number(dfs_max)
                ):
                    row["Worker DFS max / mean"] = (
                        float(dfs_max) /
                        (float(dfs_total) / threads)
                    )
            rows.append(row)

    one_thread_runtime: dict[tuple[str, str], float] = {}
    for row in rows:
        if (
            row["Backend"] == "cpu"
            and row["Threads"] == 1
            and is_number(row["Median end-to-end (s)"])
        ):
            one_thread_runtime[(row["Case ID"], row["Structure"])] = float(
                row["Median end-to-end (s)"]
            )
    for row in rows:
        if row["Backend"] != "cpu" or not isinstance(row["Threads"], int):
            continue
        reference = one_thread_runtime.get(
            (row["Case ID"], row["Structure"])
        )
        runtime = row["Median end-to-end (s)"]
        threads = row["Threads"]
        if (
            reference is None
            or not is_number(runtime)
            or float(runtime) <= 0
            or threads < 1
        ):
            continue
        scaling_speedup = reference / float(runtime)
        row["CPU structure speedup vs 1T (×)"] = scaling_speedup
        row["CPU parallel efficiency"] = scaling_speedup / threads
        if threads > 1 and scaling_speedup > 0:
            row["CPU Karp-Flatt fraction"] = (
                1.0 / scaling_speedup - 1.0 / threads
            ) / (1.0 - 1.0 / threads)
    return rows


def build_gpu_depth_summary_rows(
    depth_rows: Sequence[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Aggregate per-run GPU depth records without inventing missing work."""

    grouped: dict[tuple[str, str, str, int], list[dict[str, Any]]] = {}
    for row in depth_rows:
        if str(row.get("status", "")).lower() != "ok":
            continue
        depth_value = row.get("depth")
        depth: int | None = None
        if (
            isinstance(depth_value, int)
            and not isinstance(depth_value, bool)
        ):
            depth = depth_value
        elif (
            isinstance(depth_value, float)
            and math.isfinite(depth_value)
            and depth_value.is_integer()
        ):
            depth = int(depth_value)
        elif (
            isinstance(depth_value, str)
            and re.fullmatch(r"\d+", depth_value.strip())
        ):
            depth = int(depth_value.strip())
        if depth is None:
            continue
        key = (
            str(row.get("case_id", "")),
            str(row.get("algorithm", "")),
            str(row.get("gpu_variant", "")),
            depth,
        )
        grouped.setdefault(key, []).append(row)

    def median_metric(
        rows: Sequence[dict[str, Any]], name: str
    ) -> float | None:
        values = [
            float(row[name])
            for row in rows
            if is_number(row.get(name))
        ]
        return statistics.median(values) if values else None

    result: list[dict[str, Any]] = []
    for (case_id, algorithm, variant, depth), rows in sorted(grouped.items()):
        workload_values = [
            optional_bool(row.get("workload_metrics_available"))
            for row in rows
        ]
        workload_available = (
            all(value is True for value in workload_values)
            if workload_values
            else None
        )
        kernel_ns = median_metric(rows, "kernel_ns")
        host_round_trip_ns = median_metric(rows, "host_round_trip_ns")
        result.append(
            {
                "Case ID": case_id,
                "Algorithm": algorithm,
                "GPU variant": variant,
                "Depth": depth,
                "Successful depth runs": len(rows),
                "Workload metrics available": workload_available,
                "Input states (median)": median_metric(rows, "input_states"),
                "Output states (median)": median_metric(rows, "output_states"),
                "Generated candidates (median)": median_metric(
                    rows, "generated_candidates"
                ),
                "Max candidates/state (median)": median_metric(
                    rows, "maximum_candidates_per_state"
                ),
                "Mean candidates/state (median)": median_metric(
                    rows, "mean_candidates_per_state"
                ),
                "Max/mean load ratio (median)": median_metric(
                    rows, "maximum_to_mean_load_ratio"
                ),
                "Kernel time (s, median)": (
                    kernel_ns * 1.0e-9
                    if kernel_ns is not None
                    else None
                ),
                "Host round-trip (s, median)": (
                    host_round_trip_ns * 1.0e-9
                    if host_round_trip_ns is not None
                    else None
                ),
                "Grid blocks (median)": median_metric(rows, "grid_blocks"),
            }
        )
    return result


def build_dimension_rows(
    summaries: Sequence[dict[str, Any]], dimension: str
) -> list[dict[str, Any]]:
    selected = list(summaries)
    paper_paths = [
        entry
        for entry in selected
        if entry["semantic_group"].lower() == "consecutive"
        and entry["case_id"].lower().startswith("paper_")
        and "_path" in entry["case_id"].lower()
    ]
    control_dimension = "k" if dimension == "query_edges" else "query_edges"
    controlled: dict[tuple[str, int], list[dict[str, Any]]] = {}
    for entry in paper_paths:
        control_value = entry.get(control_dimension)
        dimension_value = entry.get(dimension)
        if isinstance(control_value, int) and isinstance(dimension_value, int):
            controlled.setdefault(
                (str(entry.get("dataset", "unknown")), control_value), []
            ).append(entry)
    if controlled:
        control_key, candidates = max(
            controlled.items(),
            key=lambda item: (
                len({entry.get(dimension) for entry in item[1]}),
                len(item[1]),
                item[0][0],
                -item[0][1],
            ),
        )
        del control_key
        if len({entry.get(dimension) for entry in candidates}) >= 2:
            selected = candidates

    rows: list[dict[str, Any]] = []
    for entry in selected:
        value = entry.get(dimension)
        if value is None:
            continue
        rows.append(
            {
                "Dataset": entry["dataset"],
                "Case ID": entry["case_id"],
                "Query": entry["query"],
                "Query edges": entry["query_edges"],
                "k": entry["k"],
                "Semantics": entry["semantic_group"],
                "Algorithm": entry["algorithm"],
                "Runtime (s)": entry["runtime_s"],
                "Match count": entry["match_count"],
                "Successful runs": entry["successful_runs"],
            }
        )
    return rows


def methodology_rows(
    supplied: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    built_in = [
        {
            "Category": "Semantic comparability",
            "Statement": (
                "Speedup is calculated only within the same case and "
                "semantic_group. Cardinality and consecutive-duration "
                "semantics are never mixed."
            ),
            "Source": "evaluation/IMPLEMENTATION_NOTES.md",
            "Applicability / limitation": (
                "A missing same-semantics TD-Tree baseline is reported as "
                "'not comparable'."
            ),
        },
        {
            "Category": "Runtime aggregation",
            "Statement": (
                "The report prefers wall_s_median and documented wall/runtime "
                "aliases, normalizing milliseconds or microseconds to seconds "
                "only in derived sheets."
            ),
            "Source": "benchmark runner outputs",
            "Applicability / limitation": (
                "Raw Runs and Summary preserve the input values and units."
            ),
        },
        {
            "Category": "Correctness gate",
            "Statement": (
                "A speedup is valid only when the case/semantic correctness "
                "status is pass and both runtimes are finite and positive."
            ),
            "Source": "correctness.csv",
            "Applicability / limitation": (
                "Failed or missing correctness checks suppress the formula result."
            ),
        },
        {
            "Category": "GPU evidence",
            "Statement": (
                "ROCm/GPU performance is reportable only when an accessible "
                "device executed the measured runs."
            ),
            "Source": "environment.json and run records",
            "Applicability / limitation": (
                "Compiler availability or a build-only result is not a GPU benchmark."
            ),
        },
        {
            "Category": "GPU denominator",
            "Statement": (
                "GPU end-to-end time is compared with a one-thread CPU "
                "Candidate-DAG no-runtime-memo path and with the full CPU "
                "Gopher system; kernel-only time is diagnostic."
            ),
            "Source": "manifest algorithm roles and summary.csv",
            "Applicability / limitation": (
                "The GPU fused frontier executor is not a literal port of "
                "the CPU dependency-key memo tables."
            ),
        },
        {
            "Category": "Dataset units",
            "Statement": (
                "Snapshot occurrence rows, unique directed pairs, and active "
                "vertices are reported in separate columns."
            ),
            "Source": "dataset_inventory.csv",
            "Applicability / limitation": (
                "A unique-pair count must not be interpreted as Static Nodes."
            ),
        },
        {
            "Category": "Chart aggregation",
            "Statement": (
                "Chart helper tables use the median of available case-level "
                "medians at each x value; detailed rows remain visible beside them."
            ),
            "Source": "summary.csv",
            "Applicability / limitation": (
                "Charts cap displayed series at eight for readability; no source "
                "rows are dropped from the tables."
            ),
        },
    ]
    for row in supplied:
        built_in.append(
            {
                "Category": pick(row, "category", "section", "topic") or "Supplied",
                "Statement": pick(row, "statement", "description", "methodology")
                or json.dumps(row, ensure_ascii=False, sort_keys=True),
                "Source": pick(row, "source", "reference") or "methodology.csv",
                "Applicability / limitation": pick(
                    row, "limitation", "applicability", "notes", "status"
                )
                or "",
            }
        )
    return built_in


def dataset_rows(
    supplied: list[dict[str, Any]], runs: Sequence[dict[str, Any]]
) -> list[dict[str, Any]]:
    if supplied:
        rows = [dict(row) for row in supplied]
    else:
        discovered: dict[str, str] = {}
        for run in runs:
            metadata = case_metadata(run)
            data_path = str(pick(run, "data") or "")
            discovered.setdefault(metadata["dataset"], data_path)
        rows = [
            {"dataset": dataset, "data_path": path}
            for dataset, path in sorted(discovered.items())
        ]
    for row in rows:
        raw = pick_numeric(
            row,
            "raw_occurrence_rows",
            "raw_occurrences",
            "input_occurrences",
        )
        filtered = pick_numeric(
            row,
            "filtered_occurrence_rows",
            "filtered_occurrences",
            "retained_occurrences",
        )
        if (
            raw is not None
            and filtered is not None
            and float(raw) > 0
            and pick(row, "occurrence_reduction", "reduction_rate") is None
        ):
            row["occurrence_reduction"] = 1.0 - float(filtered) / float(raw)
    return rows


def add_dataset_chart(ws: Any, headers: Sequence[str], rows: Sequence[dict[str, Any]]) -> int:
    dataset_col = header_column(headers, "dataset", "dataset_name", "name")
    reduction_col = header_column(
        headers, "occurrence_reduction", "reduction_rate", "reduction"
    )
    if dataset_col is None or reduction_col is None or not rows:
        return 0
    numeric_rows = [
        index
        for index, row in enumerate(rows, start=2)
        if is_number(row.get(headers[reduction_col - 1]))
    ]
    if not numeric_rows or numeric_rows != list(
        range(min(numeric_rows), max(numeric_rows) + 1)
    ):
        return 0
    chart = BarChart()
    chart.type = "bar"
    chart.style = 10
    chart.title = "Occurrence reduction by dataset"
    chart.x_axis.title = "Reduction"
    chart.y_axis.title = "Dataset"
    chart.height = 7
    chart.width = 12
    chart.legend = None
    data = Reference(
        ws,
        min_col=reduction_col,
        min_row=1,
        max_row=max(numeric_rows),
    )
    categories = Reference(
        ws,
        min_col=dataset_col,
        min_row=min(numeric_rows),
        max_row=max(numeric_rows),
    )
    chart.add_data(data, titles_from_data=True)
    chart.set_categories(categories)
    ws.add_chart(chart, f"{get_column_letter(len(headers) + 2)}2")
    return 1


def create_overview(
    ws: Any,
    *,
    title: str,
    input_dir: Path,
    source_files: dict[str, bool],
    run_headers: Sequence[str],
    summary_headers: Sequence[str],
    correctness_headers: Sequence[str],
    summaries: Sequence[dict[str, Any]],
    chart_count_hint: int,
) -> None:
    ws.merge_cells("A1:D1")
    ws["A1"] = title
    ws["A1"].font = TITLE_FONT
    ws["A1"].alignment = Alignment(vertical="center")
    ws.row_dimensions[1].height = 30
    ws["A3"] = "Metric"
    ws["B3"] = "Value"
    ws["C3"] = "Interpretation"
    style_header(ws, 3, 1, 3)

    run_status_col = header_column(run_headers, "status")
    correctness_status_col = header_column(correctness_headers, "status")
    repeat_consistency_col = header_column(
        summary_headers, "repeat_match_count_consistent"
    )
    raw_status_ref = (
        f"'Raw Runs'!{get_column_letter(run_status_col)}:{get_column_letter(run_status_col)}"
        if run_status_col
        else "'Raw Runs'!A:A"
    )
    correctness_status_ref = (
        f"'Correctness'!{get_column_letter(correctness_status_col)}:"
        f"{get_column_letter(correctness_status_col)}"
        if correctness_status_col
        else "'Correctness'!A:A"
    )
    repeat_consistency_ref = (
        f"'Summary'!{get_column_letter(repeat_consistency_col)}:"
        f"{get_column_letter(repeat_consistency_col)}"
        if repeat_consistency_col
        else "'Summary'!A:A"
    )
    rows = [
        (
            "Generated (UTC)",
            datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
            "Workbook creation timestamp; source run timestamps remain in Raw Runs.",
        ),
        ("Input directory", str(input_dir), "Resolved report source directory."),
        (
            "Run records",
            "=MAX(0,COUNTA('Raw Runs'!A:A)-1)",
            "All repetitions, including errors and timeouts.",
        ),
        (
            "Successful runs",
            f'=COUNTIF({raw_status_ref},"ok")',
            "Only status=ok observations feed performance summaries.",
        ),
        (
            "Timeouts",
            f'=COUNTIF({raw_status_ref},"timeout")',
            "Inspect Raw Runs before interpreting incomplete comparisons.",
        ),
        (
            "Correctness passed",
            f'=COUNTIF({correctness_status_ref},"pass")',
            "Speedup formulas are enabled only for passing semantic groups.",
        ),
        (
            "Correctness failed",
            f'=COUNTIF({correctness_status_ref},"fail")',
            "Any nonzero value blocks affected speedup comparisons.",
        ),
        (
            "Repeat-inconsistent summaries",
            f'=COUNTIF({repeat_consistency_ref},FALSE)',
            "Affected rows are highlighted and excluded from valid speedup.",
        ),
        (
            "Algorithms",
            len({row["algorithm"] for row in summaries}),
            "Distinct method/configuration labels in summary.csv.",
        ),
        (
            "Datasets",
            len({row["dataset"] for row in summaries}),
            "Distinct dataset labels inferred or supplied.",
        ),
        (
            "Charts",
            chart_count_hint,
            "Expected chart count at build time; validator checks the saved file.",
        ),
    ]
    for index, (metric, value, note) in enumerate(rows, start=4):
        ws.cell(row=index, column=1, value=metric)
        ws.cell(row=index, column=2, value=value)
        ws.cell(row=index, column=3, value=note)
        ws.cell(row=index, column=1).font = Font(bold=True, color=INK)
        ws.cell(row=index, column=3).font = Font(color=GREY)
        ws.cell(row=index, column=3).alignment = Alignment(wrap_text=True)

    source_start = 16
    ws.cell(row=source_start, column=1, value="Source inventory")
    ws.cell(row=source_start, column=1).font = SECTION_FONT
    ws.cell(row=source_start + 1, column=1, value="File")
    ws.cell(row=source_start + 1, column=2, value="Loaded")
    style_header(ws, source_start + 1, 1, 2)
    for offset, (name, loaded) in enumerate(source_files.items(), start=1):
        ws.cell(row=source_start + 1 + offset, column=1, value=name)
        ws.cell(row=source_start + 1 + offset, column=2, value=loaded)

    aggregate: dict[str, list[float]] = {}
    for row in summaries:
        if is_number(row.get("runtime_s")):
            aggregate.setdefault(row["algorithm"], []).append(float(row["runtime_s"]))
    chart_rows = [
        {"Algorithm": algorithm, "Median runtime (s)": statistics.median(values)}
        for algorithm, values in sorted(aggregate.items())
    ]
    start_col = 6
    ws.cell(row=3, column=start_col, value="Algorithm")
    ws.cell(row=3, column=start_col + 1, value="Median runtime (s)")
    style_header(ws, 3, start_col, start_col + 1)
    for offset, row in enumerate(chart_rows, start=1):
        ws.cell(row=3 + offset, column=start_col, value=row["Algorithm"])
        cell = ws.cell(
            row=3 + offset, column=start_col + 1, value=row["Median runtime (s)"]
        )
        cell.number_format = '0.000" s"'
    if chart_rows:
        chart = BarChart()
        chart.type = "bar"
        chart.style = 10
        chart.title = "Median end-to-end runtime by algorithm"
        chart.x_axis.title = "Seconds (median across cases)"
        chart.y_axis.title = "Algorithm"
        chart.height = 7.5
        chart.width = 13
        chart.legend = None
        data = Reference(
            ws,
            min_col=start_col + 1,
            min_row=3,
            max_row=3 + len(chart_rows),
        )
        categories = Reference(
            ws,
            min_col=start_col,
            min_row=4,
            max_row=3 + len(chart_rows),
        )
        chart.add_data(data, titles_from_data=True)
        chart.set_categories(categories)
        ws.add_chart(chart, "F12")
    ws.freeze_panes = "A4"
    ws.sheet_view.showGridLines = False
    ws.column_dimensions["A"].width = 24
    ws.column_dimensions["B"].width = 30
    ws.column_dimensions["C"].width = 66
    ws.column_dimensions["F"].width = 28
    ws.column_dimensions["G"].width = 20


def build_workbook(
    *,
    input_dir: Path,
    title: str,
    baseline: str | None,
    runtime_column: str | None,
) -> Workbook:
    paths = {name: input_dir / name for name in (*REQUIRED_INPUTS, *OPTIONAL_INPUTS)}
    missing = [name for name in REQUIRED_INPUTS if not paths[name].is_file()]
    if missing:
        raise FileNotFoundError(
            "Missing required benchmark input(s): " + ", ".join(missing)
        )

    run_headers, runs = read_csv_file(paths["runs.csv"])
    summary_headers, summary_rows = read_csv_file(paths["summary.csv"])
    correctness_headers, correctness = read_csv_file(paths["correctness.csv"])

    supplied_datasets: list[dict[str, Any]] = []
    if paths["dataset_inventory.csv"].is_file():
        _, supplied_datasets = read_csv_file(paths["dataset_inventory.csv"])
    supplied_methodology: list[dict[str, Any]] = []
    if paths["methodology.csv"].is_file():
        _, supplied_methodology = read_csv_file(paths["methodology.csv"])
    supplied_gpu_depths: list[dict[str, Any]] = []
    if paths["gpu_depths.csv"].is_file():
        _, supplied_gpu_depths = read_csv_file(paths["gpu_depths.csv"])

    environment: list[dict[str, Any]]
    if paths["environment.json"].is_file():
        raw_environment = json.loads(
            paths["environment.json"].read_text(encoding="utf-8")
        )
        environment = flattened_json_rows(raw_environment)
        if not any(row["Scope"] == "GPU / accelerator" for row in environment):
            environment.extend(
                (
                    {
                        "Scope": "GPU / accelerator",
                        "Key": "gpu.available",
                        "Value": "Not reported",
                    },
                    {
                        "Scope": "GPU / accelerator",
                        "Key": "gpu.note",
                        "Value": (
                            "GPU availability was not recorded; no GPU "
                            "performance claim can be made."
                        ),
                    },
                )
            )
    else:
        environment = [
            {
                "Scope": "GPU / accelerator",
                "Key": "gpu.available",
                "Value": False,
            },
            {
                "Scope": "GPU / accelerator",
                "Key": "gpu.reason",
                "Value": (
                    "environment.json not supplied; GPU performance is unavailable."
                ),
            },
            {
                "Scope": "Other",
                "Key": "environment.json",
                "Value": "Not supplied; CPU comparability is not established.",
            },
        ]

    enriched = enrich_summaries(summary_rows, runs, runtime_column)
    if not any(is_number(row["runtime_s"]) for row in enriched):
        requested = f" ({runtime_column})" if runtime_column else ""
        raise ValueError(
            "summary.csv has no usable median wall/runtime column"
            f"{requested}; see evaluation/EXCEL_REPORT.md"
        )

    datasets = dataset_rows(supplied_datasets, runs)
    methods = methodology_rows(supplied_methodology)
    speedups = build_speedup_rows(enriched, correctness, baseline)
    query_rows = build_dimension_rows(enriched, "query_edges")
    k_rows = build_dimension_rows(enriched, "k")
    gopher_rows = build_gopher_rows(enriched)
    scaling_rows = build_scaling_rows(runs, runtime_column)
    single_thread_rows = build_single_thread_rows(
        enriched, correctness, baseline
    )
    gpu_rows = build_gpu_rows(enriched, correctness)
    parallel_rows = build_parallel_architecture_rows(
        enriched, correctness
    )
    gpu_depth_summary_rows = build_gpu_depth_summary_rows(
        supplied_gpu_depths
    )

    wb = Workbook()
    wb.remove(wb.active)
    wb.properties.title = title
    wb.properties.subject = "Durable subgraph matching benchmark evaluation"
    wb.properties.creator = "benchmark/create_excel.py"
    wb.properties.description = (
        "Validated workbook built from runs.csv, summary.csv, and correctness.csv."
    )
    wb.calculation.fullCalcOnLoad = True
    wb.calculation.forceFullCalc = True
    wb.calculation.calcMode = "auto"
    for name in SHEET_NAMES:
        wb.create_sheet(name)

    environment_headers = ("Scope", "Key", "Value")
    write_table(
        wb["Environment & GPU"],
        environment_headers,
        environment,
        empty_note="environment.json was not supplied.",
    )
    add_false_warning(
        wb["Environment & GPU"],
        environment_headers,
        ("Value",),
        1 + len(environment),
    )

    dataset_headers = union_headers(
        datasets,
        (
            "dataset",
            "raw_occurrences",
            "filtered_occurrences",
            "occurrence_reduction",
            "data_path",
        ),
    )
    if not dataset_headers:
        dataset_headers = ["dataset", "data_path"]
    write_table(
        wb["Datasets"],
        dataset_headers,
        datasets,
        empty_note="No dataset inventory or data paths were available.",
    )
    dataset_chart_count = add_dataset_chart(
        wb["Datasets"], dataset_headers, datasets
    )

    write_table(
        wb["Correctness"],
        correctness_headers,
        correctness,
        freeze="C2",
        empty_note="No correctness checks were emitted.",
    )
    add_status_formatting(
        wb["Correctness"],
        correctness_headers,
        ("status",),
        1 + len(correctness),
    )

    write_table(
        wb["Raw Runs"],
        run_headers,
        runs,
        freeze="D2",
        empty_note="No raw runs were emitted.",
    )
    add_status_formatting(
        wb["Raw Runs"], run_headers, ("status",), 1 + len(runs)
    )

    write_table(
        wb["Summary"],
        summary_headers,
        summary_rows,
        freeze="D2",
        empty_note="No summary rows were emitted.",
    )
    add_positive_number_warning(
        wb["Summary"],
        summary_headers,
        ("timeouts", "errors"),
        1 + len(summary_rows),
    )
    add_false_warning(
        wb["Summary"],
        summary_headers,
        ("repeat_match_count_consistent",),
        1 + len(summary_rows),
    )

    speedup_headers = [
        "Dataset",
        "Case ID",
        "Semantics",
        "Baseline",
        "Algorithm",
        "Baseline runtime (s)",
        "Algorithm runtime (s)",
        "Correctness",
        "Repeat consistency",
        "Comparison status",
        "Speedup (×)",
    ]
    visible_speedups = [
        {key: value for key, value in row.items() if not key.startswith("_")}
        for row in speedups
    ]
    write_table(
        wb["Speedup"],
        speedup_headers,
        visible_speedups,
        freeze="F2",
        empty_note=(
            "No same-semantics baseline was available. "
            "See Methodology & Limits."
        ),
    )
    speedup_ws = wb["Speedup"]
    for row_number, row in enumerate(speedups, start=2):
        if row["Comparison status"] == "valid":
            speedup_ws.cell(
                row=row_number,
                column=11,
                value=f'=IF(J{row_number}="valid",IFERROR(F{row_number}/G{row_number},""),"")',
            )
            speedup_ws.cell(row=row_number, column=11).number_format = '0.00"×"'
    add_status_formatting(
        speedup_ws,
        speedup_headers,
        ("Correctness", "Repeat consistency", "Comparison status"),
        1 + len(speedups),
    )
    if speedups:
        speedup_ws.conditional_formatting.add(
            f"K2:K{1 + len(speedups)}",
            ColorScaleRule(
                start_type="num",
                start_value=0.5,
                start_color="F4CCCC",
                mid_type="num",
                mid_value=1.0,
                mid_color="FFF2CC",
                end_type="num",
                end_value=2.0,
                end_color="D9EAD3",
            ),
        )
    speedup_chart_rows = [
        {
            "Algorithm": row["Algorithm"],
            "Series": "Median valid speedup",
            "Speedup (×)": row["_speedup_numeric"],
        }
        for row in speedups
        if is_number(row["_speedup_numeric"])
        and row["Algorithm"] != row["Baseline"]
    ]
    speedup_chart_count = add_median_pivot(
        speedup_ws,
        speedup_chart_rows,
        x_key="Algorithm",
        series_key="Series",
        value_key="Speedup (×)",
        section_title="Median valid speedup by algorithm",
        x_title="Algorithm",
        y_title="Speedup (×)",
        start_col=13,
        force_bar=True,
        chart_anchor="M8",
    )

    dimension_headers = [
        "Dataset",
        "Case ID",
        "Query",
        "Query edges",
        "k",
        "Semantics",
        "Algorithm",
        "Runtime (s)",
        "Match count",
        "Successful runs",
    ]
    write_table(
        wb["Query Size"],
        dimension_headers,
        query_rows,
        freeze="G2",
        empty_note=(
            "query_edges/query_size was not present and could not be inferred "
            "from case or query names."
        ),
    )
    query_chart_count = add_median_pivot(
        wb["Query Size"],
        query_rows,
        x_key="Query edges",
        series_key="Algorithm",
        value_key="Runtime (s)",
        section_title="Runtime by query size",
        x_title="Query edges",
        y_title="Runtime (s)",
        start_col=12,
        chart_anchor="L8",
    )

    write_table(
        wb["K Sensitivity"],
        dimension_headers,
        k_rows,
        freeze="G2",
        empty_note="k was not present and could not be inferred from case names.",
    )
    k_chart_count = add_median_pivot(
        wb["K Sensitivity"],
        k_rows,
        x_key="k",
        series_key="Algorithm",
        value_key="Runtime (s)",
        section_title="Runtime by durability threshold k",
        x_title="k (snapshots)",
        y_title="Runtime (s)",
        start_col=12,
        chart_anchor="L8",
    )

    gopher_headers = [
        "Dataset",
        "Case ID",
        "Semantics",
        "Algorithm",
        "Variant / ablation",
        "Runtime (s)",
        "Cache hits",
        "Cache misses",
        "Cache hit rate",
        "Cache lookup (s)",
        "Cache memory (MiB)",
        "DAG nodes",
        "DFS states",
        "Generated candidates",
        "Active-support prunes",
        "Temporal-support prunes",
        "Structural prunes",
        "Injectivity prunes",
    ]
    write_table(
        wb["Gopher Ablation"],
        gopher_headers,
        gopher_rows,
        freeze="F2",
        empty_note="No Gopher-labelled summary rows were found.",
    )
    gopher_ws = wb["Gopher Ablation"]
    if gopher_rows:
        gopher_ws.conditional_formatting.add(
            f"I2:I{1 + len(gopher_rows)}",
            ColorScaleRule(
                start_type="num",
                start_value=0,
                start_color="F4CCCC",
                end_type="num",
                end_value=1,
                end_color="D9EAD3",
            ),
        )
    gopher_runtime_count = add_median_pivot(
        gopher_ws,
        gopher_rows,
        x_key="Variant / ablation",
        series_key="Case ID",
        value_key="Runtime (s)",
        section_title="Gopher runtime by ablation",
        x_title="Variant / ablation",
        y_title="Runtime (s)",
        start_col=21,
        force_bar=True,
        chart_anchor="U8",
    )
    gopher_cache_count = add_median_pivot(
        gopher_ws,
        gopher_rows,
        x_key="Variant / ablation",
        series_key="Case ID",
        value_key="Cache hit rate",
        section_title="Gopher cache hit rate by ablation",
        x_title="Variant / ablation",
        y_title="Cache hit rate",
        start_col=21,
        start_row=26,
        force_bar=True,
        chart_anchor="U33",
    )

    scaling_headers = [
        "Dataset",
        "Case ID",
        "Semantics",
        "Algorithm",
        "Scaling series",
        "Threads",
        "Query edges",
        "k",
        "Runs",
        "Median runtime (s)",
        "Min runtime (s)",
        "Max runtime (s)",
    ]
    write_table(
        wb["CPU Scaling"],
        scaling_headers,
        scaling_rows,
        freeze="F2",
        empty_note=(
            "No thread count was present or inferable. Emit threads/thread_count "
            "in runs.csv for scaling analysis."
        ),
    )
    scaling_chart_count = add_median_pivot(
        wb["CPU Scaling"],
        scaling_rows,
        x_key="Threads",
        series_key="Scaling series",
        value_key="Median runtime (s)",
        section_title="CPU thread scaling",
        x_title="CPU threads",
        y_title="Median runtime (s)",
        start_col=13,
        chart_anchor="M8",
    )

    single_thread_headers = [
        "Dataset",
        "Case ID",
        "Suite",
        "Query",
        "Query edges",
        "k",
        "Semantics",
        "Algorithm",
        "Role",
        "Threads",
        "Median runtime (s)",
        "Match count",
        "Successful runs",
        "Repeat consistency",
        "Correctness",
        "Baseline",
        "Speedup vs baseline (×)",
        "Headline eligible",
    ]
    write_table(
        wb["CPU Single Thread"],
        single_thread_headers,
        single_thread_rows,
        freeze="H2",
        empty_note=(
            "No CPU implementation reported or implied exactly one "
            "application thread."
        ),
    )
    add_status_formatting(
        wb["CPU Single Thread"],
        single_thread_headers,
        ("Repeat consistency", "Correctness"),
        1 + len(single_thread_rows),
    )
    headline_single_thread = [
        row
        for row in single_thread_rows
        if row["Headline eligible"]
        and row["Semantics"].lower() == "consecutive"
        and row["Suite"] == "real_dataset_comparison"
    ]
    if not headline_single_thread:
        headline_single_thread = [
            row
            for row in single_thread_rows
            if row["Headline eligible"]
            and row["Semantics"].lower() == "consecutive"
        ]
    single_thread_chart_count = add_median_pivot(
        wb["CPU Single Thread"],
        headline_single_thread,
        x_key="Case ID",
        series_key="Algorithm",
        value_key="Median runtime (s)",
        section_title="Single-thread CPU end-to-end runtime",
        x_title="Benchmark case",
        y_title="Median runtime (s)",
        start_col=20,
        force_bar=True,
        chart_anchor="T8",
    )

    gpu_headers = [
        "Dataset",
        "Case ID",
        "Suite",
        "Query",
        "Query edges",
        "k",
        "Semantics",
        "GPU algorithm",
        "GPU device",
        "GPU end-to-end (s)",
        "Host preprocess (s)",
        "GPU data flatten (s)",
        "GPU kernel (s)",
        "Host-to-device (s)",
        "Device-to-host (s)",
        "GPU search (s)",
        "GPU ready-to-result (s)",
        "GPU batches",
        "GPU work items",
        "Accepted GPU candidates",
        "Peak frontier states",
        "Peak frontier bytes",
        "Persistent device bytes",
        "H2D bytes",
        "D2H bytes",
        "Fallback used",
        "CPU verification requested",
        "CPU verification passed",
        "Execution eligible",
        "Closest CPU no-memo reference",
        "Closest CPU no-memo (s)",
        "Speedup vs closest CPU no-memo (×)",
        "CPU 1T reference",
        "CPU 1T end-to-end (s)",
        "Speedup vs CPU 1T (×)",
        "Best CPU reference",
        "Best CPU end-to-end (s)",
        "Speedup vs best CPU (×)",
        "Original end-to-end (s)",
        "Speedup vs original (×)",
        "Ours end-to-end (s)",
        "Speedup vs ours (×)",
        "Match count",
        "Successful runs",
        "Correctness",
    ]
    write_table(
        wb["GPU Evaluation"],
        gpu_headers,
        gpu_rows,
        freeze="J2",
        empty_note=(
            "No successfully summarized GPU-labelled implementation was "
            "present. GPU timings require an execution-verified device."
        ),
    )
    add_status_formatting(
        wb["GPU Evaluation"],
        gpu_headers,
        ("Correctness",),
        1 + len(gpu_rows),
    )
    add_true_warning(
        wb["GPU Evaluation"],
        gpu_headers,
        ("Fallback used",),
        1 + len(gpu_rows),
    )
    add_false_warning(
        wb["GPU Evaluation"],
        gpu_headers,
        ("Execution eligible",),
        1 + len(gpu_rows),
    )
    gpu_chart_rows: list[dict[str, Any]] = []
    for row in gpu_rows:
        gpu_chart_rows.append(
            {
                "Case ID": row["Case ID"],
                "Implementation": f"GPU | {row['GPU algorithm']}",
                "Runtime (s)": row["GPU end-to-end (s)"],
            }
        )
        gpu_chart_rows.append(
            {
                "Case ID": row["Case ID"],
                "Implementation": "CPU 1T",
                "Runtime (s)": row["CPU 1T end-to-end (s)"],
            }
        )
        if (
            row["Best CPU reference"] != row["CPU 1T reference"]
            or row["Best CPU end-to-end (s)"]
            != row["CPU 1T end-to-end (s)"]
        ):
            gpu_chart_rows.append(
                {
                    "Case ID": row["Case ID"],
                    "Implementation": "Best CPU",
                    "Runtime (s)": row["Best CPU end-to-end (s)"],
                }
            )
    gpu_chart_count = add_median_pivot(
        wb["GPU Evaluation"],
        gpu_chart_rows,
        x_key="Case ID",
        series_key="Implementation",
        value_key="Runtime (s)",
        section_title="GPU and CPU end-to-end runtime",
        x_title="Benchmark case",
        y_title="Median runtime (s)",
        start_col=47,
        force_bar=True,
        chart_anchor="AU8",
    )

    parallel_headers = [
        "Dataset",
        "Case ID",
        "Backend",
        "Algorithm",
        "Structure",
        "Selectivity",
        "Order",
        "Statistics enabled",
        "Threads",
        "Runs",
        "Median end-to-end (s)",
        "Search / GPU-ready (s)",
        "Baseline algorithm",
        "Baseline end-to-end (s)",
        "Speedup vs same-backend old structure (×)",
        "Current control algorithm",
        "Current control end-to-end (s)",
        "Speedup vs current control (×)",
        "CPU structure speedup vs 1T (×)",
        "CPU parallel efficiency",
        "CPU Karp-Flatt fraction",
        "Match count",
        "Correctness",
        "Repeat consistency",
        "Prefix tasks spawned",
        "Prefix split attempts",
        "Prefix split rejected",
        "Prefix split rejection rate",
        "Queue peak",
        "Queue capacity",
        "Queue peak / capacity",
        "Workers with tasks",
        "Workers-with-tasks ratio",
        "Worker DFS states min",
        "Worker DFS states max",
        "Worker DFS states total",
        "Worker DFS max / mean",
        "Worker DFS-state CV",
        "Worker active-time CV",
        "Worker utilization",
        "GPU kernel launches",
        "GPU depth host round trips",
        "GPU device synchronizations",
        "GPU validation passes",
        "GPU materialization passes",
        "GPU launch/sync overhead (s)",
        "GPU depth round-trip (s)",
        "GPU kernel (s)",
        "Resident capacity states",
        "Resident grid blocks",
        "Occupancy blocks per multiprocessor",
        "Peak frontier states",
        "Fallback used",
    ]
    write_table(
        wb["Parallel Architecture"],
        parallel_headers,
        parallel_rows,
        freeze="H2",
        empty_note=(
            "No parallel-v2 CPU scheduler or GPU execution-variant rows "
            "were found."
        ),
    )
    add_status_formatting(
        wb["Parallel Architecture"],
        parallel_headers,
        ("Correctness", "Repeat consistency"),
        1 + len(parallel_rows),
    )
    add_true_warning(
        wb["Parallel Architecture"],
        parallel_headers,
        ("Fallback used",),
        1 + len(parallel_rows),
    )
    parallel_ws = wb["Parallel Architecture"]
    if parallel_rows:
        parallel_ws.conditional_formatting.add(
            f"O2:O{1 + len(parallel_rows)}",
            ColorScaleRule(
                start_type="num",
                start_value=0.5,
                start_color="F4CCCC",
                mid_type="num",
                mid_value=1.0,
                mid_color="FFF2CC",
                end_type="num",
                end_value=2.0,
                end_color="D9EAD3",
            ),
        )
        parallel_ws.conditional_formatting.add(
            f"R2:R{1 + len(parallel_rows)}",
            ColorScaleRule(
                start_type="num",
                start_value=0.5,
                start_color="F4CCCC",
                mid_type="num",
                mid_value=1.0,
                mid_color="FFF2CC",
                end_type="num",
                end_value=2.0,
                end_color="D9EAD3",
            ),
        )
    parallel_cpu_chart_rows = [
        {
            "Threads": row["Threads"],
            "Series": f"{row['Case ID']} | {row['Structure']}",
            "Runtime (s)": row["Median end-to-end (s)"],
        }
        for row in parallel_rows
        if row["Backend"] == "cpu"
        and isinstance(row["Threads"], int)
        and is_number(row["Median end-to-end (s)"])
    ]
    parallel_cpu_chart_count = add_median_pivot(
        parallel_ws,
        parallel_cpu_chart_rows,
        x_key="Threads",
        series_key="Series",
        value_key="Runtime (s)",
        section_title="CPU scheduler runtime by thread count",
        x_title="CPU threads",
        y_title="Median end-to-end (s)",
        start_col=55,
        chart_anchor="BC8",
        series_limit=20,
    )
    parallel_gpu_chart_rows = [
        {
            "Case ID": row["Case ID"],
            "Structure": row["Structure"],
            "Runtime (s)": row["Median end-to-end (s)"],
        }
        for row in parallel_rows
        if row["Backend"] == "gpu"
        and is_number(row["Median end-to-end (s)"])
    ]
    parallel_gpu_chart_count = add_median_pivot(
        parallel_ws,
        parallel_gpu_chart_rows,
        x_key="Case ID",
        series_key="Structure",
        value_key="Runtime (s)",
        section_title="GPU execution-structure runtime",
        x_title="Benchmark case",
        y_title="Median end-to-end (s)",
        start_col=55,
        start_row=28,
        force_bar=True,
        chart_anchor="BC35",
        series_limit=20,
    )

    gpu_depth_headers = [
        "Case ID",
        "Algorithm",
        "GPU variant",
        "Depth",
        "Successful depth runs",
        "Workload metrics available",
        "Input states (median)",
        "Output states (median)",
        "Generated candidates (median)",
        "Max candidates/state (median)",
        "Mean candidates/state (median)",
        "Max/mean load ratio (median)",
        "Kernel time (s, median)",
        "Host round-trip (s, median)",
        "Grid blocks (median)",
    ]
    write_table(
        wb["GPU Depths"],
        gpu_depth_headers,
        gpu_depth_summary_rows,
        freeze="G2",
        empty_note=(
            "No GPU per-depth observations were supplied. "
            "Statistics-off runs intentionally leave workload fields blank."
        ),
    )
    add_false_warning(
        wb["GPU Depths"],
        gpu_depth_headers,
        ("Workload metrics available",),
        1 + len(gpu_depth_summary_rows),
    )
    gpu_depth_chart_rows = [
        {
            "Depth": row["Depth"],
            "Series": (
                f"{row['Case ID']} | {row['GPU variant']}"
            ),
            "Max/mean load ratio": row[
                "Max/mean load ratio (median)"
            ],
        }
        for row in gpu_depth_summary_rows
    ]
    gpu_depth_chart_count = add_median_pivot(
        wb["GPU Depths"],
        gpu_depth_chart_rows,
        x_key="Depth",
        series_key="Series",
        value_key="Max/mean load ratio",
        section_title="Per-depth GPU workload skew",
        x_title="Query-plan depth",
        y_title="Maximum / mean candidates per state",
        start_col=18,
        chart_anchor="R8",
        series_limit=20,
    )

    method_headers = (
        "Category",
        "Statement",
        "Source",
        "Applicability / limitation",
    )
    write_table(
        wb["Methodology & Limits"],
        method_headers,
        methods,
        freeze="B2",
    )
    wb["Methodology & Limits"].column_dimensions["A"].width = 24
    wb["Methodology & Limits"].column_dimensions["B"].width = 72
    wb["Methodology & Limits"].column_dimensions["C"].width = 38
    wb["Methodology & Limits"].column_dimensions["D"].width = 72

    chart_count_hint = (
        1
        + dataset_chart_count
        + speedup_chart_count
        + query_chart_count
        + k_chart_count
        + gopher_runtime_count
        + gopher_cache_count
        + scaling_chart_count
        + single_thread_chart_count
        + gpu_chart_count
        + parallel_cpu_chart_count
        + parallel_gpu_chart_count
        + gpu_depth_chart_count
    )
    create_overview(
        wb["Overview"],
        title=title,
        input_dir=input_dir,
        source_files={name: paths[name].is_file() for name in paths},
        run_headers=run_headers,
        summary_headers=summary_headers,
        correctness_headers=correctness_headers,
        summaries=enriched,
        chart_count_hint=chart_count_hint,
    )

    for ws in wb.worksheets:
        ws.sheet_properties.tabColor = BLUE
        ws.sheet_view.zoomScale = 90
        ws.sheet_view.zoomScaleNormal = 90
        ws.oddFooter.center.text = "Page &P of &N"
        ws.oddFooter.right.text = "Generated by benchmark/create_excel.py"
        ws.sheet_properties.pageSetUpPr.fitToPage = True
        ws.page_setup.fitToWidth = 1
        ws.page_setup.fitToHeight = 0
        ws.page_margins.left = 0.25
        ws.page_margins.right = 0.25
        ws.page_margins.top = 0.5
        ws.page_margins.bottom = 0.5
    return wb


def validate_workbook(
    path: Path,
    *,
    minimum_charts: int = 1,
    required_chart_sheets: Sequence[str] = (),
) -> dict[str, Any]:
    wb = load_workbook(path, read_only=False, data_only=False)
    if tuple(wb.sheetnames) != SHEET_NAMES:
        raise ValueError(
            f"Unexpected worksheet set/order: {wb.sheetnames}; expected {SHEET_NAMES}"
        )

    formula_cells: list[str] = []
    chart_sheets: dict[str, int] = {}
    missing_filters: list[str] = []
    missing_freezes: list[str] = []
    for ws in wb.worksheets:
        for row in ws.iter_rows():
            for cell in row:
                if isinstance(cell.value, str) and cell.value.startswith("="):
                    formula_cells.append(f"{ws.title}!{cell.coordinate}")
                    if "#REF!" in cell.value:
                        raise ValueError(
                            f"Broken formula reference at {ws.title}!{cell.coordinate}"
                        )
        if ws._charts:
            chart_sheets[ws.title] = len(ws._charts)
            for chart in ws._charts:
                if chart.__class__.__name__ not in {"BarChart", "LineChart"}:
                    raise ValueError(
                        f"Unsupported chart type on {ws.title}: "
                        f"{chart.__class__.__name__}"
                    )
                if not chart.ser:
                    raise ValueError(f"Empty chart series on {ws.title}")
        if ws.title != "Overview" and ws.auto_filter.ref is None:
            missing_filters.append(ws.title)
        if ws.freeze_panes is None:
            missing_freezes.append(ws.title)

    if not formula_cells:
        raise ValueError("Workbook has no formulas")
    chart_count = sum(chart_sheets.values())
    if chart_count < minimum_charts:
        raise ValueError(
            f"Workbook has {chart_count} charts; expected at least {minimum_charts}"
        )
    missing_chart_sheets = [
        name for name in required_chart_sheets if not chart_sheets.get(name)
    ]
    if missing_chart_sheets:
        raise ValueError(
            "Missing chart(s) on worksheet(s): " + ", ".join(missing_chart_sheets)
        )
    if missing_filters:
        raise ValueError("Missing filters on: " + ", ".join(missing_filters))
    if missing_freezes:
        raise ValueError("Missing freeze panes on: " + ", ".join(missing_freezes))
    return {
        "path": str(path.resolve()),
        "sheets": len(wb.sheetnames),
        "formulas": len(formula_cells),
        "charts": chart_count,
        "chart_sheets": chart_sheets,
        "validation": "passed",
    }


def create_report(
    *,
    input_dir: Path,
    output: Path,
    title: str,
    baseline: str | None = None,
    runtime_column: str | None = None,
) -> dict[str, Any]:
    input_dir = input_dir.resolve()
    output = output.resolve()
    if output.suffix.lower() != ".xlsx":
        raise ValueError("--output must end in .xlsx")
    output.parent.mkdir(parents=True, exist_ok=True)
    workbook = build_workbook(
        input_dir=input_dir,
        title=title,
        baseline=baseline,
        runtime_column=runtime_column,
    )
    temporary = output.with_name(f".{output.stem}.building.xlsx")
    try:
        workbook.save(temporary)
        validate_workbook(temporary)
        os.replace(temporary, output)
        return validate_workbook(output)
    finally:
        if temporary.exists():
            temporary.unlink()


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create a validated benchmark Excel workbook."
    )
    parser.add_argument(
        "--input-dir",
        type=Path,
        help="Directory containing runs.csv, summary.csv, and correctness.csv.",
    )
    parser.add_argument("--output", type=Path, help="Destination .xlsx path.")
    parser.add_argument(
        "--title",
        default="Durable Subgraph Matching Performance Evaluation",
    )
    parser.add_argument(
        "--baseline",
        help=(
            "Exact baseline algorithm label. By default a same-semantics "
            "original/TD-Tree label is detected per case."
        ),
    )
    parser.add_argument(
        "--runtime-column",
        help=(
            "Exact runtime column to use. Summary must contain its *_median "
            "form/value; units are inferred from _s/_ms/_us/_ns."
        ),
    )
    parser.add_argument(
        "--validate-only",
        type=Path,
        help="Re-open and validate an existing workbook without creating one.",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.validate_only:
            receipt = validate_workbook(args.validate_only.resolve())
        else:
            if args.input_dir is None or args.output is None:
                raise ValueError(
                    "--input-dir and --output are required unless --validate-only is used"
                )
            receipt = create_report(
                input_dir=args.input_dir,
                output=args.output,
                title=args.title,
                baseline=args.baseline,
                runtime_column=args.runtime_column,
            )
        print(json.dumps(receipt, ensure_ascii=False, indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Excel report failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
