#!/usr/bin/env python3
"""Validate completeness, correctness, provenance, and GPU execution evidence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
from typing import Any


ROOT = Path(__file__).resolve().parents[1]


def expand(value: str) -> str:
    return value.replace("{root}", str(ROOT))


def as_bool(value: Any) -> bool | None:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered in {"true", "1", "yes", "pass"}:
            return True
        if lowered in {"false", "0", "no", "fail"}:
            return False
    return None


def configured_threads(spec: dict[str, Any]) -> int | None:
    explicit = spec.get("threads")
    if isinstance(explicit, int) and explicit > 0:
        return explicit
    flags = [str(value) for value in spec.get("flags", [])]
    for index, flag in enumerate(flags):
        if flag == "--threads" and index + 1 < len(flags):
            if re.fullmatch(r"[1-9]\d*", flags[index + 1]):
                return int(flags[index + 1])
        match = re.fullmatch(r"--threads=([1-9]\d*)", flag)
        if match:
            return int(match.group(1))
    return None


def sha256_file(path: Path, cache: dict[Path, str]) -> str:
    resolved = path.resolve()
    if resolved in cache:
        return cache[resolved]
    digest = hashlib.sha256()
    with resolved.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    cache[resolved] = digest.hexdigest()
    return cache[resolved]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as source:
        return list(csv.DictReader(source))


def case_algorithm_value(
    case: dict[str, Any],
    key: str,
    algorithm: str,
) -> Any:
    overrides = case.get(f"{key}_by_algorithm", {})
    if overrides is None:
        overrides = {}
    if not isinstance(overrides, dict):
        raise ValueError(f"{key}_by_algorithm must be an object")
    return overrides.get(algorithm, case.get(key))


def validate(result_dir: Path, manifest_path: Path) -> dict[str, Any]:
    problems: list[str] = []
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    algorithms = {
        name: spec
        for name, spec in manifest["algorithms"].items()
        if spec.get("enabled", True)
    }
    cases = [
        case for case in manifest["cases"] if case.get("enabled", True)
    ]
    cases_by_id = {str(case["id"]): case for case in cases}
    runs = read_csv(result_dir / "runs.csv")
    summaries = read_csv(result_dir / "summary.csv")
    correctness = read_csv(result_dir / "correctness.csv")

    expected: set[tuple[str, str, int]] = set()
    for case in cases:
        repeats = int(case.get("repeats", manifest.get("repeats", 3)))
        allowed = set(case.get("algorithms", algorithms))
        for algorithm in allowed:
            if algorithm not in algorithms:
                problems.append(
                    f"{case['id']}: unknown/disabled algorithm {algorithm}"
                )
                continue
            for repeat in range(1, repeats + 1):
                expected.add((str(case["id"]), algorithm, repeat))

    observed: list[tuple[str, str, int]] = []
    for row in runs:
        try:
            observed.append(
                (
                    row["case_id"],
                    row["algorithm"],
                    int(row["repeat"]),
                )
            )
        except (KeyError, ValueError) as error:
            problems.append(f"malformed run identity: {error}")
    observed_set = set(observed)
    duplicates = len(observed) - len(observed_set)
    if duplicates:
        problems.append(f"{duplicates} duplicate run identities")
    missing = sorted(expected - observed_set)
    extra = sorted(observed_set - expected)
    if missing:
        problems.append(f"{len(missing)} missing run identities")
    if extra:
        problems.append(f"{len(extra)} unexpected run identities")

    non_ok = [row for row in runs if row.get("status") != "ok"]
    if non_ok:
        problems.append(f"{len(non_ok)} non-ok runs")
    non_pass = [row for row in correctness if row.get("status") != "pass"]
    if non_pass:
        problems.append(f"{len(non_pass)} non-pass correctness groups")
    inconsistent = [
        row
        for row in summaries
        if as_bool(row.get("repeat_match_count_consistent")) is not True
    ]
    if inconsistent:
        problems.append(f"{len(inconsistent)} repeat-inconsistent summaries")

    hash_cache: dict[Path, str] = {}
    hash_fields = (
        ("data", "data_sha256"),
        ("query", "query_sha256"),
        ("command executable", "executable_sha256"),
    )
    for row in runs:
        algorithm = row.get("algorithm", "")
        spec = algorithms.get(algorithm, {})
        case_id = row.get("case_id", "")
        case = cases_by_id.get(case_id)
        if case is None:
            problems.append(f"{case_id}/{algorithm}: unknown case")
            continue
        expected_threads = configured_threads(spec)
        recorded_threads = row.get("threads", "")
        if expected_threads is not None and recorded_threads != str(
            expected_threads
        ):
            problems.append(
                f"{row.get('case_id')}/{algorithm}: threads="
                f"{recorded_threads}, expected {expected_threads}"
            )

        paths = {
            "data": Path(row["data"]),
            "query": Path(row["query"]),
            "command executable": Path(expand(str(spec.get("executable", "")))),
        }
        try:
            expected_data_value = case_algorithm_value(
                case, "data", algorithm
            )
            expected_variant = case_algorithm_value(
                case, "input_variant", algorithm
            )
        except ValueError as error:
            problems.append(f"{case_id}/{algorithm}: {error}")
            expected_data_value = None
            expected_variant = None
        if expected_data_value is None:
            problems.append(f"{case_id}/{algorithm}: manifest data missing")
        else:
            expected_data = Path(
                expand(str(expected_data_value))
            ).resolve()
            if paths["data"].resolve() != expected_data:
                problems.append(
                    f"{case_id}/{algorithm}: recorded data path "
                    f"{paths['data'].resolve()} != expected {expected_data}"
                )
        if (
            expected_variant is not None
            and row.get("input_variant", "") != str(expected_variant)
        ):
            problems.append(
                f"{case_id}/{algorithm}: input_variant="
                f"{row.get('input_variant', '')}, expected {expected_variant}"
            )
        for label, hash_field in hash_fields:
            recorded_hash = row.get(hash_field, "")
            path = paths[label]
            if not recorded_hash:
                problems.append(
                    f"{row.get('case_id')}/{algorithm}: missing {hash_field}"
                )
            elif not path.is_file():
                problems.append(f"missing {label} file: {path}")
            elif recorded_hash != sha256_file(path, hash_cache):
                problems.append(
                    f"{row.get('case_id')}/{algorithm}: {hash_field} mismatch"
                )

        is_gpu = "gpu" in algorithm.lower()
        if is_gpu:
            if as_bool(row.get("gopher_fallback_used")) is not False:
                problems.append(
                    f"{row.get('case_id')}/{algorithm}: fallback not false"
                )
            if row.get("gopher_device_architecture") != "gfx1151":
                problems.append(
                    f"{row.get('case_id')}/{algorithm}: unexpected GPU arch"
                )
            if float(row.get("gopher_gpu_kernel_ms", "0") or 0) <= 0:
                problems.append(
                    f"{row.get('case_id')}/{algorithm}: missing GPU kernel time"
                )
            requested = as_bool(
                row.get("gopher_verification_requested")
            )
            passed = as_bool(row.get("gopher_verification_passed"))
            if requested is True and passed is not True:
                problems.append(
                    f"{row.get('case_id')}/{algorithm}: GPU verification failed"
                )

    recorded_manifest_hash = (
        result_dir / "manifest.used.sha256"
    ).read_text(encoding="ascii").strip()
    actual_manifest_hash = sha256_file(manifest_path, hash_cache)
    if recorded_manifest_hash != actual_manifest_hash:
        problems.append("used/current manifest SHA-256 mismatch")

    return {
        "result_dir": str(result_dir.resolve()),
        "manifest": str(manifest_path.resolve()),
        "expected_runs": len(expected),
        "observed_runs": len(runs),
        "summaries": len(summaries),
        "correctness_groups": len(correctness),
        "gpu_runs": sum("gpu" in row.get("algorithm", "").lower() for row in runs),
        "non_ok_runs": len(non_ok),
        "non_pass_correctness_groups": len(non_pass),
        "repeat_inconsistent_summaries": len(inconsistent),
        "duplicate_identities": duplicates,
        "missing_identities": len(missing),
        "unexpected_identities": len(extra),
        "hashed_files": len(hash_cache),
        "problems": problems[:100],
        "validation": "passed" if not problems else "failed",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("result_dir", type=Path)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).with_name("experiment_manifest.json"),
    )
    args = parser.parse_args()
    receipt = validate(args.result_dir.resolve(), args.manifest.resolve())
    print(json.dumps(receipt, ensure_ascii=False, indent=2, sort_keys=True))
    return 0 if receipt["validation"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
