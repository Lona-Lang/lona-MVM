#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Keep benchmark parameters fixed here so repeated runs remain comparable.
BENCH_OPT_LEVEL=1
MANAGED_HEAP_SIZE="512M"
WARMUP_RUNS=1
MEASURE_RUNS=2
TIMEOUT_SECONDS=120

MVM="$ROOT_DIR/build/mvm"
MANAGED_BC="$ROOT_DIR/build/examples/linked_list_bench.bc"
NATIVE_EXE="$ROOT_DIR/build/examples/linked_list_bench_native"

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mvm-linked-list-bench.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

require_tool() {
    local tool="$1"
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing required tool: $tool" >&2
        exit 1
    fi
}

run_timed() {
    local label="$1"
    shift

    local time_file="$TMP_DIR/$label.time"
    if ! /usr/bin/time -f "%e" -o "$time_file" timeout "${TIMEOUT_SECONDS}s" "$@" >/dev/null; then
        echo "$label failed or timed out after ${TIMEOUT_SECONDS}s" >&2
        return 1
    fi

    cat "$time_file"
}

summarize_samples() {
    local label="$1"
    local samples_file="$2"

    awk -v label="$label" '
        BEGIN {
            min = 0
            max = 0
            sum = 0
            count = 0
        }
        {
            value = $1 + 0
            if (count == 0 || value < min) {
                min = value
            }
            if (count == 0 || value > max) {
                max = value
            }
            sum += value
            count += 1
        }
        END {
            if (count == 0) {
                exit 1
            }
            avg = sum / count
            printf "%s,%d,%.6f,%.6f,%.6f\n", label, count, avg, min, max
        }
    ' "$samples_file"
}

measure_case() {
    local label="$1"
    shift

    local samples_file="$TMP_DIR/$label.samples"
    : > "$samples_file"

    for ((run = 1; run <= WARMUP_RUNS; ++run)); do
        run_timed "$label.warmup.$run" "$@" >/dev/null
    done

    for ((run = 1; run <= MEASURE_RUNS; ++run)); do
        local seconds
        seconds="$(run_timed "$label.measure.$run" "$@")"
        printf "%s run %d/%d: %ss\n" "$label" "$run" "$MEASURE_RUNS" "$seconds" >&2
        printf "%s\n" "$seconds" >> "$samples_file"
    done

    summarize_samples "$label" "$samples_file"
}

require_tool make
require_tool timeout
require_tool /usr/bin/time
require_tool awk

make -C "$ROOT_DIR" BENCH_OPT_LEVEL="$BENCH_OPT_LEVEL" bench-linked-list-build >/dev/null

cat <<EOF
linked-list benchmark
source: examples/linked_list_bench.lo
opt_level: O${BENCH_OPT_LEVEL}
managed_heap: ${MANAGED_HEAP_SIZE}
warmup_runs: ${WARMUP_RUNS}
measure_runs: ${MEASURE_RUNS}
timeout_seconds: ${TIMEOUT_SECONDS}

case,runs,avg_s,min_s,max_s
EOF

native_summary="$(measure_case native "$NATIVE_EXE")"
managed_summary="$(
    measure_case managed_heap_${MANAGED_HEAP_SIZE} \
        "$MVM" "-Xm${MANAGED_HEAP_SIZE}" "$MANAGED_BC"
)"

printf "%s\n" "$native_summary"
printf "%s\n" "$managed_summary"

native_avg="$(printf "%s\n" "$native_summary" | awk -F, '{ print $3 }')"
managed_avg="$(printf "%s\n" "$managed_summary" | awk -F, '{ print $3 }')"
awk -v native="$native_avg" -v managed="$managed_avg" '
    BEGIN {
        printf "\nmanaged/native_avg_ratio: %.3fx\n", managed / native
    }
'
