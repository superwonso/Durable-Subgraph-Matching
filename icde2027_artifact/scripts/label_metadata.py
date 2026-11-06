#!/usr/bin/env python3
"""Deterministic Milestone-4 label maps and workload metadata.

This helper deliberately does no matching and records no performance timing.
It consumes a three-column temporal graph (``source target timestamp``) and the
labelled two-token query format used by the matchers (for example,
``q0:A q1:B``).  Canonically ordered inputs use a streaming path; unordered
inputs are sorted exactly before metadata is computed.

The temporal-support density for a query arc ``a -> b`` is

    sum_{e in E[a,b]} |W_k(e)|
    --------------------------------------------- ,
    |E[a,b]| * max(0, t_max - t_min - k + 2)

where ``E[a,b]`` is the set of unique directed data edges whose endpoints have
labels ``a`` and ``b``, and ``W_k(e)`` is the set of global window starts at
which edge ``e`` is present for all ``k`` consecutive timestamps.  The query
value is the unweighted arithmetic mean over its unique directed arcs.  An
empty eligible-edge set (or an empty global window-start universe) contributes
zero.  Exact integer fractions are emitted alongside the decimal values.

The explicit global label-map artifact supports cardinalities 5, 20, and 100
and is intended to be the single version-controlled input shared by all
executors.  ``stable-id`` mirrors the C++ SplitMix64-derived control (exactly at
its current cardinality five) and generalizes the same modulo rule.  A small
query-bound projection selects at most five global classes for runtime A--E;
all other classes are invalid.  The recommended skewed scheme ranks vertices
by unique directed incidence degree and assigns Zipf(alpha=1.2) quotas in
contiguous rank blocks.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation, localcontext
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import tempfile
from typing import Iterable, Mapping, Sequence


SCHEMA_VERSION = 1
LABEL_MAP_KIND = "dsm-explicit-vertex-label-map"
METADATA_KIND = "dsm-m4-workload-metadata"
UINT32_MAX = (1 << 32) - 1
INT32_MAX = (1 << 31) - 1
INT32_MIN = -(1 << 31)
UINT64_MASK = (1 << 64) - 1
DEFAULT_LABEL_COUNT = 5
RUNTIME_LABEL_SLOTS = 5
DEFAULT_LABEL_SEED = 42
DEGREE_ZIPF_EXPONENT = Decimal("1.2")


class MetadataError(ValueError):
    """Raised when an input cannot be used for a frozen metadata artifact."""


@dataclass(frozen=True)
class TemporalGraphScan:
    path: Path
    sha256: str
    size_bytes: int
    input_record_count: int
    unique_temporal_occurrence_count: int
    unique_directed_edge_count: int
    vertices: tuple[int, ...]
    degree_by_vertex: Mapping[int, int]
    minimum_timestamp: int | None
    maximum_timestamp: int | None
    input_order: str
    # Populated only for an out-of-order input.  Canonical inputs keep the
    # streaming two-pass path and therefore do not retain all occurrences.
    sorted_records: tuple[tuple[int, int, int], ...] | None = None


@dataclass(frozen=True, order=True)
class QueryArc:
    source: str
    target: str
    source_label: str
    target_label: str


@dataclass(frozen=True)
class QueryGraph:
    path: Path
    sha256: str
    size_bytes: int
    labels_by_vertex: Mapping[str, str]
    arcs: tuple[QueryArc, ...]


@dataclass(frozen=True)
class FrozenDensityThresholds:
    """Prospectively chosen half-open low/medium/high boundaries."""

    low_medium: Decimal
    medium_high: Decimal

    @classmethod
    def from_text(cls, low_medium: str, medium_high: str) -> "FrozenDensityThresholds":
        try:
            result = cls(Decimal(low_medium), Decimal(medium_high))
        except InvalidOperation as error:
            raise MetadataError("density thresholds must be finite decimals") from error
        result.validate()
        return result

    def validate(self) -> None:
        if not self.low_medium.is_finite() or not self.medium_high.is_finite():
            raise MetadataError("density thresholds must be finite decimals")
        if not Decimal(0) < self.low_medium < self.medium_high < Decimal(1):
            raise MetadataError(
                "density thresholds must satisfy 0 < low_medium < medium_high < 1"
            )

    def classify(self, value: Fraction) -> str:
        self.validate()
        if value < Fraction(self.low_medium):
            return "low"
        if value < Fraction(self.medium_high):
            return "medium"
        return "high"

    def as_json(self) -> dict[str, object]:
        return {
            "bin_intervals": {
                "high": "[medium_high, 1]",
                "low": "[0, low_medium)",
                "medium": "[low_medium, medium_high)",
            },
            "low_medium": decimal_text(self.low_medium),
            "medium_high": decimal_text(self.medium_high),
        }


@dataclass(frozen=True)
class LabelScheme:
    scheme_id: str
    seed: int
    label_count: int
    parameters: Mapping[str, object]

    def as_json(self) -> dict[str, object]:
        return {
            "id": self.scheme_id,
            "label_count": self.label_count,
            "parameters": dict(self.parameters),
            "seed": self.seed,
        }


@dataclass(frozen=True)
class LabelMapReceipt:
    file_sha256: str
    mapping_sha256: str
    vertex_count: int
    classes: tuple[dict[str, object], ...]
    normalized_entropy: float
    scheme: LabelScheme

    def as_json(self) -> dict[str, object]:
        return {
            "artifact_kind": LABEL_MAP_KIND,
            "classes": list(self.classes),
            "file_sha256": self.file_sha256,
            "mapping_digest": {
                "algorithm": "sha256",
                "value": self.mapping_sha256,
            },
            "normalized_entropy": self.normalized_entropy,
            "schema_version": SCHEMA_VERSION,
            "scheme": self.scheme.as_json(),
            "vertex_count": self.vertex_count,
        }


@dataclass(frozen=True)
class RuntimeProjection:
    """Projection from global classes to the current executor's A--E slots."""

    selected_global_classes: tuple[int, ...]
    runtime_slot_count: int = RUNTIME_LABEL_SLOTS

    def validate(self, global_class_count: int) -> None:
        if not 1 <= len(self.selected_global_classes) <= self.runtime_slot_count:
            raise MetadataError("a runtime projection must select between one and five classes")
        if len(set(self.selected_global_classes)) != len(self.selected_global_classes):
            raise MetadataError("runtime projection global classes must be unique")
        if any(
            global_class < 0 or global_class >= global_class_count
            for global_class in self.selected_global_classes
        ):
            raise MetadataError("runtime projection contains an out-of-range global class")

    def as_json(self) -> dict[str, object]:
        return {
            "invalid_runtime_class_id": -1,
            "local_slots": [
                {
                    "global_class_id": global_class,
                    "runtime_class_id": runtime_class,
                    "runtime_label": _runtime_label_name(runtime_class),
                }
                for runtime_class, global_class in enumerate(self.selected_global_classes)
            ],
            "nonselected_global_class_policy": "invalid",
            "runtime_slot_count": self.runtime_slot_count,
            "selected_global_classes_in_A_to_E_order": list(
                self.selected_global_classes
            ),
        }


@dataclass(frozen=True)
class ProjectionReceipt:
    file_sha256: str
    projection: RuntimeProjection
    global_label_map_file_sha256: str
    global_label_map_mapping_sha256: str
    global_label_cardinality: int
    query_sha256: str

    def as_json(self) -> dict[str, object]:
        document = {
            "artifact_kind": "dsm-query-label-projection",
            "file_sha256": self.file_sha256,
            "global_label_cardinality": self.global_label_cardinality,
            "global_label_map_file_sha256": self.global_label_map_file_sha256,
            "global_label_map_mapping_digest": {
                "algorithm": "sha256",
                "value": self.global_label_map_mapping_sha256,
            },
            "query_sha256": self.query_sha256,
            "schema_version": SCHEMA_VERSION,
        }
        document.update(self.projection.as_json())
        return document


def decimal_text(value: Decimal) -> str:
    """Return one non-exponential representation of a finite Decimal."""

    if not value.is_finite():
        raise MetadataError("non-finite decimal")
    text = format(value.normalize(), "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return "0" if text in {"", "-0"} else text


def _runtime_label_name(class_id: int) -> str:
    if not 0 <= class_id < RUNTIME_LABEL_SLOTS:
        raise MetadataError("runtime class ID must fit the existing A--E slots")
    return chr(ord("A") + class_id)


def _validate_label_count(label_count: int) -> None:
    if not 1 <= label_count <= 100:
        raise MetadataError("global label_count must be between 1 and 100")


def _validate_seed(seed: int) -> None:
    if not 0 <= seed <= UINT32_MAX:
        raise MetadataError("seed must be an unsigned 32-bit integer")


def splitmix64_for_vertex(vertex_id: int, seed: int) -> int:
    """Mirror ``stableIdLabel`` in ``ours/Utils.cpp`` before its modulo."""

    _validate_seed(seed)
    if not 0 <= vertex_id <= INT32_MAX:
        raise MetadataError("vertex IDs must be non-negative signed 32-bit integers")
    mixed = ((seed & UINT32_MAX) << 32) | (vertex_id & UINT32_MAX)
    mixed = (mixed + 0x9E3779B97F4A7C15) & UINT64_MASK
    mixed = ((mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9) & UINT64_MASK
    mixed = ((mixed ^ (mixed >> 27)) * 0x94D049BB133111EB) & UINT64_MASK
    return (mixed ^ (mixed >> 31)) & UINT64_MASK


def _parse_temporal_record(raw_line: bytes, line_number: int) -> tuple[int, int, int] | None:
    try:
        line = raw_line.decode("utf-8")
    except UnicodeDecodeError as error:
        raise MetadataError(f"temporal graph line {line_number} is not UTF-8") from error
    content = line.split("#", 1)[0].strip()
    if not content:
        return None
    fields = content.split()
    if len(fields) != 3:
        raise MetadataError(
            f"temporal graph line {line_number} must contain exactly three integers"
        )
    try:
        source, target, timestamp = (int(field, 10) for field in fields)
    except ValueError as error:
        raise MetadataError(
            f"temporal graph line {line_number} must contain exactly three integers"
        ) from error
    if not 0 <= source <= INT32_MAX or not 0 <= target <= INT32_MAX:
        raise MetadataError(
            f"temporal graph line {line_number} has an invalid signed 32-bit vertex ID"
        )
    if not INT32_MIN <= timestamp <= INT32_MAX:
        raise MetadataError(
            f"temporal graph line {line_number} has an invalid signed 32-bit timestamp"
        )
    return source, target, timestamp


def scan_temporal_graph(path: str | Path) -> TemporalGraphScan:
    """Validate and summarize a temporal graph, reordering exactly if needed."""

    graph_path = Path(path)
    digest = hashlib.sha256()
    size_bytes = 0
    input_records = 0
    unique_occurrences = 0
    unique_edges = 0
    degrees: dict[int, int] = {}
    minimum_timestamp: int | None = None
    maximum_timestamp: int | None = None
    previous: tuple[int, int, int] | None = None
    previous_edge: tuple[int, int] | None = None

    with graph_path.open("rb") as source_file:
        for line_number, raw_line in enumerate(source_file, start=1):
            digest.update(raw_line)
            size_bytes += len(raw_line)
            record = _parse_temporal_record(raw_line, line_number)
            if record is None:
                continue
            if previous is not None and record < previous:
                # Restart once on the exact in-memory fallback.  Inputs that
                # are already canonical retain the memory-bounded path.
                return _scan_reordered_temporal_graph(graph_path)
            input_records += 1
            if record != previous:
                unique_occurrences += 1
            source, target, timestamp = record
            edge = (source, target)
            if edge != previous_edge:
                unique_edges += 1
                degrees[source] = degrees.get(source, 0) + 1
                degrees[target] = degrees.get(target, 0) + 1
                previous_edge = edge
            minimum_timestamp = (
                timestamp if minimum_timestamp is None else min(minimum_timestamp, timestamp)
            )
            maximum_timestamp = (
                timestamp if maximum_timestamp is None else max(maximum_timestamp, timestamp)
            )
            previous = record

    if input_records == 0:
        raise MetadataError("temporal graph contains no records")
    vertices = tuple(sorted(degrees))
    return TemporalGraphScan(
        path=graph_path,
        sha256=digest.hexdigest(),
        size_bytes=size_bytes,
        input_record_count=input_records,
        unique_temporal_occurrence_count=unique_occurrences,
        unique_directed_edge_count=unique_edges,
        vertices=vertices,
        degree_by_vertex=degrees,
        minimum_timestamp=minimum_timestamp,
        maximum_timestamp=maximum_timestamp,
        input_order="lexicographic",
        sorted_records=None,
    )


def _scan_reordered_temporal_graph(path: Path) -> TemporalGraphScan:
    """Exact fallback for a valid three-column file whose records are unordered."""

    digest = hashlib.sha256()
    size_bytes = 0
    records: list[tuple[int, int, int]] = []
    with path.open("rb") as source_file:
        for line_number, raw_line in enumerate(source_file, start=1):
            digest.update(raw_line)
            size_bytes += len(raw_line)
            record = _parse_temporal_record(raw_line, line_number)
            if record is not None:
                records.append(record)
    if not records:
        raise MetadataError("temporal graph contains no records")
    records.sort()
    degrees: dict[int, int] = {}
    unique_occurrences = 0
    unique_edges = 0
    previous: tuple[int, int, int] | None = None
    previous_edge: tuple[int, int] | None = None
    minimum_timestamp: int | None = None
    maximum_timestamp: int | None = None
    for record in records:
        if record != previous:
            unique_occurrences += 1
        source, target, timestamp = record
        edge = (source, target)
        if edge != previous_edge:
            unique_edges += 1
            degrees[source] = degrees.get(source, 0) + 1
            degrees[target] = degrees.get(target, 0) + 1
            previous_edge = edge
        minimum_timestamp = (
            timestamp if minimum_timestamp is None else min(minimum_timestamp, timestamp)
        )
        maximum_timestamp = (
            timestamp if maximum_timestamp is None else max(maximum_timestamp, timestamp)
        )
        previous = record
    return TemporalGraphScan(
        path=path,
        sha256=digest.hexdigest(),
        size_bytes=size_bytes,
        input_record_count=len(records),
        unique_temporal_occurrence_count=unique_occurrences,
        unique_directed_edge_count=unique_edges,
        vertices=tuple(sorted(degrees)),
        degree_by_vertex=degrees,
        minimum_timestamp=minimum_timestamp,
        maximum_timestamp=maximum_timestamp,
        input_order="arbitrary-reordered-exactly",
        sorted_records=tuple(records),
    )


def _parse_query_token(token: str, line_number: int) -> tuple[str, str]:
    if ":" in token:
        vertex, label = token.rsplit(":", 1)
    else:
        vertex = label = token
    if not vertex or len(label) != 1 or not "A" <= label <= "Z":
        raise MetadataError(
            f"query line {line_number} has an invalid vertex:label token"
        )
    return vertex, label


def parse_query_graph(
    path: str | Path,
    runtime_label_count: int = RUNTIME_LABEL_SLOTS,
) -> QueryGraph:
    """Parse and canonically order the labelled directed query arcs."""

    if not 1 <= runtime_label_count <= RUNTIME_LABEL_SLOTS:
        raise MetadataError("runtime_label_count must be between one and five")
    query_path = Path(path)
    digest = hashlib.sha256()
    size_bytes = 0
    labels: dict[str, str] = {}
    arcs: set[QueryArc] = set()
    with query_path.open("rb") as query_file:
        for line_number, raw_line in enumerate(query_file, start=1):
            digest.update(raw_line)
            size_bytes += len(raw_line)
            try:
                line = raw_line.decode("utf-8")
            except UnicodeDecodeError as error:
                raise MetadataError(f"query line {line_number} is not UTF-8") from error
            fields = line.split("#", 1)[0].split()
            if not fields:
                continue
            if len(fields) != 2:
                raise MetadataError(
                    f"query line {line_number} must contain exactly two vertex tokens"
                )
            source, source_label = _parse_query_token(fields[0], line_number)
            target, target_label = _parse_query_token(fields[1], line_number)
            for vertex, label in ((source, source_label), (target, target_label)):
                if ord(label) - ord("A") >= runtime_label_count:
                    raise MetadataError(
                        f"query line {line_number} uses label {label} outside the label map"
                    )
                previous_label = labels.setdefault(vertex, label)
                if previous_label != label:
                    raise MetadataError(
                        f"query vertex {vertex!r} is used with conflicting labels"
                    )
            arcs.add(QueryArc(source, target, source_label, target_label))
    if not arcs:
        raise MetadataError("query graph contains no arcs")
    return QueryGraph(
        path=query_path,
        sha256=digest.hexdigest(),
        size_bytes=size_bytes,
        labels_by_vertex=dict(sorted(labels.items())),
        arcs=tuple(sorted(arcs)),
    )


def stable_id_label_map(
    vertices: Iterable[int],
    seed: int = DEFAULT_LABEL_SEED,
    label_count: int = DEFAULT_LABEL_COUNT,
) -> tuple[dict[int, int], LabelScheme]:
    """Build the exact uniform stable-ID control used by the C++ executors."""

    _validate_seed(seed)
    _validate_label_count(label_count)
    ordered = sorted(set(vertices))
    mapping = {
        vertex: splitmix64_for_vertex(vertex, seed) % label_count
        for vertex in ordered
    }
    scheme = LabelScheme(
        scheme_id="stable-id-splitmix64-v1",
        seed=seed,
        label_count=label_count,
        parameters={
            "assignment": "splitmix64((uint32(seed) << 32) | uint32(vertex_id)) mod label_count",
            "control_role": "uniform-stable-id",
            "vertex_set_invariant": True,
        },
    )
    return mapping, scheme


def default_degree_weights(label_count: int) -> tuple[Decimal, ...]:
    """Return normalized Zipf(alpha=1.2) quotas for a global cardinality."""

    _validate_label_count(label_count)
    with localcontext() as context:
        context.prec = 80
        raw = [
            context.power(Decimal(rank), -DEGREE_ZIPF_EXPONENT)
            for rank in range(1, label_count + 1)
        ]
        total = sum(raw)
        weights = [value / total for value in raw]
    with localcontext() as context:
        context.prec = 256
        weights[-1] = Decimal(1) - sum(weights[:-1])
        if any(weight <= 0 for weight in weights) or sum(weights) != Decimal(1):
            raise AssertionError("default degree weights are not a probability vector")
    return tuple(weights)


def _apportion_class_counts(vertex_count: int, weights: Sequence[Decimal]) -> list[int]:
    class_count = len(weights)
    if vertex_count < class_count:
        # The requested cardinality is infeasible; retain a deterministic
        # prefix so the artifact reports that fact through zero class counts.
        return [1 if class_id < vertex_count else 0 for class_id in range(class_count)]
    counts = [1] * class_count
    remaining = vertex_count - class_count
    with localcontext() as context:
        context.prec = 256
        ideals = [Decimal(remaining) * weight for weight in weights]
    floors = [int(ideal) for ideal in ideals]
    for class_id, floor in enumerate(floors):
        counts[class_id] += floor
    left = remaining - sum(floors)
    remainder_order = sorted(
        range(class_count),
        key=lambda class_id: (-(ideals[class_id] - floors[class_id]), class_id),
    )
    for class_id in remainder_order[:left]:
        counts[class_id] += 1
    if sum(counts) != vertex_count:
        raise AssertionError("Hamilton apportionment did not conserve vertices")
    return counts


def degree_correlated_label_map(
    degree_by_vertex: Mapping[int, int],
    seed: int = DEFAULT_LABEL_SEED,
    label_count: int = DEFAULT_LABEL_COUNT,
) -> tuple[dict[int, int], LabelScheme]:
    """Assign high-degree vertices to low-ID classes using skewed rank blocks."""

    _validate_seed(seed)
    _validate_label_count(label_count)
    normalized_weights = default_degree_weights(label_count)
    if any(not weight.is_finite() or weight <= 0 for weight in normalized_weights):
        raise MetadataError("degree weights must be finite and positive")
    with localcontext() as context:
        context.prec = 256
        if sum(normalized_weights) != Decimal(1):
            raise MetadataError("degree weights must sum exactly to 1")
    for vertex, degree in degree_by_vertex.items():
        if not 0 <= vertex <= INT32_MAX or degree < 0:
            raise MetadataError("degree map contains an invalid vertex or degree")

    ranked_vertices = sorted(
        degree_by_vertex,
        key=lambda vertex: (
            -degree_by_vertex[vertex],
            splitmix64_for_vertex(vertex, seed),
            vertex,
        ),
    )
    counts = _apportion_class_counts(len(ranked_vertices), normalized_weights)
    mapping: dict[int, int] = {}
    offset = 0
    for class_id, count in enumerate(counts):
        for vertex in ranked_vertices[offset : offset + count]:
            mapping[vertex] = class_id
        offset += count
    if offset != len(ranked_vertices) or len(mapping) != len(ranked_vertices):
        raise AssertionError("degree-correlated assignment did not cover every vertex")

    scheme = LabelScheme(
        scheme_id="degree-correlated-zipf12-rank-block-v1",
        seed=seed,
        label_count=label_count,
        parameters={
            "apportionment": "one-per-class-when-feasible, then Hamilton largest remainder",
            "class_order": "ascending global class_id",
            "degree_basis": (
                "unique directed out-degree plus unique directed in-degree; "
                "a self-loop contributes two incidences"
            ),
            "rank_order": (
                "descending degree, then splitmix64(seed, vertex_id), then vertex_id"
            ),
            "target_class_weights": [decimal_text(weight) for weight in normalized_weights],
            "weight_family": "normalized-Zipf",
            "zipf_exponent": decimal_text(DEGREE_ZIPF_EXPONENT),
        },
    )
    return mapping, scheme


def _mapping_digest(mapping: Mapping[int, int], label_count: int) -> str:
    digest = hashlib.sha256()
    digest.update(f"dsm-explicit-label-map-v1\t{label_count}\n".encode("ascii"))
    for vertex in sorted(mapping):
        digest.update(f"{vertex}\t{mapping[vertex]}\n".encode("ascii"))
    return digest.hexdigest()


def _distribution(
    mapping: Mapping[int, int], label_count: int
) -> tuple[tuple[dict[str, object], ...], float]:
    counts = [0] * label_count
    for vertex, class_id in mapping.items():
        if not 0 <= vertex <= INT32_MAX or not 0 <= class_id < label_count:
            raise MetadataError("label map contains an invalid vertex or class ID")
        counts[class_id] += 1
    total = len(mapping)
    entropy = 0.0
    if total:
        for count in counts:
            if count:
                probability = count / total
                entropy -= probability * math.log(probability)
    normalized_entropy = (
        entropy / math.log(label_count) if total and label_count > 1 else 0.0
    )
    classes = tuple(
        {
            "global_class_id": class_id,
            "vertex_count": count,
        }
        for class_id, count in enumerate(counts)
    )
    return classes, round(normalized_entropy, 15)


def write_explicit_label_map(
    path: str | Path,
    mapping: Mapping[int, int],
    scheme: LabelScheme,
) -> LabelMapReceipt:
    """Write a compact, stable, streaming JSON label map and return its receipt."""

    classes, normalized_entropy = _distribution(mapping, scheme.label_count)
    mapping_sha256 = _mapping_digest(mapping, scheme.label_count)
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    encoder = json.JSONEncoder(ensure_ascii=True, separators=(",", ":"), sort_keys=True)
    file_digest = hashlib.sha256()
    temporary_name: str | None = None

    def emit(handle: object, text: str) -> None:
        payload = text.encode("utf-8")
        handle.write(payload)  # type: ignore[attr-defined]
        file_digest.update(payload)

    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", dir=destination.parent, prefix=f".{destination.name}.", delete=False
        ) as output:
            temporary_name = output.name
            emit(output, "{")
            emit(output, '"artifact_kind":' + encoder.encode(LABEL_MAP_KIND))
            emit(output, ',"classes":' + encoder.encode(list(classes)))
            emit(output, ',"mapping":[')
            for index, vertex in enumerate(sorted(mapping)):
                if index:
                    emit(output, ",")
                emit(output, encoder.encode([vertex, mapping[vertex]]))
            emit(output, "]")
            emit(
                output,
                ',"mapping_digest":'
                + encoder.encode({"algorithm": "sha256", "value": mapping_sha256}),
            )
            emit(output, ',"normalized_entropy":' + encoder.encode(normalized_entropy))
            emit(output, ',"schema_version":' + encoder.encode(SCHEMA_VERSION))
            emit(output, ',"scheme":' + encoder.encode(scheme.as_json()))
            emit(output, ',"vertex_count":' + encoder.encode(len(mapping)))
            emit(output, "}\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_name, destination)
        temporary_name = None
    finally:
        if temporary_name is not None:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass

    return LabelMapReceipt(
        file_sha256=file_digest.hexdigest(),
        mapping_sha256=mapping_sha256,
        vertex_count=len(mapping),
        classes=classes,
        normalized_entropy=normalized_entropy,
        scheme=scheme,
    )


def project_global_label_map(
    global_mapping: Mapping[int, int],
    projection: RuntimeProjection,
    global_class_count: int,
) -> dict[int, int]:
    """Project selected global classes to A--E IDs; map every other class to -1."""

    projection.validate(global_class_count)
    local_by_global = {
        global_class: runtime_class
        for runtime_class, global_class in enumerate(projection.selected_global_classes)
    }
    projected: dict[int, int] = {}
    for vertex, global_class in global_mapping.items():
        if not 0 <= global_class < global_class_count:
            raise MetadataError("global label map contains an out-of-range class")
        projected[vertex] = local_by_global.get(global_class, -1)
    return projected


def validate_projection_for_query(
    query: QueryGraph,
    projection: RuntimeProjection,
    global_class_count: int,
) -> None:
    projection.validate(global_class_count)
    used_runtime_classes = {
        ord(label) - ord("A") for label in query.labels_by_vertex.values()
    }
    unavailable = sorted(
        class_id
        for class_id in used_runtime_classes
        if class_id >= len(projection.selected_global_classes)
    )
    if unavailable:
        names = ", ".join(_runtime_label_name(class_id) for class_id in unavailable)
        raise MetadataError(
            f"query uses runtime labels without selected global classes: {names}"
        )


def projection_document(
    projection: RuntimeProjection,
    global_label_map_receipt: LabelMapReceipt,
    query: QueryGraph,
) -> dict[str, object]:
    """Build the small per-query A--E projection consumed with a global map."""

    validate_projection_for_query(
        query, projection, global_label_map_receipt.scheme.label_count
    )
    document: dict[str, object] = {
        "artifact_kind": "dsm-query-label-projection",
        "global_label_cardinality": global_label_map_receipt.scheme.label_count,
        "global_label_map_file_sha256": global_label_map_receipt.file_sha256,
        "global_label_map_mapping_digest": {
            "algorithm": "sha256",
            "value": global_label_map_receipt.mapping_sha256,
        },
        "query_sha256": query.sha256,
        "schema_version": SCHEMA_VERSION,
    }
    document.update(projection.as_json())
    return document


def write_query_projection(
    path: str | Path,
    projection: RuntimeProjection,
    global_label_map_receipt: LabelMapReceipt,
    query: QueryGraph,
) -> ProjectionReceipt:
    """Write and verify a query-bound projection artifact."""

    document = projection_document(
        projection, global_label_map_receipt, query
    )
    file_sha256 = write_checked_json(path, document)
    return ProjectionReceipt(
        file_sha256=file_sha256,
        projection=projection,
        global_label_map_file_sha256=global_label_map_receipt.file_sha256,
        global_label_map_mapping_sha256=global_label_map_receipt.mapping_sha256,
        global_label_cardinality=global_label_map_receipt.scheme.label_count,
        query_sha256=query.sha256,
    )


def _fraction_json(value: Fraction) -> dict[str, int]:
    return {"denominator": value.denominator, "numerator": value.numerator}


def _decimal_value(value: Fraction) -> float:
    return round(float(value), 15)


def _label_pair_support(
    graph: TemporalGraphScan,
    mapping: Mapping[int, int],
    minimum_duration: int,
) -> dict[tuple[int, int], tuple[int, int]]:
    """Return label pair -> (eligible edges, supported edge-window pairs)."""

    if minimum_duration <= 0:
        raise MetadataError("minimum_duration must be positive")
    missing_vertices = [vertex for vertex in graph.vertices if vertex not in mapping]
    if len(mapping) != len(graph.vertices) or missing_vertices:
        missing = len(missing_vertices)
        extra = max(0, len(mapping) - (len(graph.vertices) - missing))
        raise MetadataError(
            f"label map must cover the graph exactly (missing={missing}, extra={extra})"
        )
    pair_counts: dict[tuple[int, int], list[int]] = {}
    current_edge: tuple[int, int] | None = None
    current_pair: tuple[int, int] | None = None
    previous_timestamp: int | None = None
    run_length = 0
    supported_starts = 0
    second_pass_digest = hashlib.sha256()
    second_pass_records = 0

    def finish_edge() -> None:
        if current_pair is None:
            return
        counts = pair_counts.setdefault(current_pair, [0, 0])
        counts[0] += 1
        counts[1] += supported_starts

    def consume(record: tuple[int, int, int]) -> None:
        nonlocal current_edge, current_pair, previous_timestamp
        nonlocal run_length, supported_starts, second_pass_records
        second_pass_records += 1
        source, target, timestamp = record
        edge = (source, target)
        if edge != current_edge:
            finish_edge()
            current_edge = edge
            current_pair = (mapping[source], mapping[target])
            previous_timestamp = timestamp
            run_length = 1
            supported_starts = 1 if minimum_duration == 1 else 0
        elif timestamp != previous_timestamp:
            if previous_timestamp is not None and timestamp == previous_timestamp + 1:
                run_length += 1
            else:
                run_length = 1
            if run_length >= minimum_duration:
                supported_starts += 1
            previous_timestamp = timestamp

    if graph.sorted_records is not None:
        for record in graph.sorted_records:
            consume(record)
        with graph.path.open("rb") as source_file:
            for raw_line in source_file:
                second_pass_digest.update(raw_line)
    else:
        with graph.path.open("rb") as source_file:
            previous_record: tuple[int, int, int] | None = None
            for line_number, raw_line in enumerate(source_file, start=1):
                second_pass_digest.update(raw_line)
                record = _parse_temporal_record(raw_line, line_number)
                if record is None:
                    continue
                if previous_record is not None and record < previous_record:
                    raise MetadataError("temporal graph changed to a non-canonical order")
                consume(record)
                previous_record = record
    finish_edge()
    if (
        second_pass_digest.hexdigest() != graph.sha256
        or second_pass_records != graph.input_record_count
    ):
        raise MetadataError("temporal graph changed while metadata was being computed")
    return {pair: (counts[0], counts[1]) for pair, counts in pair_counts.items()}


def temporal_support_density(
    graph: TemporalGraphScan,
    query: QueryGraph,
    mapping: Mapping[int, int],
    minimum_duration: int,
    thresholds: FrozenDensityThresholds,
) -> dict[str, object]:
    """Compute exact per-arc and mean temporal-support density metadata."""

    thresholds.validate()
    pair_support = _label_pair_support(graph, mapping, minimum_duration)
    assert graph.minimum_timestamp is not None and graph.maximum_timestamp is not None
    global_window_starts = max(
        0,
        graph.maximum_timestamp - graph.minimum_timestamp - minimum_duration + 2,
    )
    arc_rows: list[dict[str, object]] = []
    arc_fractions: list[Fraction] = []
    eligible_edge_sum = 0
    supported_pair_sum = 0
    for arc in query.arcs:
        source_class = ord(arc.source_label) - ord("A")
        target_class = ord(arc.target_label) - ord("A")
        eligible_edges, supported_pairs = pair_support.get(
            (source_class, target_class), (0, 0)
        )
        denominator = eligible_edges * global_window_starts
        density = Fraction(supported_pairs, denominator) if denominator else Fraction(0, 1)
        eligible_edge_sum += eligible_edges
        supported_pair_sum += supported_pairs
        arc_fractions.append(density)
        arc_rows.append(
            {
                "density": _decimal_value(density),
                "density_exact": _fraction_json(density),
                "eligible_directed_edge_count": eligible_edges,
                "source": arc.source,
                "source_label": arc.source_label,
                "supported_edge_window_start_count": supported_pairs,
                "target": arc.target,
                "target_label": arc.target_label,
                "total_eligible_edge_window_start_count": denominator,
            }
        )
    mean_density = sum(arc_fractions, Fraction(0, 1)) / len(arc_fractions)
    return {
        "aggregation": "unweighted arithmetic mean over unique directed query arcs",
        "bin": thresholds.classify(mean_density),
        "continuous_value": _decimal_value(mean_density),
        "definition": (
            "For each query arc, supported (eligible directed data edge, global "
            "length-k window start) pairs divided by all such eligible pairs; "
            "then the unweighted mean over query arcs."
        ),
        "definition_id": "mean-label-compatible-global-window-support-v1",
        "eligible_directed_edge_count_sum_over_arcs": eligible_edge_sum,
        "empty_eligible_set_policy": "zero",
        "exact_fraction": _fraction_json(mean_density),
        "minimum_duration_k": minimum_duration,
        "observation_window": {
            "global_length_k_window_start_count": global_window_starts,
            "maximum_timestamp": graph.maximum_timestamp,
            "minimum_timestamp": graph.minimum_timestamp,
        },
        "query_arc_count": len(query.arcs),
        "query_arcs": arc_rows,
        "supported_edge_window_start_count_sum_over_arcs": supported_pair_sum,
        "thresholds": thresholds.as_json(),
    }


def build_metadata_document(
    graph: TemporalGraphScan,
    query: QueryGraph,
    label_map_receipt: LabelMapReceipt,
    projection_receipt: ProjectionReceipt,
    density: Mapping[str, object],
) -> dict[str, object]:
    """Assemble a small provenance document without embedding the full map."""

    return {
        "artifact_kind": METADATA_KIND,
        "determinism_contract": {
            "label_map_is_explicit": True,
            "performance_measurements_used": False,
            "query_selection_after_runtime": False,
            "temporal_input_order": graph.input_order,
        },
        "inputs": {
            "query": {
                "path_argument": query.path.as_posix(),
                "sha256": query.sha256,
                "size_bytes": query.size_bytes,
            },
            "temporal_graph": {
                "path_argument": graph.path.as_posix(),
                "sha256": graph.sha256,
                "size_bytes": graph.size_bytes,
            },
        },
        "label_map": label_map_receipt.as_json(),
        "query_label_projection": projection_receipt.as_json(),
        "query": {
            "directed_arc_count": len(query.arcs),
            "vertex_count": len(query.labels_by_vertex),
        },
        "schema_version": SCHEMA_VERSION,
        "temporal_graph": {
            "input_record_count": graph.input_record_count,
            "maximum_timestamp": graph.maximum_timestamp,
            "minimum_timestamp": graph.minimum_timestamp,
            "unique_directed_edge_count": graph.unique_directed_edge_count,
            "unique_temporal_occurrence_count": graph.unique_temporal_occurrence_count,
            "vertex_count": len(graph.vertices),
        },
        "temporal_support_density": dict(density),
    }


def write_checked_json(path: str | Path, document: Mapping[str, object]) -> str:
    """Write stable pretty JSON, read it back, and return the file SHA-256."""

    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    payload = (
        json.dumps(document, ensure_ascii=True, indent=2, sort_keys=True, allow_nan=False)
        + "\n"
    ).encode("utf-8")
    destination.write_bytes(payload)
    if destination.read_bytes() != payload:
        raise OSError(f"failed to verify deterministic JSON write: {destination}")
    decoded = json.loads(payload)
    if decoded != document:
        raise MetadataError("JSON round-trip changed the metadata document")
    return hashlib.sha256(payload).hexdigest()


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--graph", required=True, type=Path)
    parser.add_argument("--query", required=True, type=Path)
    parser.add_argument("--minimum-duration", required=True, type=int, metavar="K")
    parser.add_argument(
        "--label-scheme",
        required=True,
        choices=("stable-id", "degree-correlated"),
    )
    parser.add_argument("--label-seed", type=int, default=DEFAULT_LABEL_SEED)
    parser.add_argument(
        "--label-count",
        type=int,
        choices=(5, 20, 100),
        default=DEFAULT_LABEL_COUNT,
        help="global label cardinality",
    )
    parser.add_argument(
        "--projection-global-classes",
        help=(
            "comma-separated global class IDs mapped to runtime A--E in order; "
            "required for cardinalities 20 and 100, identity 0,1,2,3,4 by default for 5"
        ),
    )
    parser.add_argument("--low-medium-threshold", required=True)
    parser.add_argument("--medium-high-threshold", required=True)
    parser.add_argument("--label-map-output", required=True, type=Path)
    parser.add_argument("--query-projection-output", required=True, type=Path)
    parser.add_argument("--metadata-output", required=True, type=Path)
    return parser


def parse_projection_classes(
    text: str | None,
    global_class_count: int,
) -> RuntimeProjection:
    if text is None:
        if global_class_count != RUNTIME_LABEL_SLOTS:
            raise MetadataError(
                "--projection-global-classes is required when global label cardinality exceeds five"
            )
        selected = tuple(range(RUNTIME_LABEL_SLOTS))
    else:
        try:
            selected = tuple(int(item.strip(), 10) for item in text.split(","))
        except ValueError as error:
            raise MetadataError(
                "projection global classes must be comma-separated integers"
            ) from error
        if any(not item.strip() for item in text.split(",")):
            raise MetadataError(
                "projection global classes must be comma-separated integers"
            )
    projection = RuntimeProjection(selected)
    projection.validate(global_class_count)
    return projection


def main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    arguments = parser.parse_args(argv)
    try:
        graph = scan_temporal_graph(arguments.graph)
        query = parse_query_graph(arguments.query)
        thresholds = FrozenDensityThresholds.from_text(
            arguments.low_medium_threshold,
            arguments.medium_high_threshold,
        )
        if arguments.label_scheme == "stable-id":
            mapping, scheme = stable_id_label_map(
                graph.vertices, arguments.label_seed, arguments.label_count
            )
        else:
            mapping, scheme = degree_correlated_label_map(
                graph.degree_by_vertex,
                arguments.label_seed,
                arguments.label_count,
            )
        label_receipt = write_explicit_label_map(
            arguments.label_map_output, mapping, scheme
        )
        projection = parse_projection_classes(
            arguments.projection_global_classes, arguments.label_count
        )
        projection_receipt = write_query_projection(
            arguments.query_projection_output,
            projection,
            label_receipt,
            query,
        )
        projected_mapping = project_global_label_map(
            mapping, projection, arguments.label_count
        )
        density = temporal_support_density(
            graph,
            query,
            projected_mapping,
            arguments.minimum_duration,
            thresholds,
        )
        metadata = build_metadata_document(
            graph, query, label_receipt, projection_receipt, density
        )
        metadata_sha256 = write_checked_json(arguments.metadata_output, metadata)
    except (MetadataError, OSError) as error:
        parser.exit(2, f"error: {error}\n")
    print(
        json.dumps(
            {
                "label_map_sha256": label_receipt.file_sha256,
                "metadata_sha256": metadata_sha256,
                "query_projection_sha256": projection_receipt.file_sha256,
                "temporal_support_density_bin": density["bin"],
                "temporal_support_density_value": density["continuous_value"],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

