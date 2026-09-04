#!/usr/bin/env bash
set -euo pipefail

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

# Use the current workspace Core by default. An explicit --core-version selects
# the downloaded release package for that version instead.
if [[ -n "${CORE_VERSION_OPTION}" ]]; then
  if [[ -n "${ZLINK_CORE_SOURCE:-}" && "${ZLINK_CORE_SOURCE}" != "release" ]]; then
    echo "Error: --core-version cannot be combined with ZLINK_CORE_SOURCE=${ZLINK_CORE_SOURCE}." >&2
    exit 1
  fi
  export ZLINK_CORE_SOURCE=release
  export ZLINK_CORE_RELEASE_VERSION="${CORE_VERSION_OPTION}"
  export ZLINK_CORE_ALLOW_VERSION_MISMATCH=1
else
  export ZLINK_CORE_SOURCE="${ZLINK_CORE_SOURCE:-local}"
fi
source "${ROOT_DIR}/bindings/tools/local_core_runtime.sh"
NORMALIZE_TIMESTAMPS_SH="${ROOT_DIR}/core/tools/normalize_build_timestamps.sh"
PERF_COMPARISON_SCRIPT="${SCRIPT_DIR}/run_comparison.py"
PATTERNS="DEALER_DEALER,DEALER_ROUTER_SENDSEND,ROUTER_ROUTER_SENDSEND,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB,STREAM"
TRANSPORTS="${PERF_TRANSPORTS:-tcp,tls,ws,wss}"
DEFAULT_MULTI_MSG_SIZES="64,256,1024,4096,65536,131072"
MSG_SIZES="${PERF_MSG_SIZES:-${DEFAULT_MULTI_MSG_SIZES}}"
PART_COUNT="${PERF_PART_COUNT:-2}"
IFS=',' read -r -a PATTERN_LIST <<< "${PATTERNS}"

IS_WINDOWS=0
PLATFORM="linux"
CMAKE_GENERATOR="${CMAKE_GENERATOR:-}"
CMAKE_ARCH="${CMAKE_ARCH:-x64}"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    IS_WINDOWS=1
    PLATFORM="windows"
    ;;
  Darwin*)
    PLATFORM="macos"
    ;;
esac

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

is_uint() {
  local value="${1:-}"
  [[ "${value}" =~ ^[0-9]+$ ]]
}

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

is_positive_u64() {
  local value="${1:-}"
  local normalized="${value}"
  is_uint "${value}" || return 1
  while [[ "${#normalized}" -gt 1 && "${normalized:0:1}" == "0" ]]; do
    normalized="${normalized:1}"
  done
  [[ "${normalized}" != "0" ]] || return 1
  [[ "${#normalized}" -lt 20 ]] && return 0
  [[ "${#normalized}" -eq 20 && ! "${normalized}" > "18446744073709551615" ]]
}

is_u64() {
  local value="${1:-}"
  local normalized="${value}"
  is_uint "${value}" || return 1
  while [[ "${#normalized}" -gt 1 && "${normalized:0:1}" == "0" ]]; do
    normalized="${normalized:1}"
  done
  [[ "${#normalized}" -lt 20 ]] && return 0
  [[ "${#normalized}" -eq 20 && ! "${normalized}" > "18446744073709551615" ]]
}

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

resolve_configured_core_build_dir() {
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
  jobs="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
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
    cmake --build "${core_build_dir}" --config Release --target libzlink
  else
    # Delegate to the repository build script so build policy lives in one
    # place; --lib-only rebuilds the runtime without relinking test executables.
    if [[ "$(normalize_cmake_path "${core_build_dir}")" == "$(normalize_cmake_path "${ROOT_DIR}/core/build")" ]]; then
      JOBS="${jobs}" bash "${ROOT_DIR}/scripts/build-core.sh" release --lib-only
    else
      cmake --build "${core_build_dir}" -j"${jobs}" --target libzlink
    fi
  fi
}

prepare_core_runtime() {
  local build_dir="${1:-${OFFICIAL_BUILD_DIR}}"
  local core_build_dir=""
  local runtime_lib=""
  local newer_source=""
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

  # Always let the Core build system resolve source changes before a local
  # benchmark. An unchanged tree is an inexpensive incremental no-op.
  build_core_runtime "${core_build_dir}"
  if ! runtime_lib="$(resolve_core_runtime_library "${core_build_dir}")"; then
    echo "Error: core runtime library missing after local build under ${core_build_dir}." >&2
    return 1
  fi
  newer_source="$(
    find \
      "${ROOT_DIR}/core/src" \
      "${ROOT_DIR}/core/include" \
      -type f -newer "${runtime_lib}" -print -quit 2>/dev/null || true
  )"
  if [[ -n "${newer_source}" ]]; then
    echo "Error: core runtime still stale after local build for bindings/c/perf." >&2
    echo "  runtime: ${runtime_lib}" >&2
    echo "  newer source: ${newer_source}" >&2
    return 1
  fi

  echo "Perf core build dir: ${core_build_dir}"
  echo "Perf runtime libzlink: ${runtime_lib}"
  return 0
}

verify_benchmark_core_runtime() {
  if [[ "${IS_WINDOWS}" -eq 1 || "$(uname -s)" != Linux* ]]; then
    return 0
  fi

  local expected_runtime
  expected_runtime="$(realpath -e "${CORE_RUNTIME_PATH}")"
  local target
  local binary
  local loaded_runtime
  local -a runtime_check_targets
  mapfile -t runtime_check_targets < <(resolve_multi_build_targets)
  for target in "${runtime_check_targets[@]}"; do
    binary="${BUILD_DIR}/perf/${target}"
    if [[ ! -x "${binary}" ]]; then
      echo "Error: benchmark runtime check target is missing: ${binary}" >&2
      return 1
    fi
    # STREAM's peer is an independent raw TCP/TLS/WebSocket load generator.
    # It intentionally does not link Core; only the STREAM server participates
    # in the Core runtime identity check.
    if [[ "${target}" == "perf_stream_client" ]]; then
      continue
    fi
    loaded_runtime="$({ ldd "${binary}" | sed -n 's/^[[:space:]]*libzlink\.so\.0 => \([^[:space:]]*\).*/\1/p' | head -n 1; } || true)"
    if [[ -z "${loaded_runtime}" ]]; then
      echo "Error: failed to resolve libzlink.so.0 for benchmark: ${binary}" >&2
      return 1
    fi
    loaded_runtime="$(realpath -e "${loaded_runtime}")"
    if [[ "${loaded_runtime}" != "${expected_runtime}" ]]; then
      echo "Error: benchmark resolved a different Core runtime." >&2
      echo "  benchmark: ${binary}" >&2
      echo "  expected:  ${expected_runtime}" >&2
      echo "  resolved:  ${loaded_runtime}" >&2
      return 1
    fi
  done
  echo "Verified benchmark Core runtime: ${expected_runtime}"
}

usage() {
  cat <<'USAGE'
Usage: bindings/c/perf/run_benchmarks_multi.sh [options]

Run only multi-socket benchmark patterns.
Default PATTERN is:
  DEALER_DEALER,DEALER_ROUTER_SENDSEND,ROUTER_ROUTER_SENDSEND,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB,STREAM
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
  --core-version VERSION Download and use the specified released Core version.
                         Without this option, use the local core/build runtime.
  --help                 Show this help.
  --reuse-build          Reuse existing build directory as-is (skip configure/build).
  --clean-build          Remove build directory and do a clean build.
  --results-dir PATH     Override results root directory.
  --results-tag NAME     Optional tag appended to the results filename.
  --build-dir PATH       Official build directory (must be <repo>/bindings/c/build).
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
  --part-count N         Application frame count per measured message (1 or 2; default: 2).
  --transports LIST      Comma-separated transports.
  --duration N           Optional override for multi duration seconds (default 5).
  --clients N            Override client count (default: 100).
  --hwm BYTES            Debug-only byte override PERF_MULTI_HWM.
                         Requires PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1.
  --send-hwm BYTES       Debug-only byte override PERF_MULTI_SNDHWM (fallback: --hwm).
  --recv-hwm BYTES       Debug-only byte override PERF_MULTI_RCVHWM (fallback: --hwm).
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
                         Override PERF_MULTI_CONNECT_READY_TIMEOUT_MS (default: 10000).
  --monitor-hwm-bytes BYTES
                         Override PERF_MULTI_MONITOR_HWM_BYTES (default: 4096000).
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
    perf_c_multi_<platform>_YYYYMMDD_HHMMSS[_<tag>].txt.
  - default build mode is incremental (configure/build without deleting build dir).
  - this runner links zlink core from core/build and prints the resolved
    libzlink runtime before execution.
  - if core/build is missing or core/src or core/include is newer than the
    resolved runtime library, the runner auto-rebuilds core/build before
    proceeding (incremental cmake --build; configures with defaults if no
    CMakeCache.txt is present).
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

resolve_multi_build_targets() {
  local pattern_name=""
  local targets=()

  for pattern_name in "${RUN_PATTERNS[@]}"; do
    case "${pattern_name}" in
      DEALER_DEALER)
        targets+=("comp_src_dealer_dealer_server" "comp_src_dealer_dealer_client")
        ;;
      DEALER_ROUTER|DEALER_ROUTER_SENDSEND)
        targets+=("comp_src_dealer_router_sendsend_server" "comp_src_dealer_router_sendsend_client")
        ;;
      ROUTER_ROUTER|ROUTER_ROUTER_SENDSEND)
        targets+=("comp_src_router_router_sendsend_server" "comp_src_router_router_sendsend_client")
        if [[ "${PERF_MULTI_MATCHED_BASELINE:-0}" == "1" ]]; then
          targets+=("comp_src_router_router_sendsend_matched_client")
        fi
        ;;
      DEALER_ROUTER_REQREP)
        targets+=("comp_src_dealer_router_reqrep_server" "comp_src_dealer_router_reqrep_client")
        ;;
      ROUTER_ROUTER_REQREP)
        targets+=("comp_src_router_router_reqrep_server" "comp_src_router_router_reqrep_client")
        if [[ "${PERF_MULTI_MATCHED_BASELINE:-0}" == "1" ]]; then
          targets+=("comp_src_router_router_reqrep_matched_client")
        fi
        ;;
      PUBSUB)
        targets+=("comp_src_pubsub_server" "comp_src_pubsub_client")
        ;;
      STREAM)
        targets+=("comp_src_stream_server" "perf_stream_client")
        ;;
    esac
  done

  if [[ "${#targets[@]}" -eq 0 ]]; then
    return
  fi

  printf '%s\n' "${targets[@]}" | awk '!seen[$0]++'
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
    DEALER_ROUTER)
      add_explicit_pattern_unique "DEALER_ROUTER_SENDSEND"
      ;;
    ROUTER_ROUTER)
      add_explicit_pattern_unique "ROUTER_ROUTER_SENDSEND"
      ;;
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
EFFECTIVE_DEFAULT_STREAM_CLIENTS="${PERF_MULTI_DEFAULT_STREAM_CLIENTS:-${PERF_STREAM_DEFAULT_CLIENTS:-100}}"
HWM="${PERF_MULTI_HWM:-${PERF_HWM:-}}"
SNDHWM="${PERF_MULTI_SNDHWM:-${PERF_SNDHWM:-}}"
RCVHWM="${PERF_MULTI_RCVHWM:-${PERF_RCVHWM:-}}"
SNDBUF="${PERF_MULTI_SNDBUF:-${PERF_SNDBUF:-}}"
RCVBUF="${PERF_MULTI_RCVBUF:-${PERF_RCVBUF:-}}"
SNDTIMEO_MS="${PERF_MULTI_SNDTIMEO_MS:-${PERF_SNDTIMEO_MS:-200}}"
RCVTIMEO_MS="${PERF_MULTI_RCVTIMEO_MS:-${PERF_RCVTIMEO_MS:-200}}"
CONNECT_CONCURRENCY="${PERF_MULTI_CONNECT_CONCURRENCY:-${PERF_CONNECT_CONCURRENCY:-}}"
SERVICE_CLIENTS="${PERF_MULTI_SERVICE_CLIENTS:-${PERF_SERVICE_CLIENTS:-}}"
TIMEOUT_SECONDS="${PERF_MULTI_TIMEOUT_SECONDS:-${PERF_TIMEOUT_SECONDS:-}}"
STREAM_MSG_SIZES="${PERF_MULTI_STREAM_MSG_SIZES:-${PERF_STREAM_MSG_SIZES:-64,256,1024,65536}}"
PUBSUB_XPUB_NODROP="${PERF_MULTI_PUBSUB_XPUB_NODROP:-${PERF_PUBSUB_XPUB_NODROP:-}}"
RUN_COOLDOWN_MS="${PERF_MULTI_RUN_COOLDOWN_MS:-${PERF_RUN_COOLDOWN_MS:-3000}}"
TRANSPORT_TRANSITION_MS="${PERF_MULTI_TRANSPORT_TRANSITION_MS:-${PERF_TRANSPORT_TRANSITION_MS:-3000}}"
PATTERN_TRANSITION_MS="${PERF_MULTI_PATTERN_TRANSITION_MS:-${PERF_PATTERN_TRANSITION_MS:-3000}}"
SERVER_READY_TIMEOUT_MS="${PERF_MULTI_SERVER_READY_TIMEOUT_MS:-${PERF_SERVER_READY_TIMEOUT_MS:-10000}}"
CONNECT_READY_TIMEOUT_MS="${PERF_MULTI_CONNECT_READY_TIMEOUT_MS:-${PERF_CONNECT_READY_TIMEOUT_MS:-10000}}"
MONITOR_HWM_BYTES="${PERF_MULTI_MONITOR_HWM_BYTES:-${PERF_MONITOR_HWM_BYTES:-4096000}}"
SERVER_SHUTDOWN_TIMEOUT_MS="${PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS:-${PERF_SERVER_SHUTDOWN_TIMEOUT_MS:-5000}}"
SERVER_BIND_PORT="${PERF_MULTI_SERVER_BIND_PORT:-${PERF_SERVER_BIND_PORT:-0}}"
CTX_AUTO_HWM_ENABLE="${PERF_CTX_AUTO_HWM_ENABLE:-1}"
CTX_AUTO_HWM_PROFILE="${PERF_MULTI_CTX_AUTO_HWM_PROFILE:-${PERF_CTX_AUTO_HWM_PROFILE:-balanced}}"
DISABLE_RESOURCE_METRICS="${PERF_DISABLE_RESOURCE_METRICS:-0}"
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
ALLOW_MANUAL_SOCKET_OVERRIDES="${PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES:-${PERF_ALLOW_MANUAL_SOCKET_OVERRIDES:-0}}"
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
      TRANSPORTS="${2}"
      shift 2
      ;;
    --msg-sizes)
      HAS_EXPLICIT_MSG_SIZES=1
      if [[ $# -lt 2 ]]; then
        echo "Error: ${arg} requires a value." >&2
        exit 1
      fi
      MSG_SIZES="${2}"
      shift 2
      ;;
    --part-count)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      PART_COUNT="${2}"
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
    --core-version)
      if [[ $# -lt 2 ]]; then
        echo "Error: --core-version requires a version." >&2
        exit 1
      fi
      shift 2
      ;;
    --core-version=*)
      shift
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
      SCRIPT_ARGS+=( "$1" "$2" )
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
    --monitor-hwm-bytes)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      MONITOR_HWM_BYTES="${2}"
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
    --auto-hwm-profile)
      if [[ $# -lt 2 ]]; then
        echo "Error: $1 requires a value." >&2
        exit 1
      fi
      CTX_AUTO_HWM_PROFILE="${2}"
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

if [[ "${HAS_EXPLICIT_MSG_SIZES}" -eq 1 ]]; then
  STREAM_MSG_SIZES="${MSG_SIZES}"
fi

if [[ "${PART_COUNT}" != "1" && "${PART_COUNT}" != "2" ]]; then
  echo "Error: --part-count must be 1 or 2." >&2
  exit 1
fi

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
if ! is_uint "${MONITOR_HWM_BYTES}"; then
  echo "Error: --monitor-hwm-bytes must be a non-negative integer." >&2
  exit 1
fi
if [[ -n "${HWM}" ]] && ! is_u64 "${HWM}"; then
  echo "Error: --hwm must be an unsigned 64-bit integer." >&2
  exit 1
fi
if [[ -n "${SNDHWM}" ]] && ! is_u64 "${SNDHWM}"; then
  echo "Error: --send-hwm must be an unsigned 64-bit integer." >&2
  exit 1
fi
if [[ -n "${RCVHWM}" ]] && ! is_u64 "${RCVHWM}"; then
  echo "Error: --recv-hwm must be an unsigned 64-bit integer." >&2
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
case "${CTX_AUTO_HWM_PROFILE}" in
  ""|compact|low_latency|low-latency|balanced|throughput)
    ;;
  *)
    echo "Error: --auto-hwm-profile must be compact, low_latency, balanced, or throughput." >&2
    exit 1
    ;;
esac
case "${CTX_AUTO_HWM_ENABLE}" in
  0|1)
    ;;
  *)
    echo "Error: PERF_CTX_AUTO_HWM_ENABLE must be 0 or 1." >&2
    exit 1
    ;;
esac
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
  RESULTS_DIR_OVERRIDE="${SCRIPT_DIR}/results"
fi

RUN_ENV=()
RUN_ENV+=(PERF_ALLOW_MULTI="1")
RUN_ENV+=(PERF_POLICY="1")
RUN_ENV+=(PERF_RESULTS_DIR="${RESULTS_DIR_OVERRIDE}")
RUN_ENV+=(PERF_MULTI_DURATION_SECONDS="${DURATION_SECONDS}")
RUN_ENV+=(PERF_TRANSPORTS="${TRANSPORTS}")
RUN_ENV+=(PERF_PART_COUNT="${PART_COUNT}")
if [[ -n "${MSG_SIZES}" ]]; then
  RUN_ENV+=(PERF_MSG_SIZES="${MSG_SIZES}")
fi
RUN_ENV+=(PERF_MULTI_RUN_COOLDOWN_MS="${RUN_COOLDOWN_MS}")
RUN_ENV+=(PERF_MULTI_TRANSPORT_TRANSITION_MS="${TRANSPORT_TRANSITION_MS}")
RUN_ENV+=(PERF_MULTI_PATTERN_TRANSITION_MS="${PATTERN_TRANSITION_MS}")
if [[ "${#EXPLICIT_PATTERNS[@]}" -eq 0 ]]; then
  RUN_ENV+=(PERF_FULL_MATRIX=1)
fi
RUN_ENV+=(PERF_MULTI_SERVER_READY_TIMEOUT_MS="${SERVER_READY_TIMEOUT_MS}")
RUN_ENV+=(PERF_MULTI_CONNECT_READY_TIMEOUT_MS="${CONNECT_READY_TIMEOUT_MS}")
RUN_ENV+=(PERF_MULTI_MONITOR_HWM_BYTES="${MONITOR_HWM_BYTES}")
RUN_ENV+=(PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS="${SERVER_SHUTDOWN_TIMEOUT_MS}")
RUN_ENV+=(PERF_MULTI_SERVER_BIND_PORT="${SERVER_BIND_PORT}")
RUN_ENV+=(PERF_DISABLE_RESOURCE_METRICS="${DISABLE_RESOURCE_METRICS}")
if [[ -n "${EFFECTIVE_DEFAULT_IO_THREADS}" ]]; then
  RUN_ENV+=(PERF_MULTI_DEFAULT_IO_THREADS="${EFFECTIVE_DEFAULT_IO_THREADS}")
fi
if [[ -n "${CLIENTS}" ]]; then
  RUN_ENV+=(PERF_MULTI_CLIENTS="${CLIENTS}")
fi
if [[ -n "${SERVICE_CLIENTS}" ]]; then
  RUN_ENV+=(PERF_MULTI_SERVICE_CLIENTS="${SERVICE_CLIENTS}")
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
if [[ "${ALLOW_MANUAL_SOCKET_OVERRIDES}" == "1" ]]; then
  RUN_ENV+=(PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1)
fi
if [[ -n "${SNDTIMEO_MS}" ]]; then
  RUN_ENV+=(PERF_MULTI_SNDTIMEO_MS="${SNDTIMEO_MS}")
fi
if [[ -n "${RCVTIMEO_MS}" ]]; then
  RUN_ENV+=(PERF_MULTI_RCVTIMEO_MS="${RCVTIMEO_MS}")
fi
RUN_ENV+=(PERF_CTX_AUTO_HWM_ENABLE="${CTX_AUTO_HWM_ENABLE}")
if [[ -n "${CTX_AUTO_HWM_PROFILE}" ]]; then
  RUN_ENV+=(PERF_CTX_AUTO_HWM_PROFILE="${CTX_AUTO_HWM_PROFILE}")
fi
if [[ "${HAS_EXPLICIT_RESULTS_DIR}" -eq 0 && -n "${PERF_RESULTS_DIR:-}" ]]; then
  RUN_ENV+=(PERF_RESULTS_DIR="${PERF_RESULTS_DIR:-}")
fi
EFFECTIVE_SEND_HWM="${SNDHWM:-${HWM:-}}"

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

RUN_ENV+=(PERF_MULTI_TRANSPORT_TRANSITION_MS="${TRANSPORT_TRANSITION_MS}")

if [[ -n "${CONNECT_CONCURRENCY}" ]]; then
  RUN_ENV+=(PERF_MULTI_CONNECT_CONCURRENCY="${CONNECT_CONCURRENCY}")
fi

BUILD_DIR="${OFFICIAL_BUILD_DIR}"
# A benchmark executable records the Core library directory in its runtime
# search path. Keep each released Core version in a separate CMake build tree
# so `--reuse-build` cannot silently execute the Core that was linked by a
# previous run.
if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 1 ]]; then
  BUILD_DIR="${OFFICIAL_BUILD_DIR}-release-${ZLINK_CORE_VERSION}"
fi
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
CORE_BUILD_DIR="$(resolve_configured_core_build_dir "${BUILD_DIR}")"
OPENSSL_CMAKE_ROOT=""
if [[ "${IS_WINDOWS}" -eq 1 ]]; then
  OPENSSL_CMAKE_ROOT="$(resolve_openssl_root "${CORE_BUILD_DIR}")"
  if [[ -z "${OPENSSL_CMAKE_ROOT}" ]]; then
    echo "Error: OpenSSL was not found for Windows. Set OPENSSL_ROOT_DIR or VCPKG_ROOT." >&2
    exit 1
  fi
  echo "Perf OpenSSL root: ${OPENSSL_CMAKE_ROOT}"
fi
prepare_core_runtime "${BUILD_DIR}"

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
RUN_ENV+=(PERF_CORE_SOURCE="${ZLINK_CORE_SOURCE}")
RUN_ENV+=(PERF_CORE_VERSION="${ZLINK_CORE_VERSION}")
RUN_ENV+=(PERF_CORE_RUNTIME="${CORE_RUNTIME_PATH}")
RUN_ENV+=(PERF_CORE_REVISION="${CORE_PROVENANCE_REVISION:-unknown}")
RUN_ENV+=(PERF_CORE_DIRTY="${CORE_DIRTY}")
if [[ -n "${CORE_RELEASE_TAG}" ]]; then
  RUN_ENV+=(PERF_CORE_RELEASE_TAG="${CORE_RELEASE_TAG}")
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

  mapfile -t BUILD_TARGETS < <(resolve_multi_build_targets)
  if [[ "${#BUILD_TARGETS[@]}" -eq 0 ]]; then
    echo "Error: failed to resolve multi benchmark build targets." >&2
    exit 1
  fi

  if [[ "${IS_WINDOWS}" -eq 1 ]]; then
    cmake --build "${BUILD_DIR}" --config Release --target "${BUILD_TARGETS[@]}"
  else
    bash "${NORMALIZE_TIMESTAMPS_SH}" "${BUILD_DIR}"
    cmake --build "${BUILD_DIR}" --target "${BUILD_TARGETS[@]}"
  fi
fi

verify_benchmark_core_runtime

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
RUN_EXIT_CODE=0
if [[ ! -f "${PERF_COMPARISON_SCRIPT}" ]]; then
  echo "Error: comparison script not found: ${PERF_COMPARISON_SCRIPT}" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1 && ! command -v python >/dev/null 2>&1; then
  echo "Error: python3 or python not found in PATH." >&2
  exit 1
fi

if [[ -z "${PYTHON_BIN:-}" ]]; then
  if command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN=(python3)
  else
    PYTHON_BIN=(python)
  fi
fi

RUN_CMD=(
  "${PYTHON_BIN[@]}"
  "${PERF_COMPARISON_SCRIPT}"
  "${PATTERN_CSV}"
  "${RUN_BASE_ARGS[@]}"
  "${SCRIPT_ARGS[@]}"
  --build-dir "${BUILD_DIR}"
)

if [[ -n "${OUTPUT_FILE}" ]]; then
  mkdir -p "$(dirname "${OUTPUT_FILE}")"
  if PERF_ALLOW_MULTI=1 \
    PERF_SUPPRESS_TOTAL_TIME=1 \
    env "${RUN_ENV[@]}" \
    "${RUN_CMD[@]}" | tee "${OUTPUT_FILE}"; then
    :
  else
    RUN_EXIT_CODE="${PIPESTATUS[0]}"
    FAILED_PATTERNS+=("${PATTERN_CSV}")
  fi
else
  if PERF_ALLOW_MULTI=1 \
    PERF_SUPPRESS_TOTAL_TIME=1 \
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
