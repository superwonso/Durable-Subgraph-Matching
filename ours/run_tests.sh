#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
compiler=${CXX:-g++}
test_dir=$(mktemp -d)
cleanup() {
    rm -rf -- "$test_dir"
}
trap cleanup EXIT

"$compiler" -Wall -Wextra -Wpedantic -O3 -std=c++17 -pthread \
    "$script_dir/tests/test_ours.cpp" \
    "$script_dir/query_decomposition.cpp" \
    "$script_dir/TDTree.cpp" \
    "$script_dir/Utils.cpp" \
    -o "$test_dir/test_ours"
"$test_dir/test_ours"

"$compiler" -Wall -Wextra -Wpedantic -O3 -std=c++17 -pthread \
    "$script_dir/main.cpp" \
    "$script_dir/query_decomposition.cpp" \
    "$script_dir/TDTree.cpp" \
    "$script_dir/Utils.cpp" \
    -o "$test_dir/td_tree_cli"
"$script_dir/tests/test_cli.sh" "$test_dir/td_tree_cli"
