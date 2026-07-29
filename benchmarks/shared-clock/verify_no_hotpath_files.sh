#!/usr/bin/env bash
set -euo pipefail

bench_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${bench_dir}/../.." && pwd)"
compiler="${CC:-/usr/bin/gcc}"
faketime_lib="${FAKETIME_LIB:-${repo_root}/src/libfaketimeMT.so.1}"
trace_file="${bench_dir}/shared_clock_strace.log"
hot_trace="${bench_dir}/shared_clock_hot_path.strace"
target_ns=946684800000000000

"${compiler}" -O2 -std=gnu11 -Wall -Wextra -Werror \
    -o "${bench_dir}/clock_benchmark" "${bench_dir}/clock_benchmark.c" \
    -ldl -lrt

strace -o "${trace_file}" -e trace=%file,read,write,close \
    env LD_PRELOAD="${faketime_lib}" FAKETIME_SHARED_CLOCK=1 \
    FAKETIME_SHARED_CLOCK_INITIAL_NS="$((target_ns - 10000000000))" \
    FAKETIME_DONT_FAKE_MONOTONIC=1 BENCH_MODE=shared \
    BENCH_TARGET_NS="${target_ns}" "${bench_dir}/clock_benchmark" 10000 \
    >/dev/null

awk '/BENCHMARK_HOT_PATH_END/ {seen=0} seen {print} \
     /BENCHMARK_HOT_PATH_BEGIN/ {seen=1}' \
    "${trace_file}" > "${hot_trace}"
if grep -E '(open|stat|readlink|access|read\()' "${hot_trace}"; then
    echo "unexpected filesystem/read operation in marked hot path" >&2
    exit 1
fi
printf 'PASS: no file or read operations after hot-path marker (%s)\n' \
    "${hot_trace}"
