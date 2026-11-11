#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$root/build}"
shift || true

mib_per_compile_unit=1024
max_compiler_arena_mib=6144
reserved_cores=2

cores="$(nproc 2>/dev/null || echo 2)"
mem_mib="$(awk '/^MemTotal:/{printf "%d", $2/1024}' /proc/meminfo 2>/dev/null || echo 2048)"

arena_mib=$((mem_mib * 3 / 4))
((arena_mib > max_compiler_arena_mib)) && arena_mib=$max_compiler_arena_mib
jobs_by_memory=$((arena_mib / mib_per_compile_unit))
((jobs_by_memory < 1)) && jobs_by_memory=1

jobs_by_cpu=$((cores > 4 ? cores - reserved_cores : cores))
((jobs_by_cpu < 1)) && jobs_by_cpu=1

jobs=$((jobs_by_memory < jobs_by_cpu ? jobs_by_memory : jobs_by_cpu))
((jobs < 1)) && jobs=1

printf 'build: %s cores, %s MiB RAM -> -j %s (build dir %s)\n' \
  "$cores" "$mem_mib" "$jobs" "$build_dir"

cmake -S "$root" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  "$@"

cmake --build "$build_dir" --parallel "$jobs"
