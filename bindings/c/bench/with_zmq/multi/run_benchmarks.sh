#!/usr/bin/env bash
set -euo pipefail
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_RESULTS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)/results"

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
  if [[ "${BENCH_SUPPRESS_TOTAL_TIME:-0}" == "1" ]]; then
    return
  fi
  local status="${1:-0}"
  local elapsed="${SECONDS}"
  echo "Total benchmark time: $(format_elapsed "${elapsed}") (${elapsed}s, exit=${status})"
}
trap 'print_total_time $?' EXIT

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

cleanup_old_results_dirs() {
  local root="${1:-}"
  local retention="${BENCH_RESULTS_RETENTION_DAYS:-${PERF_RESULTS_RETENTION_DAYS:-90}}"
  if [[ -z "${root}" || ! -d "${root}" ]]; then
    return
  fi
  if [[ -z "${retention}" || ! "${retention}" =~ ^[0-9]+$ || "${retention}" -le 0 ]]; then
    return
  fi

  local cutoff
  cutoff="$(date -u -d "-${retention} days" +%Y%m%d 2>/dev/null || true)"
  if [[ -z "${cutoff}" ]]; then
    return
  fi

  local dir base
  for dir in "${root}"/*; do
    [[ -d "${dir}" ]] || continue
    base="$(basename "${dir}")"
    if [[ ! "${base}" =~ ^[0-9]{8}$ ]]; then
      continue
    fi
    if [[ "${base}" < "${cutoff}" ]]; then
      rm -rf "${dir}"
    fi
  done
}

enforce_file_retention() {
  local dir_path="${1:-}"
  local max_files="${BENCH_MAX_RESULT_FILES:-${PERF_MAX_RESULT_FILES:-100}}"

  if [[ -z "${dir_path}" || ! -d "${dir_path}" ]]; then
    return
  fi
  if [[ -z "${max_files}" || ! "${max_files}" =~ ^[0-9]+$ || "${max_files}" -lt 1 ]]; then
    return
  fi

  python3 - "${dir_path}" "${max_files}" <<'PY'
import os
import sys

dir_path = sys.argv[1]
max_files = int(sys.argv[2])

if not os.path.isdir(dir_path) or max_files < 1:
    raise SystemExit(0)

files = [
    name
    for name in os.listdir(dir_path)
    if os.path.isfile(os.path.join(dir_path, name))
]
if len(files) <= max_files:
    raise SystemExit(0)

files.sort()
for name in files[: len(files) - max_files]:
    try:
        os.remove(os.path.join(dir_path, name))
    except OSError:
        pass
PY
}

DEFAULT_PATTERN="router_router"
DEFAULT_TRANSPORT="tcp,ipc"

normalize_pattern() {
  local raw="${1:-}"
  if [[ -z "${raw}" ]]; then
    return 1
  fi

  local up
  up="$(echo "${raw}" | tr '[:lower:]' '[:upper:]')"

  case "${up}" in
    DEALER_DEALER) echo "MULTI_DEALER_DEALER" ;;
    DEALER_ROUTER) echo "MULTI_DEALER_ROUTER" ;;
    ROUTER_ROUTER) echo "MULTI_ROUTER_ROUTER" ;;
    PUBSUB) echo "MULTI_PUBSUB" ;;
    STREAM) echo "MULTI_STREAM" ;;
    *) return 1 ;;
  esac
}

comparison_pattern_arg() {
  local raw="${1:-}"
  if [[ -z "${raw}" ]]; then
    return 1
  fi

  local up
  up="$(echo "${raw}" | tr '[:lower:]' '[:upper:]')"

  case "${up}" in
    DEALER_DEALER|MULTI_DEALER_DEALER) echo "dealer_dealer" ;;
    DEALER_ROUTER|MULTI_DEALER_ROUTER) echo "dealer_router" ;;
    ROUTER_ROUTER|MULTI_ROUTER_ROUTER) echo "router_router" ;;
    PUBSUB|MULTI_PUBSUB) echo "pubsub" ;;
    STREAM|MULTI_STREAM) echo "stream" ;;
    *) return 1 ;;
  esac
}

is_uint() {
  local value="${1:-}"
  [[ "${value}" =~ ^[0-9]+$ ]]
}

ensure_nofile_limit() {
  local clients="${1:-}"
  if [[ "${BENCH_SKIP_NOFILE_CHECK:-0}" == "1" ]]; then
    return 0
  fi
  if ! is_uint "${clients}"; then
    return 0
  fi

  local required=$(( clients * 2 + 4096 ))
  local soft
  local hard
  soft="$(ulimit -Sn 2>/dev/null || true)"
  hard="$(ulimit -Hn 2>/dev/null || true)"
  if [[ -z "${soft}" || -z "${hard}" ]]; then
    return 0
  fi

  if [[ "${soft}" == "unlimited" ]]; then
    return 0
  fi
  if ! is_uint "${soft}"; then
    return 0
  fi

  local soft_num="${soft}"
  local hard_num=-1
  if [[ "${hard}" == "unlimited" ]]; then
    hard_num=-1
  elif is_uint "${hard}"; then
    hard_num="${hard}"
  else
    hard_num="${soft_num}"
  fi

  if (( soft_num < required )); then
    local target="${required}"
    if (( hard_num >= 0 && target > hard_num )); then
      target="${hard_num}"
    fi
    if (( target > soft_num )); then
      ulimit -Sn "${target}" 2>/dev/null || true
      soft="$(ulimit -Sn 2>/dev/null || true)"
      if is_uint "${soft}"; then
        soft_num="${soft}"
      fi
    fi
  fi

  if (( soft_num >= required )); then
    return 0
  fi

  echo "Error: insufficient open-file limit for high client count." >&2
  echo "  clients=${clients}" >&2
  echo "  required soft nofile >= ${required} (formula: clients*2+4096)" >&2
  echo "  current soft=${soft}, hard=${hard}" >&2
  if (( hard_num >= 0 && hard_num < required )); then
    echo "  hard limit is below required; raise hard limit first, then rerun." >&2
  else
    echo "  try: ulimit -n ${required}" >&2
  fi
  echo "  or skip this preflight check:" >&2
  echo "    BENCH_SKIP_NOFILE_CHECK=1 ./run_benchmarks.sh ..." >&2
  exit 2
}

usage() {
  cat <<'USAGE'
Usage: bindings/c/bench/with_zmq/multi/run_benchmarks.sh [options]

Run one multi benchmark pattern with one or more transports and compare libzmq vs zlink.

Options:
  --pattern NAME                One pattern (default: router_router)
                                Allowed: dealer_dealer, dealer_router,
                                         router_router,
                                         pubsub, stream
  --runs N                      Iterations per configuration (default: 3)
  --transport NAME              Transport(s), comma-separated allowed
                                (default: tcp,ipc)
  --transports NAME             Alias for --transport
  --zlink-only                  Re-measure only zlink (compare with cached libzmq)
  --single [N]                  Also run matching single pattern after multi
                                N omitted: use --runs value
                                N provided: use N for single runs
  --output PATH                 Tee console logs to a file.
  --results-dir PATH            Override result root directory.
  --results-tag NAME            Optional tag in saved result filename.
  --result-file PATH            Override result file path.
  --refresh-std-cache           Refresh libzmq cache during this run
  --std-cache-file PATH         libzmq cache file path
  --warmup N                    Override BENCH_MULTI_WARMUP_SECONDS (default: 2)
  --duration N                  Override BENCH_MULTI_DURATION_SECONDS (default: 5)
  --clients N                   Override BENCH_MULTI_CLIENTS
  --hwm N                       Override BENCH_MULTI_HWM (default: 100, stream=10)
  --monitor-hwm N               Override BENCH_MULTI_MONITOR_HWM (monitor queue HWM)
  --sndtimeo-ms N               Override BENCH_MULTI_SNDTIMEO_MS (default: 200)
  --rcvtimeo-ms N               Override BENCH_MULTI_RCVTIMEO_MS (default: 200)
  --connect-ready-timeout-ms N Override BENCH_MULTI_CONNECT_READY_TIMEOUT_MS (default: 5000)
  --connect-concurrency N       Override BENCH_MULTI_CONNECT_CONCURRENCY
  --io-threads N                Override BENCH_IO_THREADS (auto: 4 when clients>=512)
  --msg-sizes LIST              Override BENCH_MSG_SIZES (e.g. 1024,65536)
  --build-dir PATH              Forwarded to run_comparison.py
  --pin-cpu                     Forwarded to run_comparison.py
  --help                        Show help

Environment:
  BENCH_SKIP_NOFILE_CHECK=1     Disable preflight nofile(limit) check

Notes:
  - result is saved under results/multi/report/.
  - inproc is excluded in multi comparison because split server/client run in separate processes.
USAGE
}

resolve_effective_connect_concurrency() {
  local clients="${1:-}"
  if [[ -n "${MULTI_CONNECT_CONCURRENCY}" ]]; then
    echo "${MULTI_CONNECT_CONCURRENCY}"
    return
  fi
  if [[ "${clients}" =~ ^[0-9]+$ && "${clients}" -ge 10000 ]]; then
    echo "1024"
  else
    echo "128"
  fi
}

RUNS=3
TRANSPORT="${BENCH_TRANSPORTS:-${DEFAULT_TRANSPORT}}"
MSG_SIZES="${BENCH_MSG_SIZES:-}"
BUILD_DIR=""
PIN_CPU=0
ZLINK_ONLY=0
RUN_SINGLE=0
SINGLE_RUNS=""
REFRESH_STD_CACHE=0
STD_CACHE_FILE=""
PATTERN_RAW="${DEFAULT_PATTERN}"
RESULTS_DIR="${BENCH_RESULTS_DIR:-${PERF_RESULTS_DIR:-}}"
RESULTS_TAG=""
RESULT_FILE=""
OUTPUT_FILE=""
SUPPRESS_RESULT_SAVE="${BENCH_MULTI_SUPPRESS_PATTERN_RESULT_SAVE:-0}"

MULTI_WARMUP_SECONDS="${BENCH_MULTI_WARMUP_SECONDS:-2}"
MULTI_DURATION_SECONDS="${BENCH_MULTI_DURATION_SECONDS:-5}"
MULTI_CLIENTS="${BENCH_MULTI_CLIENTS:-}"
MULTI_HWM="${BENCH_MULTI_HWM:-}"
MULTI_MONITOR_HWM="${BENCH_MULTI_MONITOR_HWM:-}"
MULTI_SNDTIMEO_MS="${BENCH_MULTI_SNDTIMEO_MS:-200}"
MULTI_RCVTIMEO_MS="${BENCH_MULTI_RCVTIMEO_MS:-200}"
MULTI_CONNECT_READY_TIMEOUT_MS="${BENCH_MULTI_CONNECT_READY_TIMEOUT_MS:-5000}"
MULTI_TRANSPORT_TRANSITION_MS="${BENCH_TRANSPORT_TRANSITION_MS:-${PERF_TRANSPORT_TRANSITION_MS:-3000}}"
MULTI_SERVER_READY_TIMEOUT_MS="${BENCH_MULTI_SERVER_READY_TIMEOUT_MS:-${BENCH_SERVER_READY_TIMEOUT_MS:-${PERF_SERVER_READY_TIMEOUT_MS:-10000}}}"
MULTI_SERVER_SHUTDOWN_TIMEOUT_MS="${BENCH_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS:-${BENCH_SERVER_SHUTDOWN_TIMEOUT_MS:-${PERF_SERVER_SHUTDOWN_TIMEOUT_MS:-5000}}}"
MULTI_SERVER_BIND_PORT="${BENCH_MULTI_SERVER_BIND_PORT:-${PERF_SERVER_BIND_PORT:-0}}"
MULTI_CONNECT_CONCURRENCY="${BENCH_MULTI_CONNECT_CONCURRENCY:-}"
MULTI_IO_THREADS="${BENCH_IO_THREADS:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --pattern)
      PATTERN_RAW="${2:-}"
      shift 2
      ;;
    --runs)
      RUNS="${2:-}"
      shift 2
      ;;
    --transport|--transports)
      TRANSPORT="${2:-}"
      shift 2
      ;;
    --msg-sizes)
      MSG_SIZES="${2:-}"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="${2:-}"
      shift 2
      ;;
    --output)
      OUTPUT_FILE="${2:-}"
      shift 2
      ;;
    --results-dir)
      RESULTS_DIR="${2:-}"
      shift 2
      ;;
    --results-tag)
      RESULTS_TAG="${2:-}"
      shift 2
      ;;
    --result-file)
      RESULT_FILE="${2:-}"
      shift 2
      ;;
    --zlink-only)
      ZLINK_ONLY=1
      shift
      ;;
    --single)
      RUN_SINGLE=1
      if [[ $# -ge 2 && "${2:-}" =~ ^[0-9]+$ ]]; then
        SINGLE_RUNS="${2:-}"
        shift 2
      else
        shift
      fi
      ;;
    --refresh-std-cache)
      REFRESH_STD_CACHE=1
      shift
      ;;
    --std-cache-file)
      STD_CACHE_FILE="${2:-}"
      shift 2
      ;;
    --pin-cpu)
      PIN_CPU=1
      shift
      ;;
    --warmup|--multi-warmup-seconds)
      MULTI_WARMUP_SECONDS="${2:-}"
      shift 2
      ;;
    --duration|--multi-duration-seconds)
      MULTI_DURATION_SECONDS="${2:-}"
      shift 2
      ;;
    --clients|--multi-clients)
      MULTI_CLIENTS="${2:-}"
      shift 2
      ;;
    --hwm|--multi-hwm)
      MULTI_HWM="${2:-}"
      shift 2
      ;;
    --monitor-hwm|--multi-monitor-hwm)
      MULTI_MONITOR_HWM="${2:-}"
      shift 2
      ;;
    --sndtimeo-ms|--multi-sndtimeo-ms)
      MULTI_SNDTIMEO_MS="${2:-}"
      shift 2
      ;;
    --rcvtimeo-ms|--multi-rcvtimeo-ms)
      MULTI_RCVTIMEO_MS="${2:-}"
      shift 2
      ;;
    --connect-ready-timeout-ms|--multi-connect-ready-timeout-ms)
      MULTI_CONNECT_READY_TIMEOUT_MS="${2:-}"
      shift 2
      ;;
    --connect-concurrency|--multi-connect-concurrency)
      MULTI_CONNECT_CONCURRENCY="${2:-}"
      shift 2
      ;;
    --io-threads)
      MULTI_IO_THREADS="${2:-}"
      shift 2
      ;;
    *)
      echo "Error: unknown option '$1'" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "${TRANSPORT}" ]]; then
  echo "Error: --transport requires a non-empty value." >&2
  exit 1
fi

if [[ -z "${RUNS}" || ! "${RUNS}" =~ ^[0-9]+$ || "${RUNS}" -lt 1 ]]; then
  echo "Error: --runs must be a positive integer." >&2
  exit 1
fi
if ! is_uint "${MULTI_TRANSPORT_TRANSITION_MS}"; then
  echo "Error: BENCH_TRANSPORT_TRANSITION_MS must be a non-negative integer." >&2
  exit 1
fi
if ! is_uint "${MULTI_SERVER_READY_TIMEOUT_MS}"; then
  echo "Error: BENCH_SERVER_READY_TIMEOUT_MS must be a non-negative integer." >&2
  exit 1
fi
if ! is_uint "${MULTI_SERVER_SHUTDOWN_TIMEOUT_MS}"; then
  echo "Error: BENCH_SERVER_SHUTDOWN_TIMEOUT_MS must be a non-negative integer." >&2
  exit 1
fi
if ! is_uint "${MULTI_SERVER_BIND_PORT}" || (( MULTI_SERVER_BIND_PORT > 65535 )); then
  echo "Error: BENCH_MULTI_SERVER_BIND_PORT must be an integer in range 0..65535." >&2
  exit 1
fi

if [[ "${PATTERN_RAW}" == *","* ]]; then
  echo "Error: only one pattern is supported. Use a single value (e.g. router_router)." >&2
  exit 1
fi
PATTERN_INTERNAL="$(normalize_pattern "${PATTERN_RAW}")" || {
  echo "Error: unsupported pattern '${PATTERN_RAW}'." >&2
  echo "Supported: dealer_dealer, dealer_router, router_router, pubsub, stream" >&2
  exit 1
}
PATTERN_COMPARISON_ARG="$(comparison_pattern_arg "${PATTERN_RAW}")" || {
  echo "Error: unsupported comparison pattern '${PATTERN_RAW}'." >&2
  exit 1
}

if [[ -z "${RESULTS_DIR}" ]]; then
  RESULTS_DIR="${DEFAULT_RESULTS_ROOT}"
fi
RESULTS_DIR="$(realpath -m "${RESULTS_DIR}")"

if [[ -n "${OUTPUT_FILE}" ]]; then
  OUTPUT_FILE="$(realpath -m "${OUTPUT_FILE}")"
fi

if [[ -n "${RESULT_FILE}" ]]; then
  RESULT_FILE="$(realpath -m "${RESULT_FILE}")"
elif [[ "${SUPPRESS_RESULT_SAVE}" != "1" ]]; then
  TS="$(date +%Y%m%d_%H%M%S)"
  NAME="perf_$(detect_platform_tag)_${TS}"
  if [[ -n "${RESULTS_TAG}" ]]; then
    NAME="${NAME}_${RESULTS_TAG}"
  fi
  RESULT_FILE="${RESULTS_DIR}/multi/report/${NAME}.txt"
fi

if [[ -n "${OUTPUT_FILE}" && -n "${RESULT_FILE}" && "${OUTPUT_FILE}" == "${RESULT_FILE}" ]]; then
  echo "Error: --output cannot point to the same file as result output." >&2
  exit 1
fi

cleanup_old_results_dirs "${RESULTS_DIR}"

if [[ -n "${RESULT_FILE}" ]]; then
  mkdir -p "$(dirname "${RESULT_FILE}")"
fi
if [[ -n "${OUTPUT_FILE}" ]]; then
  mkdir -p "$(dirname "${OUTPUT_FILE}")"
fi

export BENCH_TRANSPORTS="${TRANSPORT}"
export BENCH_MULTI_WARMUP_SECONDS="${MULTI_WARMUP_SECONDS}"
export BENCH_MULTI_DURATION_SECONDS="${MULTI_DURATION_SECONDS}"
export BENCH_MULTI_SNDTIMEO_MS="${MULTI_SNDTIMEO_MS}"
export BENCH_MULTI_RCVTIMEO_MS="${MULTI_RCVTIMEO_MS}"
export BENCH_MULTI_CONNECT_READY_TIMEOUT_MS="${MULTI_CONNECT_READY_TIMEOUT_MS}"
export BENCH_TRANSPORT_TRANSITION_MS="${MULTI_TRANSPORT_TRANSITION_MS}"
export BENCH_MULTI_SERVER_READY_TIMEOUT_MS="${MULTI_SERVER_READY_TIMEOUT_MS}"
export BENCH_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS="${MULTI_SERVER_SHUTDOWN_TIMEOUT_MS}"
export BENCH_MULTI_SERVER_BIND_PORT="${MULTI_SERVER_BIND_PORT}"
if [[ -n "${MULTI_MONITOR_HWM}" ]]; then
  export BENCH_MULTI_MONITOR_HWM="${MULTI_MONITOR_HWM}"
fi

effective_clients="${MULTI_CLIENTS:-${BENCH_MULTI_CLIENTS:-}}"
if [[ -z "${effective_clients}" ]]; then
  if [[ "${PATTERN_INTERNAL}" == "MULTI_STREAM" ]]; then
    effective_clients="${BENCH_MULTI_DEFAULT_STREAM_CLIENTS:-10000}"
  else
    effective_clients="${BENCH_MULTI_DEFAULT_CLIENTS:-100}"
  fi
fi
effective_hwm="${MULTI_HWM:-}"
if [[ -z "${effective_hwm}" ]]; then
  if [[ "${PATTERN_INTERNAL}" == "MULTI_STREAM" ]]; then
    effective_hwm="${BENCH_MULTI_DEFAULT_STREAM_HWM:-10}"
  else
    effective_hwm="${BENCH_MULTI_DEFAULT_HWM:-100}"
  fi
fi
export BENCH_MULTI_HWM="${effective_hwm}"

effective_connect_concurrency="$(resolve_effective_connect_concurrency "${effective_clients}")"
ensure_nofile_limit "${effective_clients}"

if [[ -n "${MULTI_IO_THREADS}" ]]; then
  export BENCH_IO_THREADS="${MULTI_IO_THREADS}"
elif [[ "${PATTERN_INTERNAL}" == "MULTI_STREAM" && -z "${BENCH_IO_THREADS:-}" ]]; then
  export BENCH_IO_THREADS="4"
elif [[ -z "${BENCH_IO_THREADS:-}" && "${effective_clients}" =~ ^[0-9]+$ && "${effective_clients}" -ge 512 ]]; then
  export BENCH_IO_THREADS="4"
fi
if [[ -n "${BENCH_IO_THREADS:-}" ]]; then
  export PERF_IO_THREADS="${BENCH_IO_THREADS}"
fi

export BENCH_MULTI_CLIENTS="${effective_clients}"
export PERF_MULTI_CLIENTS="${effective_clients}"
export BENCH_MULTI_PATTERN="${PATTERN_INTERNAL}"
export BENCH_MULTI_CONNECT_CONCURRENCY="${effective_connect_concurrency}"
if [[ -n "${MSG_SIZES}" ]]; then
  export BENCH_MSG_SIZES="${MSG_SIZES}"
fi
if [[ "${PATTERN_INTERNAL}" == "MULTI_STREAM" ]]; then
  if [[ -z "${BENCH_STREAM_CLIENT_THREADS:-}" ]]; then
    if [[ "${effective_clients}" =~ ^[0-9]+$ && "${effective_clients}" -ge 10000 ]]; then
      export BENCH_STREAM_CLIENT_THREADS="4"
    else
      export BENCH_STREAM_CLIENT_THREADS="1"
    fi
  fi
  if [[ -z "${PERF_MULTI_STREAM_CONNECT_BATCH:-}" ]]; then
    if [[ "${effective_clients}" =~ ^[0-9]+$ && "${effective_clients}" -ge 10000 ]]; then
      export PERF_MULTI_STREAM_CONNECT_BATCH="256"
    fi
  fi
  export BENCH_STREAM_HWM="${effective_hwm}"
fi

EXTRA_ARGS=(--runs "${RUNS}")
if [[ -n "${BUILD_DIR}" ]]; then
  EXTRA_ARGS+=(--build-dir "${BUILD_DIR}")
fi
if [[ "${PIN_CPU}" -eq 1 ]]; then
  EXTRA_ARGS+=(--pin-cpu)
fi
if [[ "${ZLINK_ONLY}" -eq 1 ]]; then
  EXTRA_ARGS+=(--zlink-only)
fi
if [[ "${REFRESH_STD_CACHE}" -eq 1 ]]; then
  EXTRA_ARGS+=(--refresh-std-cache)
fi
if [[ -n "${STD_CACHE_FILE}" ]]; then
  EXTRA_ARGS+=(--std-cache-file "${STD_CACHE_FILE}")
fi

SHOW_TOTAL_TIME=1

set +e
if [[ -n "${RESULT_FILE}" && -n "${OUTPUT_FILE}" ]]; then
  PYTHONUNBUFFERED=1 python3 -u "${SCRIPT_DIR}/run_comparison.py" "${PATTERN_COMPARISON_ARG}" "${EXTRA_ARGS[@]}" 2>&1 | tee "${RESULT_FILE}" "${OUTPUT_FILE}"
elif [[ -n "${RESULT_FILE}" ]]; then
  PYTHONUNBUFFERED=1 python3 -u "${SCRIPT_DIR}/run_comparison.py" "${PATTERN_COMPARISON_ARG}" "${EXTRA_ARGS[@]}" 2>&1 | tee "${RESULT_FILE}"
elif [[ -n "${OUTPUT_FILE}" ]]; then
  PYTHONUNBUFFERED=1 python3 -u "${SCRIPT_DIR}/run_comparison.py" "${PATTERN_COMPARISON_ARG}" "${EXTRA_ARGS[@]}" 2>&1 | tee "${OUTPUT_FILE}"
else
  PYTHONUNBUFFERED=1 python3 -u "${SCRIPT_DIR}/run_comparison.py" "${PATTERN_COMPARISON_ARG}" "${EXTRA_ARGS[@]}"
fi
run_status=${PIPESTATUS[0]}
set -e

if [[ -n "${RESULT_FILE}" ]]; then
  enforce_file_retention "$(dirname "${RESULT_FILE}")"
fi

if [[ "${run_status}" -ne 0 ]]; then
  exit "${run_status}"
fi

if [[ "${RUN_SINGLE}" -eq 1 ]]; then
  case "${PATTERN_INTERNAL}" in
    MULTI_DEALER_DEALER)
      SINGLE_PATTERN="dealer_dealer"
      ;;
    MULTI_DEALER_ROUTER)
      SINGLE_PATTERN="dealer_router"
      ;;
    MULTI_ROUTER_ROUTER)
      SINGLE_PATTERN="router_router"
      ;;
    MULTI_PUBSUB)
      SINGLE_PATTERN="pubsub"
      ;;
    *)
      SINGLE_PATTERN=""
      ;;
  esac

  if [[ -z "${SINGLE_PATTERN}" ]]; then
    echo "Warning: no matching single pattern for ${PATTERN_INTERNAL}; skipping --single." >&2
    exit 0
  fi

  SINGLE_RUNS_EFFECTIVE="${SINGLE_RUNS:-${RUNS}}"
  SINGLE_SCRIPT="${SCRIPT_DIR}/../single/run_benchmarks.sh"
  SINGLE_ARGS=(--pattern "${SINGLE_PATTERN}" --transport "${TRANSPORT}" --runs "${SINGLE_RUNS_EFFECTIVE}" --results-dir "${RESULTS_DIR}")

  if [[ -n "${RESULTS_TAG}" ]]; then
    SINGLE_ARGS+=(--results-tag "${RESULTS_TAG}")
  fi
  if [[ -n "${MSG_SIZES}" ]]; then
    SINGLE_ARGS+=(--msg-sizes "${MSG_SIZES}")
  fi
  if [[ -n "${BUILD_DIR}" ]]; then
    SINGLE_ARGS+=(--build-dir "${BUILD_DIR}")
  fi
  if [[ "${PIN_CPU}" -eq 1 ]]; then
    SINGLE_ARGS+=(--pin-cpu)
  fi

  echo "=== Running matching single benchmark: ${SINGLE_PATTERN} (runs=${SINGLE_RUNS_EFFECTIVE}) ==="
  BENCH_SUPPRESS_TOTAL_TIME=1 "${SINGLE_SCRIPT}" "${SINGLE_ARGS[@]}"
fi
