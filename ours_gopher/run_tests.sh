#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
compiler="${CXX:-g++}"
common_flags=(-Wall -Wextra -Wpedantic -O2 -std=c++17 -pthread)
if [[ "${STRICT:-0}" == "1" ]]; then
    common_flags+=(-Wconversion -Wshadow -Werror)
fi
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/ours_gopher_tests.XXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT
core_sources=(
    "$script_dir/CandidateDag.cpp"
    "$script_dir/DagExecutor.cpp"
    "$script_dir/GopherMatcher.cpp"
    "$script_dir/SearchStatistics.cpp"
    "$script_dir/query_decomposition.cpp"
    "$script_dir/TDTree.cpp"
    "$script_dir/Utils.cpp"
)

"$compiler" "${common_flags[@]}" \
    "$script_dir/tests/test_ours.cpp" \
    "$script_dir/query_decomposition.cpp" \
    "$script_dir/TDTree.cpp" \
    "$script_dir/Utils.cpp" \
    -o "$build_dir/test_ours"
"$build_dir/test_ours"

"$compiler" "${common_flags[@]}" \
    "$script_dir/tests/test_gopher.cpp" \
    "${core_sources[@]}" \
    -o "$build_dir/test_gopher"
"$build_dir/test_gopher"

"$compiler" "${common_flags[@]}" \
    "$script_dir/tests/test_parallel_prefix.cpp" \
    "${core_sources[@]}" \
    -o "$build_dir/test_parallel_prefix"
"$build_dir/test_parallel_prefix"

"$compiler" "${common_flags[@]}" \
    "$script_dir/main.cpp" \
    "${core_sources[@]}" \
    -o "$build_dir/td_tree_gopher_cli_test"
"$script_dir/tests/test_cli.sh" \
    "$build_dir/td_tree_gopher_cli_test"

echo "All ours_gopher test suites passed."
