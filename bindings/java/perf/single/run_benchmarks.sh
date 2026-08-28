#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
JAVA_BINDINGS_DIR="$(cd "${ROOT_DIR}/.." && pwd)"
REPO_DIR="$(cd "${ROOT_DIR}/../../.." && pwd)"
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
source "${REPO_DIR}/bindings/tools/local_core_runtime.sh"
export ZLINK_CORE_INCLUDE_DIR ZLINK_CORE_LIB_DIR
source "${ROOT_DIR}/require_java22.sh"
VERSION_FILE="${REPO_DIR}/VERSION"
CORE_VERSION="$(awk -F= '/^LIBZLINK_VERSION=/{print $2}' "${VERSION_FILE}")"
CORE_RUNTIME="${ZLINK_LOCAL_CORE_RUNTIME}"
RESULTS_ROOT="${PERF_RESULTS_DIR:-${ROOT_DIR}/results}"
PATTERN="ALL"
TRANSPORTS=""
MSG_SIZES="${PERF_MSG_SIZES:-64,256,1024,65536,131072,262144}"
RUNS=1
DURATION="${PERF_SINGLE_DURATION_SECONDS:-${PERF_DURATION_SECONDS:-5}}"
PART_COUNT="${PERF_PART_COUNT:-2}"
RESULTS_TAG="${PERF_RESULTS_TAG:-}"
BUILD_DIR=""
OUTPUT_PATH=""
PIN_CPU=0
REUSE_BUILD=0
CLEAN_BUILD=0
IO_THREADS="${PERF_IO_THREADS:-}"
HWM="${PERF_SINGLE_HWM:-${PERF_HWM:-}}"
SEND_HWM="${PERF_SINGLE_SNDHWM:-${PERF_SNDHWM:-${HWM}}}"
RECV_HWM="${PERF_SINGLE_RCVHWM:-${PERF_RCVHWM:-${HWM}}}"
SNDBUF="${PERF_SINGLE_SNDBUF:-${PERF_SNDBUF:-}}"
RCVBUF="${PERF_SINGLE_RCVBUF:-${PERF_RCVBUF:-}}"
SNDTIMEO_MS="${PERF_SINGLE_SNDTIMEO_MS:-${PERF_SNDTIMEO_MS:-200}}"
RCVTIMEO_MS="${PERF_SINGLE_RCVTIMEO_MS:-${PERF_RCVTIMEO_MS:-200}}"
TRANSPORT_TRANSITION_MS="${PERF_TRANSPORT_TRANSITION_MS:-3000}"
if [[ -n "${PERF_CTX_AUTO_HWM_ENABLE:-}" ]]; then
  CTX_AUTO_HWM_ENABLE="${PERF_CTX_AUTO_HWM_ENABLE}"
  CTX_AUTO_HWM_ENABLE_EXPLICIT=1
else
  # Match C runner: unset means the binding/core default governs auto-HWM
  # (still enabled); the report shows "core-default".
  CTX_AUTO_HWM_ENABLE=1
  CTX_AUTO_HWM_ENABLE_EXPLICIT=0
fi
CTX_AUTO_HWM_PROFILE="${PERF_SINGLE_CTX_AUTO_HWM_PROFILE:-${PERF_CTX_AUTO_HWM_PROFILE:-balanced}}"

usage() {
  cat <<'USAGE'
Usage: perf/single/run_benchmarks.sh [options]

Options:
  -h, --help            Show this help.
  --pattern NAME         Pattern list or ALL.
  --transports LIST      Transport list override.
  --msg-sizes LIST       Payload sizes.
  --runs N               Iterations per pattern/transport/size.
  --duration N           Active duration seconds.
  --part-count N         Application frame count per measured message (1 or 2; default: 2).
  --build-dir PATH       Build directory override.
  --reuse-build          Reuse existing installDist output.
  --clean-build          Delete build dir before installDist.
  --output PATH          Tee report output to PATH.
  --pin-cpu              Pin benchmark process to CPU 0 on Linux.
  --io-threads N         Context I/O threads.
  --hwm N                Debug-only manual HWM override.
  --send-hwm N           Send HWM override.
  --recv-hwm N           Receive HWM override.
  --buf SIZE             Send/receive buffer override.
  --sndbuf SIZE          Send buffer override.
  --rcvbuf SIZE          Receive buffer override.
  --sndtimeo N           Send timeout ms.
  --rcvtimeo N           Receive timeout ms.
  --send-timeout-ms N    Alias of --sndtimeo.
  --recv-timeout-ms N    Alias of --rcvtimeo.
  --auto-hwm-profile NAME Auto-HWM profile.
  --results-dir PATH     Results root override.
  --results-tag NAME     Optional report suffix tag.
  --core-version VERSION Download and use the specified released Core version.

Notes:
  - by default the current workspace Core (core/build) is used; pass --core-version
    to fetch and use a released Core runtime instead.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pattern) PATTERN="${2:-}"; shift ;;
    --transports) TRANSPORTS="${2:-}"; shift ;;
    --msg-sizes) MSG_SIZES="${2:-}"; shift ;;
    --runs) RUNS="${2:-}"; shift ;;
    --duration) DURATION="${2:-}"; shift ;;
    --part-count) PART_COUNT="${2:-}"; shift ;;
    --build-dir) BUILD_DIR="${2:-}"; shift ;;
    --reuse-build) REUSE_BUILD=1 ;;
    --clean-build) CLEAN_BUILD=1 ;;
    --output) OUTPUT_PATH="${2:-}"; shift ;;
    --pin-cpu) PIN_CPU=1 ;;
    --io-threads) IO_THREADS="${2:-}"; shift ;;
    --hwm) HWM="${2:-}"; SEND_HWM="${2:-}"; RECV_HWM="${2:-}"; shift ;;
    --send-hwm) SEND_HWM="${2:-}"; shift ;;
    --recv-hwm) RECV_HWM="${2:-}"; shift ;;
    --buf) SNDBUF="${2:-}"; RCVBUF="${2:-}"; shift ;;
    --sndbuf) SNDBUF="${2:-}"; shift ;;
    --rcvbuf) RCVBUF="${2:-}"; shift ;;
    --auto-hwm-profile) CTX_AUTO_HWM_PROFILE="${2:-}"; shift ;;
    --sndtimeo|--send-timeout-ms) SNDTIMEO_MS="${2:-}"; shift ;;
    --rcvtimeo|--recv-timeout-ms) RCVTIMEO_MS="${2:-}"; shift ;;
    --results-dir) RESULTS_ROOT="${2:-}"; shift ;;
    --results-tag) RESULTS_TAG="${2:-}"; shift ;;
    --core-version) shift ;;
    --core-version=*) ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
  shift
done

if [[ "${PART_COUNT}" != "1" && "${PART_COUNT}" != "2" ]]; then
  echo "--part-count must be 1 or 2." >&2
  exit 1
fi
export PERF_PART_COUNT="${PART_COUNT}"

failure_reason_from_output() {
  local output_text="$1"
  python3 - "$output_text" <<'PY'
import sys

for raw in sys.argv[1].splitlines():
    if not raw.startswith("FAIL,"):
        continue
    parts = raw.strip().split(",", 5)
    if len(parts) == 6 and parts[5]:
        print(parts[5])
        raise SystemExit(0)
print("process_exit_nonzero")
PY
}

if ! [[ "${RUNS}" =~ ^[0-9]+$ ]] || [[ "${RUNS}" -lt 1 ]]; then
  echo "--runs must be >= 1" >&2
  exit 1
fi

for numeric_opt in IO_THREADS SEND_HWM RECV_HWM; do
  value="${!numeric_opt}"
  if [[ -n "${value}" ]] && { ! [[ "${value}" =~ ^[0-9]+$ ]] || [[ "${value}" -lt 1 ]]; }; then
    echo "${numeric_opt,,} must be >= 1" >&2
    exit 1
  fi
done

if [[ -n "${HWM}${SEND_HWM}${RECV_HWM}${SNDBUF}${RCVBUF}" \
  && "${PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES:-${PERF_ALLOW_MANUAL_SOCKET_OVERRIDES:-0}}" != "1" ]]; then
  echo "manual HWM/SNDBUF/RCVBUF overrides are debug-only; set PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1" >&2
  exit 1
fi

case "${CTX_AUTO_HWM_PROFILE}" in
  ""|compact|low_latency|low-latency|balanced|throughput) ;;
  *)
    echo "--auto-hwm-profile must be compact, low_latency, balanced, or throughput" >&2
    exit 1
    ;;
esac

case "${CTX_AUTO_HWM_ENABLE}" in
  0|1) ;;
  *)
    echo "PERF_CTX_AUTO_HWM_ENABLE must be 0 or 1" >&2
    exit 1
    ;;
esac

export PERF_CTX_AUTO_HWM_ENABLE="${CTX_AUTO_HWM_ENABLE}"
export PERF_CTX_AUTO_HWM_PROFILE="${CTX_AUTO_HWM_PROFILE}"
if [[ "${PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES:-${PERF_ALLOW_MANUAL_SOCKET_OVERRIDES:-0}}" == "1" ]]; then
  export PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1
fi

for numeric_opt in SNDTIMEO_MS RCVTIMEO_MS TRANSPORT_TRANSITION_MS; do
  value="${!numeric_opt}"
  if [[ -n "${value}" ]] && { ! [[ "${value}" =~ ^[0-9]+$ ]] || [[ "${value}" -lt 0 ]]; }; then
    echo "${numeric_opt,,} must be >= 0" >&2
    exit 1
  fi
done

if [[ "${PATTERN}" == "ALL" ]]; then
  PATTERN="PAIR,PUBSUB,DEALER_DEALER,DEALER_ROUTER,DEALER_ROUTER_REQREP,ROUTER_ROUTER,ROUTER_ROUTER_REQREP"
fi

detect_platform() {
  case "$(uname -s)" in
    Linux*) echo "linux" ;;
    Darwin*) echo "macos" ;;
    MINGW*|MSYS*|CYGWIN*) echo "windows" ;;
    *) echo "$(uname -s | tr '[:upper:]' '[:lower:]')" ;;
  esac
}

default_transports() {
  if [[ "$(uname -s)" == "Linux" ]]; then
    echo "tcp,tls,ws,wss,inproc,ipc"
  else
    echo "tcp,tls,ws,wss,inproc"
  fi
}

filter_transports_for_pattern() {
  local pattern="$1"
  local transports_csv="$2"
  python3 - "${pattern}" "${transports_csv}" <<'PY'
import sys

items = [item.strip() for item in sys.argv[2].split(",") if item.strip()]
allowed = {"tcp", "tls", "ws", "wss", "inproc", "ipc"}
print(",".join(item for item in items if item in allowed))
PY
}

trim_csv() {
  printf '%s' "$1" | awk -F',' '{for (i=1; i<=NF; ++i) {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $i); printf "%s%s", (i>1?",":""), $i}}'
}

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

sleep_ms() {
  python3 - "$1" <<'PY'
import sys
import time
time.sleep(int(sys.argv[1]) / 1000.0)
PY
}

resolve_build_dir() {
  if [[ -n "${BUILD_DIR}" ]]; then
    printf '%s' "${BUILD_DIR%/}/perf-single"
  else
    printf '%s' "${ROOT_DIR}/single/Zlink.BindingBench/build"
  fi
}

ensure_single_runner() {
  local build_dir="$1"
  local runner_path="$2"
  local install_dir="${build_dir}/install"
  local dist_zip="${build_dir}/distributions/zlink-java-perf-single.zip"
  if [[ -x "${runner_path}" && -s "${runner_path}" ]]; then
    return 0
  fi
  rm -f "${runner_path}"
  if [[ -f "${dist_zip}" ]]; then
    mkdir -p "${install_dir}"
    unzip -qo "${dist_zip}" -d "${install_dir}"
  fi
}

require_java22

mkdir -p "${RESULTS_ROOT}/single/report"
cd "${ROOT_DIR}"
PROJECT_BUILD_DIR="$(resolve_build_dir)"
RUNNER="${PROJECT_BUILD_DIR}/install/zlink-java-perf-single/bin/zlink-java-perf-single"
if [[ "${CLEAN_BUILD}" -eq 1 ]]; then
  rm -rf "${PROJECT_BUILD_DIR}"
fi
ensure_single_runner "${PROJECT_BUILD_DIR}" "${RUNNER}"
if [[ "${REUSE_BUILD}" -eq 0 ]]; then
  "${JAVA_BINDINGS_DIR}/gradlew" --no-daemon -p "${JAVA_BINDINGS_DIR}" \
    -PzlinkPerfBuildDir="${PROJECT_BUILD_DIR}" :perf-single:installDist >/dev/null
fi
ensure_single_runner "${PROJECT_BUILD_DIR}" "${RUNNER}"
if [[ ! -f "${CORE_RUNTIME}" ]]; then
  echo "core runtime not found: ${CORE_RUNTIME}" >&2
  echo "Build core/build before running Java perf." >&2
  exit 1
fi
if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 0 ]] && find "${REPO_DIR}/core/include" "${REPO_DIR}/core/src" \
    -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' \) \
    -newer "${CORE_RUNTIME}" -print -quit | grep -q .; then
  echo "core runtime is older than core source: ${CORE_RUNTIME}" >&2
  echo "Run: cmake --build core/build" >&2
  exit 1
fi
export ZLINK_LIBRARY_PATH="${CORE_RUNTIME}"
echo "Perf runtime libzlink: ${CORE_RUNTIME}"
if [[ ! -x "${RUNNER}" || ! -s "${RUNNER}" ]]; then
  if [[ "${REUSE_BUILD}" -eq 1 ]]; then
    echo "runner not found for --reuse-build: ${RUNNER}" >&2
  else
    echo "runner not found: ${RUNNER}" >&2
  fi
  exit 1
fi

platform="$(detect_platform)"
timestamp="$(date +%Y%m%d_%H%M%S)"
report="${RESULTS_ROOT}/single/report/perf_java_single_${platform}_${timestamp}"
if [[ -n "${RESULTS_TAG}" ]]; then
  report="${report}_${RESULTS_TAG}"
fi
report="${report}.txt"

tmp_metrics="$(mktemp)"
tmp_failures="$(mktemp)"
tmp_auto_hwm="$(mktemp)"
trap 'rm -f "${tmp_metrics}" "${tmp_failures}" "${tmp_auto_hwm}"' EXIT

metrics_regex='^(throughput|bandwidth|latency|latency_p95|latency_p99)$'
runner_cmd=("${RUNNER}")
if [[ "${PIN_CPU}" -eq 1 ]]; then
  if [[ "$(uname -s)" != "Linux" ]]; then
    echo "--pin-cpu is only supported on Linux in this runner" >&2
    exit 1
  fi
  if ! command -v taskset >/dev/null 2>&1; then
    echo "--pin-cpu requires taskset" >&2
    exit 1
  fi
  runner_cmd=("taskset" "-c" "0" "${RUNNER}")
fi

expected_result_lines=0
actual_result_lines=0

record_failure() {
  local pattern="$1"
  local transport="$2"
  local size="$3"
  local run="$4"
  local reason="$5"
  printf '%s,%s,%s,%s,%s\n' \
    "${pattern}" "${transport}" "${size}" "${run}" "${reason}" >> "${tmp_failures}"
}

IFS=',' read -r -a patterns <<< "$(trim_csv "${PATTERN}")"
IFS=',' read -r -a msg_sizes <<< "$(trim_csv "${MSG_SIZES}")"

case_timeout_seconds() {
  local pattern="$1"
  local value
  value=$((DURATION * 6 + 15))
  if [[ "${value}" -lt 30 ]]; then
    value=30
  fi
  printf '%s' "${value}"
}

# Record the pattern -> transport iteration order so the report emitter can
# reproduce the canonical C structure (progress lines + cooldown markers)
# byte-for-byte. C authority: bindings/c/perf/single/run_comparison.py
tmp_plan="$(mktemp)"
trap 'rm -f "${tmp_metrics}" "${tmp_failures}" "${tmp_auto_hwm}" "${tmp_plan}"' EXIT

stop_early=0
for pattern in "${patterns[@]}"; do
  if [[ "${stop_early}" -eq 1 ]]; then
    break
  fi
  current_transports="${TRANSPORTS:-$(default_transports "${pattern}")}"
  current_transports="$(filter_transports_for_pattern "${pattern}" "${current_transports}")"
  if [[ -z "${current_transports}" ]]; then
    continue
  fi
  IFS=',' read -r -a transports <<< "$(trim_csv "${current_transports}")"
  printf '%s\t%s\n' "${pattern}" "${current_transports}" >> "${tmp_plan}"
  for transport_index in "${!transports[@]}"; do
    if [[ "${stop_early}" -eq 1 ]]; then
      break
    fi
    transport="${transports[$transport_index]}"
    echo "    Testing ${transport}:"
    for size in "${msg_sizes[@]}"; do
      if [[ "${stop_early}" -eq 1 ]]; then
        break
      fi
      for ((run=1; run<=RUNS; run++)); do
        if [[ "${stop_early}" -eq 1 ]]; then
          break
        fi
        expected_result_lines=$((expected_result_lines + 5))
        cmd=("${runner_cmd[@]}" "${pattern}" "${transport}" "${size}" \
          --duration "${DURATION}")
        if [[ -n "${IO_THREADS}" ]]; then
          cmd+=(--io-threads "${IO_THREADS}")
        fi
        if [[ -n "${SEND_HWM}" ]]; then
          cmd+=(--send-hwm "${SEND_HWM}")
        fi
        if [[ -n "${RECV_HWM}" ]]; then
          cmd+=(--recv-hwm "${RECV_HWM}")
        fi
        if [[ -n "${SNDBUF}" ]]; then
          cmd+=(--sndbuf "${SNDBUF}")
        fi
        if [[ -n "${RCVBUF}" ]]; then
          cmd+=(--rcvbuf "${RCVBUF}")
        fi
        cmd+=(--sndtimeo "${SNDTIMEO_MS}" --rcvtimeo "${RCVTIMEO_MS}")
        if command -v timeout >/dev/null 2>&1; then
          timeout_seconds="$(case_timeout_seconds "${pattern}")"
          status=0
          output="$(timeout "${timeout_seconds}s" "${cmd[@]}" 2>&1)" || status=$?
          if [[ "${status}" -ne 0 ]]; then
            if [[ "${status}" -eq 124 ]]; then
              record_failure "${pattern}" "${transport}" "${size}" "${run}" \
                "timeout_after_${timeout_seconds}s"
              [[ "${PERF_FAIL_FAST:-0}" == "1" ]] && stop_early=1
              break
            fi
            if printf '%s\n' "${output}" | grep -q '^UNSUPPORTED,'; then
              expected_result_lines=$((expected_result_lines - 5))
              continue
            fi
            record_failure "${pattern}" "${transport}" "${size}" "${run}" \
              "$(failure_reason_from_output "${output}")"
            [[ "${PERF_FAIL_FAST:-0}" == "1" ]] && stop_early=1
            break
          fi
          unset status
        elif ! output="$("${cmd[@]}" 2>&1)"; then
          if printf '%s\n' "${output}" | grep -q '^UNSUPPORTED,'; then
            expected_result_lines=$((expected_result_lines - 5))
            continue
          fi
          record_failure "${pattern}" "${transport}" "${size}" "${run}" \
            "$(failure_reason_from_output "${output}")"
          [[ "${PERF_FAIL_FAST:-0}" == "1" ]] && stop_early=1
          break
        fi
        if (( RUNS > 1 )); then
          printf '      run %s/%s:\n' "${run}" "${RUNS}"
        fi
        if printf '%s\n' "${output}" | grep -q '^UNSUPPORTED,'; then
          expected_result_lines=$((expected_result_lines - 5))
          continue
        fi
        while IFS= read -r line; do
          if [[ "${line}" == AUTO_HWM_DETAIL,* ]]; then
            printf '%s\n' "${line}" >> "${tmp_auto_hwm}"
            continue
          fi
          [[ "${line}" == RESULT,* ]] || continue
          IFS=',' read -r tag lib result_pattern result_transport result_size metric value <<< "${line}"
          if [[ ! "${metric}" =~ ${metrics_regex} ]]; then
            continue
          fi
          printf '%s,%s,%s,%s,%s,%s\n' \
            "${pattern}" "${transport}" "${size}" "${run}" "${metric}" "${value}" >> "${tmp_metrics}"
          actual_result_lines=$((actual_result_lines + 1))
        done <<< "${output}"
        required_count="$(printf '%s\n' "${output}" \
          | awk -F',' '/^RESULT,/ && ($6=="throughput" || $6=="bandwidth" || $6=="latency" || $6=="latency_p95" || $6=="latency_p99") {count++} END {print count+0}')"
        if [[ "${required_count}" -ne 5 ]]; then
          record_failure "${pattern}" "${transport}" "${size}" "${run}" "missing_required_result_lines"
          [[ "${PERF_FAIL_FAST:-0}" == "1" ]] && stop_early=1
          break
        fi
      done
    done
    echo "    Testing ${transport}: Done"
    if [[ "${stop_early}" -ne 1 && "${TRANSPORT_TRANSITION_MS}" -gt 0 && "$((transport_index + 1))" -lt "${#transports[@]}" ]]; then
      next_transport="${transports[$((transport_index + 1))]}"
      cooldown_ms="${TRANSPORT_TRANSITION_MS}"
      if [[ "${next_transport}" == "inproc" && ( "${transport}" == "ws" || "${transport}" == "wss" ) ]]; then
        if [[ "${cooldown_ms}" -lt 30000 ]]; then
          cooldown_ms=30000
        fi
      fi
      sleep_ms "${cooldown_ms}"
    fi
  done
done

# C single timeout_seconds: max(30, dur*6+15)
TIMEOUT_SECONDS="$(python3 - "${DURATION}" <<'PY'
import sys
dur = int(sys.argv[1])
val = max(30, dur * 6 + 15)
print(val)
PY
)"

if [[ "${CTX_AUTO_HWM_ENABLE_EXPLICIT}" -eq 1 ]]; then
  CTX_AUTO_HWM_ENABLE_DISPLAY="${CTX_AUTO_HWM_ENABLE}"
else
  CTX_AUTO_HWM_ENABLE_DISPLAY="core-default"
fi

python_status=0
commit_sha="$(git -C "${REPO_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
python3 - "${ROOT_DIR}/report_common.py" "${tmp_metrics}" "${tmp_failures}" "${tmp_auto_hwm}" "${tmp_plan}" "${report}" "${PATTERN}" "${MSG_SIZES}" \
  "${RUNS}" "${DURATION}" "${TIMEOUT_SECONDS}" "${RESULTS_TAG}" "${PIN_CPU}" "${expected_result_lines}" "${actual_result_lines}" \
  "${IO_THREADS}" "${HWM}" "${SEND_HWM}" "${RECV_HWM}" "${SNDTIMEO_MS}" \
  "${RCVTIMEO_MS}" "${SNDBUF}" "${RCVBUF}" \
  "${CTX_AUTO_HWM_ENABLE_DISPLAY}" "${CTX_AUTO_HWM_PROFILE}" "${OUTPUT_PATH}" \
  "${PERF_FAIL_FAST:-0}" "${commit_sha}" "${ZLINK_CORE_SOURCE}" "${CORE_VERSION}" \
  "${CORE_RUNTIME}" <<'PY' || python_status=$?
import csv
import math
import sys
from collections import defaultdict
from pathlib import Path

(
    helper_path, metrics_path, failures_path, auto_hwm_path, plan_path,
    report_path, pattern_csv, msg_sizes_csv, runs, duration, timeout_seconds,
    results_tag, pin_cpu, expected_result_lines, actual_result_lines,
    io_threads, hwm, send_hwm, recv_hwm, sndtimeo_ms, rcvtimeo_ms, sndbuf,
    rcvbuf, ctx_auto_hwm_enable, ctx_auto_hwm_profile, output_path, fail_fast,
    commit_sha, core_source, core_version, core_runtime,
) = sys.argv[1:]
sys.path.insert(0, str(Path(helper_path).resolve().parent))
from report_common import load_failures

runs = int(runs)
expected_result_lines = int(expected_result_lines)
actual_result_lines = int(actual_result_lines)
all_metrics = ["throughput", "bandwidth", "latency", "latency_p95", "latency_p99"]

# Iteration plan (pattern -> ordered transports), preserving benchmark order.
plan = []
with open(plan_path, encoding="utf-8") as f:
    for raw in f:
        raw = raw.rstrip("\n")
        if not raw or "\t" not in raw:
            continue
        pat, tr_csv = raw.split("\t", 1)
        transports = [t.strip() for t in tr_csv.split(",") if t.strip()]
        plan.append((pat, transports))

rows = defaultdict(lambda: defaultdict(list))
plan_sizes = []
with open(metrics_path, newline="", encoding="utf-8") as f:
    for pattern, transport, size, run, metric, value in csv.reader(f):
        key = (pattern, transport, int(size))
        if int(size) not in plan_sizes:
            plan_sizes.append(int(size))
        try:
            rows[key][metric].append(float(value))
        except ValueError:
            rows[key][metric].append(math.nan)
plan_sizes.sort()

failures = load_failures(failures_path)

auto_hwm_rows = []
with open(auto_hwm_path, encoding="utf-8", errors="replace") as f:
    for raw in f:
        line = raw.strip()
        if not line.startswith("AUTO_HWM_DETAIL,"):
            continue
        fields = {}
        for item in line.split(",")[1:]:
            if "=" not in item:
                continue
            k, v = item.split("=", 1)
            fields[k.strip()] = v.strip()
        if fields:
            auto_hwm_rows.append(fields)


def median(values):
    usable = [v for v in values if not math.isnan(v)]
    if not usable:
        return math.nan
    usable.sort()
    mid = len(usable) // 2
    if len(usable) % 2 == 1:
        return usable[mid]
    return (usable[mid - 1] + usable[mid]) / 2.0


# Median-aggregate per combo (matches C statistics.median over runs).
combo = {}
for pattern, transports in plan:
    for transport in transports:
        for size in plan_sizes:
            key = (pattern, transport, size)
            mv = {m: median(rows[key].get(m, [])) for m in all_metrics}
            if any(math.isnan(mv[m]) for m in all_metrics):
                continue
            combo[key] = mv

lines = []


def emit(line=""):
    lines.append(line)


# ---- C: format_throughput / format_bandwidth / format_latency_ms ----
def fmt_tp(pattern, value):
    unit = "Kops/s" if pattern.endswith("_REQREP") else "Kmsg/s"
    return f"{value / 1e3:6.2f} {unit}"


def fmt_bw(value):
    return f"{value:8.2f} MB/s"


def fmt_lat(value):
    return f"{value:8.3f} ms"


def table_header():
    return (
        "| Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |"
        "   Lat.P95(ms) |   Lat.P99(ms) |"
    )


def table_separator():
    return (
        "|----------|------------------|--------------|--------------|--------------|"
        "--------------|"
    )


def table_row(pattern, size, mv):
    tp_s = fmt_tp(pattern, mv["throughput"])
    bw_s = fmt_bw(mv["bandwidth"])
    lat_s = fmt_lat(mv["latency"])
    lat95_s = fmt_lat(mv["latency_p95"])
    lat99_s = fmt_lat(mv["latency_p99"])
    return (
        f"| {f'{size}B':<8} | {tp_s:>16} | {bw_s:>12} | {lat_s:>12} | "
        f"{lat95_s:>12} | {lat99_s:>12} |"
    )


def bytes_to_kb(value):
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return "?"
    if parsed <= 0:
        return "0"
    if parsed % 1024 == 0:
        return str(parsed // 1024)
    return f"{parsed / 1024.0:.1f}"


def emit_options(label):
    emit(f"## Effective Options ({label})")
    emit("- lang: java")
    emit("- suite: single")
    emit(f"- runs: {runs}")
    emit(f"- duration_seconds: {duration}")
    emit(f"- timeout_seconds: {timeout_seconds}")
    emit(f"- fail_fast: {fail_fast}")
    emit(f"- io_threads: {io_threads or '1'}")
    emit(f"- hwm: {hwm or 'auto-hwm'}")
    emit(f"- sndhwm: {send_hwm or 'auto-hwm'}")
    emit(f"- rcvhwm: {recv_hwm or 'auto-hwm'}")
    emit(f"- sndbuf: {sndbuf or '-1'}")
    emit(f"- rcvbuf: {rcvbuf or '-1'}")
    emit(f"- sndtimeo_ms: {sndtimeo_ms}")
    emit(f"- rcvtimeo_ms: {rcvtimeo_ms}")
    emit(f"- ctx_auto_hwm_enable: {ctx_auto_hwm_enable}")
    emit(f"- ctx_auto_hwm_profile: {ctx_auto_hwm_profile}")
    all_tr = sorted({t for _, trs in plan for t in trs})
    emit(f"- patterns: {','.join(p for p, _ in plan)}")
    emit(f"- transports: {','.join(all_tr) if all_tr else 'none'}")
    emit(f"- msg_sizes: {','.join(str(s) for s in plan_sizes) if plan_sizes else 'none'}")


def emit_auto_hwm(pattern):
    selected = []
    seen = set()
    for row in auto_hwm_rows:
        if row.get("pattern", "").upper() != pattern.upper():
            continue
        display = dict(row)
        display["sndbuf_kb"] = bytes_to_kb(row.get("effective_sndbuf", ""))
        display["rcvbuf_kb"] = bytes_to_kb(row.get("effective_rcvbuf", ""))
        key = tuple(display.get(name, "") for name in (
            "msg_size", "component", "owner", "socket", "socket_type",
            "role", "sndhwm", "rcvhwm", "sndbuf_kb", "rcvbuf_kb",
            "effective_message_bytes", "socket_message_slots",
        ))
        if key in seen:
            continue
        seen.add(key)
        selected.append(display)
    if not selected:
        return False
    selected.sort(key=lambda row: (
        int(row.get("msg_size", "0") or "0"),
        row.get("component", ""),
        row.get("owner", ""),
        row.get("socket", ""),
    ))
    columns = (
        ("Size(B)", "msg_size"),
        ("Component", "component"),
        ("Owner", "owner"),
        ("Socket", "socket"),
        ("Type", "socket_type"),
        ("Role", "role"),
        ("SNDHWM", "sndhwm"),
        ("RCVHWM", "rcvhwm"),
        ("SNDBUF(KB)", "sndbuf_kb"),
        ("RCVBUF(KB)", "rcvbuf_kb"),
        ("MsgUnit(B)", "effective_message_bytes"),
        ("Slots", "socket_message_slots"),
    )
    widths = []
    for header, k in columns:
        w = len(header)
        for row in selected:
            w = max(w, len(str(row.get(k, "?"))))
        widths.append(w)
    emit("## Auto-HWM Detail")
    emit(f"- pattern: {pattern}")
    emit("| " + " | ".join(
        f"{columns[i][0]:<{widths[i]}}" for i in range(len(columns))
    ) + " |")
    emit("|-" + "-|-".join("-" * w for w in widths) + "-|")
    for row in selected:
        emit("| " + " | ".join(
            f"{str(row.get(columns[i][1], '?')):<{widths[i]}}"
            for i in range(len(columns))
        ) + " |")
    emit("")
    return True


PATTERN_SEPARATOR = "=" * 79

# Leading blank line then start options (matches C print("\n## Effective ...")).
emit("")
emit(f"META,commit,{commit_sha}")
emit(f"META,core_source,{core_source}")
emit(f"META,core_version,{core_version}")
emit(f"META,core_runtime,{core_runtime}")
emit("")
emit_options("start")

first_pattern = True
for pattern, transports in plan:
    if not first_pattern:
        emit("")
        emit(PATTERN_SEPARATOR)
        emit("")
    first_pattern = False
    direction = "request/reply" if pattern.endswith("_REQREP") else "one-way"
    emit(f"## PATTERN: {pattern} ({direction})")
    emit(f"  > Benchmarking current for {pattern}...")
    for t_idx, transport in enumerate(transports):
        has_next = (t_idx + 1) < len(transports)
        emit(f"    Testing {transport}:")
        emit(f"      {table_header()}")
        emit(f"      {table_separator()}")
        for size in plan_sizes:
            mv = combo.get((pattern, transport, size))
            if mv is None:
                continue
            emit(f"      {table_row(pattern, size, mv)}")
        emit(f"    Testing {transport}: Done")
        if has_next:
            cooldown_ms = 3000
            next_transport = transports[t_idx + 1]
            if next_transport == "inproc" and transport in ("ws", "wss"):
                cooldown_ms = max(cooldown_ms, 30000)
            emit(f"    [transport cooldown {cooldown_ms}ms]")

# Failures block (C prints before auto-hwm tables).
all_failures = []
for f_pattern, f_transport, f_size, f_run, f_reason in failures:
    all_failures.append((f_pattern, f_transport, f_size, f_reason))
if all_failures:
    emit("")
    emit("## Failures")
    for f_pattern, f_transport, f_size, f_reason in all_failures:
        emit(f"- {f_pattern} current {f_transport} {f_size}B: {f_reason}")

# Auto-HWM detail tables (one blank line before the first).
emit("")
for pattern, _ in plan:
    emit_auto_hwm(pattern)
# Drop a trailing empty line so result options spacing matches C.
if lines and lines[-1] == "":
    lines.pop()

emit("")
emit_options("result")

has_results = bool(combo)
if has_results:
    emit("")
    emit("## Result Data")
    for key in sorted(combo.keys()):
        p, tr, sz = key
        mv = combo[key]
        for metric in all_metrics:
            emit(f"RESULT,current,{p},{tr},{sz},{metric},{mv[metric]:.3f}")

status = "complete" if expected_result_lines == actual_result_lines else "partial"
emit("")
emit("## Completion")
emit(f"- status: {status}")
emit(f"- expected_result_lines: {expected_result_lines}")
emit(f"- actual_result_lines: {actual_result_lines}")
emit("")
emit(f"Saved result file: {Path(report_path).resolve()} (status={status})")

text = "\n".join(lines) + "\n"
with open(report_path, "w", encoding="utf-8") as fh:
    fh.write(text)
if output_path:
    with open(output_path, "a", encoding="utf-8") as fh:
        fh.write(text)
sys.stdout.write(text)
sys.exit(0 if status == "complete" else 1)
PY

prune_reports "${RESULTS_ROOT}/single/report"
if [[ -n "${OUTPUT_PATH}" ]]; then
  echo "saved report: ${report}" | tee -a "${OUTPUT_PATH}"
else
  echo "saved report: ${report}"
fi
exit "${python_status}"
