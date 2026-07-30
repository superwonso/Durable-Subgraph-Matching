#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
compiler="${CXX:-g++}"
output="${1:-$script_dir/td_tree_gopher}"
warning_flags=(-Wall -Wextra -Wpedantic)
if [[ "${STRICT:-0}" == "1" ]]; then
    warning_flags+=(-Wconversion -Wshadow -Werror)
fi

"$compiler" "${warning_flags[@]}" -O3 -std=c++17 \
    "$script_dir/main.cpp" \
    "$script_dir/CandidateDag.cpp" \
    "$script_dir/DagExecutor.cpp" \
    "$script_dir/GopherMatcher.cpp" \
    "$script_dir/SearchStatistics.cpp" \
    "$script_dir/query_decomposition.cpp" \
    "$script_dir/TDTree.cpp" \
    "$script_dir/Utils.cpp" \
    -pthread -o "$output"

echo "Built: $output"
