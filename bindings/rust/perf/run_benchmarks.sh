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
source "$HOME/.cargo/env" 2>/dev/null || true
export RUSTFLAGS="${RUSTFLAGS:+${RUSTFLAGS} }-Awarnings"

# -- Defaults (matching core/perf) -------------------------------------------
PATTERN="ALL"
DURATION="${PERF_SINGLE_DURATION_SECONDS:-5}"
MSG_SIZES="${PERF_MSG_SIZES:-64,256,1024,65536,131072,262144}"
TRANSPORTS="${PERF_TRANSPORTS:-}"
RUNS="${PERF_RUNS:-1}"
RESULTS_ROOT="${PERF_RESULTS_DIR:-${SCRIPT_DIR}/results}"
RESULTS_TAG="${PERF_RESULTS_TAG:-}"
OUTPUT_FILE=""
SMOKE=0
REUSE_BUILD=0
CLEAN_BUILD=0
PIN_CPU=0
BUILD_DIR=""
RUST_PACKAGE_EVIDENCE="${ZLINK_RUST_PACKAGE_EVIDENCE:-}"
IO_THREADS="${PERF_IO_THREADS:-}"
HWM=""
SEND_HWM=""
RECV_HWM=""
SNDBUF=""
RCVBUF=""
SNDTIMEO_MS="${PERF_SINGLE_SNDTIMEO_MS:-200}"
RCVTIMEO_MS="${PERF_SINGLE_RCVTIMEO_MS:-200}"
AUTO_HWM_PROFILE="${PERF_CTX_AUTO_HWM_PROFILE:-}"

print_help() {
    cat <<'EOF'
Usage: bindings/rust/perf/run_benchmarks.sh [options]

Options:
  -h, --help
  --pattern NAME
  --duration N
  --msg-sizes LIST
  --transports LIST
  --runs N
  --smoke
  --build-dir PATH
  --rust-package-evidence FILE
                             Rust candidate package evidence for completion-gate runs.
  --reuse-build
  --clean-build
  --pin-cpu
  --io-threads N
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
  --auto-hwm-profile NAME
  --results-dir PATH
  --results-tag NAME
  --output PATH
EOF
}

# -- Parse CLI options -------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            print_help
            exit 0
            ;;
        --pattern)   PATTERN="$2";   shift 2 ;;
        --duration)  DURATION="$2";  shift 2 ;;
        --msg-sizes) MSG_SIZES="$2"; shift 2 ;;
        --transports) TRANSPORTS="$2"; shift 2 ;;
        --runs)      RUNS="$2";      shift 2 ;;
        --smoke)     SMOKE=1;         shift ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --rust-package-evidence) RUST_PACKAGE_EVIDENCE="$2"; shift 2 ;;
        --io-threads) IO_THREADS="$2"; shift 2 ;;
        --hwm) HWM="$2"; shift 2 ;;
        --send-hwm) SEND_HWM="$2"; shift 2 ;;
        --recv-hwm) RECV_HWM="$2"; shift 2 ;;
        --buf) SNDBUF="$2"; RCVBUF="$2"; shift 2 ;;
        --sndbuf) SNDBUF="$2"; shift 2 ;;
        --rcvbuf) RCVBUF="$2"; shift 2 ;;
        --sndtimeo|--send-timeout-ms) SNDTIMEO_MS="$2"; shift 2 ;;
        --rcvtimeo|--recv-timeout-ms) RCVTIMEO_MS="$2"; shift 2 ;;
        --auto-hwm-profile) AUTO_HWM_PROFILE="$2"; shift 2 ;;
        --results-dir) RESULTS_ROOT="$2"; shift 2 ;;
        --results-tag) RESULTS_TAG="$2"; shift 2 ;;
        --output)    OUTPUT_FILE="$2"; shift 2 ;;
        --reuse-build) REUSE_BUILD=1; shift ;;
        --clean-build) CLEAN_BUILD=1; shift ;;
        --pin-cpu) PIN_CPU=1; shift ;;
        *)           echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ "${REUSE_BUILD}" -eq 1 && "${CLEAN_BUILD}" -eq 1 ]]; then
    echo "--reuse-build and --clean-build are mutually exclusive" >&2
    exit 1
fi

# -- Platform ----------------------------------------------------------------
case "$(uname -s)" in
    Linux*)  PLATFORM="linux" ;;
    Darwin*) PLATFORM="macos" ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM="windows" ;;
    *)       PLATFORM="linux" ;;
esac

default_transports_for_pattern() {
    if [[ "${PLATFORM}" == "windows" ]]; then
        printf '%s' "tcp,tls,ws,wss,inproc"
    else
        printf '%s' "tcp,tls,ws,wss,inproc,ipc"
    fi
}

is_supported_transport_for_pattern() {
    local transport="$2"
    case "${transport}" in
        tcp|tls|ws|wss|inproc)
            return 0
            ;;
        ipc)
            [[ "${PLATFORM}" != "windows" ]]
            return
            ;;
        *)
            return 1
            ;;
    esac
}

prepare_core_runtime() {
    if [[ -n "${RUST_PACKAGE_EVIDENCE}" ]]; then
        [[ -f "${RUST_RUNTIME_RESOLVER}" ]] || {
            echo "Rust candidate runtime resolver not found: ${RUST_RUNTIME_RESOLVER}" >&2
            exit 1
        }
        # Candidate package evidence owns the runtime path, hash and Core identity.
        # The resolver also requires this source revision to match the package.
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
        echo "Rebuild core/build before running run_benchmarks.sh." >&2
        exit 1
    fi
    echo "Rust perf development runtime: ${resolved_lib}"
    echo "Rust perf runtime sha256: $(sha256sum "${resolved_lib}" | awk '{print $1}')"
    export ZLINK_RUST_NATIVE_DIR="${native_dir}"
    export LD_LIBRARY_PATH="${native_dir}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
}

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
TAG_SUFFIX=""
if [[ -n "${RESULTS_TAG}" ]]; then
    TAG_SUFFIX="_${RESULTS_TAG}"
fi
REPORT_DIR="${RESULTS_ROOT}/single/report"
RESULTS_FILE="${REPORT_DIR}/perf_rust_single_${PLATFORM}_${TIMESTAMP}${TAG_SUFFIX}.txt"
if [[ "${SMOKE}" -eq 0 ]]; then
    mkdir -p "${REPORT_DIR}"
fi

prune_reports() {
    local report_dir="$1"
    local count
    count="$(find "${report_dir}" -maxdepth 1 -type f -name 'perf_*.txt' | wc -l | tr -d ' ')"
    if [[ -z "${count}" || "${count}" -le 100 ]]; then
        return
    fi
    find "${report_dir}" -maxdepth 1 -type f -name 'perf_*.txt' -printf '%f\n' \
        | sort \
        | head -n "$((count - 100))" \
        | while read -r old_file; do
            rm -f "${report_dir}/${old_file}"
        done
}

sleep_millis() {
    local millis="${1:-0}"
    if [[ ! "${millis}" =~ ^[0-9]+$ || "${millis}" -eq 0 ]]; then
        return
    fi
    sleep "$((millis / 1000)).$(printf '%03d' "$((millis % 1000))")"
}

# -- Build -------------------------------------------------------------------
TARGET_DIR="${BUILD_DIR:-${CARGO_TARGET_DIR:-/tmp/zlink-rust-target/perf-single}}"
SINGLE_DIR="${TARGET_DIR}/release"

prepare_core_runtime

if [[ "${REUSE_BUILD}" -eq 0 ]]; then
    if [[ "${CLEAN_BUILD}" -eq 1 ]]; then
        (cd "${SCRIPT_DIR}/single" && CARGO_TARGET_DIR="${TARGET_DIR}" cargo clean --quiet)
    fi
    (cd "${SCRIPT_DIR}/single" && CARGO_TARGET_DIR="${TARGET_DIR}" cargo build --release --quiet)
elif [[ ! -x "${SINGLE_DIR}/perf_pair" ]]; then
    echo "existing single perf binaries not found for --reuse-build: ${SINGLE_DIR}" >&2
    exit 1
fi
TOTAL_TIME_ENABLED=1
[[ -n "${IO_THREADS}" ]] && export PERF_IO_THREADS="${IO_THREADS}"
if [[ -n "${SEND_HWM}" ]]; then
    export PERF_SINGLE_SNDHWM="${SEND_HWM}"
elif [[ -n "${HWM}" ]]; then
    export PERF_SINGLE_SNDHWM="${HWM}"
fi
if [[ -n "${RECV_HWM}" ]]; then
    export PERF_SINGLE_RCVHWM="${RECV_HWM}"
elif [[ -n "${HWM}" ]]; then
    export PERF_SINGLE_RCVHWM="${HWM}"
fi
[[ -n "${SNDBUF}" ]] && export PERF_SINGLE_SNDBUF="${SNDBUF}"
[[ -n "${RCVBUF}" ]] && export PERF_SINGLE_RCVBUF="${RCVBUF}"
export PERF_SINGLE_SNDTIMEO_MS="${SNDTIMEO_MS}"
export PERF_SINGLE_RCVTIMEO_MS="${RCVTIMEO_MS}"
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

IFS=',' read -ra SIZE_LIST <<< "${MSG_SIZES}"

if [[ "${PATTERN}" == "ALL" ]]; then
    PATTERNS=("PAIR" "PUBSUB" "DEALER_DEALER" "DEALER_ROUTER" "ROUTER_ROUTER")
else
    IFS=',' read -ra PATTERNS <<< "${PATTERN}"
fi
PATTERN_DISPLAY="$(IFS=','; printf '%s' "${PATTERNS[*]}")"

TMP_METRICS="$(mktemp)"
TMP_CASES="$(mktemp)"
cleanup() {
    local status=$?
    rm -f "${TMP_METRICS}" "${TMP_CASES}"
    print_total_time "${status}"
}
trap cleanup EXIT
METRICS_REGEX='^(throughput|bandwidth|latency|latency_p95|latency_p99)$'
BIN_TIMEOUT_SECONDS="${PERF_SINGLE_TIMEOUT_SECONDS:-$(( DURATION * 6 + 15 ))}"
if [[ "${BIN_TIMEOUT_SECONDS}" -lt 30 ]]; then
    BIN_TIMEOUT_SECONDS=30
fi

stop_early=0
for pat in "${PATTERNS[@]}"; do
    if [[ "${stop_early}" -eq 1 ]]; then
        break
    fi
    BIN=""
    case "${pat}" in
        PAIR)            BIN="${SINGLE_DIR}/perf_pair" ;;
        PUBSUB)          BIN="${SINGLE_DIR}/perf_pubsub" ;;
        DEALER_DEALER)   BIN="${SINGLE_DIR}/perf_dealer_dealer" ;;
        DEALER_ROUTER)   BIN="${SINGLE_DIR}/perf_dealer_router" ;;
        ROUTER_ROUTER)   BIN="${SINGLE_DIR}/perf_router_router" ;;
        *)               continue ;;
    esac
    current_transports="${TRANSPORTS:-$(default_transports_for_pattern "${pat}")}"
    IFS=',' read -ra TRANSPORT_LIST <<< "${current_transports}"
    for transport in "${TRANSPORT_LIST[@]}"; do
        if [[ "${stop_early}" -eq 1 ]]; then
            break
        fi
        for size in "${SIZE_LIST[@]}"; do
            if [[ "${stop_early}" -eq 1 ]]; then
                break
            fi
            case_status="success"
            case_reason=""
            if ! is_supported_transport_for_pattern "${pat}" "${transport}"; then
                case_status="unsupported"
                case_reason="UNSUPPORTED;rust;${pat};${transport};unsupported_transport"
                printf '%s,%s,%s,%s,%s\n' "${pat}" "${transport}" "${size}" "${case_status}" "${case_reason}" >> "${TMP_CASES}"
                continue
            fi
            for run in $(seq 1 "${RUNS}"); do
                if ! OUTPUT="$(timeout "${BIN_TIMEOUT_SECONDS}s" "${RUN_PREFIX[@]}" "${BIN}" \
                    --pattern "${pat}" \
                    --transport "${transport}" \
                    --msg-size "${size}" \
                    --duration "${DURATION}" 2>&1)"; then
                    case_status="fail"
                    case_reason="binary_exit"
                fi
                unsupported_line="$(printf '%s\n' "${OUTPUT}" | awk -F',' '/^UNSUPPORTED,/ {print; exit}')"
                if [[ -n "${unsupported_line}" ]]; then
                    case_status="unsupported"
                    case_reason="${unsupported_line}"
                    break
                fi
                if [[ "${case_status}" == "fail" ]]; then
                    break
                fi
                REQUIRED_COUNT="$(printf '%s\n' "${OUTPUT}" | awk -F',' '/^RESULT,/ && ($6=="throughput" || $6=="bandwidth" || $6=="latency" || $6=="latency_p95" || $6=="latency_p99") {count++} END {print count+0}')"
                if [[ "${REQUIRED_COUNT}" -ne 5 ]]; then
                    case_status="fail"
                    case_reason="missing_required_result_lines run=${run}"
                    break
                fi
                while IFS= read -r line; do
                    [[ "${line}" == RESULT,* ]] || continue
                    IFS=',' read -r tag lib result_pattern result_transport result_size metric value <<< "${line}"
                    [[ "${metric}" =~ ${METRICS_REGEX} ]] || continue
                    printf '%s,%s,%s,%s,%s,%s\n' \
                        "${pat}" "${transport}" "${size}" "${run}" "${metric}" "${value}" >> "${TMP_METRICS}"
                done <<< "${OUTPUT}"
            done
            case_reason="${case_reason//,/;}"
            printf '%s,%s,%s,%s,%s\n' "${pat}" "${transport}" "${size}" "${case_status}" "${case_reason}" >> "${TMP_CASES}"
            if [[ "${PERF_FAIL_FAST:-0}" == "1" && "${case_status}" == "fail" ]]; then
                stop_early=1
                break
            fi
            sleep_millis "${PERF_SINGLE_CASE_COOLDOWN_MS:-0}"
        done
    done
done

if [[ "${SMOKE}" -eq 1 ]]; then
    SMOKE_CASES="$(wc -l < "${TMP_CASES}" | tr -d ' ')"
    SMOKE_NON_SUCCESS="$(awk -F',' '$4 != "success" { count++ } END { print count + 0 }' "${TMP_CASES}")"
    if [[ "${SMOKE_CASES}" -eq 0 || "${SMOKE_NON_SUCCESS}" -ne 0 ]]; then
        echo "SMOKE FAIL cases=${SMOKE_CASES} non_success=${SMOKE_NON_SUCCESS}" >&2
        exit 1
    fi
    echo "SMOKE PASS cases=${SMOKE_CASES} pattern=${PATTERN} transports=${TRANSPORTS:-default}"
    exit 0
fi

python3 "${PERF_REPORT_PY}" render-single \
  --metrics "${TMP_METRICS}" \
  --cases "${TMP_CASES}" \
  --report "${RESULTS_FILE}" \
  --patterns "${PATTERN_DISPLAY}" \
  --transports "${TRANSPORTS}" \
  --msg-sizes "${MSG_SIZES}" \
  --runs "${RUNS}" \
  --duration "${DURATION}" \
  --results-tag "${RESULTS_TAG}" \
  --output "${OUTPUT_FILE}" \
  --pin-cpu "${PIN_CPU}" \
  --io-threads "${IO_THREADS}" \
  --hwm "${HWM}" \
  --send-hwm "${SEND_HWM}" \
  --recv-hwm "${RECV_HWM}" \
  --sndtimeo-ms "${SNDTIMEO_MS}" \
  --rcvtimeo-ms "${RCVTIMEO_MS}" \
  --elapsed-seconds "${SECONDS}" \
  --lang rust \
  --fail-fast "${PERF_FAIL_FAST:-0}"

prune_reports "${REPORT_DIR}"
