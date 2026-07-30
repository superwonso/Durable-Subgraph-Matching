#!/usr/bin/env python3

import json
import unittest
from pathlib import Path
import tempfile

import run_benchmarks


def run_row(
    algorithm: str,
    repeat: int,
    *,
    count: int = 7,
    digest: str | None = "aaaa:bbbb",
) -> dict[str, object]:
    row: dict[str, object] = {
        "case_id": "correctness_case",
        "algorithm": algorithm,
        "semantic_group": "consecutive",
        "repeat": repeat,
        "status": "ok",
        "match_count": count,
    }
    if digest is not None:
        row["result_digest"] = digest
    return row


class DigestCorrectnessGateTest(unittest.TestCase):
    algorithms = {
        "cpu": {"semantic_group": "consecutive"},
        "gpu": {"semantic_group": "consecutive"},
    }

    def checks(
        self,
        rows: list[dict[str, object]],
        *,
        require_digest: bool,
        expected_match_count: int | None = None,
    ) -> dict[str, object]:
        summaries = run_benchmarks.summarize(rows)
        case: dict[str, object] = {
            "id": "correctness_case",
            "algorithms": ["cpu", "gpu"],
            "require_digest": require_digest,
        }
        if expected_match_count is not None:
            case["expected_match_count"] = expected_match_count
        return run_benchmarks.correctness_rows(
            summaries, self.algorithms, [case]
        )[0]

    def test_required_digest_passes_when_all_repeats_and_engines_match(self) -> None:
        rows = [
            run_row(algorithm, repeat)
            for repeat in (1, 2, 3)
            for algorithm in ("cpu", "gpu")
        ]
        check = self.checks(rows, require_digest=True)
        self.assertEqual(check["status"], "pass")
        self.assertEqual(check["incomplete_digest_algorithms"], "")

    def test_required_digest_fails_on_cross_engine_mismatch(self) -> None:
        rows = [
            run_row("cpu", repeat, digest="aaaa:bbbb")
            for repeat in (1, 2, 3)
        ]
        rows.extend(
            run_row("gpu", repeat, digest="cccc:dddd")
            for repeat in (1, 2, 3)
        )
        check = self.checks(rows, require_digest=True)
        self.assertEqual(check["status"], "fail")

    def test_required_digest_is_missing_when_one_engine_does_not_emit_it(self) -> None:
        rows = [
            run_row("cpu", repeat)
            for repeat in (1, 2, 3)
        ]
        rows.extend(
            run_row("gpu", repeat, digest=None)
            for repeat in (1, 2, 3)
        )
        check = self.checks(rows, require_digest=True)
        self.assertEqual(check["status"], "missing")
        self.assertEqual(check["incomplete_digest_algorithms"], "gpu")

    def test_count_only_case_does_not_require_digest(self) -> None:
        rows = [
            run_row(algorithm, repeat, digest=None)
            for repeat in (1, 2, 3)
            for algorithm in ("cpu", "gpu")
        ]
        check = self.checks(rows, require_digest=False)
        self.assertEqual(check["status"], "pass")

    def test_known_count_oracle_rejects_consistent_but_wrong_engines(self) -> None:
        rows = [
            run_row(algorithm, repeat, count=7, digest=None)
            for repeat in (1, 2, 3)
            for algorithm in ("cpu", "gpu")
        ]
        check = self.checks(
            rows,
            require_digest=False,
            expected_match_count=8,
        )
        self.assertEqual(check["status"], "fail")
        self.assertEqual(check["oracle_status"], "fail")


class RunSignatureTest(unittest.TestCase):
    def test_signature_changes_when_executable_changes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="dsm_signature_") as temporary:
            root = Path(temporary)
            executable = root / "matcher"
            data = root / "data.txt"
            query = root / "query.qry"
            executable.write_bytes(b"version-one")
            data.write_text("0 1 1\n", encoding="utf-8")
            query.write_text("a:A b:B\n", encoding="utf-8")
            algorithm = {
                "executable": str(executable),
                "semantic_group": "consecutive",
                "flags": ["--count-only"],
            }
            case = {
                "id": "case",
                "data": str(data),
                "query": str(query),
                "k": 3,
            }
            first = run_benchmarks.run_signature(
                "matcher", algorithm, case, 1
            )
            executable.write_bytes(b"version-two-longer")
            second = run_benchmarks.run_signature(
                "matcher", algorithm, case, 1
            )
            self.assertNotEqual(first, second)

    def test_signature_changes_with_cpu_affinity(self) -> None:
        with tempfile.TemporaryDirectory(prefix="dsm_affinity_signature_") as temporary:
            root = Path(temporary)
            executable = root / "matcher"
            data = root / "data.txt"
            query = root / "query.qry"
            executable.write_bytes(b"matcher")
            data.write_text("0 1 1\n", encoding="utf-8")
            query.write_text("a:A b:B\n", encoding="utf-8")
            algorithm = {
                "executable": str(executable),
                "semantic_group": "consecutive",
                "flags": ["--count-only"],
                "cpu_affinity": "0",
            }
            case = {
                "id": "case",
                "data": str(data),
                "query": str(query),
                "k": 3,
            }
            first = run_benchmarks.run_signature(
                "matcher", algorithm, case, 1
            )
            algorithm["cpu_affinity"] = "0,2"
            second = run_benchmarks.run_signature(
                "matcher", algorithm, case, 1
            )
            self.assertNotEqual(first, second)


class PerAlgorithmInputTest(unittest.TestCase):
    def test_resolve_command_uses_algorithm_specific_data(self) -> None:
        with tempfile.TemporaryDirectory(prefix="dsm_input_override_") as temporary:
            root = Path(temporary)
            executable = root / "matcher"
            raw = root / "graph.txt"
            filtered = root / "graph_filtered.txt"
            query = root / "query.qry"
            for path in (executable, raw, filtered, query):
                path.write_text("", encoding="utf-8")
            algorithm = {"executable": str(executable)}
            case = {
                "data": str(filtered),
                "data_by_algorithm": {"baseline": str(raw)},
                "query": str(query),
                "k": 3,
            }

            _, baseline_data, _, _ = run_benchmarks.resolve_run_command(
                algorithm, case, "baseline"
            )
            _, proposed_data, _, _ = run_benchmarks.resolve_run_command(
                algorithm, case, "proposed"
            )

            self.assertEqual(baseline_data, raw.resolve())
            self.assertEqual(proposed_data, filtered.resolve())

    def test_input_variant_override_falls_back_to_case_value(self) -> None:
        case = {
            "input_variant": "filtered",
            "input_variant_by_algorithm": {"baseline": "raw"},
        }
        self.assertEqual(
            run_benchmarks.case_algorithm_value(
                case, "input_variant", "baseline"
            ),
            "raw",
        )
        self.assertEqual(
            run_benchmarks.case_algorithm_value(
                case, "input_variant", "proposed"
            ),
            "filtered",
        )

    def test_non_object_override_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "must be an object"):
            run_benchmarks.case_algorithm_value(
                {"data_by_algorithm": []}, "data", "baseline"
            )


class CommandOptionTest(unittest.TestCase):
    def test_last_option_value_is_reported(self) -> None:
        command = [
            "matcher",
            "data",
            "query",
            "3",
            "42",
            "--selectivity",
            "paper",
            "--memo=none",
            "--selectivity=intended",
        ]
        self.assertEqual(
            run_benchmarks.command_option(command, "--selectivity"),
            "intended",
        )
        self.assertEqual(
            run_benchmarks.command_option(command, "--memo"), "none"
        )

    def test_threads_are_derived_from_resolved_argv(self) -> None:
        algorithm = {
            "threads": 32,
            "flags": ["--threads", "32"],
        }
        command = ["matcher", "data", "query", "3", "42", *algorithm["flags"]]
        self.assertEqual(
            run_benchmarks.configured_threads(algorithm, command), 32
        )
        self.assertEqual(run_benchmarks.argv_threads(command), 32)

    def test_declared_thread_mismatch_is_rejected(self) -> None:
        algorithm = {
            "threads": 32,
            "flags": ["--threads", "32"],
        }
        resolved = [
            "matcher",
            "data",
            "query",
            "3",
            "42",
            *algorithm["flags"],
            "--threads",
            "1",
        ]
        with self.assertRaisesRegex(ValueError, "do not match"):
            run_benchmarks.configured_threads(algorithm, resolved)


class MetricParsingTest(unittest.TestCase):
    def test_null_statistics_and_canonical_ablation_fields_are_preserved(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(prefix="dsm_metrics_") as temporary:
            run_dir = Path(temporary)
            (run_dir / "timing_results_test.txt").write_text(
                "\n".join(
                    [
                        "selectivity: intended",
                        "query_root: 2",
                        "plan_order: 2 0 1",
                        "prefilter: off",
                        "early_prune: on",
                        "statistics_enabled: off",
                        "raw_extension_candidates: null",
                    ]
                ),
                encoding="utf-8",
            )
            (run_dir / "matching_results_test.txt").write_text(
                "\n".join(
                    [
                        "[Run]",
                        "metric_schema_version: logical_extensions_v1",
                        "root_candidate_count: 7",
                        "candidate_relation_entries: 19",
                        "",
                        "[Final Matches]",
                        "Count: 3",
                    ]
                ),
                encoding="utf-8",
            )
            payload = {
                "metric_schema_version": "logical_extensions_v1",
                "query_root": 2,
                "plan": {"root": 2, "order": [2, 0, 1]},
                "selectivity_mode": "intended",
                "root_candidate_count": 7,
                "candidate_relation_entries": 19,
                "early_prune": True,
                "statistics_enabled": False,
                "statistics": {
                    "raw_extension_candidates": None,
                    "generated_candidates": None,
                    "accepted_extensions": None,
                },
                "gpu": {"frontier_budget_bytes": 64 * 1024 * 1024},
            }
            (run_dir / "gopher_metrics.json").write_text(
                json.dumps(payload), encoding="utf-8"
            )

            metrics = run_benchmarks.parse_program_outputs(run_dir, "")
            self.assertEqual(
                metrics["metric_schema_version"], "logical_extensions_v1"
            )
            self.assertEqual(metrics["query_root"], 2)
            self.assertEqual(metrics["plan_order"], "2 0 1")
            self.assertEqual(metrics["selectivity"], "intended")
            self.assertEqual(metrics["prefilter"], "off")
            self.assertIs(metrics["early_prune"], True)
            self.assertEqual(metrics["root_candidate_count"], 7)
            self.assertEqual(metrics["candidate_relation_entries"], 19)
            self.assertIsNone(metrics["raw_extension_candidates"])
            self.assertIsNone(metrics["post_structural_candidates"])
            self.assertIsNone(metrics["accepted_extensions"])
            self.assertIs(metrics["statistics_collected"], False)
            self.assertEqual(metrics["gpu_memory_mb_effective"], 64.0)

    def test_statistics_values_are_exposed_under_common_names(self) -> None:
        with tempfile.TemporaryDirectory(prefix="dsm_metrics_on_") as temporary:
            run_dir = Path(temporary)
            payload = {
                "statistics_enabled": True,
                "statistics": {
                    "raw_extension_candidates": 100,
                    "generated_candidates": 60,
                    "accepted_extensions": 25,
                },
            }
            (run_dir / "gopher_metrics.json").write_text(
                json.dumps(payload), encoding="utf-8"
            )
            metrics = run_benchmarks.parse_program_outputs(run_dir, "")
            self.assertEqual(metrics["raw_extension_candidates"], 100)
            self.assertEqual(metrics["post_structural_candidates"], 60)
            self.assertEqual(metrics["accepted_extensions"], 25)
            self.assertIs(metrics["statistics_collected"], True)

    def test_legacy_zero_counters_are_null_when_statistics_were_off(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(prefix="dsm_metrics_legacy_") as temporary:
            run_dir = Path(temporary)
            payload = {
                "statistics_enabled": False,
                "statistics": {
                    "generated_candidates": 0,
                    "accepted_candidates": 0,
                },
            }
            (run_dir / "gopher_metrics.json").write_text(
                json.dumps(payload), encoding="utf-8"
            )
            metrics = run_benchmarks.parse_program_outputs(run_dir, "")
            self.assertIsNone(metrics["post_structural_candidates"])
            self.assertIsNone(metrics["accepted_extensions"])
            self.assertIsNone(
                metrics["gopher_statistics_generated_candidates"]
            )
            self.assertIs(metrics["statistics_collected"], False)


class FactorAndDetailRowsTest(unittest.TestCase):
    def test_factors_are_flattened_with_stable_names(self) -> None:
        fields = run_benchmarks.factor_fields(
            {
                "factors": {
                    "Selectivity Mode": "intended",
                    "early-prune": False,
                }
            }
        )
        self.assertEqual(fields["factor_selectivity_mode"], "intended")
        self.assertIs(fields["factor_early_prune"], False)

    def test_candidate_and_selectivity_sections_are_normalized(self) -> None:
        with tempfile.TemporaryDirectory(prefix="dsm_detail_") as temporary:
            run_dir = Path(temporary)
            result = "\n".join(
                [
                    "[Run]",
                    "query_root: 1",
                    "plan_order: 1 0",
                    "",
                    "[Selectivity Summary]",
                    (
                        "q0 label=A degree=1 candidate_count=8 "
                        "average_lifespan=3.5 score=8 "
                        "is_root=false order_position=1"
                    ),
                    (
                        "q1 label=B degree=1 candidate_count=4 "
                        "average_lifespan=2 score=4 "
                        "is_root=true order_position=0"
                    ),
                    "",
                    "[Candidate Summary]",
                    (
                        "q0 label=A blocks=2 candidates=5 "
                        "relation_entries=7 sample=10,11"
                    ),
                    (
                        "q1 label=B blocks=0 candidates=4 "
                        "relation_entries=4 sample=12,13"
                    ),
                    "",
                    "[Final Matches]",
                    "Count: 2",
                ]
            )
            (run_dir / "matching_results_test.txt").write_text(
                result, encoding="utf-8"
            )
            parent = {
                "case_id": "case",
                "algorithm": "ours",
                "semantic_group": "consecutive",
                "repeat": 1,
                "status": "ok",
                "run_dir": str(run_dir),
                "query_root": 1,
                "plan_order": "1 0",
                "selectivity": "intended",
                "factor_prefilter": "on",
            }

            candidates = run_benchmarks.candidate_rows([parent])
            self.assertEqual(len(candidates), 2)
            self.assertEqual(candidates[0]["query_vertex"], 0)
            self.assertEqual(candidates[0]["unique_candidates"], 5)
            self.assertEqual(
                candidates[0]["candidate_relation_entries_q"], 7
            )
            self.assertEqual(candidates[0]["order_position"], 1)
            self.assertIs(candidates[0]["is_root"], False)
            self.assertEqual(candidates[0]["factor_prefilter"], "on")

            selectivity = run_benchmarks.selectivity_rows([parent])
            self.assertEqual(len(selectivity), 2)
            self.assertEqual(selectivity[1]["query_vertex"], 1)
            self.assertEqual(selectivity[1]["label_candidate_count"], 4)
            self.assertIs(selectivity[1]["is_root"], True)
            self.assertEqual(selectivity[1]["score"], 4)


class GpuDepthExplosionTest(unittest.TestCase):
    def test_depth_json_is_exploded_without_turning_null_into_zero(self) -> None:
        rows = run_benchmarks.gpu_depth_rows(
            [
                {
                    "case_id": "gpu_case",
                    "algorithm": "resident",
                    "repeat": 1,
                    "status": "ok",
                    "gopher_gpu_variant": "resident_continuation",
                    "gopher_statistics_enabled": False,
                    "gopher_depths": (
                        '[{"depth": 1, "input_states": 4, '
                        '"generated_candidates": null, '
                        '"maximum_to_mean_load_ratio": null}]'
                    ),
                }
            ]
        )
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["input_states"], 4)
        self.assertIsNone(rows[0]["generated_candidates"])
        self.assertIsNone(rows[0]["maximum_to_mean_load_ratio"])


class StableCategoricalSummaryTest(unittest.TestCase):
    def test_selectivity_order_and_parallel_variant_survive_summary(self) -> None:
        rows = [
            {
                "case_id": "parallel_case",
                "algorithm": "prefix",
                "semantic_group": "consecutive",
                "repeat": repeat,
                "status": "ok",
                "match_count": 7,
                "gopher_selectivity_mode": "intended",
                "gopher_order": "paper",
                "gopher_parallel_mode_effective": "prefix",
            }
            for repeat in (1, 2, 3)
        ]
        summary = run_benchmarks.summarize(rows)[0]
        self.assertEqual(
            summary["gopher_selectivity_mode_value"], "intended"
        )
        self.assertEqual(summary["gopher_order_value"], "paper")
        self.assertEqual(
            summary["gopher_parallel_mode_effective_value"], "prefix"
        )


if __name__ == "__main__":
    unittest.main()
