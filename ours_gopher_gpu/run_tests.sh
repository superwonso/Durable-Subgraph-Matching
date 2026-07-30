#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

"${script_dir}/build.sh"

if [[ "${RUN_GPU_TESTS:-0}" == "1" ]]; then
  "${script_dir}/tests/test_gpu_smoke.sh" \
    "${script_dir}/td_tree_gopher_gpu" \
    "${script_dir}/../ours_gopher/td_tree_gopher" \
    "${script_dir}/.."
else
  echo "GPU runtime tests skipped; set RUN_GPU_TESTS=1 to enable them."
fi
