#!/usr/bin/env bash
set -euo pipefail
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
CORE_VERSION_OPTION=""
SCRIPT_ARGUMENTS=("$@")
for ((argument_index = 0; argument_index < ${#SCRIPT_ARGUMENTS[@]}; ++argument_index)); do
  case "${SCRIPT_ARGUMENTS[argument_index]}" in
    --core-version)
      if (( argument_index + 1 >= ${#SCRIPT_ARGUMENTS[@]} )); then
        echo "Error: --core-version requires a version." >&2
        exit 1
      fi
      ((++argument_index))
      requested_core_version="${SCRIPT_ARGUMENTS[argument_index]}"
      ;;
    --core-version=*)
      requested_core_version="${SCRIPT_ARGUMENTS[argument_index]#--core-version=}"
      ;;
    *)
      continue
      ;;
  esac
  if [[ ! "${requested_core_version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: --core-version must be MAJOR.MINOR.PATCH: ${requested_core_version:-<missing>}" >&2
    exit 1
  fi
  if [[ -n "${CORE_VERSION_OPTION}" && "${CORE_VERSION_OPTION}" != "${requested_core_version}" ]]; then
    echo "Error: --core-version may be specified only once." >&2
    exit 1
  fi
  CORE_VERSION_OPTION="${requested_core_version}"
done

if [[ -n "${CORE_VERSION_OPTION}" ]]; then
  if [[ -n "${ZLINK_CORE_SOURCE:-}" && "${ZLINK_CORE_SOURCE}" != "release" ]]; then
    echo "Error: --core-version cannot be combined with ZLINK_CORE_SOURCE=${ZLINK_CORE_SOURCE}." >&2
    exit 1
  fi
  export ZLINK_CORE_SOURCE=release
  export ZLINK_CORE_RELEASE_VERSION="${CORE_VERSION_OPTION}"
  export ZLINK_CORE_ALLOW_VERSION_MISMATCH=1
  # Older released runtimes do not expose the current monitor snapshot ABI.
  # Keep the workload identical but skip optional post-run introspection.
  export PERF_PRINT_AUTO_HWM_DETAIL=0
else
  export ZLINK_CORE_SOURCE="${ZLINK_CORE_SOURCE:-local}"
fi
source "${ROOT_DIR}/bindings/tools/local_core_runtime.sh"
NORMALIZE_TIMESTAMPS_SH="${ROOT_DIR}/core/tools/normalize_build_timestamps.sh"

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
  if [[ "${PERF_SUPPRESS_TOTAL_TIME:-0}" == "1" ]]; then
    return
  fi
  local status="${1:-0}"
  local elapsed="${SECONDS}"
  echo "Total benchmark time: $(format_elapsed "${elapsed}") (${elapsed}s, exit=${status})"
}
trap 'print_total_time $?' EXIT

is_positive_u64() {
  local value="${1:-}"
  local normalized="${value}"
  [[ "${value}" =~ ^[0-9]+$ ]] || return 1
  while [[ "${#normalized}" -gt 1 && "${normalized:0:1}" == "0" ]]; do
    normalized="${normalized:1}"
  done
  [[ "${normalized}" != "0" ]] || return 1
  [[ "${#normalized}" -lt 20 ]] && return 0
  [[ "${#normalized}" -eq 20 && ! "${normalized}" > "18446744073709551615" ]]
}

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

CMAKE_GENERATOR="${CMAKE_GENERATOR:-}"
CMAKE_ARCH="${CMAKE_ARCH:-x64}"
if [[ "${IS_WINDOWS}" -eq 1 ]]; then
  OFFICIAL_BUILD_DIR="${ROOT_DIR}/bindings/c/build/windows-x64"
  if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 1 ]]; then
    DEFAULT_CORE_BUILD_DIR="${ZLINK_CORE_PACKAGE_PREFIX}"
  else
    DEFAULT_CORE_BUILD_DIR="${ROOT_DIR}/core/build/windows-x64"
  fi
  if [[ -z "${CMAKE_GENERATOR}" ]]; then
    CMAKE_GENERATOR="Visual Studio 17 2022"
  fi
else
  OFFICIAL_BUILD_DIR="${ROOT_DIR}/bindings/c/build"
  if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 1 ]]; then
    DEFAULT_CORE_BUILD_DIR="${ZLINK_CORE_PACKAGE_PREFIX}"
  else
    DEFAULT_CORE_BUILD_DIR="${ROOT_DIR}/core/build"
  fi
fi

MAKE_BIN=""
if [[ "${IS_WINDOWS}" -eq 0 ]]; then
  MAKE_BIN="$(command -v gmake || command -v make || true)"
  if [[ -z "${MAKE_BIN}" ]]; then
    echo "Error: make or gmake is required on non-Windows platforms." >&2
    exit 1
  fi
fi

normalize_cmake_path() {
  local path="${1:-}"
  if [[ -z "${path}" ]]; then
    return 0
  fi
  if [[ "${IS_WINDOWS}" -eq 1 ]] && command -v cygpath >/dev/null 2>&1; then
    cygpath -m "${path}"
  else
    realpath -m "${path}"
  fi
}

resolve_openssl_root() {
  local core_build_dir="${1:-}"
  local root="${OPENSSL_ROOT_DIR:-}"
  local cache_path="${core_build_dir}/CMakeCache.txt"
  local triplet="${VCPKG_DEFAULT_TRIPLET:-x64-windows-static}"

  if [[ -z "${root}" && -f "${cache_path}" ]]; then
    root="$(sed -n 's/^OPENSSL_ROOT_DIR:[^=]*=//p' "${cache_path}" | tail -n 1)"
  fi
  if [[ -z "${root}" && -n "${VCPKG_ROOT:-}" ]]; then
    root="${VCPKG_ROOT}/installed/${triplet}"
  fi

  if [[ -n "${root}" && -f "${root}/include/openssl/opensslv.h" ]]; then
    printf '%s\n' "${root}"
  fi
}

BUILD_DIR="${OFFICIAL_BUILD_DIR}"

STANDARD_PATTERNS="PAIR,PUBSUB,DEALER_DEALER,DEALER_ROUTER,DEALER_ROUTER_REQREP,ROUTER_ROUTER,ROUTER_ROUTER_REQREP"
PATTERN="ALL"
OUTPUT_FILE=""
RESULTS_DIR=""
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
PERF_PART_COUNT="${PERF_PART_COUNT:-2}"
SINGLE_DURATION_SECONDS="${PERF_SINGLE_DURATION_SECONDS:-5}"
SINGLE_HWM="${PERF_SINGLE_HWM:-}"
SINGLE_SNDHWM="${PERF_SINGLE_SNDHWM:-}"
SINGLE_RCVHWM="${PERF_SINGLE_RCVHWM:-}"
SINGLE_SNDBUF="${PERF_SINGLE_SNDBUF:-${PERF_SNDBUF:-}}"
SINGLE_RCVBUF="${PERF_SINGLE_RCVBUF:-${PERF_RCVBUF:-}}"
ALLOW_MANUAL_SOCKET_OVERRIDES="${PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES:-${PERF_ALLOW_MANUAL_SOCKET_OVERRIDES:-0}}"
CTX_AUTO_HWM_ENABLE="${PERF_CTX_AUTO_HWM_ENABLE:-}"
CTX_AUTO_HWM_PROFILE="${PERF_SINGLE_CTX_AUTO_HWM_PROFILE:-${PERF_CTX_AUTO_HWM_PROFILE:-balanced}}"
SINGLE_SNDTIMEO_MS="${PERF_SINGLE_SNDTIMEO_MS:-200}"
SINGLE_RCVTIMEO_MS="${PERF_SINGLE_RCVTIMEO_MS:-200}"
PERF_COMPARISON_SCRIPT="${SCRIPT_DIR}/single/run_comparison.py"

if [[ "${PERF_ALLOW_MULTI:-0}" == "1" ]]; then
  echo "Error: multi benchmarks are handled by bindings/c/perf/run_benchmarks_multi.sh." >&2
  exit 1
fi

usage() {
  cat <<'USAGE'
Usage: bindings/c/perf/run_benchmarks.sh [options]

Measure current zlink single-pattern performance.

Options:
  -h, --help                  Show this help.
  --pattern NAME              Pattern list (comma-separated) or ALL.
  --build-dir PATH            Official platform build directory (Linux: <repo>/bindings/c/build; Windows: <repo>/bindings/c/build/windows-x64).
  --reuse-build               Reuse existing build directory as-is (skip configure/build).
  --clean-build               Remove build directory and do a clean build.
  --output PATH               Tee console logs to a file.
  --results-dir PATH          Override result root directory.
  --results-tag NAME          Optional tag in saved result filename.
  --runs N                    Iterations per pattern/transport/size (default: 1).
  --duration N                Override single duration seconds (default: 5).
  --hwm BYTES                 Debug-only PERF_SINGLE_HWM byte override.
                             Requires PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1.
  --send-hwm BYTES            Debug-only PERF_SINGLE_SNDHWM byte override.
  --recv-hwm BYTES            Debug-only PERF_SINGLE_RCVHWM byte override.
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
  --part-count N              Application frame count per measured message (1 or 2; default: 2).
  --transports LIST           Comma-separated transports.
  --auto-hwm-profile NAME     Set auto-HWM profile: compact, low_latency, balanced, throughput (default: balanced).
  --core-version VERSION      Download and use the specified released Core version.

Notes:
  - result is saved under results/single/report/ as
    perf_c_single_<platform>_YYYYMMDD_HHMMSS[_<tag>].txt.
  - default build mode is incremental (configure/build without deleting build dir).
  - --output and result save can be used together.
  - run_benchmarks.sh is single-only; run_benchmarks_multi.sh owns multi mode.
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

resolve_configured_core_build_dir() {
  local build_dir="${1:-${OFFICIAL_BUILD_DIR}}"
  : "${build_dir}"
  if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 1 ]]; then
    printf '%s\n' "${ZLINK_CORE_PACKAGE_PREFIX}"
    return
  fi
  realpath -m "${DEFAULT_CORE_BUILD_DIR}"
}

resolve_core_runtime_library() {
  local core_build_dir="${1:-}"
  local candidates=()
  case "$(uname -s)" in
    Linux*)
      candidates=(
        "${core_build_dir}/lib/libzlink.so"
        "${core_build_dir}/bin/libzlink.so"
      )
      ;;
    Darwin*)
      candidates=(
        "${core_build_dir}/lib/libzlink.dylib"
        "${core_build_dir}/bin/libzlink.dylib"
      )
      ;;
    MINGW*|MSYS*|CYGWIN*)
      candidates=(
        "${core_build_dir}/bin/Release/zlink.dll"
        "${core_build_dir}/lib/Release/zlink.dll"
        "${core_build_dir}/bin/zlink.dll"
        "${core_build_dir}/lib/zlink.dll"
      )
      ;;
    *)
      candidates=(
        "${core_build_dir}/lib/libzlink.so"
        "${core_build_dir}/bin/libzlink.so"
      )
      ;;
  esac

  local candidate=""
  for candidate in "${candidates[@]}"; do
    if [[ -f "${candidate}" ]]; then
      realpath -e "${candidate}"
      return 0
    fi
  done
  return 1
}

build_core_runtime() {
  local core_build_dir="${1:-${DEFAULT_CORE_BUILD_DIR}}"
  local core_source_dir="${ROOT_DIR}/core"
  local jobs
  jobs="$(nproc 2>/dev/null || echo 4)"
  local core_cache="${core_build_dir}/CMakeCache.txt"
  local cached_generator=""

  if [[ "${IS_WINDOWS}" -eq 1 && -f "${core_cache}" ]]; then
    cached_generator="$({ sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "${core_cache}" | tail -n 1; } || true)"
    if [[ -n "${cached_generator}" && "${cached_generator}" != "${CMAKE_GENERATOR}" ]]; then
      echo "Core build generator mismatch detected:"
      echo "  cache generator: ${cached_generator}"
      echo "  required generator: ${CMAKE_GENERATOR}"
      echo "Resetting core build directory: ${core_build_dir}"
      rm -rf "${core_build_dir}"
    fi
  fi

  echo "=== Auto-building core runtime (target: ${core_build_dir}) ==="
  if [[ ! -f "${core_cache}" ]]; then
    mkdir -p "${core_build_dir}"
    local configure_args=(
      -S "${core_source_dir}"
      -B "${core_build_dir}"
      -DCMAKE_BUILD_TYPE=Release
      -DBUILD_TESTS=OFF
      -DWITH_DOCS=OFF
      -DWITH_TLS=ON
      -DBUILD_BENCHMARKS=ON
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    )
    if [[ "${IS_WINDOWS}" -eq 1 ]]; then
      configure_args+=(-G "${CMAKE_GENERATOR}")
      if [[ "${CMAKE_GENERATOR}" == Visual\ Studio* ]]; then
        configure_args+=(-A "${CMAKE_ARCH}")
      fi
    else
      configure_args+=(-DCMAKE_MAKE_PROGRAM="${MAKE_BIN}")
    fi
    OPENSSL_CONFIG_ROOT="${OPENSSL_CMAKE_ROOT:-${OPENSSL_ROOT_DIR:-}}"
    if [[ -n "${OPENSSL_CONFIG_ROOT}" ]]; then
      configure_args+=(-DOPENSSL_ROOT_DIR="${OPENSSL_CONFIG_ROOT}"
                       -DCMAKE_PREFIX_PATH="${OPENSSL_CONFIG_ROOT}")
    fi
    cmake "${configure_args[@]}"
  fi
  if [[ "${IS_WINDOWS}" -eq 0 && -f "${NORMALIZE_TIMESTAMPS_SH}" ]]; then
    bash "${NORMALIZE_TIMESTAMPS_SH}" "${core_build_dir}" || true
  fi
  if [[ "${IS_WINDOWS}" -eq 1 ]]; then
    cmake --build "${core_build_dir}" --config Release
  else
    cmake --build "${core_build_dir}" -j"${jobs}"
  fi
}

prepare_core_runtime() {
  local build_dir="${1:-${OFFICIAL_BUILD_DIR}}"
  local core_build_dir=""
  local runtime_lib=""
  local newer_source=""
  local need_build=0
  local reason=""
  core_build_dir="$(resolve_configured_core_build_dir "${build_dir}")"
  if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 1 ]]; then
    if ! runtime_lib="$(resolve_core_runtime_library "${core_build_dir}")"; then
      echo "Error: Core release runtime not found under ${core_build_dir}." >&2
      return 1
    fi
    echo "Perf Core release prefix: ${core_build_dir}"
    echo "Perf runtime libzlink: ${runtime_lib}"
    return 0
  fi

  if ! runtime_lib="$(resolve_core_runtime_library "${core_build_dir}")"; then
    need_build=1
    reason="core runtime library not found under ${core_build_dir}"
  else
    newer_source="$(
      find \
        "${ROOT_DIR}/core/src" \
        "${ROOT_DIR}/core/include" \
        -type f -newer "${runtime_lib}" -print -quit 2>/dev/null || true
    )"
    if [[ -n "${newer_source}" ]]; then
      need_build=1
      reason="stale core runtime (newer source: ${newer_source})"
    fi
  fi

  if (( need_build == 1 )); then
    echo "Note: ${reason}; rebuilding core/build automatically before perf run." >&2
    build_core_runtime "${core_build_dir}"
    if ! runtime_lib="$(resolve_core_runtime_library "${core_build_dir}")"; then
      echo "Error: core runtime library still missing after auto-build under ${core_build_dir}." >&2
      return 1
    fi
    newer_source="$(
      find \
        "${ROOT_DIR}/core/src" \
        "${ROOT_DIR}/core/include" \
        -type f -newer "${runtime_lib}" -print -quit 2>/dev/null || true
    )"
    if [[ -n "${newer_source}" ]]; then
      echo "Error: core runtime still stale after auto-build for bindings/c/perf." >&2
      echo "  runtime: ${runtime_lib}" >&2
      echo "  newer source: ${newer_source}" >&2
      return 1
    fi
  fi

  echo "Perf core build dir: ${core_build_dir}"
  echo "Perf runtime libzlink: ${runtime_lib}"
  return 0
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
    --part-count)
      PERF_PART_COUNT="${2:-}"
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
    --core-version)
      shift
      ;;
    --core-version=*)
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
        targets+=("perf_pair")
        ;;
      PUBSUB)
        targets+=("perf_pubsub")
        ;;
      DEALER_DEALER)
        targets+=("perf_dealer_dealer")
        ;;
      DEALER_ROUTER)
        targets+=("perf_dealer_router")
        ;;
      DEALER_ROUTER_REQREP)
        targets+=("perf_dealer_router_reqrep")
        ;;
      ROUTER_ROUTER)
        targets+=("perf_router_router")
        ;;
      ROUTER_ROUTER_REQREP)
        targets+=("perf_router_router_reqrep")
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

if [[ "${PERF_PART_COUNT}" != "1" && "${PERF_PART_COUNT}" != "2" ]]; then
  echo "--part-count must be 1 or 2." >&2
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
if [[ -n "${SINGLE_HWM}" ]] && ! is_positive_u64 "${SINGLE_HWM}"; then
  echo "hwm must be a positive integer." >&2
  exit 1
fi
if [[ -n "${SINGLE_SNDHWM}" ]] && ! is_positive_u64 "${SINGLE_SNDHWM}"; then
  echo "send-hwm must be a positive integer." >&2
  exit 1
fi
if [[ -n "${SINGLE_RCVHWM}" ]] && ! is_positive_u64 "${SINGLE_RCVHWM}"; then
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

if [[ "${RUNS_EXPLICIT}" -eq 0 ]]; then
  RUNS=1
fi
if [[ -z "${RUNS}" || ! "${RUNS}" =~ ^[0-9]+$ || "${RUNS}" -lt 1 ]]; then
  echo "Runs must be a positive integer." >&2
  exit 1
fi
if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 1 ]]; then
  BUILD_DIR="${OFFICIAL_BUILD_DIR}-release-${ZLINK_CORE_VERSION}"
fi
BUILD_DIR="$(realpath -m "${BUILD_DIR}")"
ROOT_DIR="$(realpath -m "${ROOT_DIR}")"
PERF_COMPARISON_SCRIPT="$(realpath -m "${PERF_COMPARISON_SCRIPT}")"
EXPECTED_BUILD_DIR="${OFFICIAL_BUILD_DIR}"
if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 1 ]]; then
  EXPECTED_BUILD_DIR="${OFFICIAL_BUILD_DIR}-release-${ZLINK_CORE_VERSION}"
fi
if [[ "${BUILD_DIR}" != "${EXPECTED_BUILD_DIR}" ]]; then
  echo "Build directory must be exactly: ${EXPECTED_BUILD_DIR}" >&2
  exit 1
fi

if [[ -z "${RESULTS_DIR}" ]]; then
  RESULTS_DIR="${SCRIPT_DIR}/results"
fi
if [[ -n "${RESULTS_DIR}" ]]; then
  RESULTS_DIR="$(realpath -m "${RESULTS_DIR}")"
fi

TS="$(date +%Y%m%d_%H%M%S)"
NAME="perf_c_single_${PLATFORM}_${TS}"
if [[ -n "${RESULTS_TAG}" ]]; then
  NAME="${NAME}_${RESULTS_TAG}"
fi
RESULT_SUITE="single"
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

CMAKE_SOURCE_DIR="${ROOT_DIR}/bindings/c"
if [[ "${BUILD_MODE}" != "reuse" && -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  CACHE_CMAKE_SOURCE="$(
    sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt" \
      | tail -n 1
  )"
  if [[ -n "${CACHE_CMAKE_SOURCE}" \
        && "$(normalize_cmake_path "${CACHE_CMAKE_SOURCE}")" \
           != "$(normalize_cmake_path "${CMAKE_SOURCE_DIR}")" ]]; then
    echo "Build cache source mismatch detected:"
    echo "  cache source: ${CACHE_CMAKE_SOURCE}"
    echo "  required source: ${CMAKE_SOURCE_DIR}"
    echo "Resetting build directory: ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
  fi
fi

if [[ "${BUILD_MODE}" != "reuse" && "${IS_WINDOWS}" -eq 1 && -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  CACHE_CMAKE_GENERATOR="$({ sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt" | tail -n 1; } || true)"
  if [[ -n "${CACHE_CMAKE_GENERATOR}" && "${CACHE_CMAKE_GENERATOR}" != "${CMAKE_GENERATOR}" ]]; then
    echo "Build cache generator mismatch detected:"
    echo "  cache generator: ${CACHE_CMAKE_GENERATOR}"
    echo "  required generator: ${CMAKE_GENERATOR}"
    echo "Resetting build directory: ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
  fi
fi

echo "Using CMake source directory: ${CMAKE_SOURCE_DIR}"

OPENSSL_CMAKE_ROOT=""
if [[ "${IS_WINDOWS}" -eq 1 ]]; then
  CORE_BUILD_DIR="$(resolve_configured_core_build_dir "${BUILD_DIR}")"
  OPENSSL_CMAKE_ROOT="$(resolve_openssl_root "${CORE_BUILD_DIR}")"
  if [[ -z "${OPENSSL_CMAKE_ROOT}" ]]; then
    echo "Error: OpenSSL was not found for Windows. Set OPENSSL_ROOT_DIR or VCPKG_ROOT." >&2
    exit 1
  fi
  echo "Perf OpenSSL root: ${OPENSSL_CMAKE_ROOT}"
fi
CORE_BUILD_DIR="${CORE_BUILD_DIR:-$(resolve_configured_core_build_dir "${BUILD_DIR}")}"
if ! prepare_core_runtime "${BUILD_DIR}"; then
  exit 1
fi

CORE_RUNTIME_PATH="$(resolve_core_runtime_library "${CORE_BUILD_DIR}")"
CORE_RUNTIME_PATH="$(realpath -m "${CORE_RUNTIME_PATH}")"
echo "Perf Core source/version: ${ZLINK_CORE_SOURCE}/${ZLINK_CORE_VERSION}"
CORE_PROVENANCE_REVISION=""
CORE_RELEASE_TAG=""
CORE_DIRTY="0"
if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 1 ]]; then
  CORE_PROVENANCE_MANIFEST="${ZLINK_CORE_PACKAGE_PREFIX}/share/zlink/core-package-provenance.json"
  CORE_PROVENANCE_REVISION="$(sed -n 's/^[[:space:]]*"revision":[[:space:]]*"\([^"]*\)".*/\1/p' "${CORE_PROVENANCE_MANIFEST}" | head -n 1)"
  CORE_RELEASE_TAG="$(sed -n 's/^[[:space:]]*"tag":[[:space:]]*"\([^"]*\)".*/\1/p' "${CORE_PROVENANCE_MANIFEST}" | head -n 1)"
else
  CORE_PROVENANCE_REVISION="$(git -C "${ROOT_DIR}" rev-parse HEAD)"
  if ! git -C "${ROOT_DIR}" diff --quiet -- core; then
    CORE_DIRTY="1"
  fi
fi

if [[ "${BUILD_MODE}" != "reuse" ]]; then
  if [[ "${IS_WINDOWS}" -eq 1 ]]; then
    CMAKE_ARGS=(
      -S "${CMAKE_SOURCE_DIR}"
      -B "${BUILD_DIR}"
      -G "${CMAKE_GENERATOR}"
      -DCMAKE_BUILD_TYPE=Release
      -DENABLE_LTO=OFF
      -DZLINK_CORE_DIR="${ROOT_DIR}/core"
      -DZLINK_C_CORE_BUILD_DIR="${CORE_BUILD_DIR}"
      -DZLINK_C_BUILD_BENCHMARKS=ON
      -DZLINK_C_BUILD_SAMPLES=OFF
    )
    if [[ "${CMAKE_GENERATOR}" == Visual\ Studio* ]]; then
      CMAKE_ARGS+=(-A "${CMAKE_ARCH}")
    fi
    if [[ -n "${OPENSSL_CMAKE_ROOT}" ]]; then
      CMAKE_ARGS+=(-DOPENSSL_ROOT_DIR="${OPENSSL_CMAKE_ROOT}"
                   -DCMAKE_PREFIX_PATH="${OPENSSL_CMAKE_ROOT}")
    fi
    cmake "${CMAKE_ARGS[@]}"
  else
    cmake -S "${CMAKE_SOURCE_DIR}" -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_MAKE_PROGRAM="${MAKE_BIN}" \
      -DENABLE_LTO=OFF \
      -DZLINK_CORE_DIR="${ROOT_DIR}/core" \
      -DZLINK_C_CORE_BUILD_DIR="${CORE_BUILD_DIR}" \
      -DZLINK_C_BUILD_BENCHMARKS=ON \
      -DZLINK_C_BUILD_SAMPLES=OFF
  fi

  mapfile -t BUILD_TARGETS < <(resolve_single_build_targets)
  if [[ "${#BUILD_TARGETS[@]}" -eq 0 ]]; then
    echo "Error: failed to resolve single benchmark build targets." >&2
    exit 1
  fi

  if [[ "${IS_WINDOWS}" -eq 1 ]]; then
    cmake --build "${BUILD_DIR}" --config Release --target "${BUILD_TARGETS[@]}"
  else
    bash "${NORMALIZE_TIMESTAMPS_SH}" "${BUILD_DIR}"
    cmake --build "${BUILD_DIR}" --target "${BUILD_TARGETS[@]}"
  fi
fi

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

case "${CTX_AUTO_HWM_PROFILE}" in
  ""|compact|low_latency|low-latency|balanced|throughput)
    ;;
  *)
    echo "Error: --auto-hwm-profile must be compact, low_latency, balanced, or throughput." >&2
    exit 1
    ;;
esac

PATTERN_CSV="$(IFS=,; echo "${PATTERN_LIST[*]}")"
RUN_CMD=("${PYTHON_BIN[@]}" "${PERF_COMPARISON_SCRIPT}" "${PATTERN_CSV}" "--build-dir" "${BUILD_DIR}" "--runs" "${RUNS}")
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
RUN_ENV+=(PERF_CORE_SOURCE="${ZLINK_CORE_SOURCE}")
RUN_ENV+=(PERF_CORE_VERSION="${ZLINK_CORE_VERSION}")
RUN_ENV+=(PERF_CORE_RUNTIME="${CORE_RUNTIME_PATH}")
RUN_ENV+=(PERF_CORE_REVISION="${CORE_PROVENANCE_REVISION:-unknown}")
RUN_ENV+=(PERF_CORE_DIRTY="${CORE_DIRTY}")
if [[ -n "${CORE_RELEASE_TAG}" ]]; then
  RUN_ENV+=(PERF_CORE_RELEASE_TAG="${CORE_RELEASE_TAG}")
fi
if [[ -n "${PERF_IO_THREADS}" ]]; then
  RUN_ENV+=(PERF_IO_THREADS="${PERF_IO_THREADS}")
fi
if [[ -n "${PERF_MSG_SIZES}" ]]; then
  RUN_ENV+=(PERF_MSG_SIZES="${PERF_MSG_SIZES}")
fi
RUN_ENV+=(PERF_PART_COUNT="${PERF_PART_COUNT}")
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
if [[ -n "${PERF_DISABLE_RESOURCE_METRICS:-}" ]]; then
  RUN_ENV+=(PERF_DISABLE_RESOURCE_METRICS="${PERF_DISABLE_RESOURCE_METRICS}")
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
print_effective_option "part_count" "${PERF_PART_COUNT}"
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
