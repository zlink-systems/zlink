#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_DIR="$(cd "${ROOT_DIR}/../.." && pwd)"
cd "${ROOT_DIR}"

export GOCACHE="${GOCACHE:-/tmp/zlink-go-cache}"
export GOTMPDIR="${GOTMPDIR:-/tmp/zlink-go-tmp}"
mkdir -p "${GOCACHE}" "${GOTMPDIR}"

# Resolve the SONAME from the binding package header so another workstream's
# root VERSION cannot redirect this runner to a different native payload.
CORE_VERSION="$(sed -n 's/^#define ZLINK_VERSION_MAJOR //p' "${ROOT_DIR}/include/zlink.h" | head -n1).$(sed -n 's/^#define ZLINK_VERSION_MINOR //p' "${ROOT_DIR}/include/zlink.h" | head -n1).$(sed -n 's/^#define ZLINK_VERSION_PATCH //p' "${ROOT_DIR}/include/zlink.h" | head -n1)"
PERF_REPORT_PY="${REPO_DIR}/bindings/python/perf/perf_report.py"
# MULTI_STREAM uses the shared C reference client binary (the measured
# surface is the Go STREAM server); the Go binding has no STREAM client.
STREAM_CLIENT_DIR="${REPO_DIR}/bindings/c/perf/common/streamclient"
STREAM_CLIENT="${REPO_DIR}/bindings/c/build/perf/perf_stream_client"
STREAM_CLIENT_FALLBACK="${STREAM_CLIENT_DIR}/build/perf_stream_client"
TOTAL_TIME_ENABLED=0

ensure_stream_client() {
  if [[ -x "${STREAM_CLIENT}" ]]; then
    return 0
  fi
  if [[ -x "${STREAM_CLIENT_FALLBACK}" ]]; then
    STREAM_CLIENT="${STREAM_CLIENT_FALLBACK}"
    return 0
  fi
  if bash "${STREAM_CLIENT_DIR}/build.sh" >/dev/null 2>&1 \
    && [[ -x "${STREAM_CLIENT_FALLBACK}" ]]; then
    STREAM_CLIENT="${STREAM_CLIENT_FALLBACK}"
    return 0
  fi
  echo "Error: shared perf_stream_client not found and build failed" >&2
  return 1
}

run_external_stream_client() {
  local transport="$1"
  local size="$2"
  local duration="$3"
  local clients="$4"
  local endpoint="$5"
  local client_out="$6"
  local client_err="$7"
  ensure_stream_client || return 1
  local stream_client_io_threads="${CLIENT_IO_THREADS:-${PERF_MULTI_CLIENT_IO_THREADS:-${IO_THREADS:-${PERF_IO_THREADS:-${PERF_MULTI_DEFAULT_IO_THREADS:-${PERF_DEFAULT_IO_THREADS:-4}}}}}}"
  local stream_clients="${clients}"
  local non_tcp_max="${PERF_STREAM_NON_TCP_CLIENTS_MAX:-${PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX:-10000}}"
  if [[ "${transport}" != "tcp" && "${stream_clients}" =~ ^[0-9]+$ \
        && "${non_tcp_max}" =~ ^[0-9]+$ && "${stream_clients}" -gt "${non_tcp_max}" ]]; then
    stream_clients="${non_tcp_max}"
  fi
  env \
    "PERF_PATTERN=STREAM" \
    "PERF_MULTI_PATTERN=STREAM" \
    "PERF_MULTI_TRANSPORT=${transport}" \
    "PERF_MULTI_COMPONENT=client" \
    "${STREAM_CLIENT}" --transport "${transport}" --pattern STREAM \
    --sizes "${size}" --runs 1 --duration "${duration}" \
    --ccu "${stream_clients}" --io-threads "${stream_client_io_threads}" \
    --send-stop-token 1 --endpoint "${endpoint}" \
    > "${client_out}" 2> "${client_err}"
}

print_total_time() {
  local status="${1:-0}"
  if [[ "${TOTAL_TIME_ENABLED}" -ne 1 ]]; then
    return
  fi
  local elapsed
  elapsed=$(($(date +%s) - START_SECONDS))
  echo "Total benchmark time: ${elapsed}s (${elapsed}s, exit=${status})"
}
PATTERN="ALL"
DURATION="5"
MSG_SIZES=""
TRANSPORTS="${PERF_TRANSPORTS:-}"
RUNS="1"
SMOKE=0
CLIENTS=""
RESULTS_DIR="${SCRIPT_DIR}/results/multi/report"
RESULTS_TAG=""
OUTPUT_FILE=""
BUILD_DIR="${SCRIPT_DIR}/build/multi"
REUSE_BUILD=0
CLEAN_BUILD=0
PIN_CPU="off"
IO_THREADS=""
SERVER_IO_THREADS=""
CLIENT_IO_THREADS=""
HWM=""
SEND_HWM=""
RECV_HWM=""
SNDBUF=""
RCVBUF=""
SNDTIMEO_MS=""
RCVTIMEO_MS=""
AUTO_HWM_PROFILE=""
CONNECT_READY_TIMEOUT_MS=""
CONNECT_CONCURRENCY=""
SERVER_READY_TIMEOUT_MS=""
MONITOR_HWM=""
SERVER_SHUTDOWN_TIMEOUT_MS=""
SERVER_BIND_PORT=""
RUN_COOLDOWN_MS="${PERF_MULTI_RUN_COOLDOWN_MS:-${PERF_RUN_COOLDOWN_MS:-3000}}"
TRANSPORT_TRANSITION_MS="${PERF_MULTI_TRANSPORT_TRANSITION_MS:-${PERF_TRANSPORT_TRANSITION_MS:-3000}}"
PATTERN_TRANSITION_MS="${PERF_MULTI_PATTERN_TRANSITION_MS:-${PERF_PATTERN_TRANSITION_MS:-3000}}"

cleanup_report_dir() {
  local dir="$1"
  local max_files="${PERF_RESULTS_MAX_FILES:-100}"
  mkdir -p "${dir}"
  mapfile -t existing < <(find "${dir}" -maxdepth 1 -type f -name 'perf_go_multi_*.txt' | sort)
  while [[ "${#existing[@]}" -ge "${max_files}" ]]; do
    rm -f "${existing[0]}"
    existing=("${existing[@]:1}")
  done
}

resolve_results_dir() {
  local dir="$1"
  case "${dir}" in
    */multi/report|*/report)
      echo "${dir}"
      ;;
    *)
      echo "${dir}/multi/report"
      ;;
  esac
}

sleep_millis() {
  local millis="${1:-0}"
  if [[ ! "${millis}" =~ ^[0-9]+$ || "${millis}" -eq 0 ]]; then
    return
  fi
  sleep "$((millis / 1000)).$(printf '%03d' "$((millis % 1000))")"
}

is_uint() {
  [[ "${1:-}" =~ ^[0-9]+$ ]]
}

is_positive_uint() {
  local value="${1:-}"
  is_uint "${value}" && (( 10#${value} > 0 ))
}

go_gomaxprocs_floor4() {
  local value="${1:-}"
  if is_positive_uint "${value}"; then
    local numeric=$((10#${value}))
    if (( numeric > 4 )); then
      printf '%s\n' "${numeric}"
      return
    fi
  fi
  printf '4\n'
}

validate_go_gomaxprocs() {
  local source="$1"
  local value="${2:-}"
  if ! is_positive_uint "${value}"; then
    echo "Error: ${source} must be a positive integer for Go perf runs." >&2
    exit 1
  fi
}

NOFILE_SKIP_REASON=""
ensure_nofile_limit() {
  local clients="${1:-}"
  NOFILE_SKIP_REASON=""
  if [[ "${PERF_SKIP_NOFILE_CHECK:-0}" == "1" ]] || ! is_uint "${clients}"; then
    return 0
  fi
  local required=$(( clients * 3 + 4096 ))
  local soft hard
  soft="$(ulimit -Sn 2>/dev/null || true)"
  hard="$(ulimit -Hn 2>/dev/null || true)"
  [[ "${soft}" == "unlimited" ]] && return 0
  is_uint "${soft}" || return 0
  local hard_num=-1
  if [[ "${hard}" != "unlimited" ]]; then
    if is_uint "${hard}"; then hard_num="${hard}"; else hard_num="${soft}"; fi
  fi
  if (( soft < required )); then
    local target="${required}"
    if (( hard_num >= 0 && target > hard_num )); then target="${hard_num}"; fi
    if (( target > soft )); then
      ulimit -Sn "${target}" 2>/dev/null || true
      soft="$(ulimit -Sn 2>/dev/null || true)"
    fi
  fi
  if is_uint "${soft}" && (( soft >= required )); then
    return 0
  fi
  NOFILE_SKIP_REASON="clients=${clients},required=${required},soft=${soft},hard=${hard}"
  return 1
}

memory_available_kb() {
  if [[ "${PERF_SKIP_MEMORY_CHECK:-0}" == "1" || ! -r /proc/meminfo ]]; then
    printf '\n'
    return
  fi
  awk '/^MemAvailable:/ { print $2; found=1; exit } END { if (!found) print "" }' /proc/meminfo 2>/dev/null || true
}

resolve_memory_max_clients() {
  local available_kb budget_pct base_mb per_client_kb usable_kb base_kb max_clients
  available_kb="$(memory_available_kb)"
  is_uint "${available_kb}" || { printf '\n'; return; }
  budget_pct="${PERF_MULTI_MEMORY_BUDGET_PCT:-${PERF_MEMORY_BUDGET_PCT:-70}}"
  base_mb="${PERF_MULTI_MEMORY_BASE_MB:-${PERF_MEMORY_BASE_MB:-512}}"
  per_client_kb="${PERF_MULTI_MEMORY_PER_CLIENT_KB:-${PERF_MEMORY_PER_CLIENT_KB:-1024}}"
  is_uint "${budget_pct}" && (( budget_pct >= 1 && budget_pct <= 95 )) || { printf '\n'; return; }
  is_uint "${base_mb}" && is_uint "${per_client_kb}" && (( per_client_kb >= 1 )) || { printf '\n'; return; }
  usable_kb=$(( available_kb * budget_pct / 100 ))
  base_kb=$(( base_mb * 1024 ))
  if (( usable_kb <= base_kb )); then
    printf '1\n'
    return
  fi
  max_clients=$(( (usable_kb - base_kb) / per_client_kb ))
  (( max_clients < 1 )) && max_clients=1
  printf '%s\n' "${max_clients}"
}

cap_default_clients_for_memory() {
  local clients="${1:-}"
  if [[ -n "${CLIENTS}" || -n "${PERF_MULTI_CLIENTS:-}" || -n "${PERF_CLIENTS:-}" ]]; then
    printf '%s\n' "${clients}"
    return
  fi
  local max_clients
  max_clients="$(resolve_memory_max_clients)"
  if is_uint "${clients}" && is_uint "${max_clients}" && (( clients > max_clients )); then
    printf '%s\n' "${max_clients}"
    return
  fi
  printf '%s\n' "${clients}"
}

MEMORY_SKIP_REASON=""
ensure_memory_budget() {
  local clients="${1:-}"
  MEMORY_SKIP_REASON=""
  if [[ "${PERF_SKIP_MEMORY_CHECK:-0}" == "1" ]] || ! is_uint "${clients}"; then
    return 0
  fi
  local max_clients available_kb budget_pct base_mb per_client_kb
  max_clients="$(resolve_memory_max_clients)"
  if ! is_uint "${max_clients}" || (( clients <= max_clients )); then
    return 0
  fi
  available_kb="$(memory_available_kb)"
  budget_pct="${PERF_MULTI_MEMORY_BUDGET_PCT:-${PERF_MEMORY_BUDGET_PCT:-70}}"
  base_mb="${PERF_MULTI_MEMORY_BASE_MB:-${PERF_MEMORY_BASE_MB:-512}}"
  per_client_kb="${PERF_MULTI_MEMORY_PER_CLIENT_KB:-${PERF_MEMORY_PER_CLIENT_KB:-1024}}"
  MEMORY_SKIP_REASON="clients=${clients},max_clients=${max_clients},mem_available_kb=${available_kb},budget_pct=${budget_pct},base_mb=${base_mb},per_client_kb=${per_client_kb}"
  return 1
}

usage() {
  cat <<'USAGE'
Usage: bindings/go/perf/run_benchmarks_multi.sh [options]

Options:
  --pattern NAME
  --duration N
  --msg-sizes LIST
  --transports LIST
  --runs N
  --smoke
  --clients N
  --results-dir PATH
  --results-tag NAME
  --output PATH
  --build-dir PATH
  --reuse-build
  --clean-build
  --pin-cpu
  --io-threads N
  --server-io-threads N
  --client-io-threads N
  --hwm N
  --send-hwm N
  --recv-hwm N
  --buf SIZE
  --sndbuf SIZE
  --rcvbuf SIZE
  --sndtimeo N
  --rcvtimeo N
  --send-timeout-ms N
  --recv-timeout-ms N
  --connect-concurrency N
  --transport-transition-ms N
  --pattern-transition-ms N
  --server-ready-timeout-ms N
  --connect-ready-timeout-ms N
  --monitor-hwm N
  --server-shutdown-timeout-ms N
  --server-bind-port N
  --auto-hwm-profile NAME
  -h, --help

Notes:
  - Supported multi patterns:
    MULTI_DEALER_DEALER,MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER,MULTI_PUBSUB,
    MULTI_STREAM
  - If GOMAXPROCS is unset, PERF_GO_GOMAXPROCS is an explicit positive-integer override.
    Otherwise --io-threads/PERF_IO_THREADS derives Go scheduler parallelism
    with a minimum of 4.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --pattern) PATTERN="$2"; shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;;
    --msg-sizes) MSG_SIZES="$2"; shift 2 ;;
    --transports) TRANSPORTS="$2"; shift 2 ;;
    --runs) RUNS="$2"; shift 2 ;;
    --smoke) SMOKE=1; shift ;;
    --clients) CLIENTS="$2"; shift 2 ;;
    --results-dir) RESULTS_DIR="$2"; shift 2 ;;
    --results-tag) RESULTS_TAG="$2"; shift 2 ;;
    --output) OUTPUT_FILE="$2"; shift 2 ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2 ;;
    --io-threads)
      IO_THREADS="$2"
      shift 2 ;;
    --server-io-threads)
      SERVER_IO_THREADS="$2"
      shift 2 ;;
    --client-io-threads)
      CLIENT_IO_THREADS="$2"
      shift 2 ;;
    --connect-concurrency)
      CONNECT_CONCURRENCY="$2"
      shift 2 ;;
    --server-ready-timeout-ms)
      SERVER_READY_TIMEOUT_MS="$2"
      shift 2 ;;
    --monitor-hwm)
      MONITOR_HWM="$2"
      shift 2 ;;
    --server-shutdown-timeout-ms)
      SERVER_SHUTDOWN_TIMEOUT_MS="$2"
      shift 2 ;;
    --server-bind-port)
      SERVER_BIND_PORT="$2"
      shift 2 ;;
    --hwm)
      HWM="$2"
      shift 2 ;;
    --send-hwm)
      SEND_HWM="$2"
      shift 2 ;;
    --recv-hwm)
      RECV_HWM="$2"
      shift 2 ;;
    --buf)
      SNDBUF="$2"
      RCVBUF="$2"
      shift 2 ;;
    --sndbuf)
      SNDBUF="$2"
      shift 2 ;;
    --rcvbuf)
      RCVBUF="$2"
      shift 2 ;;
    --sndtimeo|--send-timeout-ms)
      SNDTIMEO_MS="$2"
      shift 2 ;;
    --rcvtimeo|--recv-timeout-ms)
      RCVTIMEO_MS="$2"
      shift 2 ;;
    --connect-ready-timeout-ms)
      CONNECT_READY_TIMEOUT_MS="$2"
      shift 2 ;;
    --transport-transition-ms)
      TRANSPORT_TRANSITION_MS="$2"
      shift 2 ;;
    --pattern-transition-ms)
      PATTERN_TRANSITION_MS="$2"
      shift 2 ;;
    --auto-hwm-profile)
      AUTO_HWM_PROFILE="$2"
      shift 2 ;;
    --reuse-build)
      REUSE_BUILD=1
      shift ;;
    --clean-build)
      CLEAN_BUILD=1
      shift ;;
    --pin-cpu)
      PIN_CPU="on"
      shift ;;
    *)
      echo "Error: unknown option $1" >&2
      exit 1 ;;
  esac
done

if [[ "${REUSE_BUILD}" -eq 1 && "${CLEAN_BUILD}" -eq 1 ]]; then
  echo "Error: --reuse-build and --clean-build are mutually exclusive." >&2
  exit 1
fi
if [[ "${SMOKE}" -eq 1 && -n "${OUTPUT_FILE}" ]]; then
  echo "Error: --smoke cannot be combined with --output." >&2
  exit 1
fi

if [[ -n "${HWM}" ]]; then
  export PERF_MULTI_HWM="${HWM}"
fi
if [[ -n "${IO_THREADS}" ]]; then
  export PERF_IO_THREADS="${IO_THREADS}"
fi
if [[ -n "${SERVER_IO_THREADS}" ]]; then
  export PERF_MULTI_SERVER_IO_THREADS="${SERVER_IO_THREADS}"
fi
if [[ -n "${CLIENT_IO_THREADS}" ]]; then
  export PERF_MULTI_CLIENT_IO_THREADS="${CLIENT_IO_THREADS}"
fi
if [[ -n "${SEND_HWM}" ]]; then
  export PERF_MULTI_SNDHWM="${SEND_HWM}"
fi
if [[ -n "${RECV_HWM}" ]]; then
  export PERF_MULTI_RCVHWM="${RECV_HWM}"
fi
if [[ -n "${SNDBUF}" ]]; then
  export PERF_MULTI_SNDBUF="${SNDBUF}"
fi
if [[ -n "${RCVBUF}" ]]; then
  export PERF_MULTI_RCVBUF="${RCVBUF}"
fi
if [[ -n "${SNDTIMEO_MS}" ]]; then
  export PERF_MULTI_SNDTIMEO_MS="${SNDTIMEO_MS}"
fi
if [[ -n "${RCVTIMEO_MS}" ]]; then
  export PERF_MULTI_RCVTIMEO_MS="${RCVTIMEO_MS}"
fi
if [[ -n "${CONNECT_READY_TIMEOUT_MS}" ]]; then
  export PERF_MULTI_CONNECT_READY_TIMEOUT_MS="${CONNECT_READY_TIMEOUT_MS}"
fi
if [[ -n "${AUTO_HWM_PROFILE}" ]]; then
  export PERF_CTX_AUTO_HWM_PROFILE="${AUTO_HWM_PROFILE}"
fi
if [[ -n "${CONNECT_CONCURRENCY}" ]]; then
  export PERF_MULTI_CONNECT_CONCURRENCY="${CONNECT_CONCURRENCY}"
fi
if [[ -n "${SERVER_READY_TIMEOUT_MS}" ]]; then
  export PERF_MULTI_SERVER_READY_TIMEOUT_MS="${SERVER_READY_TIMEOUT_MS}"
fi
if [[ -n "${MONITOR_HWM}" ]]; then
  export PERF_MULTI_MONITOR_HWM="${MONITOR_HWM}"
fi
if [[ -n "${SERVER_SHUTDOWN_TIMEOUT_MS}" ]]; then
  export PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS="${SERVER_SHUTDOWN_TIMEOUT_MS}"
fi
if [[ -n "${SERVER_BIND_PORT}" ]]; then
  export PERF_MULTI_SERVER_BIND_PORT="${SERVER_BIND_PORT}"
fi
GO_GOMAXPROCS_SOURCE="env:GOMAXPROCS"
if [[ -n "${GOMAXPROCS:-}" ]]; then
  validate_go_gomaxprocs "GOMAXPROCS" "${GOMAXPROCS}"
else
  if [[ -n "${PERF_GO_GOMAXPROCS:-}" ]]; then
    validate_go_gomaxprocs "PERF_GO_GOMAXPROCS" "${PERF_GO_GOMAXPROCS}"
    export GOMAXPROCS="${PERF_GO_GOMAXPROCS}"
    GO_GOMAXPROCS_SOURCE="PERF_GO_GOMAXPROCS"
  elif is_positive_uint "${IO_THREADS}"; then
    export GOMAXPROCS="$(go_gomaxprocs_floor4 "${IO_THREADS}")"
    GO_GOMAXPROCS_SOURCE="--io-threads"
  elif is_positive_uint "${PERF_IO_THREADS:-}"; then
    export GOMAXPROCS="$(go_gomaxprocs_floor4 "${PERF_IO_THREADS}")"
    GO_GOMAXPROCS_SOURCE="PERF_IO_THREADS"
  elif is_positive_uint "${PERF_MULTI_DEFAULT_IO_THREADS:-}"; then
    export GOMAXPROCS="$(go_gomaxprocs_floor4 "${PERF_MULTI_DEFAULT_IO_THREADS}")"
    GO_GOMAXPROCS_SOURCE="PERF_MULTI_DEFAULT_IO_THREADS"
  elif is_positive_uint "${PERF_DEFAULT_IO_THREADS:-}"; then
    export GOMAXPROCS="$(go_gomaxprocs_floor4 "${PERF_DEFAULT_IO_THREADS}")"
    GO_GOMAXPROCS_SOURCE="PERF_DEFAULT_IO_THREADS"
  else
    export GOMAXPROCS="4"
    GO_GOMAXPROCS_SOURCE="default"
  fi
fi

case "$(uname -s)" in
  Linux*) PLATFORM="linux" ;;
  Darwin*) PLATFORM="macos" ;;
  *) PLATFORM="windows" ;;
esac

GO_MULTI_BIN="${BUILD_DIR}/perf_multi"

build_go_perf_binary() {
  if [[ "${CLEAN_BUILD}" -eq 1 ]]; then
    rm -rf "${BUILD_DIR}"
  fi
  mkdir -p "${BUILD_DIR}"
  if [[ "${REUSE_BUILD}" -eq 1 ]]; then
    if [[ ! -x "${GO_MULTI_BIN}" ]]; then
      echo "existing Go multi perf binary not found for --reuse-build: ${GO_MULTI_BIN}" >&2
      exit 1
    fi
    return
  fi
  go build -o "${GO_MULTI_BIN}" ./perf/multi
}

run_go_perf() {
  local package="$1"
  shift
  if [[ "${package}" != "./perf/multi" ]]; then
    echo "Error: unsupported Go perf package: ${package}" >&2
    return 1
  fi
  if [[ "${PIN_CPU}" != "on" ]]; then
    exec "${GO_MULTI_BIN}" "$@"
    return
  fi

  case "$(uname -s)" in
    Linux*)
      if ! command -v taskset >/dev/null 2>&1; then
        echo "Error: --pin-cpu requires taskset on Linux" >&2
        return 1
      fi
      exec taskset -c 0 "${GO_MULTI_BIN}" "$@"
      ;;
    *)
      echo "Error: --pin-cpu is not supported by this runner on $(uname -s)" >&2
      return 1
      ;;
  esac
}

prepare_core_runtime() {
  local native_dir="${ZLINK_GO_NATIVE_DIR:-${ROOT_DIR}/native}"
  case "${PLATFORM}:$(uname -m)" in
    linux:x86_64|linux:amd64) native_dir="${native_dir}/linux-x86_64" ;;
    linux:aarch64|linux:arm64) native_dir="${native_dir}/linux-aarch64" ;;
    macos:x86_64|macos:amd64) native_dir="${native_dir}/darwin-x86_64" ;;
    macos:arm64|macos:aarch64) native_dir="${native_dir}/darwin-aarch64" ;;
    *) echo "unsupported Go package platform: ${PLATFORM}:$(uname -m)" >&2; exit 1 ;;
  esac
  local runtime="${native_dir}/libzlink.dylib"
  if [[ "${PLATFORM}" == "linux" ]]; then
    runtime="${native_dir}/libzlink.so.${CORE_VERSION}"
  fi
  if [[ ! -f "${runtime}" ]]; then
    echo "Go package runtime not found: ${runtime}" >&2
    echo "Set ZLINK_GO_NATIVE_DIR to an extracted package runtime directory." >&2
    exit 1
  fi
  local runtime_sha
  if command -v sha256sum >/dev/null 2>&1; then
    runtime_sha="$(sha256sum "${runtime}" | awk '{print $1}')"
  else
    runtime_sha="$(shasum -a 256 "${runtime}" | awk '{print $1}')"
  fi
  echo "Go package runtime: ${runtime}"
  echo "Go package runtime sha256: ${runtime_sha}"
  export LD_LIBRARY_PATH="${native_dir}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
}

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
TAG_SUFFIX=""
if [[ -n "${RESULTS_TAG}" ]]; then
  TAG_SUFFIX="_${RESULTS_TAG}"
fi
RESULTS_DIR="$(resolve_results_dir "${RESULTS_DIR}")"
if [[ "${SMOKE}" -eq 0 ]]; then
  RESULTS_FILE="${RESULTS_DIR}/perf_go_multi_${PLATFORM}_${TIMESTAMP}${TAG_SUFFIX}.txt"
  mkdir -p "${RESULTS_DIR}"
  cleanup_report_dir "${RESULTS_DIR}"
fi
prepare_core_runtime
build_go_perf_binary
START_SECONDS="$(date +%s)"
TOTAL_TIME_ENABLED=1
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/zlink-go-multi.XXXXXX")"
RAW_RESULTS_FILE="${TMP_DIR}/result_data.log"
if [[ "${SMOKE}" -eq 1 ]]; then
  RESULTS_FILE="${TMP_DIR}/smoke-report.txt"
fi
cleanup() {
  local status=$?
  rm -rf "${TMP_DIR}"
  print_total_time "${status}"
}
trap cleanup EXIT

normalize_multi_pattern() {
  local raw="$1"
  raw="${raw^^}"
  if [[ "${raw}" == MULTI_* ]]; then
    raw="${raw#MULTI_}"
  fi
  if [[ "${raw}" == "STREAMS" ]]; then
    raw="STREAM"
  fi
  if [[ "${raw}" == MULTI_* ]]; then
    echo "${raw}"
  else
    echo "MULTI_${raw}"
  fi
}

pattern_msg_sizes() {
  local pattern="$1"
  if [[ -n "${MSG_SIZES}" ]]; then
    echo "${MSG_SIZES}"
    return
  fi
  if [[ "${pattern}" == "MULTI_STREAM" ]]; then
    if [[ -n "${PERF_MULTI_STREAM_MSG_SIZES:-}" ]]; then
      echo "${PERF_MULTI_STREAM_MSG_SIZES}"
    elif [[ -n "${PERF_STREAM_MSG_SIZES:-}" ]]; then
      echo "${PERF_STREAM_MSG_SIZES}"
    elif [[ -n "${PERF_MSG_SIZES:-}" ]]; then
      echo "${PERF_MSG_SIZES}"
    else
      echo "64,256,1024,65536"
    fi
    return
  fi
  if [[ -n "${PERF_MSG_SIZES:-}" ]]; then
    echo "${PERF_MSG_SIZES}"
  else
    echo "64,256,1024,4096,65536,131072"
  fi
}

if [[ "${PATTERN}" == "ALL" ]]; then
	PATTERNS=("MULTI_DEALER_DEALER" "MULTI_DEALER_ROUTER" "MULTI_ROUTER_ROUTER" "MULTI_PUBSUB" "MULTI_STREAM")
else
  IFS=',' read -r -a RAW_PATTERNS <<< "${PATTERN}"
  PATTERNS=()
  for raw_pattern in "${RAW_PATTERNS[@]}"; do
    PATTERNS+=("$(normalize_multi_pattern "${raw_pattern}")")
  done
fi

if [[ -n "${TRANSPORTS}" ]]; then
  IFS=',' read -r -a XPORTS_FILTER <<< "${TRANSPORTS}"
else
  XPORTS_FILTER=()
fi

pattern_transports() {
  case "$1" in
	MULTI_STREAM|MULTI_PUBSUB|MULTI_DEALER_DEALER|MULTI_DEALER_ROUTER|MULTI_ROUTER_ROUTER)
      echo "tcp tls ws wss"
      ;;
    *)
      echo "tcp tls ws wss"
      ;;
  esac
}

transport_enabled() {
  local transport="$1"
  if [[ "${#XPORTS_FILTER[@]}" -eq 0 ]]; then
    return 0
  fi
  local candidate
  for candidate in "${XPORTS_FILTER[@]}"; do
    if [[ "${candidate}" == "${transport}" ]]; then
      return 0
    fi
  done
  return 1
}

join_by() {
  local delim="$1"
  shift
  local first=1
  local item
  for item in "$@"; do
    if [[ "${first}" -eq 1 ]]; then
      printf '%s' "${item}"
      first=0
    else
      printf '%s%s' "${delim}" "${item}"
    fi
  done
}

resolve_multi_effective_transports() {
  declare -A seen=()
  local pattern transport
  local resolved=()
  for pattern in "${PATTERNS[@]}"; do
    read -r -a PATTERN_XPORTS <<< "$(pattern_transports "${pattern}")"
    for transport in "${PATTERN_XPORTS[@]}"; do
      if ! transport_enabled "${transport}"; then
        continue
      fi
      if [[ -n "${seen[${transport}]:-}" ]]; then
        continue
      fi
      seen["${transport}"]=1
      resolved+=("${transport}")
    done
  done
  join_by "," "${resolved[@]}"
}

resolve_multi_effective_msg_sizes() {
  declare -A seen=()
  local pattern size_list size
  local resolved=()
  for pattern in "${PATTERNS[@]}"; do
    size_list="$(pattern_msg_sizes "${pattern}")"
    IFS=',' read -r -a SIZE_ITEMS <<< "${size_list}"
    for size in "${SIZE_ITEMS[@]}"; do
      if [[ -n "${seen[${size}]:-}" ]]; then
        continue
      fi
      seen["${size}"]=1
      resolved+=("${size}")
    done
  done
  join_by "," "${resolved[@]}"
}

EFFECTIVE_PATTERNS_CSV="$(join_by "," "${PATTERNS[@]}")"
EFFECTIVE_TRANSPORTS_CSV="$(resolve_multi_effective_transports)"
EFFECTIVE_MSG_SIZES_CSV="$(resolve_multi_effective_msg_sizes)"
if [[ -n "${CLIENTS}" ]]; then
  CLIENTS_DISPLAY="${CLIENTS}"
elif [[ -n "${PERF_MULTI_CLIENTS:-}" ]]; then
  CLIENTS_DISPLAY="${PERF_MULTI_CLIENTS}"
elif [[ -n "${PERF_CLIENTS:-}" ]]; then
  CLIENTS_DISPLAY="${PERF_CLIENTS}"
else
  CLIENTS_DISPLAY="auto (default=${PERF_MULTI_DEFAULT_CLIENTS:-${PERF_DEFAULT_CLIENTS:-100}}, stream=${PERF_MULTI_DEFAULT_STREAM_CLIENTS:-${PERF_STREAM_DEFAULT_CLIENTS:-10000}})"
fi

effective_or_auto() {
  local value="$1"
  local fallback="${2:-auto-hwm}"
  if [[ -n "${value}" ]]; then
    printf '%s\n' "${value}"
  else
    printf '%s\n' "${fallback}"
  fi
}

effective_multi_server_io_threads() {
  if [[ -n "${SERVER_IO_THREADS}" ]]; then
    printf '%s\n' "${SERVER_IO_THREADS}"
  elif [[ -n "${PERF_MULTI_SERVER_IO_THREADS:-}" ]]; then
    printf '%s\n' "${PERF_MULTI_SERVER_IO_THREADS}"
  elif [[ -n "${IO_THREADS}" ]]; then
    printf '%s\n' "${IO_THREADS}"
  elif [[ -n "${PERF_IO_THREADS:-}" ]]; then
    printf '%s\n' "${PERF_IO_THREADS}"
  elif [[ -n "${PERF_MULTI_DEFAULT_IO_THREADS:-}" ]]; then
    printf '%s\n' "${PERF_MULTI_DEFAULT_IO_THREADS} (default)"
  elif [[ -n "${PERF_DEFAULT_IO_THREADS:-}" ]]; then
    printf '%s\n' "${PERF_DEFAULT_IO_THREADS} (default)"
  else
    printf '%s\n' "4 (default)"
  fi
}

effective_multi_client_io_threads() {
  if [[ -n "${CLIENT_IO_THREADS}" ]]; then
    printf '%s\n' "${CLIENT_IO_THREADS}"
  elif [[ -n "${PERF_MULTI_CLIENT_IO_THREADS:-}" ]]; then
    printf '%s\n' "${PERF_MULTI_CLIENT_IO_THREADS}"
  elif [[ -n "${IO_THREADS}" ]]; then
    printf '%s\n' "${IO_THREADS}"
  elif [[ -n "${PERF_IO_THREADS:-}" ]]; then
    printf '%s\n' "${PERF_IO_THREADS}"
  elif [[ -n "${PERF_MULTI_DEFAULT_IO_THREADS:-}" ]]; then
    printf '%s\n' "${PERF_MULTI_DEFAULT_IO_THREADS} (default)"
  elif [[ -n "${PERF_DEFAULT_IO_THREADS:-}" ]]; then
    printf '%s\n' "${PERF_DEFAULT_IO_THREADS} (default)"
  else
    printf '%s\n' "4 (default)"
  fi
}

emit_meta_lines() {
  local cpu_name="unknown"
  if [[ -r /proc/cpuinfo ]]; then
    cpu_name="$(awk -F: '/^model name/ { sub(/^ /, "", $2); print $2; exit }' /proc/cpuinfo)"
  fi
  echo "META,os,$(uname -s) $(uname -r)"
  echo "META,cpu,${cpu_name:-unknown}"
  echo "META,cores,$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 0)"
  echo "META,build,Release"
  echo "META,commit,$(git -C "${REPO_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "META,timestamp,$(date -Iseconds)"
  echo "META,load_avg,$(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null || echo unknown)"
  echo "META,runs,${RUNS}"
  echo "META,clients,${CLIENTS_DISPLAY}"
}

emit_effective_options_multi() {
  local section="$1"
  echo "## Effective Options (${section})"
  echo "- lang: go"
  echo "- suite: multi"
  echo "- runs: ${RUNS}"
  echo "- patterns: ${EFFECTIVE_PATTERNS_CSV}"
  echo "- transports: ${EFFECTIVE_TRANSPORTS_CSV}"
  echo "- msg_sizes: ${EFFECTIVE_MSG_SIZES_CSV}"
  echo "- duration_seconds: ${DURATION}"
  echo "- fail_fast: ${PERF_FAIL_FAST:-0}"
  echo "- clients: ${CLIENTS_DISPLAY}"
  echo "- default_clients: ${PERF_MULTI_DEFAULT_CLIENTS:-${PERF_DEFAULT_CLIENTS:-100}}"
  echo "- default_stream_clients: ${PERF_MULTI_DEFAULT_STREAM_CLIENTS:-${PERF_STREAM_DEFAULT_CLIENTS:-10000}}"
  echo "- server_io_threads: $(effective_multi_server_io_threads)"
  echo "- client_io_threads: $(effective_multi_client_io_threads)"
  echo "- go_gomaxprocs: ${GOMAXPROCS:-unset}"
  echo "- go_gomaxprocs_source: ${GO_GOMAXPROCS_SOURCE}"
  if [[ "${GO_GOMAXPROCS_SOURCE}" == "default" ]]; then
    echo "- go_gomaxprocs_case_overrides: MULTI_DEALER_DEALER/tcp/262144=8,MULTI_ROUTER_ROUTER/tcp/64=8,MULTI_ROUTER_ROUTER/tls/64=8,MULTI_ROUTER_ROUTER/tls/256=8,MULTI_ROUTER_ROUTER/tls/1024=8"
  else
    echo "- go_gomaxprocs_case_overrides: none"
  fi
  echo "- hwm: $(effective_or_auto "${HWM}")"
  echo "- sndhwm: $(effective_or_auto "${SEND_HWM:-${HWM}}")"
  echo "- rcvhwm: $(effective_or_auto "${RECV_HWM:-${HWM}}")"
  echo "- sndbuf: $(effective_or_auto "${SNDBUF}" "-1")"
  echo "- rcvbuf: $(effective_or_auto "${RCVBUF}" "-1")"
  echo "- ctx_auto_hwm_enable: ${PERF_CTX_AUTO_HWM_ENABLE:-1}"
  echo "- ctx_auto_hwm_profile: ${AUTO_HWM_PROFILE:-${PERF_MULTI_CTX_AUTO_HWM_PROFILE:-${PERF_CTX_AUTO_HWM_PROFILE:-balanced}}}"
  echo "- sndtimeo_ms: ${SNDTIMEO_MS:-${PERF_MULTI_SNDTIMEO_MS:-200}}"
  echo "- rcvtimeo_ms: ${RCVTIMEO_MS:-${PERF_MULTI_RCVTIMEO_MS:-200}}"
  echo "- connect_concurrency: ${CONNECT_CONCURRENCY:-128 (default)}"
  echo "- connect_ready_timeout_ms: ${CONNECT_READY_TIMEOUT_MS:-${PERF_MULTI_CONNECT_READY_TIMEOUT_MS:-${PERF_CONNECT_READY_TIMEOUT_MS:-1000}}}"
  echo "- monitor_hwm: ${MONITOR_HWM:-${PERF_MULTI_MONITOR_HWM:-1000}}"
  echo "- server_ready_timeout_ms: ${SERVER_READY_TIMEOUT_MS:-${PERF_MULTI_SERVER_READY_TIMEOUT_MS:-${PERF_SERVER_READY_TIMEOUT_MS:-10000}}}"
  echo "- server_shutdown_timeout_ms: ${SERVER_SHUTDOWN_TIMEOUT_MS:-${PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS:-${PERF_SERVER_SHUTDOWN_TIMEOUT_MS:-5000}}}"
  echo "- server_bind_port: ${SERVER_BIND_PORT:-${PERF_MULTI_SERVER_BIND_PORT:-0}}"
  echo "- transport_transition_ms: ${TRANSPORT_TRANSITION_MS}"
  echo "- pattern_transition_ms: ${PATTERN_TRANSITION_MS}"
  echo "- lat_timeout_ms: ${PERF_MULTI_LAT_TIMEOUT_MS:-5000}"
  echo "- stream_non_tcp_clients_max: ${PERF_STREAM_NON_TCP_CLIENTS_MAX:-${PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX:-10000}}"
  echo "- disable_resource_metrics: ${PERF_DISABLE_RESOURCE_METRICS:-0}"
  echo "- timeout_seconds: ${PERF_MULTI_TIMEOUT_SECONDS:-${PERF_TIMEOUT_SECONDS:-auto}}"
}

is_unsupported_output() {
  local output_file="$1"
  grep -Eiq 'protocol not supported' "${output_file}"
}

append_case_output() {
  local case_log="$1"
  cat "${case_log}" >> "${RAW_RESULTS_FILE}"
  if [[ -s "${case_log}" ]]; then
    printf '\n' >> "${RAW_RESULTS_FILE}"
  fi
}

progress_header_printed=0

progress_pattern_heading() {
  local pattern="$1"
  echo "  > Benchmarking current for ${pattern}..."
}

progress_table_header() {
  if [[ "${progress_header_printed}" -eq 1 ]]; then
    return
  fi
  progress_header_printed=1
  echo "      | Size     |         Throughput |      Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) |"
  echo "      |----------|--------------------|----------------|---------------|---------------|---------------|"
}

progress_case_row() {
  local pattern="$1"
  local size="$2"
  local case_log="$3"
  if [[ "${SMOKE}" -eq 1 ]]; then
    return
  fi
  if grep -Eq '^UNSUPPORTED,' "${case_log}" || is_unsupported_output "${case_log}"; then
    python3 "${PERF_REPORT_PY}" status-row --suite multi --size "$size" --status unsupported
    return
  fi
  if grep -Eq '^SKIP,' "${case_log}"; then
    python3 "${PERF_REPORT_PY}" status-row --suite multi --size "$size" --status skip
    return
  fi
  python3 "${PERF_REPORT_PY}" progress-case-row \
    --suite multi \
    --pattern "$pattern" \
    --size "$size" \
    --case-log "$case_log"
}

count_result_lines() {
  local pattern="$1"
  local transport="$2"
  local size="$3"
  local case_log="$4"
  awk -F',' -v pattern="${pattern}" -v transport="${transport}" -v size="${size}" '
    $1 == "RESULT" && $2 == "current" && $3 == pattern && $4 == transport && $5 == size { count++ }
    END { print count + 0 }
  ' "${case_log}"
}

timeout_seconds_from_ms() {
  local millis="${1:-0}"
  if [[ ! "${millis}" =~ ^[0-9]+$ || "${millis}" -le 0 ]]; then
    echo 0
    return
  fi
  echo $(((millis + 999) / 1000))
}

SERVER_READY_TIMEOUT_SECONDS="$(timeout_seconds_from_ms "${PERF_MULTI_SERVER_READY_TIMEOUT_MS:-${PERF_SERVER_READY_TIMEOUT_MS:-10000}}")"
SERVER_SHUTDOWN_TIMEOUT_SECONDS="$(timeout_seconds_from_ms "${PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS:-${PERF_SERVER_SHUTDOWN_TIMEOUT_MS:-5000}}")"
ONE_WAY_CLIENT_READY_TIMEOUT=10

wait_for_file_prefix() {
  local file_path="$1"
  local prefix="$2"
  local timeout_seconds="$3"
  local deadline=$((SECONDS + timeout_seconds))
  while (( SECONDS < deadline )); do
    if [[ -f "${file_path}" ]]; then
      local line
      line="$(awk -v prefix="${prefix}" 'index($0, prefix) == 1 { print; exit }' "${file_path}" 2>/dev/null || true)"
      if [[ -n "${line}" ]]; then
        printf '%s\n' "${line}"
        return 0
      fi
    fi
    sleep 0.1
  done
  return 1
}

wait_for_ready_or_unsupported() {
  local file_path="$1"
  local timeout_seconds="$2"
  local deadline=$((SECONDS + timeout_seconds))
  while (( SECONDS < deadline )); do
    if [[ -f "${file_path}" ]]; then
      local line
      line="$(awk '/^(READY,|UNSUPPORTED,)/ { print; exit }' "${file_path}" 2>/dev/null || true)"
      if [[ -n "${line}" ]]; then
        printf '%s\n' "${line}"
        return 0
      fi
    fi
    sleep 0.1
  done
  return 1
}

wait_for_pid() {
  local pid="$1"
  local timeout_seconds="$2"
  local deadline=$((SECONDS + timeout_seconds))
  while kill -0 "${pid}" 2>/dev/null; do
    if (( SECONDS >= deadline )); then
      return 1
    fi
    sleep 0.1
  done
  return 0
}

kill_process_tree() {
  local pid="$1"
  local child
  while read -r child; do
    [[ -z "${child}" ]] && continue
    kill_process_tree "${child}"
  done < <(pgrep -P "${pid}" 2>/dev/null || true)
  kill "${pid}" 2>/dev/null || true
}

shutdown_server() {
  local pid="$1"
  local control_fd="$2"
  if [[ -n "${control_fd}" ]]; then
    if kill -0 "${pid}" 2>/dev/null; then
      { printf 'STOP\n' >&"${control_fd}"; } 2>/dev/null || true
    fi
    eval "exec ${control_fd}>&-"
  fi
  if wait_for_pid "${pid}" "${SERVER_SHUTDOWN_TIMEOUT_SECONDS}"; then
    wait "${pid}" 2>/dev/null || true
    return
  fi
  kill_process_tree "${pid}"
  if wait_for_pid "${pid}" "${SERVER_SHUTDOWN_TIMEOUT_SECONDS}"; then
    wait "${pid}" 2>/dev/null || true
    return
  fi
  kill -9 "${pid}" 2>/dev/null || true
  wait "${pid}" 2>/dev/null || true
}

resolve_client_timeout_seconds() {
  local pattern="$1"
  local transport="$2"
  local size="$3"
  local duration="$4"
  if [[ -n "${PERF_MULTI_TIMEOUT_SECONDS:-${PERF_TIMEOUT_SECONDS:-}}" ]]; then
    echo "${PERF_MULTI_TIMEOUT_SECONDS:-${PERF_TIMEOUT_SECONDS}}"
    return
  fi
  if [[ "${pattern}" == "MULTI_STREAM" ]]; then
    local stream_timeout=$((duration * 3 + 20))
    (( stream_timeout < 45 )) && stream_timeout=45
    echo "${stream_timeout}"
    return
  fi
  if { [[ "${pattern}" == "MULTI_PUBSUB" ]] && (( size >= 262144 )); } \
    || { [[ "${transport}" == "tls" || "${transport}" == "wss" ]] && (( size >= 131072 )); }; then
	local large_case_timeout=$((duration * 6 + 30))
	(( large_case_timeout < 90 )) && large_case_timeout=90
	echo "${large_case_timeout}"
    return
  fi
  local timeout=$((duration * 3 + 20))
  (( timeout < 45 )) && timeout=45
  echo "${timeout}"
}

resolve_case_gomaxprocs() {
  local pattern="$1"
  local transport="$2"
  local size="$3"

  if [[ "${GO_GOMAXPROCS_SOURCE}" == "default" \
    && "${pattern}" == "MULTI_DEALER_DEALER" \
    && "${transport}" == "tcp" \
    && "${size}" == "262144" ]]; then
    echo "8"
    return
  fi
  if [[ "${GO_GOMAXPROCS_SOURCE}" == "default" \
    && "${pattern}" == "MULTI_ROUTER_ROUTER" \
    && "${transport}" == "tcp" \
    && "${size}" == "64" ]]; then
    echo "8"
    return
  fi
  if [[ "${GO_GOMAXPROCS_SOURCE}" == "default" \
    && "${pattern}" == "MULTI_ROUTER_ROUTER" \
    && "${transport}" == "tls" \
    && "${size}" == "64" ]]; then
    echo "8"
    return
  fi
  if [[ "${GO_GOMAXPROCS_SOURCE}" == "default" \
    && "${pattern}" == "MULTI_ROUTER_ROUTER" \
    && "${transport}" == "tls" \
    && "${size}" == "256" ]]; then
    echo "8"
    return
  fi
  if [[ "${GO_GOMAXPROCS_SOURCE}" == "default" \
    && "${pattern}" == "MULTI_ROUTER_ROUTER" \
    && "${transport}" == "tls" \
    && "${size}" == "1024" ]]; then
    echo "8"
    return
  fi
  echo "${GOMAXPROCS}"
}

run_multi_process_case() {
  local pattern="$1"
  local transport="$2"
  local size="$3"
  local duration="$4"
  local clients="$5"
  local case_log="$6"

  local srv_out client_out client_err server_fifo ready_line endpoint
  local server_pid server_control_fd client_pid client_control_fd client_fifo client_ready_line
  local client_timeout case_status case_gomaxprocs
  case_gomaxprocs="$(resolve_case_gomaxprocs "${pattern}" "${transport}" "${size}")"
  srv_out="$(mktemp "${TMP_DIR}/server.XXXXXX")"
  client_out="$(mktemp "${TMP_DIR}/client.XXXXXX")"
  client_err="$(mktemp "${TMP_DIR}/client_err.XXXXXX")"
  server_fifo="$(mktemp -u "${TMP_DIR}/server_fifo.XXXXXX")"
  mkfifo "${server_fifo}"

  GOMAXPROCS="${case_gomaxprocs}" run_go_perf ./perf/multi \
    --role server \
    --pattern "${pattern}" \
    --transport "${transport}" \
    --msg-size "${size}" \
    --duration "${duration}" \
    --clients "${clients}" \
    < "${server_fifo}" > "${srv_out}" 2>&1 &
  server_pid=$!
  exec {server_control_fd}> "${server_fifo}"
  rm -f "${server_fifo}"

  ready_line="$(wait_for_ready_or_unsupported "${srv_out}" "${SERVER_READY_TIMEOUT_SECONDS}" || true)"
  if [[ "${ready_line}" == UNSUPPORTED,* ]]; then
    shutdown_server "${server_pid}" "${server_control_fd}"
    cat "${srv_out}" > "${case_log}"
    rm -f "${srv_out}" "${client_out}" "${client_err}"
    return 1
  fi
  if [[ "${ready_line}" != READY,* ]]; then
    shutdown_server "${server_pid}" "${server_control_fd}"
    cat "${srv_out}" > "${case_log}"
    printf 'FAIL,current,%s,%s,%s,server_ready_timeout\n' "${pattern}" "${transport}" "${size}" >> "${case_log}"
    rm -f "${srv_out}" "${client_out}" "${client_err}"
    return 1
  fi
  endpoint="${ready_line#READY,}"
  endpoint="${endpoint//0.0.0.0/127.0.0.1}"
  client_timeout="$(resolve_client_timeout_seconds "${pattern}" "${transport}" "${size}" "${duration}")"
  case_status=0

  if [[ "${pattern}" == "MULTI_DEALER_DEALER" || "${pattern}" == "MULTI_PUBSUB" ]]; then
    client_fifo="$(mktemp -u "${TMP_DIR}/client_fifo.XXXXXX")"
    mkfifo "${client_fifo}"
    GOMAXPROCS="${case_gomaxprocs}" run_go_perf ./perf/multi \
      --role client \
      --pattern "${pattern}" \
      --transport "${transport}" \
      --msg-size "${size}" \
      --duration "${duration}" \
      --clients "${clients}" \
      --endpoint "${endpoint}" \
      < "${client_fifo}" > "${client_out}" 2> "${client_err}" &
    client_pid=$!
    exec {client_control_fd}> "${client_fifo}"
    rm -f "${client_fifo}"

    client_ready_line="$(wait_for_file_prefix "${client_out}" "CLIENT_READY," "${ONE_WAY_CLIENT_READY_TIMEOUT}" || true)"
    if [[ "${client_ready_line}" != "CLIENT_READY,${size}" ]]; then
      case_status=1
      kill_process_tree "${client_pid}"
      wait "${client_pid}" 2>/dev/null || true
    else
      printf 'START,%s\n' "${size}" >&"${server_control_fd}" || true
      printf 'START,%s\n' "${size}" >&"${client_control_fd}" || true
      if ! wait_for_pid "${client_pid}" "${client_timeout}"; then
        case_status=1
        kill_process_tree "${client_pid}"
        wait "${client_pid}" 2>/dev/null || true
      elif ! wait "${client_pid}"; then
        case_status=1
      fi
    fi
    exec {client_control_fd}>&- || true
  elif [[ "${pattern}" == "MULTI_STREAM" ]]; then
    # Shared C reference client; the Go binding has no STREAM client
    # (measured surface is the Go STREAM server).
    run_external_stream_client \
      "${transport}" "${size}" "${duration}" "${clients}" \
      "${endpoint}" "${client_out}" "${client_err}" &
    client_pid=$!
    if ! wait_for_pid "${client_pid}" "${client_timeout}"; then
      case_status=1
      kill_process_tree "${client_pid}"
      wait "${client_pid}" 2>/dev/null || true
    elif ! wait "${client_pid}"; then
      case_status=1
    fi
  else
    run_go_perf ./perf/multi \
      --role client \
      --pattern "${pattern}" \
      --transport "${transport}" \
      --msg-size "${size}" \
      --duration "${duration}" \
      --clients "${clients}" \
      --endpoint "${endpoint}" \
      > "${client_out}" 2> "${client_err}" &
    client_pid=$!
    if ! wait_for_pid "${client_pid}" "${client_timeout}"; then
      case_status=1
      kill_process_tree "${client_pid}"
      wait "${client_pid}" 2>/dev/null || true
    elif ! wait "${client_pid}"; then
      case_status=1
    fi
  fi

  shutdown_server "${server_pid}" "${server_control_fd}"
  {
    cat "${srv_out}"
    if [[ -s "${client_out}" ]]; then
      if [[ "${pattern}" == "MULTI_STREAM" ]]; then
        sed 's/^RESULT,current,STREAM,/RESULT,current,MULTI_STREAM,/' "${client_out}"
      else
        cat "${client_out}"
      fi
    fi
    if [[ -s "${client_err}" ]]; then
      cat "${client_err}"
    fi
  } > "${case_log}"
  rm -f "${srv_out}" "${client_out}" "${client_err}"
  return "${case_status}"
}

render_tables() {
  python3 "${PERF_REPORT_PY}" render-log-tables --suite multi --tmp-dir "$TMP_DIR"
}

{
  emit_meta_lines
  echo
  emit_effective_options_multi "start"
} > "${RESULTS_FILE}"
exec 3>&1
exec >/dev/null

result_lines=0
unsupported=0
fail=0
expected_cases=0
stop_early=0
FAILURES=()
SKIPS=()

for pattern_index in "${!PATTERNS[@]}"; do
  if [[ "${stop_early}" -eq 1 ]]; then
    break
  fi
  pattern="${PATTERNS[pattern_index]}"
  progress_pattern_heading "${pattern}"
  read -r -a PATTERN_XPORTS <<< "$(pattern_transports "${pattern}")"
  size_list="$(pattern_msg_sizes "${pattern}")"
  IFS=',' read -r -a SIZES <<< "${size_list}"
  resolved_clients="${CLIENTS}"
  if [[ -z "${resolved_clients}" ]]; then
    resolved_clients="${PERF_MULTI_CLIENTS:-}"
  fi
  if [[ -z "${resolved_clients}" ]]; then
    resolved_clients="${PERF_CLIENTS:-}"
  fi
  if [[ -z "${resolved_clients}" ]]; then
    if [[ "${pattern}" == "MULTI_STREAM" ]]; then
      resolved_clients="${PERF_MULTI_DEFAULT_STREAM_CLIENTS:-${PERF_STREAM_DEFAULT_CLIENTS:-10000}}"
    else
      resolved_clients="${PERF_MULTI_DEFAULT_CLIENTS:-${PERF_DEFAULT_CLIENTS:-100}}"
    fi
  fi
  resolved_clients="$(cap_default_clients_for_memory "${resolved_clients}")"

  transport_total=0
  for candidate_transport in "${PATTERN_XPORTS[@]}"; do
    if transport_enabled "${candidate_transport}"; then
      transport_total=$((transport_total + 1))
    fi
  done
  transport_seen=0

  for transport in "${PATTERN_XPORTS[@]}"; do
    if [[ "${stop_early}" -eq 1 ]]; then
      break
    fi
    if ! transport_enabled "${transport}"; then
      continue
    fi
    transport_seen=$((transport_seen + 1))
    echo "    Testing ${transport} | ${size_list}:"
    case_clients="${resolved_clients}"
    if [[ "${pattern}" == "MULTI_STREAM" && "${transport}" != "tcp" ]]; then
      non_tcp_max="${PERF_STREAM_NON_TCP_CLIENTS_MAX:-${PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX:-10000}}"
      if [[ "${case_clients}" =~ ^[0-9]+$ && "${non_tcp_max}" =~ ^[0-9]+$ \
            && "${case_clients}" -gt "${non_tcp_max}" ]]; then
        case_clients="${non_tcp_max}"
      fi
    fi
    transport_failures=0
    transport_unsupported=0

    for run in $(seq 1 "${RUNS}"); do
      if [[ "${stop_early}" -eq 1 ]]; then
        break
      fi
      if [[ "${RUNS}" -gt 1 ]]; then
        echo "      run ${run}/${RUNS}:"
      fi
      progress_header_printed=0

      for size in "${SIZES[@]}"; do
        expected_cases=$((expected_cases + 1))
        case_log="${TMP_DIR}/${pattern}_${transport}_${size}_run${run}.log"
        if ! ensure_nofile_limit "${case_clients}"; then
          printf 'SKIP,current,%s,%s,nofile_guard:%s\n' "${pattern}" "${transport}" "${NOFILE_SKIP_REASON}" > "${case_log}"
          append_case_output "${case_log}"
          expected_cases=$((expected_cases - 1))
          SKIPS+=("${pattern} current ${transport} ${size}B: nofile_guard:${NOFILE_SKIP_REASON}")
          progress_table_header
          progress_case_row "${pattern}" "${size}" "${case_log}"
          continue
        fi
        if ! ensure_memory_budget "${case_clients}"; then
          printf 'SKIP,current,%s,%s,memory_guard:%s\n' "${pattern}" "${transport}" "${MEMORY_SKIP_REASON}" > "${case_log}"
          append_case_output "${case_log}"
          expected_cases=$((expected_cases - 1))
          SKIPS+=("${pattern} current ${transport} ${size}B: memory_guard:${MEMORY_SKIP_REASON}")
          progress_table_header
          progress_case_row "${pattern}" "${size}" "${case_log}"
          continue
        fi
        export PERF_MULTI_MSG_UNIT_BYTES="${size}"
        case_ok=0
        if run_multi_process_case \
          "${pattern}" \
          "${transport}" \
          "${size}" \
          "${DURATION}" \
          "${case_clients}" \
          "${case_log}"; then
          case_ok=1
        fi
        if [[ "${case_ok}" -eq 1 ]]; then
          append_case_output "${case_log}"
          case_result_lines="$(count_result_lines "${pattern}" "${transport}" "${size}" "${case_log}")"
          if [[ "${case_result_lines}" -gt 0 ]]; then
            result_lines=$((result_lines + case_result_lines))
            progress_table_header
            progress_case_row "${pattern}" "${size}" "${case_log}"
            continue
          fi
          if grep -Eq '^UNSUPPORTED,' "${case_log}" || is_unsupported_output "${case_log}"; then
            unsupported=$((unsupported + 1))
            expected_cases=$((expected_cases - 1))
            transport_unsupported=1
            progress_table_header
            progress_case_row "${pattern}" "${size}" "${case_log}"
            break
          fi
          if grep -Eq '^SKIP,' "${case_log}"; then
            expected_cases=$((expected_cases - 1))
            SKIPS+=("${pattern} current ${transport} ${size}B: skipped")
            progress_table_header
            progress_case_row "${pattern}" "${size}" "${case_log}"
            continue
          fi
          echo "FAIL,current,${pattern},${transport},${size},no_result_lines" >> "${RAW_RESULTS_FILE}"
          fail=$((fail + 1))
          transport_failures=$((transport_failures + 1))
          FAILURES+=("${pattern} current ${transport} ${size}B: no_result_lines")
        else
          append_case_output "${case_log}"
          case_result_lines="$(count_result_lines "${pattern}" "${transport}" "${size}" "${case_log}")"
          if [[ "${case_result_lines}" -gt 0 ]]; then
            result_lines=$((result_lines + case_result_lines))
            progress_table_header
            progress_case_row "${pattern}" "${size}" "${case_log}"
            continue
          fi
          if grep -Eq '^UNSUPPORTED,' "${case_log}" || is_unsupported_output "${case_log}"; then
            echo "UNSUPPORTED,current,${pattern},${transport}" >> "${RAW_RESULTS_FILE}"
            unsupported=$((unsupported + 1))
            expected_cases=$((expected_cases - 1))
            transport_unsupported=1
            progress_table_header
            progress_case_row "${pattern}" "${size}" "${case_log}"
            break
          fi
          if grep -Eq '^SKIP,' "${case_log}"; then
            expected_cases=$((expected_cases - 1))
            SKIPS+=("${pattern} current ${transport} ${size}B: skipped")
            progress_table_header
            progress_case_row "${pattern}" "${size}" "${case_log}"
            continue
          fi
          echo "FAIL,current,${pattern},${transport},${size},exit_nonzero" >> "${RAW_RESULTS_FILE}"
          fail=$((fail + 1))
          transport_failures=$((transport_failures + 1))
          FAILURES+=("${pattern} current ${transport} ${size}B: exit_nonzero")
        fi
        progress_table_header
        progress_case_row "${pattern}" "${size}" "${case_log}"
        if [[ "${PERF_FAIL_FAST:-0}" == "1" && "${fail}" -gt 0 ]]; then
          stop_early=1
          break
        fi
        sleep_millis "${RUN_COOLDOWN_MS}"
      done

      if [[ "${stop_early}" -eq 1 || "${transport_unsupported}" -eq 1 ]]; then
        break
      fi

      if [[ "${RUNS}" -gt 1 && "${run}" -lt "${RUNS}" ]]; then
        echo "      [cooldown ${RUN_COOLDOWN_MS}ms]"
        sleep_millis "${RUN_COOLDOWN_MS}"
      fi
    done

    if [[ "${transport_unsupported}" -eq 1 ]]; then
      echo "    Testing ${transport}: unsupported Done"
    elif [[ "${transport_failures}" -gt 0 ]]; then
      echo "    Testing ${transport}: (failures=${transport_failures}) Done"
    else
      echo "    Testing ${transport}: Done"
    fi

    if [[ "${transport_seen}" -lt "${transport_total}" ]]; then
      if [[ "${stop_early}" -eq 1 ]]; then
        break
      fi
      echo "    [transport cooldown ${TRANSPORT_TRANSITION_MS}ms]"
      sleep_millis "${TRANSPORT_TRANSITION_MS}"
    fi
  done

  if [[ "${stop_early}" -ne 1 && $((pattern_index + 1)) -lt "${#PATTERNS[@]}" ]]; then
    echo "[pattern cooldown ${PATTERN_TRANSITION_MS}ms]"
    sleep_millis "${PATTERN_TRANSITION_MS}"
  fi
done

if [[ "${SMOKE}" -eq 1 ]]; then
  expected_result_lines=$((expected_cases * 5))
  if [[ "${#FAILURES[@]}" -gt 0 || "${result_lines}" -ne "${expected_result_lines}" ]]; then
    exec >&3
    grep -E '^(READY,|ACTIVE,|RESULT,|FAIL,)' "${RAW_RESULTS_FILE}" || true
    exit 1
  fi
  exec >&3
  grep -E '^(READY,|ACTIVE,|RESULT,)' "${RAW_RESULTS_FILE}" || true
  exit 0
fi

table_output="$(render_tables)"
if [[ -n "${table_output}" ]]; then
  printf '%s\n' "${table_output}" >> "${RESULTS_FILE}"
fi

expected_result_lines=$((expected_cases * 5))
status="partial"
if [[ "${result_lines}" -eq "${expected_result_lines}" ]]; then
  status="complete"
fi

{
  echo
  echo "## Effective Options (result)"
  emit_effective_options_multi "result" | sed '1d'
  if [[ "${result_lines}" -gt 0 && -s "${RAW_RESULTS_FILE}" ]]; then
    echo
    echo "## Result Data"
    python3 "${PERF_REPORT_PY}" sort-result-data "${RAW_RESULTS_FILE}"
  fi
  echo
  echo "## Completion"
  echo "- success: $((result_lines / 5))"
  echo "- unsupported: ${unsupported}"
  echo "- skip: ${#SKIPS[@]}"
  echo "- fail: ${fail}"
  echo "- status: ${status}"
  echo "- expected_result_lines: ${expected_result_lines}"
  echo "- actual_result_lines: ${result_lines}"
  if [[ "${#SKIPS[@]}" -gt 0 ]]; then
    echo
    echo "## Skips"
    for skip in "${SKIPS[@]}"; do
      echo "- ${skip}"
    done
  fi
  if [[ "${#FAILURES[@]}" -gt 0 ]]; then
    echo
    echo "## Failures"
    for failure in "${FAILURES[@]}"; do
      echo "- ${failure}"
    done
  fi
  echo
  echo "Saved result file: ${RESULTS_FILE} (status=${status})"
} >> "${RESULTS_FILE}"

if [[ -n "${OUTPUT_FILE}" ]]; then
  cp "${RESULTS_FILE}" "${OUTPUT_FILE}"
fi

exec >&3
cat "${RESULTS_FILE}"

if [[ "${status}" != "complete" ]]; then
  exit 1
fi
