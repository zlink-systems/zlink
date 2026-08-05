#!/usr/bin/env bash
set -euo pipefail
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
CPP_PERF_DIR="${ROOT_DIR}/bindings/cpp/perf"
CPP_RUNTIME_ROOT="${CPP_PERF_DIR}/.runtime"
OFFICIAL_BUILD_DIR="${ROOT_DIR}/bindings/cpp/build"
DEFAULT_CORE_BUILD_DIR="${ROOT_DIR}/core/build"
NORMALIZE_TIMESTAMPS_SH="${ROOT_DIR}/core/tools/normalize_build_timestamps.sh"
MAKE_BIN="$(command -v gmake || command -v make)"

SECONDS=0
SHOW_TOTAL_TIME=0
source "${CPP_PERF_DIR}/run_binding_common.sh"
trap 'print_total_time $?' EXIT

IS_WINDOWS=0
PLATFORM="linux"
ARCH="x64"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    IS_WINDOWS=1
    PLATFORM="windows"
    ;;
  Darwin*)
    PLATFORM="macos"
    ;;
  Linux*)
    PLATFORM="linux"
    ;;
esac

case "$(uname -m)" in
  x86_64|amd64)
    ARCH="x64"
    ;;
  aarch64|arm64)
    ARCH="arm64"
    ;;
  *)
    ARCH="$(uname -m)"
    ;;
esac

BUILD_DIR="${ROOT_DIR}/bindings/cpp/build"

STANDARD_PATTERNS="PAIR,PUBSUB,DEALER_DEALER,DEALER_ROUTER,DEALER_ROUTER_REQREP,ROUTER_ROUTER,ROUTER_ROUTER_REQREP,SPOT"
PATTERN="ALL"
OUTPUT_FILE=""
RESULTS_DIR="${CPP_PERF_DIR}/results"
RESULTS_TAG=""
RESULT_FILE=""
RUNS=""
RUNS_EXPLICIT=0
BUILD_MODE="incremental"
BUILD_MODE_EXPLICIT=0
PIN_CPU=0
PERF_IO_THREADS="${PERF_IO_THREADS:-}"
PERF_MSG_SIZES="${PERF_MSG_SIZES:-}"
PERF_TRANSPORTS="${PERF_TRANSPORTS:-}"
SINGLE_DURATION_SECONDS="${PERF_SINGLE_DURATION_SECONDS:-5}"
SINGLE_HWM="${PERF_SINGLE_HWM:-}"
SINGLE_SNDHWM="${PERF_SINGLE_SNDHWM:-}"
SINGLE_RCVHWM="${PERF_SINGLE_RCVHWM:-}"
SINGLE_SNDBUF="${PERF_SINGLE_SNDBUF:-${PERF_SNDBUF:-}}"
SINGLE_RCVBUF="${PERF_SINGLE_RCVBUF:-${PERF_RCVBUF:-}}"
ALLOW_MANUAL_SOCKET_OVERRIDES="${PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES:-${PERF_ALLOW_MANUAL_SOCKET_OVERRIDES:-0}}"
CTX_AUTO_HWM_ENABLE="${PERF_CTX_AUTO_HWM_ENABLE:-}"
SINGLE_SNDTIMEO_MS="${PERF_SINGLE_SNDTIMEO_MS:-200}"
SINGLE_RCVTIMEO_MS="${PERF_SINGLE_RCVTIMEO_MS:-200}"
CTX_AUTO_HWM_PROFILE="${PERF_SINGLE_CTX_AUTO_HWM_PROFILE:-${PERF_CTX_AUTO_HWM_PROFILE:-balanced}}"
PERF_COMPARISON_SCRIPT="${ROOT_DIR}/bindings/cpp/perf/single/run_comparison.py"

usage() {
  cat <<'USAGE'
Usage: bindings/cpp/perf/run_benchmarks.sh [options]

Measure current zlink C++ binding single-pattern performance.

Options:
  -h, --help                  Show this help.
  --pattern NAME              Pattern list (comma-separated) or ALL.
  --build-dir PATH            Official build directory (must be bindings/cpp/build).
  --reuse-build               Reuse existing build directory as-is (skip configure/build).
  --clean-build               Remove build directory and do a clean build.
  --output PATH               Tee console logs to a file.
  --results-dir PATH          Override result root directory.
  --results-tag NAME          Optional tag in saved result filename.
  --runs N                    Iterations per pattern/transport/size (default: 1).
  --duration N                Override single duration seconds (default: 5).
  --hwm N                     Debug-only PERF_SINGLE_HWM override.
                              Requires PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1.
  --send-hwm N                Debug-only PERF_SINGLE_SNDHWM override.
  --recv-hwm N                Debug-only PERF_SINGLE_RCVHWM override.
  --buf SIZE                  Debug-only override for both PERF_SINGLE_SNDBUF and PERF_SINGLE_RCVBUF.
  --sndbuf SIZE               Debug-only PERF_SINGLE_SNDBUF override (e.g. 64b, 1k, 64k).
  --rcvbuf SIZE               Debug-only PERF_SINGLE_RCVBUF override (e.g. 64b, 1k, 64k).
  --sndtimeo N                Override PERF_SINGLE_SNDTIMEO_MS (default: 200).
  --rcvtimeo N                Override PERF_SINGLE_RCVTIMEO_MS (default: 200).
  --send-timeout-ms N         Alias of --sndtimeo.
  --recv-timeout-ms N         Alias of --rcvtimeo.
  --pin-cpu                   Pin CPU core during benchmark runs (Linux taskset).
  --io-threads N              Set PERF_IO_THREADS for benchmark binaries.
  --msg-sizes LIST            Comma-separated sizes (e.g., 64,1024,65536).
  --transports LIST           Comma-separated transports.
  --auto-hwm-profile NAME     Set auto-HWM profile: compact, low_latency, balanced, throughput (default: balanced).

Notes:
  - result is saved under results/single/report/ as
    perf_cpp_single_<platform>_YYYYMMDD_HHMMSS[_<tag>].txt.
  - default build mode is incremental (configure/build without deleting build dir).
  - --output and result save can be used together.
  - multi patterns are handled only by run_benchmarks_multi.sh.
USAGE
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

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pattern)
      PATTERN="${2:-}"
      shift
      ;;
    --reuse-build)
      set_build_mode "reuse"
      ;;
    --clean-build)
      set_build_mode "clean"
      ;;
    --build-dir)
      BUILD_DIR="${2:-}"
      shift
      ;;
    --output)
      OUTPUT_FILE="${2:-}"
      shift
      ;;
    --results-dir)
      RESULTS_DIR="${2:-}"
      shift
      ;;
    --results-tag)
      RESULTS_TAG="${2:-}"
      shift
      ;;
    --runs)
      RUNS="${2:-}"
      RUNS_EXPLICIT=1
      shift
      ;;
    --duration)
      SINGLE_DURATION_SECONDS="${2:-}"
      shift
      ;;
    --hwm)
      SINGLE_HWM="${2:-}"
      shift
      ;;
    --send-hwm)
      SINGLE_SNDHWM="${2:-}"
      shift
      ;;
    --recv-hwm)
      SINGLE_RCVHWM="${2:-}"
      shift
      ;;
    --buf)
      SINGLE_SNDBUF="${2:-}"
      SINGLE_RCVBUF="${2:-}"
      shift
      ;;
    --sndbuf)
      SINGLE_SNDBUF="${2:-}"
      shift
      ;;
    --rcvbuf)
      SINGLE_RCVBUF="${2:-}"
      shift
      ;;
    --sndtimeo|--send-timeout-ms)
      SINGLE_SNDTIMEO_MS="${2:-}"
      shift
      ;;
    --rcvtimeo|--recv-timeout-ms)
      SINGLE_RCVTIMEO_MS="${2:-}"
      shift
      ;;
    --pin-cpu)
      PIN_CPU=1
      ;;
    --io-threads)
      PERF_IO_THREADS="${2:-}"
      shift
      ;;
    --msg-sizes)
      PERF_MSG_SIZES="${2:-}"
      shift
      ;;
    --transports)
      PERF_TRANSPORTS="${2:-}"
      shift
      ;;
    --auto-hwm-profile)
      CTX_AUTO_HWM_PROFILE="${2:-}"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      if [[ "$1" != --* ]]; then
        if [[ -z "${PATTERN}" || "${PATTERN}" == "ALL" ]]; then
          PATTERN="$1"
        else
          PATTERN="${PATTERN},$1"
        fi
      else
        echo "Unknown option: $1" >&2
        usage >&2
        exit 1
      fi
      ;;
  esac
  shift
done

if [[ -z "${PATTERN}" ]]; then
  echo "Pattern name is required." >&2
  usage >&2
  exit 1
fi

FULL_MATRIX_REQUEST=0
if [[ "${PATTERN}" != "ALL" ]]; then
  PATTERN="$(printf '%s' "${PATTERN}" | tr '[:lower:]' '[:upper:]')"
else
  FULL_MATRIX_REQUEST=1
  PATTERN="${STANDARD_PATTERNS}"
fi

IFS=',' read -r -a PATTERN_LIST <<< "${PATTERN}"
if [[ "${#PATTERN_LIST[@]}" -eq 0 ]]; then
  echo "Error: no valid pattern specified." >&2
  exit 1
fi

for i in "${!PATTERN_LIST[@]}"; do
  PATTERN_LIST[i]="${PATTERN_LIST[i]//[[:space:]]/}"
  if [[ -z "${PATTERN_LIST[i]}" ]]; then
    echo "Error: empty pattern entry in list." >&2
    exit 1
  fi
done

for p in "${PATTERN_LIST[@]}"; do
  case ",${STANDARD_PATTERNS}," in
    *,"${p}",*)
      ;;
    *)
      echo "Error: unsupported single pattern: ${p}" >&2
      echo "Supported single patterns: ${STANDARD_PATTERNS}" >&2
      exit 1
      ;;
  esac
done

resolve_single_build_targets() {
  local pattern_name=""
  local targets=()
  for pattern_name in "${PATTERN_LIST[@]}"; do
    case "${pattern_name}" in
      PAIR)
        targets+=("cpp_perf_pair")
        ;;
      PUBSUB)
        targets+=("cpp_perf_pubsub")
        ;;
      DEALER_DEALER)
        targets+=("cpp_perf_dealer_dealer")
        ;;
      DEALER_ROUTER)
        targets+=("cpp_perf_dealer_router")
        ;;
      DEALER_ROUTER_REQREP)
        targets+=("cpp_perf_dealer_router_reqrep")
        ;;
      ROUTER_ROUTER)
        targets+=("cpp_perf_router_router")
        ;;
      ROUTER_ROUTER_REQREP)
        targets+=("cpp_perf_router_router_reqrep")
        ;;
    esac
  done

  if [[ "${#targets[@]}" -eq 0 ]]; then
    return
  fi

  printf '%s\n' "${targets[@]}" | awk '!seen[$0]++'
}

if [[ ! -f "${PERF_COMPARISON_SCRIPT}" ]]; then
  echo "Error: comparison script not found: ${PERF_COMPARISON_SCRIPT}" >&2
  exit 1
fi

if [[ -n "${PERF_IO_THREADS}" && ! "${PERF_IO_THREADS}" =~ ^[0-9]+$ ]]; then
  echo "PERF_IO_THREADS must be a non-negative integer." >&2
  exit 1
fi

if [[ -n "${PERF_MSG_SIZES}" && ! "${PERF_MSG_SIZES}" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
  echo "PERF_MSG_SIZES must be a comma-separated list of integers." >&2
  exit 1
fi

if [[ -n "${PERF_TRANSPORTS}" && ! "${PERF_TRANSPORTS}" =~ ^[a-z]+(,[a-z]+)*$ ]]; then
  echo "PERF_TRANSPORTS must be a comma-separated list of names." >&2
  exit 1
fi

if [[ -n "${SINGLE_DURATION_SECONDS}" && ( ! "${SINGLE_DURATION_SECONDS}" =~ ^[0-9]+$ || "${SINGLE_DURATION_SECONDS}" -lt 1 ) ]]; then
  echo "duration must be a positive integer." >&2
  exit 1
fi
if [[ -n "${SINGLE_HWM}" && ( ! "${SINGLE_HWM}" =~ ^[0-9]+$ || "${SINGLE_HWM}" -lt 1 ) ]]; then
  echo "hwm must be a positive integer." >&2
  exit 1
fi
if [[ -n "${SINGLE_SNDHWM}" && ( ! "${SINGLE_SNDHWM}" =~ ^[0-9]+$ || "${SINGLE_SNDHWM}" -lt 1 ) ]]; then
  echo "send-hwm must be a positive integer." >&2
  exit 1
fi
if [[ -n "${SINGLE_RCVHWM}" && ( ! "${SINGLE_RCVHWM}" =~ ^[0-9]+$ || "${SINGLE_RCVHWM}" -lt 1 ) ]]; then
  echo "recv-hwm must be a positive integer." >&2
  exit 1
fi
if [[ -n "${SINGLE_SNDTIMEO_MS}" && ( ! "${SINGLE_SNDTIMEO_MS}" =~ ^[0-9]+$ || "${SINGLE_SNDTIMEO_MS}" -lt 1 ) ]]; then
  echo "sndtimeo must be a positive integer." >&2
  exit 1
fi
if [[ -n "${SINGLE_RCVTIMEO_MS}" && ( ! "${SINGLE_RCVTIMEO_MS}" =~ ^[0-9]+$ || "${SINGLE_RCVTIMEO_MS}" -lt 1 ) ]]; then
  echo "rcvtimeo must be a positive integer." >&2
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

if [[ "${RUNS_EXPLICIT}" -eq 0 ]]; then
  RUNS=1
fi
if [[ -z "${RUNS}" || ! "${RUNS}" =~ ^[0-9]+$ || "${RUNS}" -lt 1 ]]; then
  echo "Runs must be a positive integer." >&2
  exit 1
fi

BUILD_DIR="$(realpath -m "${BUILD_DIR}")"
ROOT_DIR="$(realpath -m "${ROOT_DIR}")"
PERF_COMPARISON_SCRIPT="$(realpath -m "${PERF_COMPARISON_SCRIPT}")"

if [[ "${BUILD_DIR}" != "${OFFICIAL_BUILD_DIR}" ]]; then
  echo "Build directory must be exactly: ${OFFICIAL_BUILD_DIR}" >&2
  exit 1
fi

if [[ -z "${RESULTS_DIR}" ]]; then
  RESULTS_DIR="${SCRIPT_DIR}/results"
fi
if [[ -n "${RESULTS_DIR}" ]]; then
  RESULTS_DIR="$(realpath -m "${RESULTS_DIR}")"
fi

RESULT_SUITE="single"
TS="$(date +%Y%m%d_%H%M%S)"
NAME="perf_cpp_${RESULT_SUITE}_${PLATFORM}_${TS}"
if [[ -n "${RESULTS_TAG}" ]]; then
  NAME="${NAME}_${RESULTS_TAG}"
fi
RESULT_FILE="${RESULTS_DIR}/${RESULT_SUITE}/report/${NAME}.txt"

if [[ -n "${OUTPUT_FILE}" ]]; then
  OUTPUT_FILE="$(realpath -m "${OUTPUT_FILE}")"
fi

if [[ -n "${RESULT_FILE}" && -n "${OUTPUT_FILE}" && "${RESULT_FILE}" == "${OUTPUT_FILE}" ]]; then
  echo "Error: --output cannot point to the same file as result output." >&2
  exit 1
fi

cleanup_old_results_dirs() {
  local root="${1:-}"
  local retention="${PERF_RESULTS_RETENTION_DAYS:-90}"
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

case "${BUILD_MODE}" in
  reuse)
    if [[ ! -d "${BUILD_DIR}" ]]; then
      echo "Error: --reuse-build requires an existing build directory: ${BUILD_DIR}" >&2
      exit 1
    fi
    echo "Reusing build directory: ${BUILD_DIR}"
    ;;
  clean)
    echo "Cleaning build directory: ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
    ;;
  incremental)
    if [[ -d "${BUILD_DIR}" ]]; then
      echo "Using incremental build directory: ${BUILD_DIR}"
    else
      echo "Creating build directory: ${BUILD_DIR}"
    fi
    ;;
  *)
    echo "Error: invalid build mode: ${BUILD_MODE}" >&2
    exit 1
    ;;
esac

CMAKE_SOURCE_DIR="${ROOT_DIR}/bindings/cpp"
if [[ "${BUILD_MODE}" != "reuse" && -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  CACHE_CMAKE_SOURCE="$(
    sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt" \
      | tail -n 1
  )"
  if [[ -n "${CACHE_CMAKE_SOURCE}" && "${CACHE_CMAKE_SOURCE}" != "${CMAKE_SOURCE_DIR}" ]]; then
    echo "Build cache source mismatch detected:"
    echo "  cache source: ${CACHE_CMAKE_SOURCE}"
    echo "  required source: ${CMAKE_SOURCE_DIR}"
    echo "Resetting build directory: ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
  fi
fi

echo "Using CMake source directory: ${CMAKE_SOURCE_DIR}"

if [[ "${BUILD_MODE}" != "reuse" ]]; then
  if [[ "${IS_WINDOWS}" -eq 1 ]]; then
    CMAKE_GENERATOR="${CMAKE_GENERATOR:-Visual Studio 17 2022}"
    CMAKE_ARCH="${CMAKE_ARCH:-x64}"
    cmake -S "${CMAKE_SOURCE_DIR}" -B "${BUILD_DIR}" \
      -G "${CMAKE_GENERATOR}" \
      -A "${CMAKE_ARCH}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_LTO=OFF \
      -DZLINK_CORE_DIR="${ROOT_DIR}/core" \
      -DZLINK_CPP_CORE_BUILD_DIR="${ROOT_DIR}/core/build" \
      -DZLINK_CPP_USE_CORE_BUILD_RUNTIME=ON \
      -DZLINK_CPP_BUILD_BENCHMARKS=ON
  else
    cmake -S "${CMAKE_SOURCE_DIR}" -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_MAKE_PROGRAM="${MAKE_BIN}" \
      -DENABLE_LTO=OFF \
      -DZLINK_CORE_DIR="${ROOT_DIR}/core" \
      -DZLINK_CPP_CORE_BUILD_DIR="${ROOT_DIR}/core/build" \
      -DZLINK_CPP_USE_CORE_BUILD_RUNTIME=ON \
      -DZLINK_CPP_BUILD_BENCHMARKS=ON
  fi

  mapfile -t CPP_SINGLE_TARGETS < <(resolve_single_build_targets)
  if [[ "${#CPP_SINGLE_TARGETS[@]}" -eq 0 ]]; then
    echo "Error: failed to resolve single benchmark build targets." >&2
    exit 1
  fi

  if [[ "${IS_WINDOWS}" -eq 1 ]]; then
    cmake --build "${BUILD_DIR}" --config Release --target "${CPP_SINGLE_TARGETS[@]}"
  else
    bash "${NORMALIZE_TIMESTAMPS_SH}" "${BUILD_DIR}"
    cmake --build "${BUILD_DIR}" --target "${CPP_SINGLE_TARGETS[@]}"
  fi
fi

prepare_cpp_runtime_dir() {
  local core_build_dir=""
  local runtime_lib=""
  core_build_dir="$(resolve_configured_core_build_dir "${BUILD_DIR}")"
  runtime_lib="$(resolve_core_runtime_library "${core_build_dir}")"
  "${SCRIPT_DIR}/prepare_cpp_runtime.py" --suite single --runtime-lib "${runtime_lib}"
}

PYTHON_BIN=()
if [[ "${IS_WINDOWS}" -eq 1 ]]; then
  if command -v py >/dev/null 2>&1; then
    PYTHON_BIN=(py -3)
  elif command -v python >/dev/null 2>&1; then
    PYTHON_BIN=(python)
  elif command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN=(python3)
  else
    echo "Python not found. Install Python 3 or ensure it is on PATH." >&2
    exit 1
  fi
else
  if command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN=(python3)
  elif command -v python >/dev/null 2>&1; then
    PYTHON_BIN=(python)
  else
    echo "Python not found. Install Python 3 or ensure it is on PATH." >&2
    exit 1
  fi
fi

if [[ -n "${RESULTS_DIR}" ]]; then
  cleanup_old_results_dirs "${RESULTS_DIR}"
fi

print_core_runtime_binding "${BUILD_DIR}"
ensure_core_runtime_not_stale "${BUILD_DIR}" "run_benchmarks.sh"
ensure_cpp_core_build_runtime_enabled "${BUILD_DIR}"

PATTERN_CSV="$(IFS=,; echo "${PATTERN_LIST[*]}")"
RUNTIME_BUILD_DIR="$(prepare_cpp_runtime_dir)"
RUNTIME_BIN_DIR="${RUNTIME_BUILD_DIR}/bin"

RUN_CMD=("${PYTHON_BIN[@]}" "${PERF_COMPARISON_SCRIPT}" "${PATTERN_CSV}" "--build-dir" "${RUNTIME_BIN_DIR}" "--runs" "${RUNS}")
if [[ "${PIN_CPU}" -eq 1 ]]; then
  RUN_CMD+=("--pin-cpu")
fi
if [[ -n "${SINGLE_DURATION_SECONDS}" ]]; then
  RUN_CMD+=("--duration" "${SINGLE_DURATION_SECONDS}")
fi

if [[ -n "${RESULTS_DIR}" ]]; then
  RUN_CMD+=("--results-dir" "${RESULTS_DIR}")
fi
if [[ -n "${RESULTS_TAG}" ]]; then
  RUN_CMD+=("--results-tag" "${RESULTS_TAG}")
fi
RUN_CMD+=("--result-file" "${RESULT_FILE}")

RUN_ENV=()
RUN_ENV+=(PYTHONUNBUFFERED=1)
if [[ -n "${PERF_IO_THREADS}" ]]; then
  RUN_ENV+=(PERF_IO_THREADS="${PERF_IO_THREADS}")
fi
if [[ -n "${PERF_MSG_SIZES}" ]]; then
  RUN_ENV+=(PERF_MSG_SIZES="${PERF_MSG_SIZES}")
fi
if [[ -n "${PERF_TRANSPORTS}" ]]; then
  RUN_ENV+=(PERF_TRANSPORTS="${PERF_TRANSPORTS}")
fi
if [[ -n "${SINGLE_DURATION_SECONDS}" ]]; then
  RUN_ENV+=(PERF_SINGLE_DURATION_SECONDS="${SINGLE_DURATION_SECONDS}")
fi
if [[ -n "${CTX_AUTO_HWM_ENABLE}" ]]; then
  RUN_ENV+=(PERF_CTX_AUTO_HWM_ENABLE="${CTX_AUTO_HWM_ENABLE}")
fi
if [[ -n "${CTX_AUTO_HWM_PROFILE}" ]]; then
  RUN_ENV+=(PERF_CTX_AUTO_HWM_PROFILE="${CTX_AUTO_HWM_PROFILE}")
fi
if [[ -n "${SINGLE_HWM}" ]]; then
  RUN_ENV+=(PERF_SINGLE_HWM="${SINGLE_HWM}")
fi
if [[ -n "${SINGLE_SNDHWM}" ]]; then
  RUN_ENV+=(PERF_SINGLE_SNDHWM="${SINGLE_SNDHWM}")
fi
if [[ -n "${SINGLE_RCVHWM}" ]]; then
  RUN_ENV+=(PERF_SINGLE_RCVHWM="${SINGLE_RCVHWM}")
fi
if [[ -n "${SINGLE_SNDBUF}" ]]; then
  RUN_ENV+=(PERF_SINGLE_SNDBUF="${SINGLE_SNDBUF}")
fi
if [[ -n "${SINGLE_RCVBUF}" ]]; then
  RUN_ENV+=(PERF_SINGLE_RCVBUF="${SINGLE_RCVBUF}")
fi
if [[ "${ALLOW_MANUAL_SOCKET_OVERRIDES}" == "1" ]]; then
  RUN_ENV+=(PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1)
fi
if [[ -n "${SINGLE_SNDTIMEO_MS}" ]]; then
  RUN_ENV+=(PERF_SINGLE_SNDTIMEO_MS="${SINGLE_SNDTIMEO_MS}")
fi
if [[ -n "${SINGLE_RCVTIMEO_MS}" ]]; then
  RUN_ENV+=(PERF_SINGLE_RCVTIMEO_MS="${SINGLE_RCVTIMEO_MS}")
fi
if [[ "${BUILD_MODE}" == "reuse" ]]; then
  RUN_ENV+=(PERF_NO_AUTOBUILD=1)
fi
if [[ "${FULL_MATRIX_REQUEST}" -eq 1 ]]; then
  RUN_ENV+=(PERF_FULL_MATRIX=1)
fi
value_or_default() {
  local value="${1:-}"
  local fallback="${2:-}"
  if [[ -n "${value}" ]]; then
    printf '%s' "${value}"
  else
    printf '%s' "${fallback}"
  fi
}

print_effective_option() {
  local key="${1:-}"
  local value="${2:-}"
  printf -- "- %s: %s\n" "${key}" "${value}"
}

if [[ -n "${SINGLE_HWM}${SINGLE_SNDHWM}${SINGLE_RCVHWM}${SINGLE_SNDBUF}${SINGLE_RCVBUF}" ]]; then
  if [[ "${ALLOW_MANUAL_SOCKET_OVERRIDES}" != "1" ]]; then
    echo "Error: single manual HWM/SNDBUF/RCVBUF overrides are debug-only." >&2
    echo "Set PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1 to use --hwm/--send-hwm/--recv-hwm/--buf/--sndbuf/--rcvbuf." >&2
    exit 1
  fi
fi

EFFECTIVE_SEND_HWM="${SINGLE_SNDHWM:-${SINGLE_HWM:-}}"
EFFECTIVE_RECV_HWM="${SINGLE_RCVHWM:-${SINGLE_HWM:-}}"
DISPLAY_PATTERN_CSV="${PATTERN_CSV}"
DISPLAY_DURATION_SECONDS="${SINGLE_DURATION_SECONDS}"
DISPLAY_HWM="${SINGLE_HWM}"
DISPLAY_SEND_HWM="${EFFECTIVE_SEND_HWM}"
DISPLAY_RECV_HWM="${EFFECTIVE_RECV_HWM}"
DISPLAY_SNDBUF="${SINGLE_SNDBUF}"
DISPLAY_RCVBUF="${SINGLE_RCVBUF}"
DISPLAY_SNDTIMEO_MS="${SINGLE_SNDTIMEO_MS}"
DISPLAY_RCVTIMEO_MS="${SINGLE_RCVTIMEO_MS}"
DEFAULT_IO_THREADS_LABEL="default(binary=1)"
EFFECTIVE_IO_THREADS="$(value_or_default "${PERF_IO_THREADS}" "${DEFAULT_IO_THREADS_LABEL}")"

echo
echo "## Effective Options (runner)"
print_effective_option "patterns" "${DISPLAY_PATTERN_CSV}"
print_effective_option "build_dir" "${BUILD_DIR}"
print_effective_option "build_mode" "${BUILD_MODE}"
print_effective_option "reuse_build" "$( [[ "${BUILD_MODE}" == "reuse" ]] && echo 1 || echo 0 )"
print_effective_option "clean_build" "$( [[ "${BUILD_MODE}" == "clean" ]] && echo 1 || echo 0 )"
print_effective_option "runs" "${RUNS}"
print_effective_option "duration_seconds" "${DISPLAY_DURATION_SECONDS}"
if [[ "${ALLOW_MANUAL_SOCKET_OVERRIDES}" == "1" ]]; then
  print_effective_option "hwm" "$(value_or_default "${DISPLAY_HWM}" "auto-hwm")"
  print_effective_option "send_hwm" "$(value_or_default "${DISPLAY_SEND_HWM}" "$(value_or_default "${DISPLAY_HWM}" "auto-hwm")")"
  print_effective_option "recv_hwm" "$(value_or_default "${DISPLAY_RECV_HWM}" "$(value_or_default "${DISPLAY_HWM}" "auto-hwm")")"
  print_effective_option "sndbuf" "$(value_or_default "${DISPLAY_SNDBUF}" "-1")"
  print_effective_option "rcvbuf" "$(value_or_default "${DISPLAY_RCVBUF}" "-1")"
else
  print_effective_option "hwm" "auto-hwm"
  print_effective_option "send_hwm" "auto-hwm"
  print_effective_option "recv_hwm" "auto-hwm"
  print_effective_option "sndbuf" "-1"
  print_effective_option "rcvbuf" "-1"
fi
print_effective_option "sndtimeo_ms" "${DISPLAY_SNDTIMEO_MS}"
print_effective_option "rcvtimeo_ms" "${DISPLAY_RCVTIMEO_MS}"
print_effective_option "ctx_auto_hwm_enable" "$(value_or_default "${CTX_AUTO_HWM_ENABLE}" "core-default")"
print_effective_option "ctx_auto_hwm_profile" "${CTX_AUTO_HWM_PROFILE}"
print_effective_option "pin_cpu" "${PIN_CPU}"
print_effective_option "io_threads" "${EFFECTIVE_IO_THREADS}"
print_effective_option "msg_sizes" "$(value_or_default "${PERF_MSG_SIZES}" "default(benchmark)")"
print_effective_option "transports" "$(value_or_default "${PERF_TRANSPORTS}" "default(benchmark)")"
print_effective_option "results_dir" "${RESULTS_DIR}"
print_effective_option "results_tag" "$(value_or_default "${RESULTS_TAG}" "none")"
print_effective_option "result_file" "${RESULT_FILE}"
print_effective_option "output_file" "$(value_or_default "${OUTPUT_FILE}" "none")"
print_effective_option "comparison_script" "${PERF_COMPARISON_SCRIPT}"
print_effective_option "python" "${PYTHON_BIN[*]}"
echo
echo "## Effective Env (runner)"
for entry in "${RUN_ENV[@]}"; do
  key="${entry%%=*}"
  value="${entry#*=}"
  print_effective_option "${key}" "${value}"
done
echo

SHOW_TOTAL_TIME=1
if [[ -n "${OUTPUT_FILE}" ]]; then
  mkdir -p "$(dirname "${OUTPUT_FILE}")"
  env "${RUN_ENV[@]}" "${RUN_CMD[@]}" | tee "${OUTPUT_FILE}"
else
  env "${RUN_ENV[@]}" "${RUN_CMD[@]}"
fi
