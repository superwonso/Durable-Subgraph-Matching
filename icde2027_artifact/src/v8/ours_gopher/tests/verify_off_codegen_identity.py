#!/usr/bin/env python3
"""Fail-closed product/erased statistics-OFF code-generation identity gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shlex
import shutil
import subprocess
from pathlib import Path
from typing import Any


OFF_SOURCES = ("DagExecutor.cpp", "DirectEvaluator.cpp", "GopherMatcher.cpp")
REQUIRED_OFF_SYMBOLS = (
    "RuntimeKeyHash::operator()(RuntimeKey const&) const",
    "buildRuntimeKey(RuntimeKeyMode, DagNode const&, SearchState const&, int, int)",
    "DagExecutor::makeRuntimeKey(DagNode const&, SearchState const&, int, int) const",
    "DagExecutor::resolveBinding(int, SearchState const&, int, int) const",
    "DagExecutor::structuralMemoEnabled() const",
    "DagExecutor::temporalMemoEnabled() const",
    "DagExecutor::longestMemoEnabled() const",
    "DagExecutor::structuralV2ProjectionCanRepeat(DagNode const&, SearchState const&) const",
    "DirectEvaluator::resolveBinding(int, SearchState const&, int, int) const",
    "DagExecutor::structuralCandidatesSpecialized<false, false>",
    "DagExecutor::temporalSupportSpecialized<false, false>",
    "DagExecutor::evaluateCandidateNode<false, false>",
    "DagExecutor::evaluateIntervalNode<false, false>",
    "DagExecutor::evaluateLongestNode<false, false>",
    "DirectEvaluator::structuralCandidatesSpecialized<false>",
    "DirectEvaluator::temporalSupportSpecialized<false>",
    "GopherMatcher::runSpecialized<false, false>",
)
PROFILE_TRUE_SYMBOLS = (
    "DagExecutor::structuralCandidatesSpecialized<true, false>",
    "DagExecutor::temporalSupportSpecialized<true, false>",
    "DagExecutor::evaluateCandidateNode<true, false>",
    "DagExecutor::evaluateIntervalNode<true, false>",
    "DagExecutor::evaluateLongestNode<true, false>",
    "DagExecutor::structuralCandidatesSpecialized<true, true>",
    "DagExecutor::temporalSupportSpecialized<true, true>",
    "DagExecutor::evaluateCandidateNode<true, true>",
    "DagExecutor::evaluateIntervalNode<true, true>",
    "DagExecutor::evaluateLongestNode<true, true>",
    "DirectEvaluator::structuralCandidatesSpecialized<true>",
    "DirectEvaluator::temporalSupportSpecialized<true>",
    "GopherMatcher::runSpecialized<true, false>",
    "GopherMatcher::runSpecialized<true, true>",
)
PROFILE_EXACT_SYMBOLS = (
    "DagExecutor::structuralCandidatesSpecialized<false, true>",
    "DagExecutor::temporalSupportSpecialized<false, true>",
    "DagExecutor::evaluateCandidateNode<false, true>",
    "DagExecutor::evaluateIntervalNode<false, true>",
    "DagExecutor::evaluateLongestNode<false, true>",
    "GopherMatcher::runSpecialized<false, true>",
)
NORMALIZED_SYMBOL_RE = re.compile(
    r"RuntimeKeyHash::operator\(\)\(RuntimeKey const&\) const"
    r"|buildRuntimeKey\(RuntimeKeyMode, DagNode const&, "
    r"SearchState const&, int, int\)"
    r"|GopherMatcher::runSpecialized<false, false>"
    r"|DagExecutor::(?:structuralCandidatesSpecialized|"
    r"temporalSupportSpecialized|evaluateCandidateNode|"
    r"evaluateIntervalNode|evaluateLongestNode)<false, false>"
    r"|DagExecutor::(?:makeRuntimeKey|resolveBinding|"
    r"structuralV2ProjectionCanRepeat)\("
    r"|DagExecutor::(?:structuralMemoEnabled|temporalMemoEnabled|"
    r"longestMemoEnabled)\(\) const"
    r"|DirectEvaluator::(?:structuralCandidatesSpecialized|"
    r"temporalSupportSpecialized)<false>"
    r"|DirectEvaluator::resolveBinding\("
)
HEADER_RE = re.compile(r"^[0-9a-fA-F]+ <.*>:$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SPECIALIZATION_GATE_RELATIVE_PATH = (
    "tests/test_statistics_specialization_objdump.sh"
)
SPECIALIZATION_GATE_SHA256 = (
    "bcd08e723b49d871525e444283d05424b843357330012fd60bacdaef094f5cad"
)


class GateFailure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateFailure(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def text_sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def run_text(argv: list[str]) -> str:
    completed = subprocess.run(
        argv,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise GateFailure(
            f"command failed ({completed.returncode}): {' '.join(argv)}\n"
            f"{completed.stderr}"
        )
    return completed.stdout


def externally_bindable_functions(nm: str, path: Path) -> set[str]:
    """Return raw external function names without demangling ambiguity."""
    return set(externally_bindable_function_records(nm, path))


def externally_bindable_function_records(
    nm: str,
    path: Path,
) -> dict[str, tuple[int, int]]:
    symbols = run_text([
        nm,
        "--defined-only",
        "--print-size",
        "--format=posix",
        str(path),
    ])
    result: dict[str, tuple[int, int]] = {}
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] in {"T", "W"}:
            if len(fields) < 4:
                continue
            result[fields[0]] = (int(fields[2], 16), int(fields[3], 16))
    return result


def require_file(path: Path, label: str) -> Path:
    require(path.is_file(), f"missing {label}: {path}")
    return path


def run_statistics_specialization_gate(
    source_root: Path,
    objdump: str,
    probe: Path,
    archive: Path,
) -> None:
    gate = require_file(
        source_root / SPECIALIZATION_GATE_RELATIVE_PATH,
        "statistics specialization objdump gate",
    )
    require(
        sha256(gate) == SPECIALIZATION_GATE_SHA256,
        "statistics specialization objdump gate dependency hash drifted",
    )
    run_text([str(gate), objdump, str(probe), str(archive)])


def find_object(build: Path, source: str) -> Path:
    candidates = sorted(
        build.glob(
            f"CMakeFiles/ours_gopher_off_kernel.dir/**/{source}.o"
        )
    )
    require(
        len(candidates) == 1,
        f"expected one OFF object for {source}, found {len(candidates)}",
    )
    return candidates[0]


def define_value(tokens: list[str], name: str) -> str:
    prefix = f"-D{name}="
    values = [token[len(prefix):].strip('"') for token in tokens
              if token.startswith(prefix)]
    require(len(values) == 1, f"expected one {name} definition")
    require(SHA256_RE.fullmatch(values[0]) is not None,
            f"invalid {name}: {values[0]}")
    return values[0]


def compile_manifest(build: Path) -> dict[str, Any]:
    path = require_file(build / "compile_commands.json", "compile database")
    entries = json.loads(path.read_text(encoding="utf-8"))
    result: dict[str, Any] = {}
    for source in OFF_SOURCES:
        matches = [
            entry for entry in entries
            if Path(entry["file"]).name == source
            and "ours_gopher_off_kernel.dir" in entry.get("output", "")
        ]
        require(len(matches) == 1,
                f"expected one OFF compile command for {source}")
        entry = matches[0]
        if "arguments" in entry:
            tokens = list(entry["arguments"])
        else:
            tokens = shlex.split(entry["command"])
        normalized: list[str] = []
        skip_next = False
        for token in tokens:
            if skip_next:
                skip_next = False
                continue
            if token == "-o":
                skip_next = True
                continue
            normalized.append(token.replace(str(build), "<BUILD>"))
        require("-DDSM_GOPHER_OFF_TRANSLATION_UNIT=1" in normalized,
                f"{source} is not the OFF-only translation unit")
        require(not any("DSM_ERASE_SEARCH_STATISTICS" in token
                        for token in normalized),
                f"{source} leaked the erased profile definition")
        for required_flag in (
            "-O3", "-DNDEBUG", "-std=c++17", "-Wconversion",
            "-Wshadow", "-Werror", "-Wall", "-Wextra", "-Wpedantic",
        ):
            require(required_flag in normalized,
                    f"{source} missing canonical flag {required_flag}")
        result[source] = {
            "tokens": normalized,
            "source_identity": define_value(
                normalized, "DSM_GOPHER_SOURCE_IDENTITY"),
            "common_flags_identity": define_value(
                normalized, "DSM_GOPHER_COMMON_FLAGS_IDENTITY"),
        }
    return result


def canonical_source_identity(source_root: Path) -> str:
    source_names = {
        "CMakeLists.txt",
        "main.cpp",
        "CandidateDag.cpp",
        "DagExecutor.cpp",
        "DirectEvaluator.cpp",
        "GopherMatcher.cpp",
        "LabelProjection.cpp",
        "SearchStatistics.cpp",
        "query_decomposition.cpp",
        "temporal_cost_planner.cpp",
        "TDTree.cpp",
        "Utils.cpp",
    }
    source_names.update(path.name for path in source_root.glob("*.h"))
    material = "".join(
        f"{name}:{sha256(require_file(source_root / name, 'receipt source'))}\n"
        for name in sorted(source_names)
    )
    return text_sha256(material)


def normalize_final_off_disassembly(objdump: str, binary: Path) -> str:
    disassembly = run_text(
        [objdump, "-dC", "--no-show-raw-insn", str(binary)]
    )
    selected = False
    output: list[str] = []
    for raw_line in disassembly.splitlines():
        line = raw_line.rstrip()
        if HEADER_RE.fullmatch(line):
            selected = NORMALIZED_SYMBOL_RE.search(line) is not None
        if not selected:
            continue
        line = re.sub(r"^\s*[0-9a-fA-F]+:", "ADDR:", line)
        if re.match(r"^ADDR:\s+(?:cs )?nop(?:l|w)?(?:\s|$)", line):
            continue
        if re.match(r"^ADDR:\s+xchg\s+%ax,%ax$", line):
            continue
        line = re.sub(
            r"[-+]?0x[0-9a-fA-F]+\(%rip\)", "ADDR(%rip)", line
        )
        line = re.sub(r"\b[0-9a-fA-F]+\s+<", "ADDR <", line)
        line = re.sub(r"#\s+[0-9a-fA-F]+\s+<", "# ADDR <", line)
        line = re.sub(r"\+0x[0-9a-fA-F]+>", "+OFF>", line)
        output.append(line)
    normalized = "\n".join(output) + "\n"
    for symbol in REQUIRED_OFF_SYMBOLS:
        require(symbol in normalized,
                f"normalized binary disassembly missed {symbol}")
    return normalized


def normalize_symbol_disassembly(
    objdump: str,
    binary: Path,
    symbol: str,
    start: int,
    size: int,
) -> str:
    require(size > 0, f"linked OFF function has zero size: {symbol}")
    disassembly = run_text([
        objdump,
        "-d",
        "--no-show-raw-insn",
        f"--start-address={start}",
        f"--stop-address={start + size}",
        str(binary),
    ])
    output: list[str] = []
    for raw_line in disassembly.splitlines():
        line = raw_line.rstrip()
        if re.match(r"^\s*[0-9a-fA-F]+:", line) is None:
            continue
        line = re.sub(r"^\s*[0-9a-fA-F]+:", "ADDR:", line)
        if re.match(r"^ADDR:\s+(?:cs )?nop(?:l|w)?(?:\s|$)", line):
            continue
        if re.match(r"^ADDR:\s+xchg\s+%ax,%ax$", line):
            continue
        line = re.sub(
            r"[-+]?0x[0-9a-fA-F]+\(%rip\)", "ADDR(%rip)", line
        )
        line = re.sub(r"\b[0-9a-fA-F]+\s+<", "ADDR <", line)
        line = re.sub(r"#\s+[0-9a-fA-F]+\s+<", "# ADDR <", line)
        line = re.sub(r"\+0x[0-9a-fA-F]+>", "+OFF>", line)
        output.append(line)
    require(output, f"linked OFF function disassembly missed {symbol}")
    return "\n".join(output) + "\n"


def verify_all_linked_off_functions(
    nm: str,
    objdump: str,
    archive: Path,
    product_binary: Path,
    erased_binary: Path,
) -> dict[str, Any]:
    archive_functions = externally_bindable_functions(nm, archive)
    require(archive_functions, "OFF archive defined no external functions")
    product_records = externally_bindable_function_records(
        nm, product_binary)
    erased_records = externally_bindable_function_records(
        nm, erased_binary)
    product_linked = archive_functions & set(product_records)
    erased_linked = archive_functions & set(erased_records)
    require(product_linked == erased_linked,
            "linked OFF external-function sets differ across profiles")
    require(product_linked,
            "no OFF external function was linked into the binaries")

    normalized: list[str] = []
    for symbol in sorted(product_linked):
        product_body = normalize_symbol_disassembly(
            objdump, product_binary, symbol, *product_records[symbol])
        erased_body = normalize_symbol_disassembly(
            objdump, erased_binary, symbol, *erased_records[symbol])
        require(product_body == erased_body,
                f"linked OFF function differs across profiles: {symbol}")
        normalized.append(f"{symbol}\n{product_body}")

    unlinked = archive_functions - product_linked
    return {
        "off_external_function_count": len(archive_functions),
        "off_external_functions_sha256": text_sha256(
            "\n".join(sorted(archive_functions)) + "\n"),
        "linked_off_external_function_count": len(product_linked),
        "linked_off_external_functions_sha256": text_sha256(
            "\n".join(sorted(product_linked)) + "\n"),
        "unlinked_off_external_function_count": len(unlinked),
        "unlinked_off_external_functions_sha256": text_sha256(
            "\n".join(sorted(unlinked)) + ("\n" if unlinked else "")),
        "normalized_all_linked_off_functions_sha256": text_sha256(
            "".join(normalized)),
        "local_function_and_relocation_coverage":
            "exact_off_object_bytes",
    }


def verify_archive_symbols(nm: str, archive: Path) -> str:
    symbols = run_text([nm, "-C", "--defined-only", str(archive)])
    for symbol in REQUIRED_OFF_SYMBOLS:
        require(symbol in symbols, f"OFF archive missed {symbol}")
    for forbidden in (
        "runSpecialized<true,",
        "runSpecialized<false, true>",
        "structuralCandidatesSpecialized<true",
        "temporalSupportSpecialized<true",
        "structuralCandidatesSpecialized<false, true>",
        "temporalSupportSpecialized<false, true>",
        "evaluateCandidateNode<true",
        "evaluateIntervalNode<true",
        "evaluateLongestNode<true",
        "evaluateCandidateNode<false, true>",
        "evaluateIntervalNode<false, true>",
        "evaluateLongestNode<false, true>",
        "ExactRuntimeKeyCollector",
    ):
        require(forbidden not in symbols,
                f"OFF archive retained profile/diagnostic symbol {forbidden}")
    return text_sha256(symbols)


def verify_profile_symbols(
    nm: str,
    product_core: Path,
    erased_core: Path,
) -> dict[str, str]:
    product = run_text([nm, "-C", "--defined-only", str(product_core)])
    erased = run_text([nm, "-C", "--defined-only", str(erased_core)])
    for symbol in PROFILE_TRUE_SYMBOLS:
        require(symbol in product, f"product profile missed {symbol}")
        require(symbol not in erased, f"erased profile retained {symbol}")
    for symbol in PROFILE_EXACT_SYMBOLS:
        require(symbol in product, f"product profile missed {symbol}")
        require(symbol in erased, f"erased profile missed {symbol}")
    for symbols, label in ((product, "product"), (erased, "erased")):
        for symbol in REQUIRED_OFF_SYMBOLS:
            require(symbol not in symbols,
                    f"{label} profile duplicated shared OFF symbol {symbol}")
    return {
        "product_profile_symbols_sha256": text_sha256(product),
        "erased_profile_symbols_sha256": text_sha256(erased),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--product-build", required=True, type=Path)
    parser.add_argument("--erased-build", required=True, type=Path)
    parser.add_argument("--objdump", default="objdump")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    product_build = args.product_build.resolve(strict=True)
    erased_build = args.erased_build.resolve(strict=True)
    require(product_build != erased_build,
            "product and erased builds must be distinct")
    source_root = Path(__file__).resolve().parents[1]
    objdump = shutil.which(args.objdump)
    nm = shutil.which(args.nm)
    require(objdump is not None, f"objdump not found: {args.objdump}")
    require(nm is not None, f"nm not found: {args.nm}")

    product_manifest = compile_manifest(product_build)
    erased_manifest = compile_manifest(erased_build)
    require(product_manifest == erased_manifest,
            "OFF compile commands/definitions differ across profiles")
    identities = {
        item["source_identity"] for item in product_manifest.values()
    }
    common_identities = {
        item["common_flags_identity"] for item in product_manifest.values()
    }
    require(len(identities) == 1, "OFF source identity is not uniform")
    require(len(common_identities) == 1,
            "OFF common-flags identity is not uniform")
    source_identity = next(iter(identities))
    require(source_identity == canonical_source_identity(source_root),
            "embedded OFF source identity is stale or noncanonical")

    product_archive = require_file(
        product_build / "libours_gopher_off_kernel.a", "product OFF archive"
    )
    erased_archive = require_file(
        erased_build / "libours_gopher_off_kernel.a", "erased OFF archive"
    )
    archive_sha = sha256(product_archive)
    require(archive_sha == sha256(erased_archive),
            "product/erased OFF archives are not byte-identical")

    object_hashes: dict[str, str] = {}
    for source in OFF_SOURCES:
        product_object = find_object(product_build, source)
        erased_object = find_object(erased_build, source)
        product_sha = sha256(product_object)
        require(product_sha == sha256(erased_object),
                f"product/erased OFF object differs: {source}")
        object_hashes[source] = product_sha

    product_binary = require_file(
        product_build / "td_tree_gopher", "product binary"
    )
    erased_binary = require_file(
        erased_build / "td_tree_gopher", "erased binary"
    )
    product_core = require_file(
        product_build / "libours_gopher_core.a", "product profile archive"
    )
    erased_core = require_file(
        erased_build / "libours_gopher_core.a", "erased profile archive"
    )
    archive_symbols_sha = verify_archive_symbols(nm, product_archive)
    profile_symbol_hashes = verify_profile_symbols(
        nm, product_core, erased_core
    )

    product_disassembly = normalize_final_off_disassembly(
        objdump, product_binary
    )
    erased_disassembly = normalize_final_off_disassembly(
        objdump, erased_binary
    )
    require(product_disassembly == erased_disassembly,
            "normalized final-binary OFF disassembly differs")
    all_linked_function_hashes = verify_all_linked_off_functions(
        nm,
        objdump,
        product_archive,
        product_binary,
        erased_binary,
    )

    probe = require_file(
        product_build / "test_statistics_specialization",
        "product specialization probe",
    )
    run_statistics_specialization_gate(
        source_root, objdump, probe, product_archive
    )

    receipt: dict[str, Any] = {
        "schema": "gopher_off_codegen_identity_v1",
        "status": "PASS",
        "source_identity_sha256": source_identity,
        "common_flags_identity_sha256": next(iter(common_identities)),
        "off_compile_manifest_sha256": text_sha256(json.dumps(
            product_manifest, sort_keys=True, separators=(",", ":")
        )),
        "off_object_sha256": object_hashes,
        "off_archive_sha256": archive_sha,
        "off_archive_symbols_sha256": archive_symbols_sha,
        "normalized_final_off_disassembly_sha256": text_sha256(
            product_disassembly
        ),
        **all_linked_function_hashes,
        "product_binary_sha256": sha256(product_binary),
        "erased_binary_sha256": sha256(erased_binary),
        "product_profile_archive_sha256": sha256(product_core),
        "erased_profile_archive_sha256": sha256(erased_core),
        **profile_symbol_hashes,
    }
    serialized = json.dumps(receipt, sort_keys=True, indent=2) + "\n"
    if args.output is not None:
        output = args.output
        require(output.is_absolute(), "receipt output must be absolute")
        require(not output.exists(), f"refusing to overwrite receipt: {output}")
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(serialized, encoding="utf-8")
    print(serialized, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GateFailure, OSError, ValueError, json.JSONDecodeError) as error:
        print(json.dumps({
            "schema": "gopher_off_codegen_identity_v1",
            "status": "FAIL",
            "error": str(error),
        }, sort_keys=True))
        raise SystemExit(1)
