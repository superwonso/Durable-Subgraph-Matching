#!/usr/bin/env python3
"""Benchmark the repository's raw-to-_filtered materialization utility safely.

The benchmark compiles Dataset/effective_filtering_with_time_cond.cpp for
Linux, runs it against symlinks in per-run temporary directories, validates the
first generated artifact per dataset against the supplied _filtered file after
numeric canonical sorting, and writes auditable CSV/JSON results.

Generated datasets live only under /tmp unless --keep-generated is requested;
the supplied Dataset files are never opened for writing.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "Dataset" / "effective_filtering_with_time_cond.cpp"

DATASETS = (
    {
        "dataset": "sx_superuser",
        "raw": ROOT / "Dataset" / "sx-superuser-temporal-1w-unique.txt",
        "filtered": ROOT
        / "Dataset"
        / "sx-superuser-temporal-1w-unique_filtered.txt",
    },
    {
        "dataset": "wiki_talk",
        "raw": ROOT
        / "Dataset"
        / "wiki-talk-temporal-timeinstance-unique-1w.txt",
        "filtered": ROOT
        / "Dataset"
        / "wiki-talk-temporal-timeinstance-unique-1w_filtered.txt",
    },
    {
        "dataset": "sx_stackoverflow",
        "raw": ROOT
        / "Dataset"
        / "sx-stackoverflow-timeinstance-unique-1w.txt",
        "filtered": ROOT
        / "Dataset"
        / "sx-stackoverflow-timeinstance-unique-1w_filtered.txt",
    },
    {
        "dataset": "bitcoin",
        "raw": ROOT
        / "Dataset"
        / "bitcoin-temporal-timeinstance-unique-1w.txt",
        "filtered": ROOT
        / "Dataset"
        / "bitcoin-temporal-timeinstance-unique-1w_filtered.txt",
    },
)

STDOUT_PATTERNS = {
    "filter_core_ms": re.compile(r"Time consumed \(filtering\): (\d+) ms"),
    "serialize_loop_ms": re.compile(r"Time consumed \(I/O\): (\d+) ms"),
    "raw_unique_edges": re.compile(r"Total original edges: (\d+)"),
    "retained_unique_edges": re.compile(r"Total filtered edges: (\d+)"),
}

RESOURCE_PATTERNS = {
    "max_rss_kb": re.compile(
        r"Maximum resident set size \(kbytes\):\s*(\d+)"
    ),
    "major_faults": re.compile(
        r"Major \(requiring I/O\) page faults:\s*(\d+)"
    ),
    "minor_faults": re.compile(r"Minor \(reclaiming a frame\) page faults:\s*(\d+)"),
    "voluntary_context_switches": re.compile(
        r"Voluntary context switches:\s*(\d+)"
    ),
    "involuntary_context_switches": re.compile(
        r"Involuntary context switches:\s*(\d+)"
    ),
}

RUN_FIELDS = (
    "dataset",
    "repeat",
    "execution_position",
    "status",
    "exit_code",
    "wall_s",
    "filter_core_ms",
    "serialize_loop_ms",
    "unattributed_read_group_close_ms",
    "raw_unique_edges",
    "retained_unique_edges",
    "generated_rows",
    "generated_bytes",
    "generated_sha256",
    "canonical_validation",
    "canonical_generated_sha256",
    "canonical_supplied_sha256",
    "max_rss_kb",
    "major_faults",
    "minor_faults",
    "voluntary_context_switches",
    "involuntary_context_switches",
    "cpu_affinity",
    "raw_path",
    "supplied_filtered_path",
    "source_sha256",
    "executable_sha256",
    "started_at_utc",
    "ended_at_utc",
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def file_rows_bytes_sha256(path: Path) -> tuple[int, int, str]:
    digest = hashlib.sha256()
    row_count = 0
    byte_count = 0
    with path.open("rb") as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            byte_count += len(chunk)
            row_count += chunk.count(b"\n")
            digest.update(chunk)
    return row_count, byte_count, digest.hexdigest()


def parse_required(patterns: dict[str, re.Pattern[str]], text: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for name, pattern in patterns.items():
        match = pattern.search(text)
        if match is None:
            raise RuntimeError(f"Missing {name!r} in tool output")
        values[name] = int(match.group(1))
    return values


def parse_optional(patterns: dict[str, re.Pattern[str]], text: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for name, pattern in patterns.items():
        match = pattern.search(text)
        values[name] = int(match.group(1)) if match is not None else 0
    return values


def canonical_sha256(input_path: Path, output_path: Path, temp_dir: Path) -> str:
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    environment["TMPDIR"] = str(temp_dir)
    with output_path.open("wb") as output:
        subprocess.run(
            [
                "sort",
                "-n",
                "-k1,1",
                "-k2,2",
                "-k3,3",
                str(input_path),
            ],
            stdout=output,
            stderr=subprocess.PIPE,
            check=True,
            env=environment,
        )
    return sha256_file(output_path)


def write_csv(path: Path, rows: list[dict[str, Any]], fields: tuple[str, ...]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
        stream.flush()
        os.fsync(stream.fileno())


def compile_utility(build_dir: Path) -> tuple[Path, list[str]]:
    executable = build_dir / "effective_filtering_with_time_cond"
    command = [
        "g++",
        "-O3",
        "-DNDEBUG",
        "-std=c++17",
        str(SOURCE),
        "-o",
        str(executable),
    ]
    subprocess.run(command, check=True)
    return executable, command


def summarize(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    numeric_fields = (
        "wall_s",
        "filter_core_ms",
        "serialize_loop_ms",
        "unattributed_read_group_close_ms",
        "max_rss_kb",
    )
    for item in DATASETS:
        dataset = str(item["dataset"])
        group = [
            row
            for row in rows
            if row["dataset"] == dataset and row["status"] == "ok"
        ]
        if not group:
            continue
        record: dict[str, Any] = {
            "dataset": dataset,
            "repetitions": len(group),
            "canonical_validation": group[0]["canonical_validation"],
            "raw_unique_edges": group[0]["raw_unique_edges"],
            "retained_unique_edges": group[0]["retained_unique_edges"],
            "generated_rows": group[0]["generated_rows"],
            "generated_bytes": group[0]["generated_bytes"],
        }
        for field in numeric_fields:
            values = [float(row[field]) for row in group]
            record[f"{field}_median"] = statistics.median(values)
            record[f"{field}_min"] = min(values)
            record[f"{field}_max"] = max(values)
        result.append(record)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--cpu-affinity", default="0")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT / "evaluation" / "filter_materialization_20260729",
    )
    parser.add_argument(
        "--datasets",
        default=",".join(str(item["dataset"]) for item in DATASETS),
        help="Comma-separated dataset IDs",
    )
    parser.add_argument("--skip-canonical-validation", action="store_true")
    parser.add_argument("--keep-generated", action="store_true")
    arguments = parser.parse_args()

    if arguments.repeats <= 0:
        parser.error("--repeats must be positive")
    selected_ids = {value.strip() for value in arguments.datasets.split(",") if value.strip()}
    selected = [item for item in DATASETS if str(item["dataset"]) in selected_ids]
    if {str(item["dataset"]) for item in selected} != selected_ids:
        known = ", ".join(str(item["dataset"]) for item in DATASETS)
        parser.error(f"Unknown dataset ID; known IDs: {known}")

    for path in (SOURCE, *(Path(item[key]) for item in selected for key in ("raw", "filtered"))):
        if not path.is_file():
            raise FileNotFoundError(path)

    output_dir = arguments.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=False)
    runs_path = output_dir / "runs.csv"
    rows: list[dict[str, Any]] = []
    validations: dict[str, dict[str, Any]] = {}

    with tempfile.TemporaryDirectory(prefix="dsm_filter_build_", dir="/tmp") as build_name:
        build_dir = Path(build_name)
        executable, compile_command = compile_utility(build_dir)
        source_sha256 = sha256_file(SOURCE)
        executable_sha256 = sha256_file(executable)

        metadata = {
            "captured_at_utc": datetime.now(timezone.utc).isoformat(),
            "host": platform.node(),
            "platform": platform.platform(),
            "python": platform.python_version(),
            "source": str(SOURCE),
            "source_sha256": source_sha256,
            "compile_command": compile_command,
            "executable_sha256": executable_sha256,
            "repetitions": arguments.repeats,
            "cpu_affinity": arguments.cpu_affinity,
            "page_cache_policy": "OS page cache not flushed",
            "timing_boundaries": {
                "filter_core_ms": (
                    "Repository utility timer after raw read/group: per-edge "
                    "timestamp sort/dedup, consecutive-pair selection, and "
                    "filtered-map insertion."
                ),
                "serialize_loop_ms": (
                    "Repository utility output formatting/write loop; output "
                    "close is outside this internal timer."
                ),
                "wall_s": (
                    "Parent perf_counter from taskset/time process launch through "
                    "utility exit; includes read/parse/group, filter core, "
                    "serialization, close, and process overhead."
                ),
            },
        }
        (output_dir / "environment.json").write_text(
            json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
        )

        for repeat in range(1, arguments.repeats + 1):
            rotation = (repeat - 1) % len(selected)
            ordered = selected[rotation:] + selected[:rotation]
            for position, item in enumerate(ordered, start=1):
                dataset = str(item["dataset"])
                raw_path = Path(item["raw"]).resolve()
                supplied_path = Path(item["filtered"]).resolve()
                started = datetime.now(timezone.utc)
                with tempfile.TemporaryDirectory(
                    prefix=f"dsm_filter_{dataset}_r{repeat}_", dir="/tmp"
                ) as run_name:
                    run_dir = Path(run_name)
                    input_link = run_dir / raw_path.name
                    input_link.symlink_to(raw_path)
                    generated_path = run_dir / f"{raw_path.stem}_filtered.txt"
                    resource_path = run_dir / "resource.txt"
                    command = [
                        "taskset",
                        "-c",
                        arguments.cpu_affinity,
                        "/usr/bin/time",
                        "-v",
                        "-o",
                        str(resource_path),
                        str(executable),
                        str(input_link),
                    ]
                    start_ns = time.perf_counter_ns()
                    process = subprocess.run(
                        command,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                    )
                    end_ns = time.perf_counter_ns()
                    ended = datetime.now(timezone.utc)
                    wall_s = (end_ns - start_ns) / 1_000_000_000.0
                    resource_text = (
                        resource_path.read_text(encoding="utf-8", errors="replace")
                        if resource_path.exists()
                        else ""
                    )
                    status = (
                        "ok"
                        if process.returncode == 0 and generated_path.is_file()
                        else "failed"
                    )
                    parsed = (
                        parse_required(STDOUT_PATTERNS, process.stdout)
                        if status == "ok"
                        else {
                            "filter_core_ms": 0,
                            "serialize_loop_ms": 0,
                            "raw_unique_edges": 0,
                            "retained_unique_edges": 0,
                        }
                    )
                    resources = parse_optional(RESOURCE_PATTERNS, resource_text)
                    generated_rows = 0
                    generated_bytes = 0
                    generated_sha256 = ""
                    canonical_validation = "not_run"
                    canonical_generated_sha256 = ""
                    canonical_supplied_sha256 = ""

                    if status == "ok":
                        (
                            generated_rows,
                            generated_bytes,
                            generated_sha256,
                        ) = file_rows_bytes_sha256(generated_path)
                        if (
                            repeat == 1
                            and not arguments.skip_canonical_validation
                        ):
                            canonical_generated = run_dir / "generated.sorted"
                            canonical_supplied = run_dir / "supplied.sorted"
                            canonical_generated_sha256 = canonical_sha256(
                                generated_path, canonical_generated, run_dir
                            )
                            canonical_supplied_sha256 = canonical_sha256(
                                supplied_path, canonical_supplied, run_dir
                            )
                            canonical_validation = (
                                "pass"
                                if canonical_generated_sha256
                                == canonical_supplied_sha256
                                else "fail"
                            )
                            validations[dataset] = {
                                "status": canonical_validation,
                                "generated_sha256": canonical_generated_sha256,
                                "supplied_sha256": canonical_supplied_sha256,
                                "generated_rows": generated_rows,
                                "generated_bytes": generated_bytes,
                            }
                        elif dataset in validations:
                            canonical_validation = validations[dataset]["status"]

                    unattributed_ms = max(
                        0.0,
                        wall_s * 1000.0
                        - parsed["filter_core_ms"]
                        - parsed["serialize_loop_ms"],
                    )
                    row: dict[str, Any] = {
                        "dataset": dataset,
                        "repeat": repeat,
                        "execution_position": position,
                        "status": status,
                        "exit_code": process.returncode,
                        "wall_s": f"{wall_s:.9f}",
                        "filter_core_ms": parsed["filter_core_ms"],
                        "serialize_loop_ms": parsed["serialize_loop_ms"],
                        "unattributed_read_group_close_ms": f"{unattributed_ms:.6f}",
                        "raw_unique_edges": parsed["raw_unique_edges"],
                        "retained_unique_edges": parsed["retained_unique_edges"],
                        "generated_rows": generated_rows,
                        "generated_bytes": generated_bytes,
                        "generated_sha256": generated_sha256,
                        "canonical_validation": canonical_validation,
                        "canonical_generated_sha256": canonical_generated_sha256,
                        "canonical_supplied_sha256": canonical_supplied_sha256,
                        **resources,
                        "cpu_affinity": arguments.cpu_affinity,
                        "raw_path": str(raw_path),
                        "supplied_filtered_path": str(supplied_path),
                        "source_sha256": source_sha256,
                        "executable_sha256": executable_sha256,
                        "started_at_utc": started.isoformat(),
                        "ended_at_utc": ended.isoformat(),
                    }
                    rows.append(row)
                    write_csv(runs_path, rows, RUN_FIELDS)
                    print(
                        f"{dataset} r{repeat}/{arguments.repeats} "
                        f"status={status} wall={wall_s:.3f}s "
                        f"filter={parsed['filter_core_ms']}ms "
                        f"io={parsed['serialize_loop_ms']}ms "
                        f"rss={resources['max_rss_kb']}KiB "
                        f"validation={canonical_validation}",
                        flush=True,
                    )
                    if status != "ok":
                        (output_dir / f"{dataset}_r{repeat}_stderr.txt").write_text(
                            process.stderr, encoding="utf-8"
                        )
                        return 1
                    if arguments.keep_generated:
                        destination = output_dir / f"{dataset}_r{repeat}_filtered.txt"
                        shutil.copy2(generated_path, destination)

    summary_rows = summarize(rows)
    if summary_rows:
        summary_fields = tuple(summary_rows[0].keys())
        write_csv(output_dir / "summary.csv", summary_rows, summary_fields)
    validation = {
        "status": (
            "pass"
            if len(validations) == len(selected)
            and all(item["status"] == "pass" for item in validations.values())
            else "fail"
        ),
        "datasets": validations,
        "observed_runs": len(rows),
        "expected_runs": len(selected) * arguments.repeats,
        "non_ok_runs": sum(row["status"] != "ok" for row in rows),
    }
    (output_dir / "validation.json").write_text(
        json.dumps(validation, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(validation, indent=2), flush=True)
    return 0 if validation["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
