#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
CPP_PERF_DIR="${ROOT_DIR}/bindings/cpp/perf"
OFFICIAL_BUILD_DIR="${ROOT_DIR}/bindings/cpp/build"
DEFAULT_CORE_BUILD_DIR="${ROOT_DIR}/core/build"
NORMALIZE_TIMESTAMPS_SH="${ROOT_DIR}/core/tools/normalize_build_timestamps.sh"
MAKE_BIN="$(command -v gmake || command -v make)"
PERF_COMPARISON_SCRIPT="${ROOT_DIR}/bindings/cpp/perf/multi/run_comparison.py"
PATTERNS="DEALER_DEALER,DEALER_ROUTER,DEALER_ROUTER_REQREP,ROUTER_ROUTER,ROUTER_ROUTER_REQREP,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM"
TRANSPORTS="tcp,tls,ws,wss"
DEFAULT_MULTI_MSG_SIZES="64,256,1024,4096,65536,131072"
IFS=',' read -r -a PATTERN_LIST <<< "${PATTERNS}"

SECONDS=0
SHOW_TOTAL_TIME=0
source "${CPP_PERF_DIR}/run_binding_common.sh"
trap 'print_total_time $?' EXIT

IS_WINDOWS=0
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    IS_WINDOWS=1
    ;;
esac

NOFILE_SKIP_REASON=""
ensure_nofile_limit() {
  local clients="${1:-}"
  NOFILE_SKIP_REASON=""

  if [[ "${PERF_SKIP_NOFILE_CHECK:-0}" == "1" ]]; then
    return 0
  fi
  if ! is_uint "${clients}"; then
    return 0
  fi

  local required=$(( clients * 3 + 4096 ))
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

  NOFILE_SKIP_REASON="clients=${clients},required=${required},soft=${soft},hard=${hard}"
  return 1
}

MEMORY_SKIP_REASON=""
memory_available_kb() {
  if [[ "${PERF_SKIP_MEMORY_CHECK:-0}" == "1" ]]; then
    echo ""
    return
  fi
  if [[ ! -r /proc/meminfo ]]; then
    echo ""
    return
  fi
  awk '/^MemAvailable:/ { print $2; found=1; exit } END { if (!found) print "" }' /proc/meminfo 2>/dev/null || true
}

resolve_memory_max_clients() {
  local available_kb
  available_kb="$(memory_available_kb)"
  if ! is_uint "${available_kb}"; then
    echo ""
    return
  fi

  local budget_pct
  local base_mb
  local per_client_kb
  budget_pct="${PERF_MULTI_MEMORY_BUDGET_PCT:-${PERF_MEMORY_BUDGET_PCT:-70}}"
  base_mb="${PERF_MULTI_MEMORY_BASE_MB:-${PERF_MEMORY_BASE_MB:-512}}"
  per_client_kb="${PERF_MULTI_MEMORY_PER_CLIENT_KB:-${PERF_MEMORY_PER_CLIENT_KB:-1024}}"
  if ! is_uint "${budget_pct}" || (( budget_pct < 1 || budget_pct > 95 )); then
    echo ""
    return
  fi
  if ! is_uint "${base_mb}"; then
    echo ""
    return
  fi
  if ! is_uint "${per_client_kb}" || (( per_client_kb < 1 )); then
    echo ""
    return
  fi

  local usable_kb=$(( available_kb * budget_pct / 100 ))
  local base_kb=$(( base_mb * 1024 ))
  if (( usable_kb <= base_kb )); then
    echo "1"
    return
  fi

  local max_clients=$(( (usable_kb - base_kb) / per_client_kb ))
  if (( max_clients < 1 )); then
    max_clients=1
  fi
  echo "${max_clients}"
}

ensure_memory_budget() {
  local clients="${1:-}"
  MEMORY_SKIP_REASON=""

  if [[ "${PERF_SKIP_MEMORY_CHECK:-0}" == "1" ]]; then
    return 0
  fi
  if ! is_uint "${clients}"; then
    return 0
  fi

  local max_clients
  max_clients="$(resolve_memory_max_clients)"
  if ! is_uint "${max_clients}"; then
    return 0
  fi
  if (( clients <= max_clients )); then
    return 0
  fi

  local available_kb
  available_kb="$(memory_available_kb)"
  local budget_pct
  local base_mb
  local per_client_kb
  budget_pct="${PERF_MULTI_MEMORY_BUDGET_PCT:-${PERF_MEMORY_BUDGET_PCT:-70}}"
  base_mb="${PERF_MULTI_MEMORY_BASE_MB:-${PERF_MEMORY_BASE_MB:-512}}"
  per_client_kb="${PERF_MULTI_MEMORY_PER_CLIENT_KB:-${PERF_MEMORY_PER_CLIENT_KB:-1024}}"
  MEMORY_SKIP_REASON="clients=${clients},max_clients=${max_clients},mem_available_kb=${available_kb},budget_pct=${budget_pct},base_mb=${base_mb},per_client_kb=${per_client_kb}"
  return 1
}

usage() {
  cat <<'USAGE'
Usage: bindings/cpp/perf/run_benchmarks_multi.sh [options]

Run only multi-socket benchmark patterns.
Default PATTERN is:
  DEALER_DEALER,DEALER_ROUTER,DEALER_ROUTER_REQREP,ROUTER_ROUTER,ROUTER_ROUTER_REQREP,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM
This script invokes the shared comparison runner directly.
By default, multi-bench uses ready -> active with a 5s duration window.
By default, multi-bench uses transports: tcp,tls,ws,wss (can be overridden with --transports).
Policy contract:
  - benchmark binaries execute one pattern/transport/size/run case only
  - this runner owns pattern/transport/size iteration, cooldown, aggregation,
    markdown table output, and result-file persistence

Options:
  --pattern NAME         Benchmark pattern (default: all patterns above).
                         Alias: streams => STREAM
  --help                 Show this help.
  --reuse-build          Reuse existing build directory as-is (skip configure/build).
  --clean-build          Remove build directory and do a clean build.
  --results-dir PATH     Override results root directory.
  --results-tag NAME     Optional tag appended to the results filename.
  --build-dir PATH       Official build directory (must be <repo>/bindings/cpp/build).
  --output PATH          Tee results to a file.
  --runs N               Iterations per configuration (default: 1).
  --pin-cpu              Pin CPU core during benchmarks (Linux taskset).
  --io-threads N         Legacy alias: set PERF_IO_THREADS for both roles.
  --server-io-threads N  Set PERF_MULTI_SERVER_IO_THREADS
                         (default: non-stream=4, stream=4).
  --client-io-threads N  Set PERF_MULTI_CLIENT_IO_THREADS
                         (default: non-stream=4, stream=4).
  --msg-sizes LIST       Comma-separated message sizes
                         (default: 64,256,1024,4096,65536,131072).
                         MULTI_STREAM uses 64,256,1024,65536 by default;
                         override it with PERF_MULTI_STREAM_MSG_SIZES.
  --transports LIST      Comma-separated transports.
  --duration N           Optional override for multi duration seconds (default 5).
  --clients N            Override number of client sockets per pattern (default: 100, stream=10000).
  --hwm N                Debug-only override PERF_MULTI_HWM.
                         Requires PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1.
  --send-hwm N           Debug-only override PERF_MULTI_SNDHWM (fallback: --hwm).
  --recv-hwm N           Debug-only override PERF_MULTI_RCVHWM (fallback: --hwm).
  --buf SIZE             Debug-only override for both PERF_MULTI_SNDBUF and PERF_MULTI_RCVBUF.
  --sndbuf SIZE          Debug-only override PERF_MULTI_SNDBUF (e.g. 64b, 1k, 64k).
  --rcvbuf SIZE          Debug-only override PERF_MULTI_RCVBUF (e.g. 64b, 1k, 64k).
  --sndtimeo N           Override PERF_MULTI_SNDTIMEO_MS (default: 200).
  --rcvtimeo N           Override PERF_MULTI_RCVTIMEO_MS (default: 200).
  --send-timeout-ms N    Alias of --sndtimeo.
  --recv-timeout-ms N    Alias of --rcvtimeo.
  --connect-concurrency N
                         Override concurrent connect count.
  --transport-transition-ms N
                         Override PERF_MULTI_TRANSPORT_TRANSITION_MS (default: 3000).
  --pattern-transition-ms N
                         Override PERF_MULTI_PATTERN_TRANSITION_MS (default: 3000).
  --server-ready-timeout-ms N
                         Override PERF_MULTI_SERVER_READY_TIMEOUT_MS (default: 10000).
  --connect-ready-timeout-ms N
                         Override PERF_MULTI_CONNECT_READY_TIMEOUT_MS (default: 5000).
  --monitor-hwm N        Override PERF_MULTI_MONITOR_HWM (default: 1000).
  --server-shutdown-timeout-ms N
                         Override PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS (default: 5000).
  --server-bind-port N
                         Override PERF_MULTI_SERVER_BIND_PORT (default: 0=auto).
  --auto-hwm-profile NAME
                         Set auto-HWM profile: compact, low_latency, balanced, throughput (default: balanced).

Environment:
  PERF_SKIP_NOFILE_CHECK=1   Disable resource guard nofile(limit) check
  PERF_SKIP_MEMORY_CHECK=1   Disable resource guard memory check
  PERF_MULTI_MEMORY_BUDGET_PCT=70
                            Percent of MemAvailable reserved for multi benchmark sockets
  PERF_MULTI_MEMORY_BASE_MB=512
                            Fixed memory reserve before per-client estimate
  PERF_MULTI_MEMORY_PER_CLIENT_KB=1024
                            Estimated memory per client socket for guard
Notes:
  - result is saved under results/multi/report/ as
    perf_cpp_multi_<platform>_YYYYMMDD_HHMMSS[_<tag>].txt.
  - default build mode is incremental (configure/build without deleting build dir).
  - this runner links zlink core from core/build and prints the resolved
    libzlink runtime before execution.
  - if core/src or core/include is newer than the resolved runtime library,
    the runner fails fast and asks for a core/build rebuild.
USAGE
}

resolve_pattern_connect_concurrency() {
  local clients="${1:-}"
  if [[ -n "${CONNECT_CONCURRENCY}" ]]; then
    echo "${CONNECT_CONCURRENCY}"
    return
  fi
  if [[ "${clients}" =~ ^[0-9]+$ ]] && (( clients >= 10000 )); then
    echo "1024"
  else
    echo "128"
  fi
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

public_multi_pattern() {
  local pattern="${1:-}"
  pattern="$(printf '%s' "${pattern}" | tr '[:lower:]' '[:upper:]')"
  if [[ -z "${pattern}" ]]; then
    printf '%s' ""
    return
  fi
  if [[ "${pattern}" == MULTI_* ]]; then
    printf '%s' "${pattern}"
    return
  fi
  printf 'MULTI_%s' "${pattern}"
}

expand_and_add_explicit_pattern() {
  local raw="${1:-}"
  raw="${raw#"${raw%%[![:space:]]*}"}"
  raw="${raw%"${raw##*[![:space:]]}"}"
  raw="$(printf '%s' "${raw}" | tr '[:lower:]' '[:upper:]')"
  if [[ -z "${raw}" ]]; then
    return
  fi

  if [[ "${raw}" == MULTI_* ]]; then
    raw="${raw#MULTI_}"
  fi

  case "${raw}" in
    STREAM)
      add_explicit_pattern_unique "STREAM"
      ;;
    STREAMS)
      add_explicit_pattern_unique "STREAM"
      ;;
    *)
      add_explicit_pattern_unique "${raw}"
      ;;
  esac
}

HAS_EXPLICIT_TRANSPORT=0
HAS_EXPLICIT_MSG_SIZES=0
HAS_EXPLICIT_RESULTS_TAG=0
HAS_EXPLICIT_RUNS=0
HAS_EXPLICIT_RESULTS_DIR=0
BUILD_MODE="incremental"
BUILD_MODE_EXPLICIT=0
DURATION_SECONDS="${PERF_MULTI_DURATION_SECONDS:-${PERF_DURATION_SECONDS:-5}}"
CLIENTS="${PERF_MULTI_CLIENTS:-${PERF_CLIENTS:-}}"
EFFECTIVE_DEFAULT_CLIENTS="${PERF_MULTI_DEFAULT_CLIENTS:-${PERF_DEFAULT_CLIENTS:-100}}"
EFFECTIVE_DEFAULT_STREAM_CLIENTS="${PERF_MULTI_DEFAULT_STREAM_CLIENTS:-${PERF_STREAM_DEFAULT_CLIENTS:-10000}}"
HWM="${PERF_MULTI_HWM:-${PERF_HWM:-}}"
SNDHWM="${PERF_MULTI_SNDHWM:-${PERF_SNDHWM:-}}"
RCVHWM="${PERF_MULTI_RCVHWM:-${PERF_RCVHWM:-}}"
SNDBUF="${PERF_MULTI_SNDBUF:-${PERF_SNDBUF:-}}"
RCVBUF="${PERF_MULTI_RCVBUF:-${PERF_RCVBUF:-}}"
SNDTIMEO_MS="${PERF_MULTI_SNDTIMEO_MS:-${PERF_SNDTIMEO_MS:-200}}"
RCVTIMEO_MS="${PERF_MULTI_RCVTIMEO_MS:-${PERF_RCVTIMEO_MS:-200}}"
CONNECT_CONCURRENCY="${PERF_MULTI_CONNECT_CONCURRENCY:-${PERF_CONNECT_CONCURRENCY:-}}"
SERVICE_CLIENTS="${PERF_MULTI_SERVICE_CLIENTS:-${PERF_SERVICE_CLIENTS:-}}"
LATENCY_SAMPLE_CAP="${PERF_MULTI_LATENCY_SAMPLE_CAP:-}"
TIMEOUT_SECONDS="${PERF_MULTI_TIMEOUT_SECONDS:-${PERF_TIMEOUT_SECONDS:-}}"
STREAM_MSG_SIZES="${PERF_MULTI_STREAM_MSG_SIZES:-${PERF_STREAM_MSG_SIZES:-64,256,1024,65536}}"
PUBSUB_XPUB_NODROP="${PERF_MULTI_PUBSUB_XPUB_NODROP:-${PERF_PUBSUB_XPUB_NODROP:-}}"
RUN_COOLDOWN_MS="${PERF_MULTI_RUN_COOLDOWN_MS:-${PERF_RUN_COOLDOWN_MS:-3000}}"
TRANSPORT_TRANSITION_MS="${PERF_MULTI_TRANSPORT_TRANSITION_MS:-${PERF_TRANSPORT_TRANSITION_MS:-3000}}"
PATTERN_TRANSITION_MS="${PERF_MULTI_PATTERN_TRANSITION_MS:-${PERF_PATTERN_TRANSITION_MS:-3000}}"
SERVER_READY_TIMEOUT_MS="${PERF_MULTI_SERVER_READY_TIMEOUT_MS:-${PERF_SERVER_READY_TIMEOUT_MS:-10000}}"
CONNECT_READY_TIMEOUT_MS="${PERF_MULTI_CONNECT_READY_TIMEOUT_MS:-${PERF_CONNECT_READY_TIMEOUT_MS:-5000}}"
MONITOR_HWM="${PERF_MULTI_MONITOR_HWM:-${PERF_MONITOR_HWM:-1000}}"
SERVER_SHUTDOWN_TIMEOUT_MS="${PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS:-${PERF_SERVER_SHUTDOWN_TIMEOUT_MS:-5000}}"
SERVER_BIND_PORT="${PERF_MULTI_SERVER_BIND_PORT:-${PERF_SERVER_BIND_PORT:-0}}"
CTX_AUTO_HWM_ENABLE="${PERF_CTX_AUTO_HWM_ENABLE:-1}"
CTX_AUTO_HWM_PROFILE="${PERF_MULTI_CTX_AUTO_HWM_PROFILE:-${PERF_CTX_AUTO_HWM_PROFILE:-balanced}}"
ALLOW_MANUAL_SOCKET_OVERRIDES="${PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES:-${PERF_ALLOW_MANUAL_SOCKET_OVERRIDES:-0}}"
RESULTS_DIR_OVERRIDE="${PERF_RESULTS_DIR:-}"
OUTPUT_FILE=""
EXPLICIT_PATTERNS=()
SCRIPT_ARGS=()
EFFECTIVE_DEFAULT_IO_THREADS="${PERF_MULTI_DEFAULT_IO_THREADS:-${PERF_DEFAULT_IO_THREADS:-}}"
COMMON_IO_THREADS="${PERF_IO_THREADS:-}"
SERVER_IO_THREADS="${PERF_MULTI_SERVER_IO_THREADS:-${PERF_SERVER_IO_THREADS:-}}"
CLIENT_IO_THREADS="${PERF_MULTI_CLIENT_IO_THREADS:-${PERF_CLIENT_IO_THREADS:-}}"
STREAM_SERVER_IO_THREADS="${PERF_MULTI_STREAM_SERVER_IO_THREADS:-${PERF_STREAM_SERVER_IO_THREADS:-}}"
STREAM_CLIENT_IO_THREADS="${PERF_MULTI_STREAM_CLIENT_IO_THREADS:-${PERF_STREAM_CLIENT_IO_THREADS:-}}"
TRANSPORTS_OVERRIDE="${PERF_TRANSPORTS:-${TRANSPORTS}}"
MSG_SIZES_OVERRIDE="${PERF_MSG_SIZES:-${DEFAULT_MULTI_MSG_SIZES}}"

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

while [[ $# -gt 0 ]]; do
  arg="$1"
  case "${arg}" in
    -h|--help)
      usage
      exit 0
      ;;
    --transports)
      HAS_EXPLICIT_TRANSPORT=1
      if [[ $# -lt 2 ]]; then
        echo "Error: ${arg} requires a value." >&2
        exit 1
      fi
      TRANSPORTS_OVERRIDE="${2}"
      shift 2
      ;;
    --msg-sizes)
      HAS_EXPLICIT_MSG_SIZES=1
      if [[ $# -lt 2 ]]; then
        echo "Error: ${arg} requires a value." >&2
        exit 1
      fi
      MSG_SIZES_OVERRIDE="${2}"
      shift 2
      ;;
    --pattern)
      if [[ $# -lt 2 ]]; then
        echo "Error: --pattern requires a value." >&2
        exit 1
      fi
      if [[ "$(printf '%s' "$2" | tr '[:lower:]' '[:upper:]')" == "ALL" ]]; then
        EXPLICIT_PATTERNS=("${PATTERN_LIST[@]}")
        shift 2
        continue
      fi
      IFS=',' read -r -a pattern_list <<< "$2"
      for p in "${pattern_list[@]}"; do
        expand_and_add_explicit_pattern "${p}"
      done
      shift 2
      ;;
    --results-tag)
      HAS_EXPLICIT_RESULTS_TAG=1
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      SCRIPT_ARGS+=( "$1" "$2" )
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
      HAS_EXPLICIT_RUNS=1
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      SCRIPT_ARGS+=( "$1" "$2" )
      shift 2
      ;;
    --runs=*)
      HAS_EXPLICIT_RUNS=1
      SCRIPT_ARGS+=( "$1" )
      shift
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
      SCRIPT_ARGS+=( "$1" )
      shift
      ;;
    --results-dir)
      HAS_EXPLICIT_RESULTS_DIR=1
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      RESULTS_DIR_OVERRIDE="${2}"
      SCRIPT_ARGS+=( "$1" "$2" )
      shift 2
      ;;
    --build-dir)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
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
    --io-threads)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      COMMON_IO_THREADS="${2}"
      SCRIPT_ARGS+=( "$1" "$2" )
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
    --send-hwm)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      SNDHWM="${2}"
      shift 2
      ;;
    --recv-hwm)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      RCVHWM="${2}"
      shift 2
      ;;
    --buf)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      SNDBUF="${2}"
      RCVBUF="${2}"
      shift 2
      ;;
    --sndbuf)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      SNDBUF="${2}"
      shift 2
      ;;
    --rcvbuf)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      RCVBUF="${2}"
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
    --auto-hwm-profile)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      CTX_AUTO_HWM_PROFILE="${2}"
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

if [[ -n "${HWM}" || -n "${SNDHWM}" || -n "${RCVHWM}" || -n "${SNDBUF}" || -n "${RCVBUF}" ]]; then
  if [[ "${ALLOW_MANUAL_SOCKET_OVERRIDES}" != "1" ]]; then
    echo "Error: manual HWM/SNDBUF/RCVBUF overrides are debug-only." >&2
    echo "Set PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1 to use --hwm/--send-hwm/--recv-hwm/--buf/--sndbuf/--rcvbuf." >&2
    exit 1
  fi
fi

if ! is_uint "${TRANSPORT_TRANSITION_MS}"; then
  echo "Error: --transport-transition-ms must be a non-negative integer." >&2
  exit 1
fi
if ! is_uint "${RUN_COOLDOWN_MS}"; then
  echo "Error: run cooldown must be a non-negative integer." >&2
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
if ! is_uint "${CTX_AUTO_HWM_ENABLE}"; then
  echo "Error: PERF_CTX_AUTO_HWM_ENABLE must be a non-negative integer." >&2
  exit 1
fi
case "${CTX_AUTO_HWM_PROFILE}" in
  ""|compact|low_latency|low-latency|balanced|throughput)
    ;;
  *)
    echo "Error: --auto-hwm-profile must be compact, low_latency, balanced, or throughput." >&2
    exit 1
    ;;
esac
if [[ -n "${HWM}" ]] && ( ! is_uint "${HWM}" || (( HWM < 1 )) ); then
  echo "Error: --hwm must be a positive integer." >&2
  exit 1
fi
if [[ -n "${SNDHWM}" ]] && ( ! is_uint "${SNDHWM}" || (( SNDHWM < 1 )) ); then
  echo "Error: --send-hwm must be a positive integer." >&2
  exit 1
fi
if [[ -n "${RCVHWM}" ]] && ( ! is_uint "${RCVHWM}" || (( RCVHWM < 1 )) ); then
  echo "Error: --recv-hwm must be a positive integer." >&2
  exit 1
fi
if [[ -n "${SNDTIMEO_MS}" ]] && ( ! is_uint "${SNDTIMEO_MS}" || (( SNDTIMEO_MS < 1 )) ); then
  echo "Error: --sndtimeo must be a positive integer." >&2
  exit 1
fi
if [[ -n "${RCVTIMEO_MS}" ]] && ( ! is_uint "${RCVTIMEO_MS}" || (( RCVTIMEO_MS < 1 )) ); then
  echo "Error: --rcvtimeo must be a positive integer." >&2
  exit 1
fi
if [[ -n "${COMMON_IO_THREADS}" ]] && ( ! is_uint "${COMMON_IO_THREADS}" || (( COMMON_IO_THREADS < 1 )) ); then
  echo "Error: --io-threads must be a positive integer." >&2
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
if ! is_uint "${SERVER_SHUTDOWN_TIMEOUT_MS}"; then
  echo "Error: --server-shutdown-timeout-ms must be a non-negative integer." >&2
  exit 1
fi
if ! is_uint "${SERVER_BIND_PORT}" || (( SERVER_BIND_PORT > 65535 )); then
  echo "Error: --server-bind-port must be an integer in range 0..65535." >&2
  exit 1
fi
for (( idx=0; idx<${#SCRIPT_ARGS[@]}; ++idx )); do
  if [[ "${SCRIPT_ARGS[idx]}" != "--build-dir" ]]; then
    continue
  fi
  if (( idx + 1 >= ${#SCRIPT_ARGS[@]} )); then
    echo "Error: --build-dir requires a value." >&2
    exit 1
  fi
  requested_build_dir="$(realpath -m "${SCRIPT_ARGS[idx + 1]}")"
  if [[ "${requested_build_dir}" != "$(realpath -m "${OFFICIAL_BUILD_DIR}")" ]]; then
    echo "Error: build directory must be exactly $(realpath -m "${OFFICIAL_BUILD_DIR}")." >&2
    exit 1
  fi
done

if [[ ! -f "${PERF_COMPARISON_SCRIPT}" ]]; then
  echo "Error: comparison script not found: ${PERF_COMPARISON_SCRIPT}" >&2
  exit 1
fi

case "${BUILD_MODE}" in
  reuse)
    if [[ ! -d "${OFFICIAL_BUILD_DIR}" ]]; then
      echo "Error: --reuse-build requires an existing build directory: ${OFFICIAL_BUILD_DIR}" >&2
      exit 1
    fi
    echo "Reusing build directory: ${OFFICIAL_BUILD_DIR}"
    ;;
  clean)
    echo "Cleaning build directory: ${OFFICIAL_BUILD_DIR}"
    rm -rf "${OFFICIAL_BUILD_DIR}"
    ;;
  incremental)
    if [[ -d "${OFFICIAL_BUILD_DIR}" ]]; then
      echo "Using incremental build directory: ${OFFICIAL_BUILD_DIR}"
    else
      echo "Creating build directory: ${OFFICIAL_BUILD_DIR}"
    fi
    ;;
  *)
    echo "Error: invalid build mode: ${BUILD_MODE}" >&2
    exit 1
    ;;
esac

CMAKE_SOURCE_DIR="${ROOT_DIR}/bindings/cpp"
if [[ "${BUILD_MODE}" != "reuse" && -f "${OFFICIAL_BUILD_DIR}/CMakeCache.txt" ]]; then
  CACHE_CMAKE_SOURCE="$(
    sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${OFFICIAL_BUILD_DIR}/CMakeCache.txt" \
      | tail -n 1
  )"
  if [[ -n "${CACHE_CMAKE_SOURCE}" && "${CACHE_CMAKE_SOURCE}" != "${CMAKE_SOURCE_DIR}" ]]; then
    echo "Build cache source mismatch detected:"
    echo "  cache source: ${CACHE_CMAKE_SOURCE}"
    echo "  required source: ${CMAKE_SOURCE_DIR}"
    echo "Resetting build directory: ${OFFICIAL_BUILD_DIR}"
    rm -rf "${OFFICIAL_BUILD_DIR}"
  fi
fi

echo "Using CMake source directory: ${CMAKE_SOURCE_DIR}"
print_core_runtime_binding "${OFFICIAL_BUILD_DIR}"
ensure_core_runtime_not_stale "${OFFICIAL_BUILD_DIR}" "run_benchmarks_multi.sh"

if [[ "${BUILD_MODE}" != "reuse" ]]; then
  if [[ "${IS_WINDOWS}" -eq 1 ]]; then
    CMAKE_GENERATOR="${CMAKE_GENERATOR:-Visual Studio 17 2022}"
    CMAKE_ARCH="${CMAKE_ARCH:-x64}"
    cmake -S "${CMAKE_SOURCE_DIR}" -B "${OFFICIAL_BUILD_DIR}" \
      -G "${CMAKE_GENERATOR}" \
      -A "${CMAKE_ARCH}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_LTO=OFF \
      -DZLINK_CORE_DIR="${ROOT_DIR}/core" \
      -DZLINK_CPP_CORE_BUILD_DIR="${ROOT_DIR}/core/build" \
      -DZLINK_CPP_USE_CORE_BUILD_RUNTIME=ON \
      -DZLINK_CPP_BUILD_BENCHMARKS=ON
    cmake --build "${OFFICIAL_BUILD_DIR}" --config Release --target \
      cpp_comp_src_dealer_dealer_server \
      cpp_comp_src_dealer_dealer_client \
      cpp_comp_src_dealer_router_server \
      cpp_comp_src_dealer_router_client \
      cpp_comp_src_dealer_router_reqrep_server \
      cpp_comp_src_dealer_router_reqrep_client \
      cpp_comp_src_router_router_server \
      cpp_comp_src_router_router_client \
      cpp_comp_src_router_router_reqrep_server \
      cpp_comp_src_router_router_reqrep_client \
      cpp_comp_src_pubsub_server \
      cpp_comp_src_pubsub_client \
      cpp_comp_src_stream_server
  else
    cmake -S "${CMAKE_SOURCE_DIR}" -B "${OFFICIAL_BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_MAKE_PROGRAM="${MAKE_BIN}" \
      -DENABLE_LTO=OFF \
      -DZLINK_CORE_DIR="${ROOT_DIR}/core" \
      -DZLINK_CPP_CORE_BUILD_DIR="${ROOT_DIR}/core/build" \
      -DZLINK_CPP_USE_CORE_BUILD_RUNTIME=ON \
      -DZLINK_CPP_BUILD_BENCHMARKS=ON
    bash "${NORMALIZE_TIMESTAMPS_SH}" "${OFFICIAL_BUILD_DIR}"
    cmake --build "${OFFICIAL_BUILD_DIR}" --target \
      cpp_comp_src_dealer_dealer_server \
      cpp_comp_src_dealer_dealer_client \
      cpp_comp_src_dealer_router_server \
      cpp_comp_src_dealer_router_client \
      cpp_comp_src_dealer_router_reqrep_server \
      cpp_comp_src_dealer_router_reqrep_client \
      cpp_comp_src_router_router_server \
      cpp_comp_src_router_router_client \
      cpp_comp_src_router_router_reqrep_server \
      cpp_comp_src_router_router_reqrep_client \
      cpp_comp_src_pubsub_server \
      cpp_comp_src_pubsub_client \
      cpp_comp_src_stream_server
  fi
fi
ensure_cpp_core_build_runtime_enabled "${OFFICIAL_BUILD_DIR}"

prepare_cpp_runtime_dir() {
  local core_build_dir=""
  local runtime_lib=""
  core_build_dir="$(resolve_configured_core_build_dir "${OFFICIAL_BUILD_DIR}")"
  runtime_lib="$(resolve_core_runtime_library "${core_build_dir}")"
  "${SCRIPT_DIR}/prepare_cpp_runtime.py" --suite multi --runtime-lib "${runtime_lib}"
}

RUNTIME_BUILD_DIR="$(prepare_cpp_runtime_dir)"
RUNTIME_BIN_DIR="${RUNTIME_BUILD_DIR}/bin"

PYTHON_BIN=()
if command -v python3 >/dev/null 2>&1; then
  PYTHON_BIN=(python3)
elif command -v python >/dev/null 2>&1; then
  PYTHON_BIN=(python)
else
  echo "Python not found. Install Python 3 or ensure it is on PATH." >&2
  exit 1
fi

if [[ -z "${CLIENTS}" ]]; then
  memory_max_clients="$(resolve_memory_max_clients)"
  if is_uint "${memory_max_clients}"; then
    default_clients_before="${EFFECTIVE_DEFAULT_CLIENTS}"
    default_stream_clients_before="${EFFECTIVE_DEFAULT_STREAM_CLIENTS}"
    if is_uint "${EFFECTIVE_DEFAULT_CLIENTS}" && (( EFFECTIVE_DEFAULT_CLIENTS > memory_max_clients )); then
      EFFECTIVE_DEFAULT_CLIENTS="${memory_max_clients}"
    fi
    if is_uint "${EFFECTIVE_DEFAULT_STREAM_CLIENTS}" && (( EFFECTIVE_DEFAULT_STREAM_CLIENTS > memory_max_clients )); then
      EFFECTIVE_DEFAULT_STREAM_CLIENTS="${memory_max_clients}"
    fi
    if [[ "${EFFECTIVE_DEFAULT_CLIENTS}" != "${default_clients_before}" || "${EFFECTIVE_DEFAULT_STREAM_CLIENTS}" != "${default_stream_clients_before}" ]]; then
      mem_kb_now="$(memory_available_kb)"
      mem_mb_now=""
      if is_uint "${mem_kb_now}"; then
        mem_mb_now="$(( mem_kb_now / 1024 ))"
      fi
      echo "Info: memory guard capped default clients (general ${default_clients_before}->${EFFECTIVE_DEFAULT_CLIENTS}, stream ${default_stream_clients_before}->${EFFECTIVE_DEFAULT_STREAM_CLIENTS}, mem_available_mb=${mem_mb_now:-unknown})."
    fi
  fi
fi

PATTERNS=("${PATTERN_LIST[@]}")
if [[ "${#EXPLICIT_PATTERNS[@]}" -gt 0 ]]; then
  PATTERNS=("${EXPLICIT_PATTERNS[@]}")
fi

RUN_BASE_ARGS=()
RUN_BASE_ARGS+=(--duration "${DURATION_SECONDS}")
if [[ "${HAS_EXPLICIT_RUNS}" -eq 0 ]]; then
  RUN_BASE_ARGS+=(--runs "1")
fi

if [[ -z "${RESULTS_DIR_OVERRIDE}" ]]; then
  RESULTS_DIR_OVERRIDE="${ROOT_DIR}/bindings/cpp/perf/results"
fi

RUN_ENV=()
RUN_ENV+=(PERF_ALLOW_MULTI="1")
RUN_ENV+=(PERF_POLICY="1")
RUN_ENV+=(PERF_RESULTS_DIR="${RESULTS_DIR_OVERRIDE}")
RUN_ENV+=(PERF_TRANSPORTS="${TRANSPORTS_OVERRIDE}")
RUN_ENV+=(PERF_MULTI_DURATION_SECONDS="${DURATION_SECONDS}")
RUN_ENV+=(PERF_MULTI_RUN_COOLDOWN_MS="${RUN_COOLDOWN_MS}")
RUN_ENV+=(PERF_MULTI_TRANSPORT_TRANSITION_MS="${TRANSPORT_TRANSITION_MS}")
RUN_ENV+=(PERF_MULTI_PATTERN_TRANSITION_MS="${PATTERN_TRANSITION_MS}")
if [[ "${#EXPLICIT_PATTERNS[@]}" -eq 0 ]]; then
  RUN_ENV+=(PERF_FULL_MATRIX=1)
fi
RUN_ENV+=(PERF_MULTI_SERVER_READY_TIMEOUT_MS="${SERVER_READY_TIMEOUT_MS}")
RUN_ENV+=(PERF_MULTI_CONNECT_READY_TIMEOUT_MS="${CONNECT_READY_TIMEOUT_MS}")
RUN_ENV+=(PERF_MULTI_MONITOR_HWM="${MONITOR_HWM}")
RUN_ENV+=(PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS="${SERVER_SHUTDOWN_TIMEOUT_MS}")
RUN_ENV+=(PERF_MULTI_SERVER_BIND_PORT="${SERVER_BIND_PORT}")
if [[ -n "${EFFECTIVE_DEFAULT_IO_THREADS}" ]]; then
  RUN_ENV+=(PERF_MULTI_DEFAULT_IO_THREADS="${EFFECTIVE_DEFAULT_IO_THREADS}")
fi
if [[ -n "${CLIENTS}" ]]; then
  RUN_ENV+=(PERF_MULTI_CLIENTS="${CLIENTS}")
fi
if [[ -n "${MSG_SIZES_OVERRIDE}" ]]; then
  RUN_ENV+=(PERF_MSG_SIZES="${MSG_SIZES_OVERRIDE}")
fi
if [[ -n "${SERVICE_CLIENTS}" ]]; then
  RUN_ENV+=(PERF_MULTI_SERVICE_CLIENTS="${SERVICE_CLIENTS}")
fi
if [[ -n "${LATENCY_SAMPLE_CAP}" ]]; then
  RUN_ENV+=(PERF_MULTI_LATENCY_SAMPLE_CAP="${LATENCY_SAMPLE_CAP}")
fi
if [[ -n "${TIMEOUT_SECONDS}" ]]; then
  RUN_ENV+=(PERF_MULTI_TIMEOUT_SECONDS="${TIMEOUT_SECONDS}")
fi
if [[ -n "${STREAM_MSG_SIZES}" ]]; then
  RUN_ENV+=(PERF_MULTI_STREAM_MSG_SIZES="${STREAM_MSG_SIZES}")
fi
if [[ -n "${PUBSUB_XPUB_NODROP}" ]]; then
  RUN_ENV+=(PERF_MULTI_PUBSUB_XPUB_NODROP="${PUBSUB_XPUB_NODROP}")
fi
if [[ -n "${SERVER_IO_THREADS}" ]]; then
  RUN_ENV+=(PERF_MULTI_SERVER_IO_THREADS="${SERVER_IO_THREADS}")
fi
if [[ -n "${CLIENT_IO_THREADS}" ]]; then
  RUN_ENV+=(PERF_MULTI_CLIENT_IO_THREADS="${CLIENT_IO_THREADS}")
fi
if [[ -n "${STREAM_SERVER_IO_THREADS}" ]]; then
  RUN_ENV+=(PERF_MULTI_STREAM_SERVER_IO_THREADS="${STREAM_SERVER_IO_THREADS}")
fi
if [[ -n "${STREAM_CLIENT_IO_THREADS}" ]]; then
  RUN_ENV+=(PERF_MULTI_STREAM_CLIENT_IO_THREADS="${STREAM_CLIENT_IO_THREADS}")
fi
if [[ -n "${HWM}" ]]; then
  RUN_ENV+=(PERF_MULTI_HWM="${HWM}")
fi
if [[ -n "${SNDHWM}" ]]; then
  RUN_ENV+=(PERF_MULTI_SNDHWM="${SNDHWM}")
fi
if [[ -n "${RCVHWM}" ]]; then
  RUN_ENV+=(PERF_MULTI_RCVHWM="${RCVHWM}")
fi
if [[ -n "${SNDBUF}" ]]; then
  RUN_ENV+=(PERF_MULTI_SNDBUF="${SNDBUF}")
fi
if [[ -n "${RCVBUF}" ]]; then
  RUN_ENV+=(PERF_MULTI_RCVBUF="${RCVBUF}")
fi
if [[ -n "${SNDTIMEO_MS}" ]]; then
  RUN_ENV+=(PERF_MULTI_SNDTIMEO_MS="${SNDTIMEO_MS}")
fi
if [[ -n "${RCVTIMEO_MS}" ]]; then
  RUN_ENV+=(PERF_MULTI_RCVTIMEO_MS="${RCVTIMEO_MS}")
fi
if [[ "${ALLOW_MANUAL_SOCKET_OVERRIDES}" == "1" ]]; then
  RUN_ENV+=(PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1)
fi
if [[ -n "${CTX_AUTO_HWM_ENABLE}" ]]; then
  RUN_ENV+=(PERF_CTX_AUTO_HWM_ENABLE="${CTX_AUTO_HWM_ENABLE}")
fi
if [[ -n "${CTX_AUTO_HWM_PROFILE}" ]]; then
  RUN_ENV+=(PERF_CTX_AUTO_HWM_PROFILE="${CTX_AUTO_HWM_PROFILE}")
  RUN_ENV+=(PERF_AUTO_HWM_PROFILE="${CTX_AUTO_HWM_PROFILE}")
fi
if [[ "${HAS_EXPLICIT_RESULTS_DIR}" -eq 0 && -n "${PERF_RESULTS_DIR:-}" ]]; then
  RUN_ENV+=(PERF_RESULTS_DIR="${PERF_RESULTS_DIR:-}")
fi
SHOW_TOTAL_TIME=1
FAILED_PATTERNS=()
RUN_PATTERNS=()
SKIPPED_PATTERNS=()

record_skip() {
  local pattern="${1:-}"
  local reason="${2:-skip}"
  SKIPPED_PATTERNS+=("$(public_multi_pattern "${pattern}"): ${reason}")
}

print_skip_summary() {
  if [[ "${#SKIPPED_PATTERNS[@]}" -eq 0 ]]; then
    return
  fi
  echo
  echo "## Skips"
  local item=""
  for item in "${SKIPPED_PATTERNS[@]}"; do
    echo "- ${item}"
  done
}

for raw_pattern in "${PATTERNS[@]}"; do
  pattern="$(printf '%s' "${raw_pattern}" | tr '[:lower:]' '[:upper:]')"

  pattern_clients="${CLIENTS}"
  if [[ -z "${pattern_clients}" ]]; then
    if [[ "${pattern}" == "STREAM" || "${pattern}" == STREAM_* ]]; then
      pattern_clients="${EFFECTIVE_DEFAULT_STREAM_CLIENTS}"
    else
      pattern_clients="${EFFECTIVE_DEFAULT_CLIENTS}"
    fi
  fi

  if ! ensure_nofile_limit "${pattern_clients}"; then
    record_skip "${pattern}" "guard_nofile_${NOFILE_SKIP_REASON}"
    continue
  fi

  if ! ensure_memory_budget "${pattern_clients}"; then
    record_skip "${pattern}" "guard_memory_${MEMORY_SKIP_REASON}"
    continue
  fi

  RUN_PATTERNS+=("${pattern}")
done

if [[ "${#RUN_PATTERNS[@]}" -eq 0 ]]; then
  if [[ "${#SKIPPED_PATTERNS[@]}" -eq 0 ]]; then
    echo "Error: no patterns selected to run." >&2
    exit 1
  fi
  print_skip_summary
  exit 0
fi

if [[ -n "${CONNECT_CONCURRENCY}" ]]; then
  RUN_ENV+=(PERF_MULTI_CONNECT_CONCURRENCY="${CONNECT_CONCURRENCY}")
fi

PATTERN_CSV="$(IFS=,; echo "${RUN_PATTERNS[*]}")"
PATTERN_CSV_DISPLAY="$(
  local_items=()
  for pattern in "${RUN_PATTERNS[@]}"; do
    local_items+=("$(public_multi_pattern "${pattern}")")
  done
  IFS=,
  echo "${local_items[*]}"
)"
echo "=== Running multi benchmark: ${PATTERN_CSV_DISPLAY} ==="
echo "    duration=${DURATION_SECONDS}s"

RUN_CMD=("${PYTHON_BIN[@]}" "${PERF_COMPARISON_SCRIPT}" "${PATTERN_CSV}" "--build-dir" "${RUNTIME_BIN_DIR}")
RUN_CMD+=("${RUN_BASE_ARGS[@]}")
RUN_CMD+=("${SCRIPT_ARGS[@]}")
RUN_EXIT_CODE=0
if [[ -n "${OUTPUT_FILE}" ]]; then
  mkdir -p "$(dirname "${OUTPUT_FILE}")"
  if PERF_SUPPRESS_TOTAL_TIME=1 \
    env "${RUN_ENV[@]}" \
    "${RUN_CMD[@]}" | tee "${OUTPUT_FILE}"; then
    :
  else
    RUN_EXIT_CODE=${PIPESTATUS[0]}
    FAILED_PATTERNS+=("${PATTERN_CSV}")
  fi
else
  if PERF_SUPPRESS_TOTAL_TIME=1 \
    env "${RUN_ENV[@]}" \
    "${RUN_CMD[@]}"; then
    :
  else
    RUN_EXIT_CODE=$?
    FAILED_PATTERNS+=("${PATTERN_CSV}")
  fi
fi

if [[ "${#FAILED_PATTERNS[@]}" -gt 0 ]]; then
  print_skip_summary
  echo
  echo "## Failures"
  for pattern in "${FAILED_PATTERNS[@]}"; do
    echo "- $(public_multi_pattern "${pattern}")"
  done
  exit 1
fi

print_skip_summary
