#!/usr/bin/env bash

set -uo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
TARGET_PATH="${TARGET_PATH:-/api/session-status}"
DURATION="${DURATION:-30s}"
COOLDOWN_SECONDS="${COOLDOWN_SECONDS:-8}"
CONNECTION_LEVELS="${CONNECTION_LEVELS:-50 100 300 500}"
SID="${SID:-}"
OUTPUT_ROOT="${OUTPUT_ROOT:-benchmark-results}"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
OUTPUT_DIR="${OUTPUT_ROOT}/${TIMESTAMP}"
TARGET_URL="${BASE_URL}${TARGET_PATH}"
MONITOR_PID=""

mkdir -p "${OUTPUT_DIR}"

cleanup() {
    if [[ -n "${MONITOR_PID}" ]]; then
        kill "${MONITOR_PID}" 2>/dev/null || true
        wait "${MONITOR_PID}" 2>/dev/null || true
    fi
}

trap cleanup EXIT INT TERM

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing command: $1" >&2
        exit 1
    fi
}

threads_for_connections() {
    local connections="$1"

    if (( connections <= 50 )); then
        echo 2
    else
        echo 4
    fi
}

curl_headers=()
wrk_headers=()

if [[ -n "${SID}" ]]; then
    curl_headers=(-H "Cookie: sid=${SID}")
    wrk_headers=(-H "Cookie: sid=${SID}")
fi

capture_metrics() {
    local output_file="$1"
    curl -sS "${curl_headers[@]}" "${BASE_URL}/metrics" >"${output_file}" || true
}

capture_runtime_snapshot() {
    local output_file="$1"

    {
        echo "timestamp=$(date -Is)"
        echo
        docker stats --no-stream \
            --format 'table {{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}\t{{.NetIO}}\t{{.BlockIO}}\t{{.PIDs}}' \
            mywebserver-app mywebserver-nginx mywebserver-redis mywebserver-mysql \
            2>&1 || true
        echo
        ss -s 2>&1 || true
        echo
        curl -sS "${BASE_URL}/metrics" 2>&1 || true
    } >>"${output_file}"
}

monitor_runtime() {
    local output_file="$1"

    while true; do
        capture_runtime_snapshot "${output_file}"
        sleep 2
    done
}

require_command curl
require_command docker
require_command ss
require_command wrk

{
    echo "timestamp=${TIMESTAMP}"
    echo "target=${TARGET_URL}"
    echo "duration=${DURATION}"
    echo "connections=${CONNECTION_LEVELS}"
    echo "logical_cpus=$(nproc)"
    echo "sid_enabled=$([[ -n "${SID}" ]] && echo yes || echo no)"
    echo
    uname -a
    echo
    free -h || true
    echo
    docker ps --format 'table {{.Names}}\t{{.Image}}\t{{.Status}}'
    echo
    docker inspect mywebserver-app \
        --format 'image={{.Config.Image}} cmd={{json .Config.Cmd}}' 2>&1 || true
} >"${OUTPUT_DIR}/environment.txt"

echo "Checking target: ${TARGET_URL}"
status_code="$(curl -sS -o /dev/null -w '%{http_code}' \
    "${curl_headers[@]}" "${TARGET_URL}" || true)"

if [[ ! "${status_code}" =~ ^2 ]]; then
    echo "target returned HTTP ${status_code}; aborting" >&2
    echo "For /api/me, export a valid SID before running:" >&2
    echo "  SID='<token>' TARGET_PATH='/api/me' bash scripts/benchmark_v4.sh" >&2
    exit 1
fi

capture_metrics "${OUTPUT_DIR}/metrics-before.json"

echo "Warm-up: 5s, 2 threads, 20 connections"
wrk --latency -t2 -c20 -d5s "${wrk_headers[@]}" "${TARGET_URL}" \
    >"${OUTPUT_DIR}/warmup.txt" 2>&1

for connections in ${CONNECTION_LEVELS}; do
    threads="$(threads_for_connections "${connections}")"
    label="t${threads}-c${connections}"
    result_file="${OUTPUT_DIR}/${label}.wrk.txt"
    stats_file="${OUTPUT_DIR}/${label}.stats.txt"

    echo "Running ${label} for ${DURATION}: ${TARGET_URL}"
    capture_metrics "${OUTPUT_DIR}/${label}.metrics-before.json"

    monitor_runtime "${stats_file}" &
    MONITOR_PID=$!

    wrk --latency \
        -t"${threads}" \
        -c"${connections}" \
        -d"${DURATION}" \
        "${wrk_headers[@]}" \
        "${TARGET_URL}" >"${result_file}" 2>&1
    wrk_status=$?

    kill "${MONITOR_PID}" 2>/dev/null || true
    wait "${MONITOR_PID}" 2>/dev/null || true
    MONITOR_PID=""

    capture_metrics "${OUTPUT_DIR}/${label}.metrics-after.json"

    cat "${result_file}"

    if (( wrk_status != 0 )); then
        echo "wrk failed for ${label}; see ${result_file}" >&2
        exit "${wrk_status}"
    fi

    echo "Cooling down for ${COOLDOWN_SECONDS}s"
    sleep "${COOLDOWN_SECONDS}"
done

capture_metrics "${OUTPUT_DIR}/metrics-after.json"

echo
echo "Benchmark complete: ${OUTPUT_DIR}"
echo "Key result files:"
find "${OUTPUT_DIR}" -maxdepth 1 -name '*.wrk.txt' -print | sort
