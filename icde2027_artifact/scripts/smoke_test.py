#!/usr/bin/env python3
"""Bounded release smoke: one tiny fixture, no real datasets or timing study."""
import argparse
from itertools import permutations
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def oracle(fixture):
    """Count injective triangle mappings from the fixture's explicit labels."""
    classes = dict(json.loads((fixture / "label_projection_global20.json").read_text())["mapping"])
    histories = {}
    for line in (fixture / "label_projection_generic_c.txt").read_text().splitlines():
        u, v, t = map(int, line.split())
        histories.setdefault((u, v), set()).add(t)
    total = 0
    for a, b, c in permutations(sorted(classes), 3):
        if [classes[a], classes[b], classes[c]] != [1, 2, 3]:
            continue
        common = histories.get((a, b), set()) & histories.get((a, c), set()) & histories.get((b, c), set())
        if any({t, t + 1, t + 2} <= common for t in common):
            total += 1
    assert total == 6, "The fixed fixture's independent expected count changed"
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, default=ROOT / "build")
    parser.add_argument("--output", type=Path, help="New scratch directory to retain receipts; otherwise use a temporary directory")
    args = parser.parse_args()
    build = args.build_root.resolve()
    fixture = ROOT / "src/v9/ours_gopher/tests/fixtures"
    expected = oracle(fixture)
    graph = fixture / "label_projection_generic_c.txt"
    query = fixture / "label_projection_generic_c.qry"
    labels = ["--global-label-map", str(fixture / "label_projection_global20.json"),
              "--query-label-projection", str(fixture / "label_projection_generic_c.json")]
    temp = None
    if args.output:
        work = args.output.resolve()
        work.mkdir(parents=True, exist_ok=False)
    else:
        temp = tempfile.TemporaryDirectory(prefix="windur-release-smoke-")
        work = Path(temp.name)
    commands, rows = [], []

    def launch(argv, folder):
        result = subprocess.run([str(x) for x in argv], cwd=folder, capture_output=True, text=True, timeout=60)
        commands.append(dict(argv=[str(x) for x in argv], returncode=result.returncode))
        (folder / "stdout.log").write_text(result.stdout)
        (folder / "stderr.log").write_text(result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)

    try:
        eroded, raw = work / "eroded.store", work / "raw.store"
        for name, tool, ks, store in (
            ("eroded", build / "v7/build_wstore", "3", eroded),
            ("raw", build / "v8/build_raw_bitset_store_v8", "1", raw)):
            folder = work / ("build_" + name)
            folder.mkdir()
            launch([tool, "build", "--graph", graph, "--dataset", "release-smoke", "--ks", ks,
                    "--store", store, "--metrics", folder / "metrics.json"], folder)
        cases = [
            ("v6_window", "v6/td_tree_direct_v6", ["--direct-p4", "on", "--window-algebra", "on", "--wstore", eroded]),
            ("v7_dfs", "v7/td_tree_direct_v7", ["--direct-p4", "off", "--window-algebra", "off"]),
            ("v7_final", "v7/td_tree_direct_v7", ["--direct-p4", "on", "--window-algebra", "on", "--wstore", eroded,
                "--bindex", "always", "--bindex-alpha", "0.5", "--bindex-c", "1", "--bindex-block-size", "16", "--bindex-min-size", "64"]),
            ("v8_bitset", "v8/td_tree_bitset_v8", ["--direct-p4", "on", "--window-algebra", "on", "--raw-bitset", "on", "--wstore", raw]),
            ("v9_window", "v9/td_tree_followup_v9", ["--direct-p4", "on", "--window-algebra", "on", "--wstore", eroded]),
            ("v9_padded", "v9/td_tree_followup_v9", ["--direct-p4", "on", "--window-algebra", "on", "--wstore", eroded, "--window-pad-raw-words", "on"]),
            ("v9_union", "v9/td_tree_followup_v9", ["--direct-p4", "on", "--window-algebra", "on", "--wstore", eroded, "--window-block-union", "on"]),
        ]
        for name, executable, switches in cases:
            folder = work / name
            folder.mkdir()
            argv = [build / executable, graph, query, "3", *labels,
                    "--engine", "gopher", "--selectivity", "paper", "--order", "paper",
                    "--parallel", "roots", "--threads", "2", "--count-only", "--experiment-variant", "durable-dfs",
                    "--dag-passes", "none", "--early-prune", "on", "--statistics", "off",
                    "--ops-counters", "off", "--result-digest", "off", "--output-dir", folder,
                    "--metrics", folder / "metrics.json", *switches]
            launch(argv, folder)
            metrics = json.loads((folder / "metrics.json").read_text())
            assert metrics["match_count"] == expected and not metrics.get("count_overflow"), name
            rows.append(dict(arm=name, expected=expected, observed=metrics["match_count"], status="PASS"))
        for name, executable in (("td", "baselines/td_tree_baseline_v8"),
                                 ("tinla", "baselines/tinla_baseline_v8"),
                                 ("td_profile", "profile_v9/td_tree_profile_v9")):
            folder = work / name
            folder.mkdir()
            launch([build / executable, graph, query, "3", *labels, "--threads", "2",
                    "--count-only", "--duration-mode", "consecutive", "--label-mode", "stable-id",
                    "--prefilter", "off", "--output-dir", folder], folder)
            metrics = json.loads((folder / "baseline_metrics.json").read_text())
            assert metrics["match_count"] == expected and not metrics.get("count_overflow"), name
            rows.append(dict(arm=name, expected=expected, observed=metrics["match_count"], status="PASS"))
        receipt = dict(status="PASS", fixture="included explicit-label directed triangle", expected_count=expected,
                       native_matcher_runs=len(rows), native_store_builds=2, real_dataset_runs=0,
                       scope="Tiny build/CLI regression only; not a replacement for the archived correctness tests or performance results",
                       rows=rows, commands=commands)
        (work / "SMOKE.json").write_text(json.dumps(receipt, indent=2) + "\n")
        print(json.dumps({k: receipt[k] for k in ("status", "expected_count", "native_matcher_runs", "native_store_builds", "real_dataset_runs")}))
    finally:
        if temp:
            temp.cleanup()


if __name__ == "__main__":
    main()
