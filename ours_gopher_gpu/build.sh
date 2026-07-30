#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${script_dir}/build"
gpu_arch="${GPU_ARCH:-gfx1151}"
build_type="${BUILD_TYPE:-Release}"

cmake \
  -S "${script_dir}" \
  -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE="${build_type}" \
  -DGOPHER_GPU_ARCHITECTURES="${gpu_arch}" \
  -DGOPHER_GPU_STRICT_WARNINGS="${STRICT:-OFF}" \
  -DGOPHER_GPU_RUNTIME_TESTS="${GPU_RUNTIME_TESTS:-OFF}"
cmake --build "${build_dir}" --parallel
cp "${build_dir}/td_tree_gopher_gpu" \
   "${script_dir}/td_tree_gopher_gpu"

printf '%s\n' \
  "Built ${script_dir}/td_tree_gopher_gpu for ${gpu_arch}"
