#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 3 ]]; then
  echo "usage: $0 GPU_BINARY CPU_BINARY REPOSITORY_ROOT" >&2
  exit 2
fi

gpu_binary="$1"
cpu_binary="$2"
repository_root="$3"

if [[ ! -x "${gpu_binary}" ]]; then
  echo "missing GPU binary: ${gpu_binary}" >&2
  exit 2
fi
if [[ ! -x "${cpu_binary}" ]]; then
  echo "missing CPU binary: ${cpu_binary}" >&2
  exit 2
fi

temporary_dir="$(mktemp -d)"
trap 'rm -rf -- "${temporary_dir}"' EXIT

"${gpu_binary}" \
  "${repository_root}/ours_gopher_gpu/tests/layered_temporal.txt" \
  "${repository_root}/ours_gopher_gpu/tests/layered_query.txt" \
  3 42 \
  --count-only \
  --order paper \
  --selectivity paper \
  --statistics on \
  --result-digest on \
  --verify-cpu \
  --output-dir "${temporary_dir}/paper"

"${gpu_binary}" \
  "${repository_root}/ours_gopher_gpu/tests/layered_temporal.txt" \
  "${repository_root}/ours_gopher_gpu/tests/layered_query.txt" \
  3 42 \
  --count-only \
  --gpu-variant resident \
  --order reuse \
  --selectivity intended \
  --statistics off \
  --result-digest on \
  --verify-cpu \
  --output-dir "${temporary_dir}/intended"

"${gpu_binary}" \
  "${repository_root}/ours_gopher_gpu/tests/layered_temporal.txt" \
  "${repository_root}/ours_gopher_gpu/tests/layered_query.txt" \
  3 42 \
  --count-only \
  --gpu-variant resident \
  --order paper \
  --selectivity paper \
  --statistics on \
  --result-digest on \
  --verify-cpu \
  --gpu-memory-mb 64 \
  --output-dir "${temporary_dir}/resident"

"${gpu_binary}" \
  "${repository_root}/ours_gopher_gpu/tests/layered_temporal.txt" \
  "${repository_root}/ours_gopher_gpu/tests/layered_query.txt" \
  3 42 \
  --count-only \
  --label-mode stable-id \
  --gpu-variant resident \
  --order paper \
  --selectivity paper \
  --statistics off \
  --result-digest on \
  --verify-cpu \
  --gpu-memory-mb 64 \
  --output-dir "${temporary_dir}/stable_id"

python3 -m json.tool \
  "${temporary_dir}/paper/metrics_results_layered_temporal.json" \
  >/dev/null
python3 -m json.tool \
  "${temporary_dir}/intended/metrics_results_layered_temporal.json" \
  >/dev/null
python3 -m json.tool \
  "${temporary_dir}/resident/metrics_results_layered_temporal.json" \
  >/dev/null
python3 -m json.tool \
  "${temporary_dir}/stable_id/metrics_results_layered_temporal.json" \
  >/dev/null
grep -q '"passed": true' \
  "${temporary_dir}/paper/metrics_results_layered_temporal.json"
grep -q '"passed": true' \
  "${temporary_dir}/intended/metrics_results_layered_temporal.json"
grep -q '"passed": true' \
  "${temporary_dir}/resident/metrics_results_layered_temporal.json"
grep -q '"passed": true' \
  "${temporary_dir}/stable_id/metrics_results_layered_temporal.json"
grep -q '"gpu_variant": "level_synchronous"' \
  "${temporary_dir}/paper/metrics_results_layered_temporal.json"
grep -q '"label_mode": "stable-id"' \
  "${temporary_dir}/stable_id/metrics_results_layered_temporal.json"
grep -q '^label_mode: stable-id$' \
  "${temporary_dir}/stable_id/timing_results_layered_temporal.txt"
grep -q '"gpu_variant": "resident_continuation"' \
  "${temporary_dir}/resident/metrics_results_layered_temporal.json"
grep -q '^Count:' \
  "${temporary_dir}/paper/matching_results_layered_temporal.txt"
grep -q '^Count: 36$' \
  "${temporary_dir}/paper/matching_results_layered_temporal.txt"
grep -q '^Count: 36$' \
  "${temporary_dir}/resident/matching_results_layered_temporal.txt"

python3 - \
  "${temporary_dir}/paper/metrics_results_layered_temporal.json" \
  "${temporary_dir}/resident/metrics_results_layered_temporal.json" <<'PY'
import json
import sys

level = json.load(open(sys.argv[1], encoding="utf-8"))
resident = json.load(open(sys.argv[2], encoding="utf-8"))
assert level["match_count"] == resident["match_count"] == 36
assert level["result_digest"] == resident["result_digest"]
assert level["gpu"]["depth_host_round_trips"] > 0
assert resident["gpu"]["depth_host_round_trips"] == 0
assert resident["gpu"]["frontier_validation_passes"] * 2 == \
    level["gpu"]["frontier_validation_passes"]
PY

python3 - \
  "${temporary_dir}/intended/metrics_results_layered_temporal.json" <<'PY'
import json
import sys

metrics = json.load(open(sys.argv[1], encoding="utf-8"))
assert metrics["statistics_enabled"] is False
assert all(
    value is None for value in metrics["statistics"].values()
)
for depth in metrics["depths"]:
    assert depth["workload_metrics_available"] is False
    assert depth["generated_candidates"] is None
    assert depth["maximum_candidates_per_state"] is None
    assert depth["mean_candidates_per_state"] is None
    assert depth["maximum_to_mean_load_ratio"] is None
PY
grep -q '^generated_candidates: null$' \
  "${temporary_dir}/intended/timing_results_layered_temporal.txt"
grep -q '^pruned_temporal: null$' \
  "${temporary_dir}/intended/timing_results_layered_temporal.txt"

if "${gpu_binary}" \
  "${repository_root}/benchmark/data/reuse_star_skewed.txt" \
  "${repository_root}/benchmark/data/reuse_star_skewed.qry" \
  3 42 \
  --count-only \
  --gpu-variant resident \
  --order reuse \
  --statistics off \
  --gpu-memory-mb 1 \
  --output-dir "${temporary_dir}/budget_failure" \
  >"${temporary_dir}/budget_failure.log" 2>&1; then
  echo "resident memory-cap test unexpectedly succeeded" >&2
  exit 1
fi
grep -q 'implicit CPU fallback is disabled' \
  "${temporary_dir}/budget_failure.log"

echo "GPU/CPU exactness smoke tests passed."
