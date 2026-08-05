#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"

PATTERNS="DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,STREAM"
TRANSPORTS_DEFAULT="tcp,ipc"
IFS=',' read -r -a PATTERN_LIST <<< "${PATTERNS}"

SECONDS=0
SHOW_TOTAL_TIME=0
format_elapsed() {
  local total_sec="${1:-0}"
  local hours=$(( total_sec / 3600 ))
  local minutes=$(( (total_sec % 3600) / 60 ))
  local seconds=$(( total_sec % 60 ))
  if (( hours > 0 )); then
    printf "%dh %dm %ds" "${hours}" "${minutes}" "${seconds}"
  elif (( minutes > 0 )); then
    printf "%dm %ds" "${minutes}" "${seconds}"
  else
    printf "%ds" "${seconds}"
  fi
}
print_total_time() {
  if [[ "${SHOW_TOTAL_TIME}" -ne 1 ]]; then
    return
  fi
  if [[ "${BENCH_SUPPRESS_TOTAL_TIME:-0}" == "1" || "${PERF_SUPPRESS_TOTAL_TIME:-0}" == "1" ]]; then
    return
  fi
  local status="${1:-0}"
  local elapsed="${SECONDS}"
  echo "Total benchmark time: $(format_elapsed "${elapsed}") (${elapsed}s, exit=${status})"
}
trap 'print_total_time $?' EXIT

is_uint() {
  local value="${1:-}"
  [[ "${value}" =~ ^[0-9]+$ ]]
}

detect_platform_tag() {
  local platform="linux"
  case "$(uname -s)" in
    Darwin*)
      platform="macos"
      ;;
    MINGW*|MSYS*|CYGWIN*)
      platform="windows"
      ;;
  esac
  printf '%s' "${platform}"
}

detect_arch_tag() {
  local arch="$(uname -m)"
  case "${arch}" in
    x86_64|amd64)
      echo "x64"
      ;;
    aarch64|arm64)
      echo "arm64"
      ;;
    *)
      echo "${arch}"
      ;;
  esac
}

sleep_ms() {
  local delay_ms="${1:-0}"
  if [[ -z "${delay_ms}" || ! "${delay_ms}" =~ ^[0-9]+$ || "${delay_ms}" -le 0 ]]; then
    return
  fi
  local sec=$(( delay_ms / 1000 ))
  local ms=$(( delay_ms % 1000 ))
  sleep "${sec}.$(printf '%03d' "${ms}")"
}

default_build_dir() {
  local platform
  local arch
  platform="$(detect_platform_tag)"
  arch="$(detect_arch_tag)"
  if [[ "${platform}" == "windows" ]]; then
    echo "${ROOT_DIR}/bindings/c/build/windows-x64"
    return
  fi
  echo "${ROOT_DIR}/bindings/c/build"
}

usage() {
  cat <<'USAGE'
Usage: bindings/c/bench/with_zmq/run_benchmarks_multi.sh [options]

Run multi-socket benchmark patterns with libzmq vs zlink comparison.
Default PATTERN is:
  DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,STREAM
By default, multi-bench keeps warmup at 2s and duration window at 5s.
By default, multi-bench uses transports: tcp,ipc (can be overridden with --transports).

Options:
  --pattern NAME         Benchmark pattern (default: all patterns above).
                         Alias: stream/streams => STREAM
  --help                 Show this help.
  --reuse-build          Reuse existing build directory as-is (skip auto-build).
  --clean-build          Remove build directory and do a clean build.
  --results-dir PATH     Override results root directory.
  --results-tag NAME     Optional tag appended to the results filename.
  --result-file PATH     Override the consolidated multi result file.
  --build-dir PATH       Override build directory.
  --output PATH          Tee results to a file.
  --runs N               Iterations per configuration (default: 1).
  --pin-cpu              Pin CPU core during benchmarks (Linux taskset).
  --server-io-threads N  Set server io-threads (mapped to common io-threads).
  --client-io-threads N  Set client io-threads (mapped to common io-threads).
  --msg-sizes LIST       Comma-separated message sizes.
  --transports LIST      Comma-separated transports.
  --warmup N             Optional override for multi warmup seconds (default 2).
  --duration N           Optional override for multi duration seconds (default 5).
  --clients N            Override number of client sockets per pattern (default: 100, stream=10000).
  --hwm N                Override HWM (default: 100, stream=10).
  --sndtimeo N           Override send timeout ms (default: 200).
  --rcvtimeo N           Override recv timeout ms (default: 200).
  --send-timeout-ms N    Alias of --sndtimeo.
  --recv-timeout-ms N    Alias of --rcvtimeo.
  --connect-concurrency N
                         Override concurrent connect count.
  --transport-transition-ms N
                         Override BENCH_TRANSPORT_TRANSITION_MS (default: 3000).
  --pattern-transition-ms N
                         Override BENCH_MULTI_PATTERN_TRANSITION_MS (default: 3000).
  --server-ready-timeout-ms N
                         Override BENCH_SERVER_READY_TIMEOUT_MS (default: 10000).
  --connect-ready-timeout-ms N
                         Override connect-ready timeout ms (default: 5000).
  --monitor-hwm N        Override monitor hwm (default: 1000).
  --server-shutdown-timeout-ms N
                         Override BENCH_SERVER_SHUTDOWN_TIMEOUT_MS (default: 5000).
  --server-bind-port N
                         Override BENCH_MULTI_SERVER_BIND_PORT (default: 0=auto).

Notes:
  - result is saved under results/multi/report/.
  - multi comparison uses split server/client processes, so inproc is excluded.
  - STREAM is recv-only in with_zmq multi comparison mode.
USAGE
}

add_explicit_pattern_unique() {
  local pattern="${1:-}"
  if [[ -z "${pattern}" ]]; then
    return
  fi
  local existing
  for existing in "${EXPLICIT_PATTERNS[@]}"; do
    if [[ "${existing}" == "${pattern}" ]]; then
      return
    fi
  done
  EXPLICIT_PATTERNS+=("${pattern}")
}

expand_and_add_explicit_pattern() {
  local raw="${1:-}"
  raw="${raw#"${raw%%[![:space:]]*}"}"
  raw="${raw%"${raw##*[![:space:]]}"}"
  raw="$(printf '%s' "${raw}" | tr '[:lower:]' '[:upper:]')"
  if [[ -z "${raw}" ]]; then
    return
  fi

  case "${raw}" in
    STREAM|STREAMS)
      add_explicit_pattern_unique "STREAM"
      ;;
    *)
      add_explicit_pattern_unique "${raw}"
      ;;
  esac
}

normalize_with_zmq_pattern() {
  local raw="${1:-}"
  local up
  up="$(printf '%s' "${raw}" | tr '[:lower:]' '[:upper:]')"

  case "${up}" in
    DEALER_DEALER)
      echo "dealer_dealer"
      ;;
    DEALER_ROUTER)
      echo "dealer_router"
      ;;
    ROUTER_ROUTER)
      echo "router_router"
      ;;
    PUBSUB)
      echo "pubsub"
      ;;
    STREAM)
      echo "stream"
      ;;
    *)
      return 1
      ;;
  esac
}

resolve_hwm_value() {
  local pattern="${1:-}"
  local explicit_hwm="${2:-}"
  local default_hwm="${3:-100}"
  local default_stream_hwm="${4:-10}"

  if [[ -n "${explicit_hwm}" ]]; then
    echo "${explicit_hwm}"
    return
  fi

  if [[ "${pattern}" == "stream" ]]; then
    echo "${default_stream_hwm}"
  else
    echo "${default_hwm}"
  fi
}

resolve_clients_value() {
  local pattern="${1:-}"
  local explicit_clients="${2:-}"
  local default_clients="${3:-100}"
  local default_stream_clients="${4:-10000}"

  if [[ -n "${explicit_clients}" ]]; then
    echo "${explicit_clients}"
    return
  fi

  if [[ "${pattern}" == "stream" ]]; then
    echo "${default_stream_clients}"
  else
    echo "${default_clients}"
  fi
}

resolve_io_threads_value() {
  local pattern="${1:-}"
  local server_io="${2:-}"
  local client_io="${3:-}"
  local default_io="${4:-2}"
  local default_stream_io="${5:-4}"

  if [[ -n "${server_io}" && -n "${client_io}" ]]; then
    if [[ "${server_io}" == "${client_io}" ]]; then
      echo "${server_io}"
      return
    fi
    if (( server_io > client_io )); then
      echo "${server_io}"
    else
      echo "${client_io}"
    fi
    return
  fi

  if [[ -n "${server_io}" ]]; then
    echo "${server_io}"
    return
  fi
  if [[ -n "${client_io}" ]]; then
    echo "${client_io}"
    return
  fi

  if [[ "${pattern}" == "stream" ]]; then
    echo "${default_stream_io}"
  else
    echo "${default_io}"
  fi
}

resolve_connect_concurrency() {
  local explicit_value="${1:-}"
  local clients="${2:-}"
  if [[ -n "${explicit_value}" ]]; then
    echo "${explicit_value}"
    return
  fi
  if [[ "${clients}" =~ ^[0-9]+$ ]] && (( clients >= 10000 )); then
    echo "1024"
  else
    echo "128"
  fi
}

set_build_mode() {
  local next_mode="${1:-}"
  if [[ "${next_mode}" != "incremental" && "${next_mode}" != "reuse" && "${next_mode}" != "clean" ]]; then
    echo "Error: invalid build mode: ${next_mode}" >&2
    exit 1
  fi
  if [[ "${BUILD_MODE_EXPLICIT}" -eq 1 && "${BUILD_MODE}" != "${next_mode}" ]]; then
    echo "Error: --reuse-build and --clean-build are mutually exclusive." >&2
    exit 1
  fi
  BUILD_MODE="${next_mode}"
  BUILD_MODE_EXPLICIT=1
}

BUILD_MODE="incremental"
BUILD_MODE_EXPLICIT=0

WARMUP_SECONDS="${PERF_WARMUP_SECONDS:-2}"
DURATION_SECONDS="${PERF_DURATION_SECONDS:-5}"
RUNS="1"
TRANSPORTS="${PERF_TRANSPORTS:-${TRANSPORTS_DEFAULT}}"
MSG_SIZES="${PERF_MSG_SIZES:-}"
RESULTS_DIR_OVERRIDE="${PERF_RESULTS_DIR:-}"
RESULTS_TAG=""
BUILD_DIR=""
OUTPUT_FILE=""
RESULT_FILE=""
PIN_CPU=0

CLIENTS="${PERF_CLIENTS:-}"
HWM="${PERF_HWM:-}"
SNDTIMEO_MS="${PERF_SNDTIMEO_MS:-200}"
RCVTIMEO_MS="${PERF_RCVTIMEO_MS:-200}"
CONNECT_CONCURRENCY="${PERF_CONNECT_CONCURRENCY:-}"
TRANSPORT_TRANSITION_MS="${PERF_TRANSPORT_TRANSITION_MS:-3000}"
PATTERN_TRANSITION_MS="${PERF_PATTERN_TRANSITION_MS:-3000}"
SERVER_READY_TIMEOUT_MS="${PERF_SERVER_READY_TIMEOUT_MS:-10000}"
CONNECT_READY_TIMEOUT_MS="${PERF_CONNECT_READY_TIMEOUT_MS:-5000}"
MONITOR_HWM="${PERF_MONITOR_HWM:-1000}"
SERVER_SHUTDOWN_TIMEOUT_MS="${PERF_SERVER_SHUTDOWN_TIMEOUT_MS:-5000}"
SERVER_BIND_PORT="${PERF_SERVER_BIND_PORT:-0}"

EFFECTIVE_DEFAULT_CLIENTS="${PERF_DEFAULT_CLIENTS:-100}"
EFFECTIVE_DEFAULT_STREAM_CLIENTS="${PERF_DEFAULT_STREAM_CLIENTS:-10000}"
EFFECTIVE_DEFAULT_HWM="${PERF_DEFAULT_HWM:-100}"
EFFECTIVE_DEFAULT_STREAM_HWM="${PERF_DEFAULT_STREAM_HWM:-10}"
EFFECTIVE_DEFAULT_IO_THREADS="${PERF_DEFAULT_IO_THREADS:-4}"
EFFECTIVE_DEFAULT_STREAM_IO_THREADS="${PERF_DEFAULT_STREAM_IO_THREADS:-4}"
SERVER_IO_THREADS="${PERF_SERVER_IO_THREADS:-}"
CLIENT_IO_THREADS="${PERF_CLIENT_IO_THREADS:-}"

EXPLICIT_PATTERNS=()

while [[ $# -gt 0 ]]; do
  arg="$1"
  case "${arg}" in
    -h|--help)
      usage
      exit 0
      ;;
    --transports)
      if [[ $# -lt 2 ]]; then
        echo "Error: ${arg} requires a value." >&2
        exit 1
      fi
      TRANSPORTS="${2}"
      shift 2
      ;;
    --msg-sizes)
      if [[ $# -lt 2 ]]; then
        echo "Error: ${arg} requires a value." >&2
        exit 1
      fi
      MSG_SIZES="${2}"
      shift 2
      ;;
    --pattern)
      if [[ $# -lt 2 ]]; then
        echo "Error: --pattern requires a value." >&2
        exit 1
      fi
      if [[ "$(printf '%s' "$2" | tr '[:lower:]' '[:upper:]')" == "ALL" ]]; then
        EXPLICIT_PATTERNS=("${PATTERN_LIST[@]}")
      else
        IFS=',' read -r -a pattern_list <<< "$2"
        for p in "${pattern_list[@]}"; do
          expand_and_add_explicit_pattern "${p}"
        done
      fi
      shift 2
      ;;
    --results-tag)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      RESULTS_TAG="${2}"
      shift 2
      ;;
    --reuse-build)
      set_build_mode "reuse"
      shift
      ;;
    --clean-build)
      set_build_mode "clean"
      shift
      ;;
    --runs)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      RUNS="${2}"
      shift 2
      ;;
    --runs=*)
      RUNS="${1#*=}"
      shift
      ;;
    --warmup)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      WARMUP_SECONDS="${2}"
      shift 2
      ;;
    --duration)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      DURATION_SECONDS="${2}"
      shift 2
      ;;
    --pin-cpu)
      PIN_CPU=1
      shift
      ;;
    --results-dir)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      RESULTS_DIR_OVERRIDE="${2}"
      shift 2
      ;;
    --build-dir)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      BUILD_DIR="${2}"
      shift 2
      ;;
    --output)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      OUTPUT_FILE="${2}"
      shift 2
      ;;
    --result-file)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      RESULT_FILE="${2}"
      shift 2
      ;;
    --server-io-threads)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      SERVER_IO_THREADS="${2}"
      shift 2
      ;;
    --client-io-threads)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      CLIENT_IO_THREADS="${2}"
      shift 2
      ;;
    --clients)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      CLIENTS="${2}"
      shift 2
      ;;
    --hwm)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      HWM="${2}"
      shift 2
      ;;
    --sndtimeo|--send-timeout-ms)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      SNDTIMEO_MS="${2}"
      shift 2
      ;;
    --rcvtimeo|--recv-timeout-ms)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      RCVTIMEO_MS="${2}"
      shift 2
      ;;
    --connect-concurrency)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      CONNECT_CONCURRENCY="${2}"
      shift 2
      ;;
    --transport-transition-ms)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      TRANSPORT_TRANSITION_MS="${2}"
      shift 2
      ;;
    --pattern-transition-ms)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      PATTERN_TRANSITION_MS="${2}"
      shift 2
      ;;
    --server-ready-timeout-ms)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      SERVER_READY_TIMEOUT_MS="${2}"
      shift 2
      ;;
    --connect-ready-timeout-ms)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      CONNECT_READY_TIMEOUT_MS="${2}"
      shift 2
      ;;
    --monitor-hwm)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      MONITOR_HWM="${2}"
      shift 2
      ;;
    --server-shutdown-timeout-ms)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      SERVER_SHUTDOWN_TIMEOUT_MS="${2}"
      shift 2
      ;;
    --server-bind-port)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      SERVER_BIND_PORT="${2}"
      shift 2
      ;;
    --*)
      echo "Error: unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      echo "Error: unknown positional argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if ! is_uint "${RUNS}" || (( RUNS < 1 )); then
  echo "Error: --runs must be a positive integer." >&2
  exit 1
fi
if ! is_uint "${WARMUP_SECONDS}"; then
  echo "Error: --warmup must be a non-negative integer." >&2
  exit 1
fi
if ! is_uint "${DURATION_SECONDS}" || (( DURATION_SECONDS < 1 )); then
  echo "Error: --duration must be a positive integer." >&2
  exit 1
fi
if [[ -n "${CLIENTS}" ]] && ( ! is_uint "${CLIENTS}" || (( CLIENTS < 1 )) ); then
  echo "Error: --clients must be a positive integer." >&2
  exit 1
fi
if [[ -n "${HWM}" ]] && ( ! is_uint "${HWM}" || (( HWM < 1 )) ); then
  echo "Error: --hwm must be a positive integer." >&2
  exit 1
fi
if ! is_uint "${SNDTIMEO_MS}" || (( SNDTIMEO_MS < 1 )); then
  echo "Error: --sndtimeo must be a positive integer." >&2
  exit 1
fi
if ! is_uint "${RCVTIMEO_MS}" || (( RCVTIMEO_MS < 1 )); then
  echo "Error: --rcvtimeo must be a positive integer." >&2
  exit 1
fi
if [[ -n "${CONNECT_CONCURRENCY}" ]] && ( ! is_uint "${CONNECT_CONCURRENCY}" || (( CONNECT_CONCURRENCY < 1 )) ); then
  echo "Error: --connect-concurrency must be a positive integer." >&2
  exit 1
fi
if ! is_uint "${TRANSPORT_TRANSITION_MS}"; then
  echo "Error: --transport-transition-ms must be a non-negative integer." >&2
  exit 1
fi
if ! is_uint "${PATTERN_TRANSITION_MS}"; then
  echo "Error: --pattern-transition-ms must be a non-negative integer." >&2
  exit 1
fi
if ! is_uint "${SERVER_READY_TIMEOUT_MS}"; then
  echo "Error: --server-ready-timeout-ms must be a non-negative integer." >&2
  exit 1
fi
if ! is_uint "${CONNECT_READY_TIMEOUT_MS}"; then
  echo "Error: --connect-ready-timeout-ms must be a non-negative integer." >&2
  exit 1
fi
if ! is_uint "${MONITOR_HWM}"; then
  echo "Error: --monitor-hwm must be a non-negative integer." >&2
  exit 1
fi
if ! is_uint "${SERVER_SHUTDOWN_TIMEOUT_MS}"; then
  echo "Error: --server-shutdown-timeout-ms must be a non-negative integer." >&2
  exit 1
fi
if ! is_uint "${SERVER_BIND_PORT}" || (( SERVER_BIND_PORT > 65535 )); then
  echo "Error: --server-bind-port must be an integer in range 0..65535." >&2
  exit 1
fi
if [[ -n "${SERVER_IO_THREADS}" ]] && ( ! is_uint "${SERVER_IO_THREADS}" || (( SERVER_IO_THREADS < 1 )) ); then
  echo "Error: --server-io-threads must be a positive integer." >&2
  exit 1
fi
if [[ -n "${CLIENT_IO_THREADS}" ]] && ( ! is_uint "${CLIENT_IO_THREADS}" || (( CLIENT_IO_THREADS < 1 )) ); then
  echo "Error: --client-io-threads must be a positive integer." >&2
  exit 1
fi
if [[ -n "${TRANSPORTS}" && ! "${TRANSPORTS}" =~ ^[a-z]+(,[a-z]+)*$ ]]; then
  echo "Error: --transports must be a comma-separated list of names." >&2
  exit 1
fi
if [[ -n "${MSG_SIZES}" && ! "${MSG_SIZES}" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
  echo "Error: --msg-sizes must be a comma-separated list of integers." >&2
  exit 1
fi

SELECTED_PATTERNS=("${PATTERN_LIST[@]}")
if [[ "${#EXPLICIT_PATTERNS[@]}" -gt 0 ]]; then
  SELECTED_PATTERNS=("${EXPLICIT_PATTERNS[@]}")
fi

RUN_PATTERNS=()
for raw_pattern in "${SELECTED_PATTERNS[@]}"; do
  mapped="$(normalize_with_zmq_pattern "${raw_pattern}" || true)"
  if [[ -z "${mapped}" ]]; then
    echo "Error: unsupported pattern '${raw_pattern}'." >&2
    echo "Supported in with_zmq: DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,STREAM" >&2
    exit 1
  fi

  exists=0
  for item in "${RUN_PATTERNS[@]}"; do
    if [[ "${item}" == "${mapped}" ]]; then
      exists=1
      break
    fi
  done
  if [[ "${exists}" -eq 0 ]]; then
    RUN_PATTERNS+=("${mapped}")
  fi
done

if [[ "${#RUN_PATTERNS[@]}" -eq 0 ]]; then
  exit 0
fi

if [[ -z "${RESULTS_DIR_OVERRIDE}" ]]; then
  RESULTS_DIR_OVERRIDE="${SCRIPT_DIR}/results"
fi
RESULTS_DIR_OVERRIDE="$(realpath -m "${RESULTS_DIR_OVERRIDE}")"

if [[ -n "${RESULT_FILE}" ]]; then
  RESULT_FILE="$(realpath -m "${RESULT_FILE}")"
else
  TS="$(date +%Y%m%d_%H%M%S)"
  NAME="perf_$(detect_platform_tag)_${TS}"
  if [[ -n "${RESULTS_TAG}" ]]; then
    NAME="${NAME}_${RESULTS_TAG}"
  fi
  RESULT_FILE="${RESULTS_DIR_OVERRIDE}/multi/report/${NAME}.txt"
fi
mkdir -p "$(dirname "${RESULT_FILE}")"

if [[ -n "${OUTPUT_FILE}" ]]; then
  OUTPUT_FILE="$(realpath -m "${OUTPUT_FILE}")"
  mkdir -p "$(dirname "${OUTPUT_FILE}")"
fi

if [[ -n "${OUTPUT_FILE}" && "${OUTPUT_FILE}" == "${RESULT_FILE}" ]]; then
  echo "Error: --output cannot point to the same file as consolidated result output." >&2
  exit 1
fi

if [[ -z "${BUILD_DIR}" ]]; then
  BUILD_DIR="$(default_build_dir)"
fi
BUILD_DIR="$(realpath -m "${BUILD_DIR}")"

if [[ "${BUILD_MODE}" == "reuse" ]]; then
  if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "Error: --reuse-build requires an existing build directory: ${BUILD_DIR}" >&2
    exit 1
  fi
fi
if [[ "${BUILD_MODE}" == "clean" ]]; then
  echo "Cleaning build directory: ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

run_all_patterns() {
  local failed=0

  echo "=== Running multi benchmark: $(IFS=,; echo "${RUN_PATTERNS[*]}") ==="
  echo "    warmup=${WARMUP_SECONDS}s duration=${DURATION_SECONDS}s runs=${RUNS} transports=${TRANSPORTS}"

  for idx in "${!RUN_PATTERNS[@]}"; do
    local pattern="${RUN_PATTERNS[idx]}"
    local pattern_clients
    local pattern_hwm
    local pattern_io_threads
    local pattern_connect_concurrency

    pattern_clients="$(resolve_clients_value "${pattern}" "${CLIENTS}" "${EFFECTIVE_DEFAULT_CLIENTS}" "${EFFECTIVE_DEFAULT_STREAM_CLIENTS}")"
    pattern_hwm="$(resolve_hwm_value "${pattern}" "${HWM}" "${EFFECTIVE_DEFAULT_HWM}" "${EFFECTIVE_DEFAULT_STREAM_HWM}")"
    pattern_io_threads="$(resolve_io_threads_value "${pattern}" "${SERVER_IO_THREADS}" "${CLIENT_IO_THREADS}" "${EFFECTIVE_DEFAULT_IO_THREADS}" "${EFFECTIVE_DEFAULT_STREAM_IO_THREADS}")"
    pattern_connect_concurrency="$(resolve_connect_concurrency "${CONNECT_CONCURRENCY}" "${pattern_clients}")"

    cmd=("${SCRIPT_DIR}/multi/run_benchmarks.sh")
    cmd+=(--pattern "${pattern}")
    cmd+=(--runs "${RUNS}")
    cmd+=(--transports "${TRANSPORTS}")
    cmd+=(--warmup "${WARMUP_SECONDS}")
    cmd+=(--duration "${DURATION_SECONDS}")
    cmd+=(--clients "${pattern_clients}")
    cmd+=(--hwm "${pattern_hwm}")
    cmd+=(--sndtimeo-ms "${SNDTIMEO_MS}")
    cmd+=(--rcvtimeo-ms "${RCVTIMEO_MS}")
    cmd+=(--connect-ready-timeout-ms "${CONNECT_READY_TIMEOUT_MS}")
    cmd+=(--monitor-hwm "${MONITOR_HWM}")
    cmd+=(--connect-concurrency "${pattern_connect_concurrency}")
    cmd+=(--io-threads "${pattern_io_threads}")
    cmd+=(--build-dir "${BUILD_DIR}")
    cmd+=(--results-dir "${RESULTS_DIR_OVERRIDE}")

    if [[ -n "${RESULTS_TAG}" ]]; then
      cmd+=(--results-tag "${RESULTS_TAG}")
    fi
    if [[ -n "${MSG_SIZES}" ]]; then
      cmd+=(--msg-sizes "${MSG_SIZES}")
    fi
    if [[ "${PIN_CPU}" -eq 1 ]]; then
      cmd+=(--pin-cpu)
    fi

    echo
    echo "--- Pattern: ${pattern} ---"

    env_args=()
    if [[ "${BUILD_MODE}" == "reuse" ]]; then
      env_args+=(BENCH_NO_AUTOBUILD=1)
      env_args+=(PERF_NO_AUTOBUILD=1)
    fi
    env_args+=(BENCH_TRANSPORT_TRANSITION_MS="${TRANSPORT_TRANSITION_MS}")
    env_args+=(BENCH_MULTI_PATTERN_TRANSITION_MS="${PATTERN_TRANSITION_MS}")
    env_args+=(BENCH_SERVER_READY_TIMEOUT_MS="${SERVER_READY_TIMEOUT_MS}")
    env_args+=(BENCH_SERVER_SHUTDOWN_TIMEOUT_MS="${SERVER_SHUTDOWN_TIMEOUT_MS}")
    env_args+=(BENCH_MULTI_SERVER_BIND_PORT="${SERVER_BIND_PORT}")
    env_args+=(BENCH_SUPPRESS_TOTAL_TIME=1)
    env_args+=(BENCH_MULTI_SUPPRESS_PATTERN_RESULT_SAVE=1)

    if ! env "${env_args[@]}" "${cmd[@]}"; then
      failed=1
    fi

    if (( idx + 1 < ${#RUN_PATTERNS[@]} )); then
      sleep_ms "${PATTERN_TRANSITION_MS}"
    fi
  done

  return "${failed}"
}

SHOW_TOTAL_TIME=1
if [[ -n "${OUTPUT_FILE}" ]]; then
  set +e
  run_all_patterns 2>&1 | tee "${RESULT_FILE}" "${OUTPUT_FILE}"
  status=${PIPESTATUS[0]}
  set -e
  exit "${status}"
else
  set +e
  run_all_patterns 2>&1 | tee "${RESULT_FILE}"
  status=${PIPESTATUS[0]}
  set -e
  exit "${status}"
fi
