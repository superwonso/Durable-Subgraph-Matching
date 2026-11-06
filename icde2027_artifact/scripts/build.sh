#!/usr/bin/env bash
# Build measured source snapshots; never starts a benchmark or CTest campaign.
set -euo pipefail
artifact_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_root="$artifact_root/build"
jobs=4
snapshot=all
while (($#)); do
    case "$1" in
        --jobs) jobs=${2:?missing job count}; shift 2 ;;
        --build-root) build_root=${2:?missing build directory}; shift 2 ;;
        --snapshot) snapshot=${2:?missing snapshot}; shift 2 ;;
        --help)
            echo 'Usage: bash scripts/build.sh [--jobs N] [--build-root DIR] [--snapshot all|v6|v7|v8|v9|baselines|profile_v9]'
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'jobs must be a positive integer' >&2; exit 2; }
case "$snapshot" in all|v6|v7|v8|v9|baselines|profile_v9) ;; *) echo 'Unknown snapshot' >&2; exit 2 ;; esac
mkdir -p -- "$build_root"
build_root=$(CDPATH= cd -- "$build_root" && pwd)
for part in v6 v7 v8 v9 baselines profile_v9; do
    [[ "$snapshot" == all || "$snapshot" == "$part" ]] || continue
    source_dir="$artifact_root/src/$part"
    if [[ "$part" == v6 || "$part" == v7 || "$part" == v8 || "$part" == v9 ]]; then
        source_dir="$source_dir/ours_gopher"
        cmake -S "$source_dir" -B "$build_root/$part" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
        cmake --build "$build_root/$part" --parallel "$jobs" --target td_tree_gopher build_wstore
    else
        cmake -S "$source_dir" -B "$build_root/$part" -DCMAKE_BUILD_TYPE=Release
        cmake --build "$build_root/$part" --parallel "$jobs"
    fi
done
