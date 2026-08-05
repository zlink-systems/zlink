#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_DIR="$(cd "${PROJECT_DIR}/../.." && pwd)"
CORE_BUILD_DIR="${REPO_DIR}/core/build"
CORE_LIB_DIR="${CORE_BUILD_DIR}/lib"
CORE_LIB="${CORE_LIB_DIR}/libzlink.so"
RUST_RUNTIME_RESOLVER="${REPO_DIR}/scripts/local-package/rust/resolve-candidate-runtime.sh"
PERF_REPORT_PY="${REPO_DIR}/bindings/python/perf/perf_report.py"
START_SECONDS="$(date +%s)"
TOTAL_TIME_ENABLED=0

print_total_time() {
    local status="${1:-0}"
    if [[ "${TOTAL_TIME_ENABLED}" -ne 1 ]]; then
        return
    fi
    local elapsed
    elapsed=$(($(date +%s) - START_SECONDS))
    echo "Total benchmark time: ${elapsed}s (${elapsed}s, exit=${status})"
}
STREAM_BUILD_DIR="${SCRIPT_DIR}/build/stream-client"
STREAM_CLIENT=""
REUSE_BUILD=0
CLEAN_BUILD=0

source "$HOME/.cargo/env" 2>/dev/null || true
export RUSTFLAGS="${RUSTFLAGS:+${RUSTFLAGS} }-Awarnings"

PATTERN="ALL"
DURATION="${PERF_MULTI_DURATION_SECONDS:-5}"
MSG_SIZES="${PERF_MSG_SIZES:-64,256,1024,4096,65536,131072}"
TRANSPORTS="${PERF_TRANSPORTS:-tcp,tls,ws,wss}"
RUNS="1"
CLIENTS="${PERF_MULTI_CLIENTS:-${PERF_CLIENTS:-${PERF_MULTI_DEFAULT_CLIENTS:-${PERF_DEFAULT_CLIENTS:-100}}}}"
RUN_COOLDOWN_MS="${PERF_MULTI_RUN_COOLDOWN_MS:-${PERF_RUN_COOLDOWN_MS:-3000}}"
RESULTS_ROOT="${PERF_RESULTS_DIR:-${SCRIPT_DIR}/results}"
RESULTS_TAG="${PERF_RESULTS_TAG:-}"
OUTPUT_FILE=""
SMOKE=0
PIN_CPU=0
BUILD_DIR=""
RUST_PACKAGE_EVIDENCE="${ZLINK_RUST_PACKAGE_EVIDENCE:-}"
EXPLICIT_MSG_SIZES=0
COMMON_IO_THREADS="${PERF_IO_THREADS:-}"
SERVER_IO_THREADS=""
CLIENT_IO_THREADS=""
HWM=""
SEND_HWM=""
RECV_HWM=""
SNDBUF=""
RCVBUF=""
SNDTIMEO_MS="${PERF_MULTI_SNDTIMEO_MS:-200}"
RCVTIMEO_MS="${PERF_MULTI_RCVTIMEO_MS:-200}"
AUTO_HWM_PROFILE="${PERF_CTX_AUTO_HWM_PROFILE:-}"
CONNECT_CONCURRENCY=""
TRANSPORT_TRANSITION_MS="${PERF_MULTI_TRANSPORT_TRANSITION_MS:-${PERF_TRANSPORT_TRANSITION_MS:-3000}}"
PATTERN_TRANSITION_MS="${PERF_MULTI_PATTERN_TRANSITION_MS:-${PERF_PATTERN_TRANSITION_MS:-3000}}"
SERVER_READY_TIMEOUT_MS="${PERF_MULTI_SERVER_READY_TIMEOUT_MS:-${PERF_SERVER_READY_TIMEOUT_MS:-10000}}"
SERVER_SHUTDOWN_TIMEOUT_MS="${PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS:-${PERF_SERVER_SHUTDOWN_TIMEOUT_MS:-5000}}"
CONNECT_READY_TIMEOUT_MS="${PERF_MULTI_CONNECT_READY_TIMEOUT_MS:-${PERF_CONNECT_READY_TIMEOUT_MS:-1000}}"
MONITOR_HWM="${PERF_MULTI_MONITOR_HWM:-1000}"
SERVER_BIND_PORT="0"
ENV_PERF_IO_THREADS="${PERF_IO_THREADS:-}"
ENV_MULTI_SERVER_IO_THREADS="${PERF_MULTI_SERVER_IO_THREADS:-}"
ENV_MULTI_CLIENT_IO_THREADS="${PERF_MULTI_CLIENT_IO_THREADS:-}"
ENV_MULTI_STREAM_SERVER_IO_THREADS="${PERF_MULTI_STREAM_SERVER_IO_THREADS:-}"
ENV_MULTI_STREAM_CLIENT_IO_THREADS="${PERF_MULTI_STREAM_CLIENT_IO_THREADS:-}"
ENV_MULTI_HWM="${PERF_MULTI_HWM:-}"
ENV_MULTI_SNDHWM="${PERF_MULTI_SNDHWM:-}"
ENV_MULTI_RCVHWM="${PERF_MULTI_RCVHWM:-}"
ENV_MULTI_SNDBUF="${PERF_MULTI_SNDBUF:-}"
ENV_MULTI_RCVBUF="${PERF_MULTI_RCVBUF:-}"
EXPLICIT_CLIENTS=0
[[ -n "${PERF_MSG_SIZES+x}" ]] && EXPLICIT_MSG_SIZES=1
[[ -n "${PERF_MULTI_CLIENTS+x}" || -n "${PERF_CLIENTS+x}" ]] && EXPLICIT_CLIENTS=1

is_uint() {
    local value="${1:-}"
    [[ "${value}" =~ ^[0-9]+$ ]]
}

print_help() {
    cat <<'EOF'
Usage: bindings/rust/perf/run_benchmarks_multi.sh [options]

Options:
  -h, --help
  --pattern NAME
  --duration N
  --msg-sizes LIST
  --transports LIST
  --runs N
  --clients N
  --smoke
  --build-dir PATH
  --rust-package-evidence FILE
                             Rust candidate package evidence for completion-gate runs.
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
  --results-dir PATH
  --results-tag NAME
  --output PATH

Notes:
  - MULTI_STREAM uses the shared perf_stream_client required by policy.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            print_help
            exit 0
            ;;
        --pattern)     PATTERN="$2";     shift 2 ;;
        --duration)    DURATION="$2";    shift 2 ;;
        --msg-sizes)   MSG_SIZES="$2"; EXPLICIT_MSG_SIZES=1; shift 2 ;;
        --transports)  TRANSPORTS="$2";  shift 2 ;;
        --runs)        RUNS="$2";        shift 2 ;;
        --clients)     CLIENTS="$2"; EXPLICIT_CLIENTS=1; shift 2 ;;
        --smoke)       SMOKE=1; shift ;;
        --build-dir)   BUILD_DIR="$2";   shift 2 ;;
        --rust-package-evidence) RUST_PACKAGE_EVIDENCE="$2"; shift 2 ;;
        --io-threads) COMMON_IO_THREADS="$2"; shift 2 ;;
        --server-io-threads) SERVER_IO_THREADS="$2"; shift 2 ;;
        --client-io-threads) CLIENT_IO_THREADS="$2"; shift 2 ;;
        --hwm) HWM="$2"; shift 2 ;;
        --send-hwm) SEND_HWM="$2"; shift 2 ;;
        --recv-hwm) RECV_HWM="$2"; shift 2 ;;
        --buf) SNDBUF="$2"; RCVBUF="$2"; shift 2 ;;
        --sndbuf) SNDBUF="$2"; shift 2 ;;
        --rcvbuf) RCVBUF="$2"; shift 2 ;;
        --sndtimeo|--send-timeout-ms) SNDTIMEO_MS="$2"; shift 2 ;;
        --rcvtimeo|--recv-timeout-ms) RCVTIMEO_MS="$2"; shift 2 ;;
        --connect-concurrency) CONNECT_CONCURRENCY="$2"; shift 2 ;;
        --transport-transition-ms) TRANSPORT_TRANSITION_MS="$2"; shift 2 ;;
        --pattern-transition-ms) PATTERN_TRANSITION_MS="$2"; shift 2 ;;
        --server-ready-timeout-ms) SERVER_READY_TIMEOUT_MS="$2"; shift 2 ;;
        --connect-ready-timeout-ms) CONNECT_READY_TIMEOUT_MS="$2"; shift 2 ;;
        --monitor-hwm) MONITOR_HWM="$2"; shift 2 ;;
        --server-shutdown-timeout-ms) SERVER_SHUTDOWN_TIMEOUT_MS="$2"; shift 2 ;;
        --server-bind-port) SERVER_BIND_PORT="$2"; shift 2 ;;
        --auto-hwm-profile) AUTO_HWM_PROFILE="$2"; shift 2 ;;
        --results-dir) RESULTS_ROOT="$2"; shift 2 ;;
        --results-tag) RESULTS_TAG="$2"; shift 2 ;;
        --output)      OUTPUT_FILE="$2"; shift 2 ;;
        --reuse-build) REUSE_BUILD=1; shift ;;
        --clean-build) CLEAN_BUILD=1; shift ;;
        --pin-cpu) PIN_CPU=1; shift ;;
        *)             echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ "${REUSE_BUILD}" -eq 1 && "${CLEAN_BUILD}" -eq 1 ]]; then
    echo "--reuse-build and --clean-build are mutually exclusive" >&2
    exit 1
fi

case "$(uname -s)" in
    Linux*)  PLATFORM="linux" ;;
    Darwin*) PLATFORM="macos" ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM="windows" ;;
    *)       PLATFORM="linux" ;;
esac
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
TAG_SUFFIX=""; [[ -n "${RESULTS_TAG}" ]] && TAG_SUFFIX="_${RESULTS_TAG}"
REPORT_DIR="${RESULTS_ROOT}/multi/report"
RESULTS_FILE="${REPORT_DIR}/perf_rust_multi_${PLATFORM}_${TIMESTAMP}${TAG_SUFFIX}.txt"
if [[ "${SMOKE}" -eq 0 ]]; then
    mkdir -p "${REPORT_DIR}"
fi

prune_reports() {
    local report_dir="$1"
    local max_files="${PERF_RESULTS_MAX_FILES:-100}"
    if [[ ! "${max_files}" =~ ^[0-9]+$ ]] || [[ "${max_files}" -lt 1 ]]; then
        echo "PERF_RESULTS_MAX_FILES must be >= 1" >&2
        exit 1
    fi

    local count
    count="$(find "${report_dir}" -maxdepth 1 -type f -name 'perf_*.txt' | wc -l | tr -d ' ')"
    if [[ -z "${count}" || "${count}" -le "${max_files}" ]]; then
        return
    fi
    find "${report_dir}" -maxdepth 1 -type f -name 'perf_*.txt' -printf '%f\n' \
        | sort \
        | head -n "$((count - max_files))" \
        | while read -r old_file; do
            rm -f "${report_dir}/${old_file}"
        done
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
        printf '\n'
        return
    fi
    if [[ ! -r /proc/meminfo ]]; then
        printf '\n'
        return
    fi
    awk '/^MemAvailable:/ { print $2; found=1; exit } END { if (!found) print "" }' /proc/meminfo 2>/dev/null || true
}

prepare_core_runtime() {
    if [[ -n "${RUST_PACKAGE_EVIDENCE}" ]]; then
        [[ -f "${RUST_RUNTIME_RESOLVER}" ]] || {
            echo "Rust candidate runtime resolver not found: ${RUST_RUNTIME_RESOLVER}" >&2
            exit 1
        }
        # Candidate package evidence owns the runtime path, hash and Core identity.
        source "${RUST_RUNTIME_RESOLVER}"
        resolve_rust_package_runtime "${RUST_PACKAGE_EVIDENCE}" "linux-x86_64" \
            "$(git -C "${REPO_DIR}" rev-parse HEAD)"
        echo "Rust candidate package evidence: ${RUST_PACKAGE_EVIDENCE}"
        echo "Rust candidate manifest sha256: ${RUST_CANDIDATE_MANIFEST_SHA256}"
        echo "Rust candidate aggregate sha256: ${RUST_CANDIDATE_AGGREGATE_SHA256}"
        echo "Rust perf runtime: ${RUST_CANDIDATE_RUNTIME}"
        echo "Rust perf runtime sha256: ${RUST_CANDIDATE_RUNTIME_SHA256}"
        export ZLINK_RUST_NATIVE_DIR="${RUST_CANDIDATE_NATIVE_DIR}"
        export LD_LIBRARY_PATH="${RUST_CANDIDATE_NATIVE_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
        return
    fi

    # Compare against the resolved runtime, not the libzlink.so symlink: the
    # symlink keeps its original creation mtime when only the versioned .so is
    # rebuilt, which made this guard false-positive "stale" after every core
    # rebuild (the Go runner already compares the versioned file directly).
    local native_dir="${ZLINK_RUST_NATIVE_DIR:-${CORE_LIB_DIR}}"
    local runtime="${native_dir}/libzlink.so"
    local package_version
    package_version="$(sed -n 's/^version = "\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)"/\1/p' "${PROJECT_DIR}/Cargo.toml" | head -n1)"
    [[ -f "${runtime}" ]] || runtime="${native_dir}/libzlink.so.${package_version}"
    if [[ ! -f "${runtime}" ]]; then
        echo "Rust perf runtime not found: ${native_dir}" >&2
        echo "Build core/build or pass --rust-package-evidence." >&2
        exit 1
    fi
    local resolved_lib
    resolved_lib="$(readlink -f "${runtime}" 2>/dev/null || echo "${runtime}")"
    local newer_source
    newer_source="$(
        find "${REPO_DIR}/core/src" "${REPO_DIR}/core/include" \
            -type f -newer "${resolved_lib}" -print -quit 2>/dev/null || true
    )"
    if [[ -n "${newer_source}" ]]; then
        echo "Error: stale core runtime detected for bindings/rust/perf." >&2
        echo "  runtime: ${resolved_lib}" >&2
        echo "  newer source: ${newer_source}" >&2
        echo "Rebuild core/build before running run_benchmarks_multi.sh." >&2
        exit 1
    fi
    echo "Rust perf development runtime: ${resolved_lib}"
    echo "Rust perf runtime sha256: $(sha256sum "${resolved_lib}" | awk '{print $1}')"
    export ZLINK_RUST_NATIVE_DIR="${native_dir}"
    export LD_LIBRARY_PATH="${native_dir}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
}

resolve_memory_max_clients() {
    local available_kb
    available_kb="$(memory_available_kb)"
    if ! is_uint "${available_kb}"; then
        printf '\n'
        return
    fi

    local budget_pct
    local base_mb
    local per_client_kb
    budget_pct="${PERF_MULTI_MEMORY_BUDGET_PCT:-${PERF_MEMORY_BUDGET_PCT:-70}}"
    base_mb="${PERF_MULTI_MEMORY_BASE_MB:-${PERF_MEMORY_BASE_MB:-512}}"
    per_client_kb="${PERF_MULTI_MEMORY_PER_CLIENT_KB:-${PERF_MEMORY_PER_CLIENT_KB:-1024}}"
    if ! is_uint "${budget_pct}" || (( budget_pct < 1 || budget_pct > 95 )); then
        printf '\n'
        return
    fi
    if ! is_uint "${base_mb}" || ! is_uint "${per_client_kb}" || (( per_client_kb < 1 )); then
        printf '\n'
        return
    fi

    local usable_kb=$(( available_kb * budget_pct / 100 ))
    local base_kb=$(( base_mb * 1024 ))
    if (( usable_kb <= base_kb )); then
        printf '%s\n' "1"
        return
    fi

    local max_clients=$(( (usable_kb - base_kb) / per_client_kb ))
    if (( max_clients < 1 )); then
        max_clients=1
    fi
    printf '%s\n' "${max_clients}"
}

cap_default_clients_for_memory() {
    local clients="${1:-}"
    if [[ "${EXPLICIT_CLIENTS}" -eq 1 ]]; then
        printf '%s\n' "${clients}"
        return
    fi

    local max_clients
    max_clients="$(resolve_memory_max_clients)"
    if ! is_uint "${clients}" || ! is_uint "${max_clients}"; then
        printf '%s\n' "${clients}"
        return
    fi
    if (( clients > max_clients )); then
        printf '%s\n' "${max_clients}"
        return
    fi
    printf '%s\n' "${clients}"
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
    local budget_pct
    local base_mb
    local per_client_kb
    available_kb="$(memory_available_kb)"
    budget_pct="${PERF_MULTI_MEMORY_BUDGET_PCT:-${PERF_MEMORY_BUDGET_PCT:-70}}"
    base_mb="${PERF_MULTI_MEMORY_BASE_MB:-${PERF_MEMORY_BASE_MB:-512}}"
    per_client_kb="${PERF_MULTI_MEMORY_PER_CLIENT_KB:-${PERF_MEMORY_PER_CLIENT_KB:-1024}}"
    MEMORY_SKIP_REASON="clients=${clients},max_clients=${max_clients},mem_available_kb=${available_kb},budget_pct=${budget_pct},base_mb=${base_mb},per_client_kb=${per_client_kb}"
    return 1
}

normalize_patterns() {
    local raw="${1:-ALL}"
    raw="${raw^^}"
    if [[ "${raw}" == "ALL" ]]; then
        printf '%s\n' "MULTI_DEALER_DEALER,MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER,MULTI_PUBSUB,MULTI_STREAM"
        return
    fi

    local -a items=()
    local token value
    IFS=',' read -ra raw_items <<< "${raw}"
    for token in "${raw_items[@]}"; do
        value="${token//[[:space:]]/}"
        [[ -n "${value}" ]] || continue
        value="${value#MULTI_}"
        if [[ "${value}" == "STREAMS" ]]; then
            value="STREAM"
        fi
        case "${value}" in
            DEALER_DEALER|DEALER_ROUTER|ROUTER_ROUTER|PUBSUB|STREAM)
                items+=("MULTI_${value}")
                ;;
            *)
                echo "unsupported multi pattern: ${value}" >&2
                return 1
                ;;
        esac
    done
    if [[ "${#items[@]}" -eq 0 ]]; then
        echo "no valid multi pattern specified" >&2
        return 1
    fi
    local IFS=,
    printf '%s\n' "${items[*]}"
}

default_msg_sizes_for_pattern() {
    local pattern="$1"
    if [[ "${EXPLICIT_MSG_SIZES}" -eq 1 ]]; then
        printf '%s' "${MSG_SIZES}"
        return
    fi
    case "${pattern}" in
        MULTI_STREAM)
            printf '%s' "${PERF_MULTI_STREAM_MSG_SIZES:-${PERF_STREAM_MSG_SIZES:-64,256,1024,65536}}"
            ;;
        *)
            printf '%s' "${MSG_SIZES}"
            ;;
    esac
}

default_io_threads_for_pattern() {
    local pattern="$1"
    printf '%s' "${PERF_MULTI_DEFAULT_IO_THREADS:-${PERF_DEFAULT_IO_THREADS:-4}}"
}

ensure_shared_stream_client() {
    if [[ "${REUSE_BUILD}" -eq 1 ]]; then
        if [[ -z "${STREAM_CLIENT}" ]]; then
            STREAM_CLIENT="${STREAM_BUILD_DIR}/perf_stream_client"
        fi
        if [[ -z "${STREAM_CLIENT}" || ! -x "${STREAM_CLIENT}" ]]; then
            echo "shared stream client not found for --reuse-build: ${STREAM_CLIENT}" >&2
            exit 1
        fi
        return
    fi

    if [[ "${CLEAN_BUILD}" -eq 1 ]]; then
        rm -rf "${STREAM_BUILD_DIR}"
    fi
    mkdir -p "${STREAM_BUILD_DIR}"
    local cxx_bin
    cxx_bin="${CXX:-g++}"
    "${cxx_bin}" \
        -O2 \
        -std=c++17 \
        -Wall \
        -Wextra \
        "${REPO_DIR}/bindings/c/perf/common/streamclient/perf_stream_client.cpp" \
        -o "${STREAM_BUILD_DIR}/perf_stream_client" \
        -pthread \
        -lssl \
        -lcrypto >/dev/null
    STREAM_CLIENT="${STREAM_BUILD_DIR}/perf_stream_client"
    if [[ -z "${STREAM_CLIENT}" || ! -x "${STREAM_CLIENT}" ]]; then
        echo "shared stream client binary not found after build: ${STREAM_BUILD_DIR}" >&2
        exit 1
    fi
}

if [[ -n "${BUILD_DIR}" ]]; then
    TARGET_DIR="${BUILD_DIR}/rust-multi"
    STREAM_BUILD_DIR="${BUILD_DIR}/stream-client"
else
    TARGET_DIR="${CARGO_TARGET_DIR:-/tmp/zlink-rust-target/perf-multi}"
    STREAM_BUILD_DIR="${TMPDIR:-/tmp}/zlink-rust-perf-stream-client"
fi
BIN_DIR="${TARGET_DIR}/release"

export PERF_MULTI_DURATION_SECONDS="${DURATION}"
prepare_core_runtime
TOTAL_TIME_ENABLED=1
export PERF_MULTI_SERVER_BIND_PORT="${SERVER_BIND_PORT}"
export PERF_MULTI_MONITOR_HWM="${MONITOR_HWM}"
[[ -n "${CONNECT_CONCURRENCY}" ]] && export PERF_MULTI_CONNECT_CONCURRENCY="${CONNECT_CONCURRENCY}"
if [[ -n "${COMMON_IO_THREADS}" ]]; then
    export PERF_IO_THREADS="${COMMON_IO_THREADS}"
elif [[ -n "${ENV_PERF_IO_THREADS}" ]]; then
    export PERF_IO_THREADS="${ENV_PERF_IO_THREADS}"
else
    unset PERF_IO_THREADS || true
fi
export PERF_MULTI_SNDTIMEO_MS="${SNDTIMEO_MS}"
export PERF_MULTI_RCVTIMEO_MS="${RCVTIMEO_MS}"
export PERF_MULTI_CONNECT_READY_TIMEOUT_MS="${CONNECT_READY_TIMEOUT_MS}"
export PERF_MULTI_SERVER_BIND_PORT="${SERVER_BIND_PORT}"
[[ -n "${SNDBUF:-${ENV_MULTI_SNDBUF}}" ]] && export PERF_MULTI_SNDBUF="${SNDBUF:-${ENV_MULTI_SNDBUF}}"
[[ -n "${RCVBUF:-${ENV_MULTI_RCVBUF}}" ]] && export PERF_MULTI_RCVBUF="${RCVBUF:-${ENV_MULTI_RCVBUF}}"
[[ -n "${AUTO_HWM_PROFILE}" ]] && export PERF_CTX_AUTO_HWM_PROFILE="${AUTO_HWM_PROFILE}"

RUN_PREFIX=()
if [[ "${PIN_CPU}" -eq 1 ]]; then
    if [[ "$(uname -s)" != "Linux" ]]; then
        echo "--pin-cpu is only supported on Linux in this runner" >&2
        exit 1
    fi
    if ! command -v taskset >/dev/null 2>&1; then
        echo "--pin-cpu requires taskset" >&2
        exit 1
    fi
    RUN_PREFIX=("taskset" "-c" "0")
fi

if [[ "${REUSE_BUILD}" -eq 0 ]]; then
    if [[ "${CLEAN_BUILD}" -eq 1 ]]; then
        (cd "${SCRIPT_DIR}/multi" && CARGO_TARGET_DIR="${TARGET_DIR}" cargo clean --quiet)
    fi
    (cd "${SCRIPT_DIR}/multi" && CARGO_TARGET_DIR="${TARGET_DIR}" cargo build --release --quiet)
elif [[ ! -x "${BIN_DIR}/perf_multi_dealer_dealer_server" ]]; then
    echo "existing multi perf binaries not found for --reuse-build: ${BIN_DIR}" >&2
    exit 1
fi

PATTERN="$(normalize_patterns "${PATTERN}")"
IFS=',' read -ra PATTERNS <<< "${PATTERN}"
if printf '%s\n' "${PATTERNS[@]}" | grep -qx 'MULTI_STREAM'; then
    ensure_shared_stream_client
fi

IFS=',' read -ra TRANSPORT_LIST <<< "${TRANSPORTS}"
ONE_WAY_CLIENT_READY_TIMEOUT=10
SERVER_READY_TIMEOUT_SECONDS=$(( (SERVER_READY_TIMEOUT_MS + 999) / 1000 ))
SERVER_SHUTDOWN_TIMEOUT_SECONDS=$(( (SERVER_SHUTDOWN_TIMEOUT_MS + 999) / 1000 ))

TMP_METRICS="$(mktemp)"
TMP_CASES="$(mktemp)"
cleanup() {
    local status=$?
    rm -f "${TMP_METRICS}" "${TMP_CASES}"
    print_total_time "${status}"
}
trap cleanup EXIT
METRICS_REGEX='^(throughput|bandwidth|latency|latency_p95|latency_p99)$'
wait_for_pid() {
    local pid="$1"
    local timeout_seconds="$2"
    local deadline=$((SECONDS + timeout_seconds))
    while kill -0 "${pid}" 2>/dev/null; do
        if ps -o stat= -p "${pid}" 2>/dev/null | grep -q '^[[:space:]]*Z'; then
            return 0
        fi
        if (( SECONDS >= deadline )); then
            return 1
        fi
        sleep 0.1
    done
    return 0
}

shutdown_server() {
    local pid="$1"
    local control_fd="$2"

    if kill -0 "${pid}" 2>/dev/null; then
        printf 'STOP\n' >&"${control_fd}" || true
        exec {control_fd}>&- || true
        if wait_for_pid "${pid}" "${SERVER_SHUTDOWN_TIMEOUT_SECONDS}"; then
            return
        fi
        kill "${pid}" 2>/dev/null || true
        if wait_for_pid "${pid}" 2; then
            return
        fi
        kill -9 "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
}

wait_for_file_line() {
    local file_path="$1"
    local timeout_seconds="$2"
    local deadline=$((SECONDS + timeout_seconds))
    while (( SECONDS < deadline )); do
        if [[ -s "${file_path}" ]]; then
            head -1 "${file_path}" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done
    return 1
}

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

read_first_case_signal() {
    local file_path="$1"
    awk '/^(READY,|UNSUPPORTED,)/ { print; exit }' "${file_path}" 2>/dev/null || true
}

wait_for_ready_or_unsupported() {
    local file_path="$1"
    local timeout_seconds="$2"
    local deadline=$((SECONDS + timeout_seconds))
    while (( SECONDS < deadline )); do
        if [[ -f "${file_path}" ]]; then
            local line
            line="$(read_first_case_signal "${file_path}")"
            if [[ -n "${line}" ]]; then
                printf '%s\n' "${line}"
                return 0
            fi
        fi
        sleep 0.1
    done
    return 1
}

resolve_client_timeout_seconds() {
    local pattern="$1"
    local transport="$2"
    local size="$3"
    local duration="$4"
    local override="${PERF_MULTI_TIMEOUT_SECONDS:-${PERF_TIMEOUT_SECONDS:-}}"
    local timeout_seconds=0

    if [[ -n "${override}" ]]; then
        printf '%s\n' "${override}"
        return
    fi

    if [[ "${pattern}" == "MULTI_STREAM" ]]; then
        timeout_seconds=$(( duration * 3 + 20 ))
        if (( timeout_seconds < 45 )); then
            timeout_seconds=45
        fi
        printf '%s\n' "${timeout_seconds}"
        return
    fi

    if { [[ "${pattern}" == "MULTI_PUBSUB" ]] && (( size >= 65536 )); } \
        || { [[ "${transport}" == "tls" || "${transport}" == "wss" ]] && (( size >= 131072 )); }; then
        timeout_seconds=$(( duration * 6 + 30 ))
        if (( timeout_seconds < 90 )); then
            timeout_seconds=90
        fi
        printf '%s\n' "${timeout_seconds}"
        return
    fi

    timeout_seconds=$(( duration * 3 + 20 ))
    if (( timeout_seconds < 45 )); then
        timeout_seconds=45
    fi
    printf '%s\n' "${timeout_seconds}"
}

stop_early=0
for run in $(seq 1 "${RUNS}"); do
    if [[ "${stop_early}" -eq 1 ]]; then
        break
    fi
    [[ "${RUNS}" -gt 1 ]] && echo "--- Run ${run}/${RUNS} ---"
    for pat_index in "${!PATTERNS[@]}"; do
        if [[ "${stop_early}" -eq 1 ]]; then
            break
        fi
        pat="${PATTERNS[pat_index]}"
        IFS=',' read -ra SIZE_LIST <<< "$(default_msg_sizes_for_pattern "${pat}")"
        PATTERN_CLIENTS="${CLIENTS}"
        if [[ "${EXPLICIT_CLIENTS}" != "1" ]]; then
            if [[ "${pat}" == "MULTI_STREAM" ]]; then
                PATTERN_CLIENTS="${PERF_MULTI_DEFAULT_STREAM_CLIENTS:-${PERF_STREAM_DEFAULT_CLIENTS:-10000}}"
            else
                PATTERN_CLIENTS="${PERF_MULTI_DEFAULT_CLIENTS:-${PERF_DEFAULT_CLIENTS:-100}}"
            fi
        fi
        PATTERN_CLIENTS="$(cap_default_clients_for_memory "${PATTERN_CLIENTS}")"
        SERVER_BIN=""
        CLIENT_BIN=""
        case "${pat}" in
            MULTI_DEALER_DEALER)
                SERVER_BIN="${BIN_DIR}/perf_multi_dealer_dealer_server"
                CLIENT_BIN="${BIN_DIR}/perf_multi_dealer_dealer_client" ;;
            MULTI_DEALER_ROUTER)
                SERVER_BIN="${BIN_DIR}/perf_multi_dealer_router_server"
                CLIENT_BIN="${BIN_DIR}/perf_multi_dealer_router_client" ;;
            MULTI_PUBSUB)
                SERVER_BIN="${BIN_DIR}/perf_multi_pubsub_server"
                CLIENT_BIN="${BIN_DIR}/perf_multi_pubsub_client" ;;
            MULTI_ROUTER_ROUTER)
                SERVER_BIN="${BIN_DIR}/perf_multi_router_router_server"
                CLIENT_BIN="${BIN_DIR}/perf_multi_router_router_client" ;;
            MULTI_STREAM)
                SERVER_BIN="${BIN_DIR}/perf_multi_stream_server"
                CLIENT_BIN="" ;;
            *)
                echo "UNSUPPORTED,rust,${pat},unknown"
                continue ;;
        esac

        for transport_index in "${!TRANSPORT_LIST[@]}"; do
            if [[ "${stop_early}" -eq 1 ]]; then
                break
            fi
            transport="${TRANSPORT_LIST[transport_index]}"
            for size in "${SIZE_LIST[@]}"; do
                if [[ "${stop_early}" -eq 1 ]]; then
                    break
                fi
                CASE_CLIENTS="${PATTERN_CLIENTS}"
                if [[ "${pat}" == "MULTI_STREAM" && "${transport}" != "tcp" ]]; then
                    STREAM_NON_TCP_CLIENTS_MAX="${PERF_STREAM_NON_TCP_CLIENTS_MAX:-${PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX:-10000}}"
                    if [[ "${CASE_CLIENTS}" =~ ^[0-9]+$ && "${STREAM_NON_TCP_CLIENTS_MAX}" =~ ^[0-9]+$ \
                          && "${CASE_CLIENTS}" -gt "${STREAM_NON_TCP_CLIENTS_MAX}" ]]; then
                        CASE_CLIENTS="${STREAM_NON_TCP_CLIENTS_MAX}"
                    fi
                fi
                if ! ensure_nofile_limit "${CASE_CLIENTS}"; then
                    printf '%s,%s,%s,%s,%s\n' "${pat}" "${transport}" "${size}" "skip" "nofile_guard:${NOFILE_SKIP_REASON}" >> "${TMP_CASES}"
                    continue
                fi
                if ! ensure_memory_budget "${CASE_CLIENTS}"; then
                    printf '%s,%s,%s,%s,%s\n' "${pat}" "${transport}" "${size}" "skip" "memory_guard:${MEMORY_SKIP_REASON}" >> "${TMP_CASES}"
                    continue
                fi
                CLIENT_TIMEOUT_SECONDS="$(resolve_client_timeout_seconds "${pat}" "${transport}" "${size}" "${DURATION}")"
                export PERF_MULTI_CLIENTS="${CASE_CLIENTS}"
                pattern_default_io_threads="$(default_io_threads_for_pattern "${pat}")"
                if [[ "${pat}" == "MULTI_STREAM" ]]; then
                    pattern_server_io_threads="${ENV_MULTI_STREAM_SERVER_IO_THREADS:-${ENV_MULTI_SERVER_IO_THREADS:-${pattern_default_io_threads}}}"
                    pattern_client_io_threads="${ENV_MULTI_STREAM_CLIENT_IO_THREADS:-${ENV_MULTI_CLIENT_IO_THREADS:-${pattern_default_io_threads}}}"
                else
                    pattern_server_io_threads="${ENV_MULTI_SERVER_IO_THREADS:-${pattern_default_io_threads}}"
                    pattern_client_io_threads="${ENV_MULTI_CLIENT_IO_THREADS:-${pattern_default_io_threads}}"
                fi
                export PERF_MULTI_SERVER_IO_THREADS="${SERVER_IO_THREADS:-${COMMON_IO_THREADS:-${ENV_PERF_IO_THREADS:-${pattern_server_io_threads}}}}"
                export PERF_MULTI_CLIENT_IO_THREADS="${CLIENT_IO_THREADS:-${COMMON_IO_THREADS:-${ENV_PERF_IO_THREADS:-${pattern_client_io_threads}}}}"
                export PERF_MULTI_MSG_UNIT_BYTES="${size}"
                unset PERF_MULTI_HWM PERF_MULTI_SNDHWM PERF_MULTI_RCVHWM
                if [[ -n "${HWM}" || -n "${ENV_MULTI_HWM}" ]]; then
                    export PERF_MULTI_HWM="${HWM:-${ENV_MULTI_HWM}}"
                fi
                if [[ -n "${SEND_HWM}" || -n "${ENV_MULTI_SNDHWM}" ]]; then
                    export PERF_MULTI_SNDHWM="${SEND_HWM:-${ENV_MULTI_SNDHWM}}"
                fi
                if [[ -n "${RECV_HWM}" || -n "${ENV_MULTI_RCVHWM}" ]]; then
                    export PERF_MULTI_RCVHWM="${RECV_HWM:-${ENV_MULTI_RCVHWM}}"
                fi
                if [[ "${pat}" == "MULTI_STREAM" ]]; then
                    export PERF_RECV_MODE="recv"
                    export PERF_DURATION_SECONDS="${DURATION}"
                fi
                case_status="success"
                case_reason=""
                CLIENT_OUTPUT=""
                SRV_OUT=$(mktemp)
                SERVER_FIFO="$(mktemp -u)"
                mkfifo "${SERVER_FIFO}"
                "${RUN_PREFIX[@]}" "${SERVER_BIN}" "${transport}" "${size}" < "${SERVER_FIFO}" > "${SRV_OUT}" 2>&1 &
                SERVER_PID=$!
                exec {SERVER_CONTROL_FD}> "${SERVER_FIFO}"
                rm -f "${SERVER_FIFO}"

                # Wait for READY
                ENDPOINT=""
                READY_LINE="$(wait_for_ready_or_unsupported "${SRV_OUT}" "${SERVER_READY_TIMEOUT_SECONDS}" || true)"
                if [[ "${READY_LINE}" == UNSUPPORTED,* ]]; then
                    case_status="unsupported"
                    case_reason="${READY_LINE//,/;}"
                    shutdown_server "${SERVER_PID}" "${SERVER_CONTROL_FD}"
                    rm -f "${SRV_OUT}"
                    printf '%s,%s,%s,%s,%s\n' "${pat}" "${transport}" "${size}" "${case_status}" "${case_reason}" >> "${TMP_CASES}"
                    continue
                fi
                if [[ "${READY_LINE}" == READY,* ]]; then
                    ENDPOINT="${READY_LINE#READY,}"
                    ENDPOINT="${ENDPOINT//0.0.0.0/127.0.0.1}"
                fi

                if [[ -z "${ENDPOINT}" ]]; then
                    case_status="fail"
                    case_reason="server_ready_timeout"
                    shutdown_server "${SERVER_PID}" "${SERVER_CONTROL_FD}"
                    rm -f "${SRV_OUT}"
                    printf '%s,%s,%s,%s,%s\n' "${pat}" "${transport}" "${size}" "${case_status}" "${case_reason}" >> "${TMP_CASES}"
                    if [[ "${PERF_FAIL_FAST:-0}" == "1" ]]; then
                        stop_early=1
                        break
                    fi
                    continue
                fi

                CONTROL_ENDPOINT=""

                if [[ "${pat}" == "MULTI_DEALER_DEALER" || "${pat}" == "MULTI_PUBSUB" ]]; then
                    CLIENT_OUT="$(mktemp)"
                    CLIENT_ERR="$(mktemp)"
                    CLIENT_FIFO="$(mktemp -u)"
                    mkfifo "${CLIENT_FIFO}"
                    "${RUN_PREFIX[@]}" "${CLIENT_BIN}" "${transport}" "${size}" "${ENDPOINT}" < "${CLIENT_FIFO}" > "${CLIENT_OUT}" 2> "${CLIENT_ERR}" &
                    CLIENT_PID=$!
                    exec {CLIENT_CONTROL_FD}> "${CLIENT_FIFO}"
                    rm -f "${CLIENT_FIFO}"

                    CLIENT_READY_LINE=""
                    if CLIENT_READY_LINE="$(wait_for_file_line "${CLIENT_OUT}" "${ONE_WAY_CLIENT_READY_TIMEOUT}")"; then
                        CLIENT_READY_LINE="${CLIENT_READY_LINE%%$'\n'*}"
                    fi
                    if [[ "${CLIENT_READY_LINE}" != "CLIENT_READY,${size}" ]]; then
                        case_status="fail"
                        case_reason="client_ready_timeout_or_invalid"
                    else
                        printf 'START,%s\n' "${size}" >&"${SERVER_CONTROL_FD}" || true
                        printf 'START,%s\n' "${size}" >&"${CLIENT_CONTROL_FD}" || true
                    fi

                    if [[ "${case_status}" == "success" ]]; then
                        if ! wait_for_pid "${CLIENT_PID}" "${CLIENT_TIMEOUT_SECONDS}"; then
                            case_status="fail"
                            case_reason="binary_exit_or_timeout"
                            kill "${CLIENT_PID}" 2>/dev/null || true
                            wait "${CLIENT_PID}" 2>/dev/null || true
                        elif ! wait "${CLIENT_PID}"; then
                            case_status="fail"
                            case_reason="binary_exit_or_timeout"
                        fi
                    else
                        kill "${CLIENT_PID}" 2>/dev/null || true
                        wait "${CLIENT_PID}" 2>/dev/null || true
                    fi

                    exec {CLIENT_CONTROL_FD}>&- || true
                    OUTPUT=""
                    if [[ -f "${CLIENT_OUT}" ]]; then
                        CLIENT_OUTPUT="$(cat "${CLIENT_OUT}")"
                    fi
                    if [[ -s "${CLIENT_ERR}" ]]; then
                        if [[ -n "${CLIENT_OUTPUT}" ]]; then
                            CLIENT_OUTPUT+=$'\n'
                        fi
                        CLIENT_OUTPUT+="$(cat "${CLIENT_ERR}")"
                    fi
                    rm -f "${CLIENT_OUT}" "${CLIENT_ERR}"
                elif [[ "${pat}" == "MULTI_STREAM" ]]; then
                    if ! CLIENT_OUTPUT="$(timeout "${CLIENT_TIMEOUT_SECONDS}s" "${RUN_PREFIX[@]}" "${STREAM_CLIENT}" \
                        --transport "${transport}" \
                        --pattern STREAM \
                        --sizes "${size}" \
                        --runs 1 \
                        --duration "${DURATION}" \
                        --ccu "${CASE_CLIENTS}" \
                        --io-threads "${PERF_MULTI_CLIENT_IO_THREADS}" \
                        --print-perf-result 1 \
                        --send-stop-token 0 \
                        --endpoint "${ENDPOINT}" 2>&1)"; then
                        case_status="fail"
                        case_reason="binary_exit_or_timeout"
                    fi
                else
                    if ! CLIENT_OUTPUT="$(timeout "${CLIENT_TIMEOUT_SECONDS}s" "${RUN_PREFIX[@]}" "${CLIENT_BIN}" "${transport}" "${size}" "${ENDPOINT}" 2>&1)"; then
                        case_status="fail"
                        case_reason="binary_exit_or_timeout"
                    fi
                fi

                shutdown_server "${SERVER_PID}" "${SERVER_CONTROL_FD}"
                SERVER_OUTPUT=""
                if [[ -f "${SRV_OUT}" ]]; then
                    SERVER_OUTPUT="$(cat "${SRV_OUT}")"
                fi
                rm -f "${SRV_OUT}"
                OUTPUT="${SERVER_OUTPUT}"
                if [[ -n "${CLIENT_OUTPUT}" ]]; then
                    if [[ -n "${OUTPUT}" ]]; then
                        OUTPUT+=$'\n'
                    fi
                    OUTPUT+="${CLIENT_OUTPUT}"
                fi
                if [[ "${case_status}" == "success" ]]; then
                    unsupported_line="$(printf '%s\n' "${OUTPUT}" | awk -F',' '/^UNSUPPORTED,/ {print; exit}')"
                    if [[ -n "${unsupported_line}" ]]; then
                        case_status="unsupported"
                        case_reason="${unsupported_line}"
                    elif printf '%s\n' "${OUTPUT}" | grep -qi 'protocol not supported'; then
                        case_status="unsupported"
                        case_reason="protocol_not_supported"
                    fi
                fi
                if [[ "${case_status}" == "success" ]]; then
                    skip_line="$(printf '%s\n' "${OUTPUT}" | awk -F',' '/^SKIP,/ {print; exit}')"
                    if [[ -n "${skip_line}" ]]; then
                        case_status="skip"
                        case_reason="${skip_line}"
                    fi
                fi
                case_reason="${case_reason//,/;}"
                while IFS= read -r line; do
                    [[ "${line}" == RESULT,* ]] || continue
                    IFS=',' read -r tag lib result_pattern result_transport result_size metric value <<< "${line}"
                    [[ "${metric}" =~ ${METRICS_REGEX} ]] || continue
                    printf '%s,%s,%s,%s,%s,%s\n' \
                        "${pat}" "${transport}" "${size}" "${run}" "${metric}" "${value}" >> "${TMP_METRICS}"
                done <<< "${OUTPUT}"
                REQUIRED_COUNT="$(printf '%s\n' "${OUTPUT}" | awk -F',' '/^RESULT,/ && ($6=="throughput" || $6=="bandwidth" || $6=="latency" || $6=="latency_p95" || $6=="latency_p99") {count++} END {print count+0}')"
                if [[ "${case_status}" == "success" && "${REQUIRED_COUNT}" -ne 5 ]]; then
                    case_status="fail"
                    case_reason="missing_required_result_lines run=${run}"
                fi
                printf '%s,%s,%s,%s,%s\n' "${pat}" "${transport}" "${size}" "${case_status}" "${case_reason}" >> "${TMP_CASES}"
                if [[ "${PERF_FAIL_FAST:-0}" == "1" && "${case_status}" == "fail" ]]; then
                    stop_early=1
                    break
                fi
            done
        if (( transport_index + 1 < ${#TRANSPORT_LIST[@]} )); then
            if [[ "${stop_early}" -eq 1 ]]; then
                break
            fi
            sleep "$(awk "BEGIN { printf \"%.3f\", ${TRANSPORT_TRANSITION_MS} / 1000 }")"
        fi
    done
    if [[ "${stop_early}" -ne 1 ]] && (( pat_index + 1 < ${#PATTERNS[@]} )); then
        sleep "$(awk "BEGIN { printf \"%.3f\", ${PATTERN_TRANSITION_MS} / 1000 }")"
    fi
done
    if [[ "${stop_early}" -ne 1 ]] && (( run < RUNS )); then
        sleep "$(awk "BEGIN { printf \"%.3f\", ${RUN_COOLDOWN_MS} / 1000 }")"
    fi
done
TOTAL_CASES="$(wc -l < "${TMP_CASES}" | tr -d ' ')"
NON_SKIP_CASES="$(awk -F',' '$4 != "skip" {count++} END {print count+0}' "${TMP_CASES}")"
if [[ "${SMOKE}" -eq 1 ]]; then
    SMOKE_NON_SUCCESS="$(awk -F',' '$4 != "success" { count++ } END { print count + 0 }' "${TMP_CASES}")"
    if [[ "${TOTAL_CASES}" -eq 0 || "${SMOKE_NON_SUCCESS}" -ne 0 ]]; then
        echo "SMOKE FAIL cases=${TOTAL_CASES} non_success=${SMOKE_NON_SUCCESS}" >&2
        exit 1
    fi
    echo "SMOKE PASS cases=${TOTAL_CASES} pattern=${PATTERN} transports=${TRANSPORTS} clients=${CLIENTS}"
    exit 0
fi
if [[ "${TOTAL_CASES}" -gt 0 && "${NON_SKIP_CASES}" -eq 0 ]]; then
    python3 "${PERF_REPORT_PY}" render-skips --cases "${TMP_CASES}" --output "${OUTPUT_FILE}"
    exit 0
fi
REPORT_CLIENTS="${CLIENTS}"
if [[ "${EXPLICIT_CLIENTS}" != "1" ]]; then
    has_stream=0
    all_stream=1
    for pat in "${PATTERNS[@]}"; do
        if [[ "${pat}" == "MULTI_STREAM" ]]; then
            has_stream=1
        else
            all_stream=0
        fi
    done
    if [[ "${all_stream}" == "1" && "${has_stream}" == "1" ]]; then
        REPORT_CLIENTS="${PERF_MULTI_DEFAULT_STREAM_CLIENTS:-${PERF_STREAM_DEFAULT_CLIENTS:-10000}}"
    elif [[ "${has_stream}" == "1" ]]; then
        REPORT_CLIENTS="${PERF_MULTI_DEFAULT_CLIENTS:-${PERF_DEFAULT_CLIENTS:-100}} (stream=${PERF_MULTI_DEFAULT_STREAM_CLIENTS:-${PERF_STREAM_DEFAULT_CLIENTS:-10000}})"
    else
        REPORT_CLIENTS="${PERF_MULTI_DEFAULT_CLIENTS:-${PERF_DEFAULT_CLIENTS:-100}}"
    fi
fi
REPORT_MSG_SIZES="${MSG_SIZES}"
if [[ "${EXPLICIT_MSG_SIZES}" != "1" ]]; then
    has_stream=0
    all_stream=1
    for pat in "${PATTERNS[@]}"; do
        if [[ "${pat}" == "MULTI_STREAM" ]]; then
            has_stream=1
        else
            all_stream=0
        fi
    done
    if [[ "${all_stream}" == "1" && "${has_stream}" == "1" ]]; then
        REPORT_MSG_SIZES="$(default_msg_sizes_for_pattern "MULTI_STREAM")"
    fi
fi
python3 "${PERF_REPORT_PY}" render-multi \
  --metrics "${TMP_METRICS}" \
  --cases "${TMP_CASES}" \
  --report "${RESULTS_FILE}" \
  --patterns "${PATTERN}" \
  --transports "${TRANSPORTS}" \
  --msg-sizes "${REPORT_MSG_SIZES}" \
  --clients "${REPORT_CLIENTS}" \
  --runs "${RUNS}" \
  --duration "${DURATION}" \
  --results-tag "${RESULTS_TAG}" \
  --output "${OUTPUT_FILE}" \
  --pin-cpu "${PIN_CPU}" \
  --common-io-threads "${COMMON_IO_THREADS}" \
  --server-io-threads "${SERVER_IO_THREADS}" \
  --client-io-threads "${CLIENT_IO_THREADS}" \
  --hwm "${HWM}" \
  --send-hwm "${SEND_HWM}" \
  --recv-hwm "${RECV_HWM}" \
  --sndtimeo-ms "${SNDTIMEO_MS}" \
  --rcvtimeo-ms "${RCVTIMEO_MS}" \
  --run-cooldown-ms "${RUN_COOLDOWN_MS}" \
  --transport-transition-ms "${TRANSPORT_TRANSITION_MS}" \
  --pattern-transition-ms "${PATTERN_TRANSITION_MS}" \
  --connect-concurrency "${CONNECT_CONCURRENCY:-128 (default)}" \
  --elapsed-seconds "${SECONDS}" \
  --lang rust \
  --fail-fast "${PERF_FAIL_FAST:-0}"

prune_reports "${REPORT_DIR}"
