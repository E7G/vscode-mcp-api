#!/usr/bin/env bash
# Linux/macOS build. Requires CMake >=3.24, Qt 6.4+, a C++20 compiler.
set -eo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
build_dir="$here/build-unix"
if [[ -n "$QT_ROOT" ]]; then
    cmake -S "$here" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QT_ROOT"
else
    cmake -S "$here" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$build_dir" --parallel
printf 'Built: %s\n' "$build_dir"
