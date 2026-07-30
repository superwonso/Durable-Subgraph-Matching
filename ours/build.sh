#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
compiler=${CXX:-g++}
output=${1:-"$script_dir/td_tree"}

"$compiler" -Wall -Wextra -Wpedantic -O3 -std=c++17 -pthread \
    "$script_dir/main.cpp" \
    "$script_dir/query_decomposition.cpp" \
    "$script_dir/TDTree.cpp" \
    "$script_dir/Utils.cpp" \
    -o "$output"

printf 'Built: %s\n' "$output"
