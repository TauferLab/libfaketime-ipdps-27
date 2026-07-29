#!/usr/bin/env bash
set -euo pipefail

bench_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${bench_dir}/../.." && pwd)"
compiler="${CC:-/usr/bin/gcc}"
faketime_lib="${FAKETIME_LIB:-${repo_root}/src/libfaketimeMT.so.1}"
calls="${CALLS:-1000000}"
repetitions="${REPETITIONS:-9}"
update_calls="${UPDATE_CALLS:-1000}"
results="${RESULTS:-${bench_dir}/raw_results.csv}"
metadata="${METADATA:-${bench_dir}/machine_metadata.txt}"
stamp="/dev/shm/libfaketime-shared-clock-benchmark-$$"
target_base=946684800000000000

cleanup()
{
    rm -f "${stamp}" "${stamp}."*.tmp
}
trap cleanup EXIT

"${compiler}" -O2 -std=gnu11 -Wall -Wextra -Werror \
    -o "${bench_dir}/clock_benchmark" "${bench_dir}/clock_benchmark.c" \
    -ldl -lrt

allowed_list="$(awk '/Cpus_allowed_list/ {print $2}' /proc/self/status)"
benchmark_cpu="${BENCH_CPU:-${allowed_list%%[-,]*}}"
runner=(taskset -c "${benchmark_cpu}")
if ! taskset -c "${benchmark_cpu}" true 2>/dev/null; then
    runner=()
    benchmark_cpu="unpinned"
fi

{
    uname -a
    printf 'hostname=%s\n' "$(hostname)"
    printf 'cpu_model=%s\n' "$(awk -F: '/model name/ {sub(/^ /, "", $2); print $2; exit}' /proc/cpuinfo)"
    printf 'allowed_cpus=%s\n' "${allowed_list}"
    printf 'benchmark_cpu=%s\n' "${benchmark_cpu}"
    printf 'compiler=%s\n' "$("${compiler}" --version | head -n 1)"
    printf 'compiler_flags=-O2 -std=gnu11 -Wall -Wextra -Werror\n'
    printf 'libfaketime_head_at_run=%s\n' "$(git -C "${repo_root}" rev-parse HEAD)"
    printf 'libfaketime_diff=%s\n' "$(git -C "${repo_root}" status --short | wc -l) changed paths"
    printf 'calls=%s\nrepetitions=%s\nupdate_calls=%s\n' \
        "${calls}" "${repetitions}" "${update_calls}"
} > "${metadata}"

printf '%s\n' 'mode,repetition,order,calls,total_ns,latency_ns,update_ns,observed_ns,sink' > "${results}"
printf '%s\n' '+0.000000000s' > "${stamp}"

for repetition in $(seq 1 "${repetitions}"); do
    order=0
    while IFS= read -r mode; do
        order=$((order + 1))
        target_ns=$((target_base + repetition * 100000000000))
        case "${mode}" in
            native)
                measurement=$(env -u LD_PRELOAD -u FAKETIME -u FAKETIME_TIMESTAMP_FILE \
                    -u FAKETIME_NO_CACHE -u FAKETIME_SHARED_CLOCK \
                    -u FAKETIME_SHARED_CLOCK_INITIAL_NS \
                    BENCH_MODE=native "${runner[@]}" \
                    "${bench_dir}/clock_benchmark" "${calls}")
                ;;
            legacy)
                measurement=$(env LD_PRELOAD="${faketime_lib}" \
                    FAKETIME_TIMESTAMP_FILE="${stamp}" FAKETIME_NO_CACHE=1 \
                    FAKETIME_DONT_FAKE_MONOTONIC=1 BENCH_MODE=legacy \
                    BENCH_LEGACY_FILE="${stamp}" BENCH_TARGET_NS="${target_ns}" \
                    BENCH_UPDATE_CALLS="${update_calls}" \
                    "${runner[@]}" "${bench_dir}/clock_benchmark" "${calls}")
                ;;
            shared)
                measurement=$(env LD_PRELOAD="${faketime_lib}" \
                    FAKETIME_SHARED_CLOCK=1 \
                    FAKETIME_SHARED_CLOCK_INITIAL_NS="$((target_ns - 10000000000))" \
                    FAKETIME_DONT_FAKE_MONOTONIC=1 BENCH_MODE=shared \
                    BENCH_TARGET_NS="${target_ns}" \
                    BENCH_UPDATE_CALLS="${update_calls}" "${runner[@]}" \
                    "${bench_dir}/clock_benchmark" "${calls}")
                ;;
        esac
        printf '%s,%s,%s,%s\n' "${mode}" "${repetition}" "${order}" \
            "${measurement}" >> "${results}"
    done < <(printf '%s\n' native legacy shared | shuf)
done

python3 "${bench_dir}/plot_results.py" "${results}" "${bench_dir}"
printf 'Raw results: %s\n' "${results}"
