#!/usr/bin/env bash
set -euo pipefail
trap '' PIPE

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOTNET_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "${DOTNET_DIR}/perf/common/report_helpers.sh"
PROJECT="${DOTNET_DIR}/perf/multi/Zlink.BindingBench.Multi/Zlink.BindingBench.Multi.csproj"
PROJECT_DIR="${DOTNET_DIR}/perf/multi/Zlink.BindingBench.Multi"
REPO_DIR="$(cd "${DOTNET_DIR}/../.." && pwd)"
source "${REPO_DIR}/bindings/tools/local_core_runtime.sh"
STREAM_CLIENT="${REPO_DIR}/bindings/c/build/perf/perf_stream_client"
STREAM_BUILD_DIR="${REPO_DIR}/bindings/c/build"
CORE_LIB="${ZLINK_LOCAL_CORE_RUNTIME}"
RESULTS_ROOT="${DOTNET_DIR}/perf/results"
PATTERN="ALL"
TRANSPORTS="${PERF_TRANSPORTS:-tcp,tls,ws,wss}"
DEFAULT_MULTI_MSG_SIZES="64,256,1024,4096,65536,131072"
DEFAULT_MULTI_STREAM_MSG_SIZES="64,256,1024,65536"
MSG_SIZES="${PERF_MSG_SIZES:-}"
STREAM_MSG_SIZES="${PERF_MULTI_STREAM_MSG_SIZES:-${PERF_STREAM_MSG_SIZES:-${DEFAULT_MULTI_STREAM_MSG_SIZES}}}"
CLIENTS="${PERF_MULTI_CLIENTS:-}"
EFFECTIVE_DEFAULT_CLIENTS="${PERF_MULTI_DEFAULT_CLIENTS:-${PERF_DEFAULT_CLIENTS:-100}}"
EFFECTIVE_DEFAULT_STREAM_CLIENTS="${PERF_MULTI_DEFAULT_STREAM_CLIENTS:-${PERF_STREAM_DEFAULT_CLIENTS:-10000}}"
EFFECTIVE_DEFAULT_IO_THREADS="${PERF_MULTI_DEFAULT_IO_THREADS:-${PERF_DEFAULT_IO_THREADS:-4}}"
DURATION="${PERF_MULTI_DURATION_SECONDS:-5}"
RUNS="${PERF_RUNS:-1}"
READY_TIMEOUT_MS="${PERF_MULTI_CONNECT_READY_TIMEOUT_MS:-${PERF_CONNECT_READY_TIMEOUT_MS:-1000}}"
SPOT_READY_TIMEOUT_MS="$(( READY_TIMEOUT_MS > READY_TIMEOUT_MS * 6 ? READY_TIMEOUT_MS : READY_TIMEOUT_MS * 6 ))"
if (( SPOT_READY_TIMEOUT_MS < 1000 )); then
  SPOT_READY_TIMEOUT_MS=1000
fi
SERVER_READY_TIMEOUT_MS="${PERF_MULTI_SERVER_READY_TIMEOUT_MS:-10000}"
SERVER_SHUTDOWN_TIMEOUT_MS="${PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS:-5000}"
RESULT_TIMEOUT_SECONDS="${PERF_MULTI_TIMEOUT_SECONDS:-60}"
TIMEOUT_SECONDS_DISPLAY="${PERF_MULTI_TIMEOUT_SECONDS:-${PERF_TIMEOUT_SECONDS:-auto}}"
TRANSPORT_TRANSITION_MS="${PERF_MULTI_TRANSPORT_TRANSITION_MS:-3000}"
PATTERN_TRANSITION_MS="${PERF_MULTI_PATTERN_TRANSITION_MS:-3000}"
RESULTS_TAG=""
CONFIGURATION="${PERF_CONFIGURATION:-Release}"
REPORT=""
REUSE_BUILD=0
CLEAN_BUILD=0
BUILD_DIR=""
OUTPUT_PATH=""
PIN_CPU=0
COMMON_IO_THREADS="${PERF_IO_THREADS:-}"
SERVER_IO_THREADS="${PERF_MULTI_SERVER_IO_THREADS:-${PERF_SERVER_IO_THREADS:-}}"
CLIENT_IO_THREADS="${PERF_MULTI_CLIENT_IO_THREADS:-${PERF_CLIENT_IO_THREADS:-}}"
HWM="${PERF_MULTI_HWM:-${PERF_HWM:-}}"
SNDHWM="${PERF_MULTI_SNDHWM:-${PERF_SNDHWM:-}}"
RCVHWM="${PERF_MULTI_RCVHWM:-${PERF_RCVHWM:-}}"
SNDBUF="${PERF_MULTI_SNDBUF:-${PERF_SNDBUF:-}}"
RCVBUF="${PERF_MULTI_RCVBUF:-${PERF_RCVBUF:-}}"
SNDTIMEO_MS="${PERF_MULTI_SNDTIMEO_MS:-${PERF_SNDTIMEO_MS:-200}}"
RCVTIMEO_MS="${PERF_MULTI_RCVTIMEO_MS:-${PERF_RCVTIMEO_MS:-200}}"
CONNECT_CONCURRENCY="${PERF_MULTI_CONNECT_CONCURRENCY:-${PERF_CONNECT_CONCURRENCY:-}}"
MONITOR_HWM="${PERF_MULTI_MONITOR_HWM:-${PERF_MONITOR_HWM:-1000}}"
SERVER_BIND_PORT="${PERF_MULTI_SERVER_BIND_PORT:-${PERF_SERVER_BIND_PORT:-0}}"
CTX_AUTO_HWM_ENABLE="${PERF_CTX_AUTO_HWM_ENABLE:-1}"
CTX_AUTO_HWM_PROFILE="${PERF_MULTI_CTX_AUTO_HWM_PROFILE:-${PERF_CTX_AUTO_HWM_PROFILE:-balanced}}"
ALLOW_MANUAL_SOCKET_OVERRIDES="${PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES:-${PERF_ALLOW_MANUAL_SOCKET_OVERRIDES:-0}}"
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

prune_report_dir() {
  local report_dir="$1"
  local max_files="${2:-100}"
  python3 - "${report_dir}" "${max_files}" <<'PY'
import pathlib
import sys

report_dir = pathlib.Path(sys.argv[1])
max_files = int(sys.argv[2])
if max_files <= 0 or not report_dir.exists():
    raise SystemExit(0)

files = sorted(
    [p for p in report_dir.iterdir() if p.is_file()],
    key=lambda p: p.name,
)
overflow = len(files) - max_files
for path in files[:max(0, overflow)]:
    try:
        path.unlink()
    except FileNotFoundError:
        pass
PY
}

resolve_perf_binary() {
  local project_dir="$1"
  local assembly_name="$2"
  local binary_path="${project_dir}/bin/${CONFIGURATION}/net8.0/${assembly_name}"
  local dll_path="${project_dir}/bin/${CONFIGURATION}/net8.0/${assembly_name}.dll"
  if [[ -x "${binary_path}" ]]; then
    printf '%s' "${binary_path}"
    return 0
  fi
  if [[ -f "${dll_path}" ]]; then
    printf 'dotnet %q' "${dll_path}"
    return 0
  fi
  return 1
}

usage() {
  cat <<'USAGE'
Usage: perf/multi/run_benchmarks.sh [options]

Run .NET multi-socket benchmark patterns.

Options:
  -h, --help            Show this help.
  --pattern NAME        Pattern list (comma-separated) or ALL.
  --duration N          Active duration seconds (default: 5).
  --msg-sizes LIST      Message size list.
  --transports LIST     Transport list override (default: tcp,tls,ws,wss).
  --clients N           Client socket count (default: 100, stream=10000).
  --runs N              Iterations per configuration (default: 1).
  --build-dir PATH      Accepted for policy compatibility.
  --reuse-build         Reuse existing build output.
  --clean-build         Remove project bin/obj before build.
  --output PATH         Tee report output to PATH.
  --pin-cpu             Pin benchmark processes to CPU 1 on Linux.
  --io-threads N        Set both server/client io threads.
  --server-io-threads N Server io threads override.
  --client-io-threads N Client io threads override.
  --hwm N               Debug-only manual HWM override.
  --send-hwm N          Send HWM override.
  --recv-hwm N          Receive HWM override.
  --buf SIZE            Send/receive buffer override.
  --sndbuf SIZE         Send buffer override.
  --rcvbuf SIZE         Receive buffer override.
  --sndtimeo N          Send timeout ms.
  --rcvtimeo N          Receive timeout ms.
  --send-timeout-ms N   Alias of --sndtimeo.
  --recv-timeout-ms N   Alias of --rcvtimeo.
  --connect-concurrency N Client connect concurrency.
  --transport-transition-ms N Transport cooldown.
  --pattern-transition-ms N Pattern cooldown.
  --server-ready-timeout-ms N Server ready timeout.
  --connect-ready-timeout-ms N Client connect-ready timeout.
  --monitor-hwm N       Monitor socket HWM.
  --server-shutdown-timeout-ms N Server shutdown timeout.
  --server-bind-port N  Fixed bind port (0=auto).
  --auto-hwm-profile NAME Auto-HWM profile.
  --results-dir PATH    Override result root directory.
  --results-tag NAME    Optional report suffix tag.

Notes:
  - result is saved under results/multi/report/ as
    perf_dotnet_multi_<platform>_YYYYMMDD_HHMMSS[_<tag>].txt.
USAGE
}

ensure_build_output() {
  if [[ "${CLEAN_BUILD}" -eq 1 ]]; then
    rm -rf "${PROJECT_DIR}/bin" "${PROJECT_DIR}/obj"
  fi
  if [[ "${REUSE_BUILD}" -eq 1 ]]; then
    return
  fi

  dotnet build "${PROJECT}" -c "${CONFIGURATION}" >/dev/null
}

prepare_core_runtime() {
  if [[ ! -f "${CORE_LIB}" ]]; then
    echo "core runtime not found: ${CORE_LIB}" >&2
    echo "Build core/build before running dotnet perf." >&2
    exit 1
  fi
  if find "${REPO_DIR}/core/include" "${REPO_DIR}/core/src" \
      -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' \) \
      -newer "${CORE_LIB}" -print -quit | grep -q .; then
    echo "core runtime is older than core source: ${CORE_LIB}" >&2
    echo "Run: cmake --build core/build" >&2
    exit 1
  fi
  echo "Perf runtime libzlink: ${CORE_LIB}"
  export ZLINK_LIBRARY_PATH="${CORE_LIB}"
  zlink_sync_linux_native_dirs_by_find "${PROJECT_DIR}/bin" '*linux-x64/native'
}

ensure_stream_client() {
  if [[ -x "${STREAM_CLIENT}" ]]; then
    return
  fi

  cmake -S "${REPO_DIR}/bindings/c" -B "${STREAM_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LTO=OFF >/dev/null
  cmake --build "${STREAM_BUILD_DIR}" --target perf_stream_client >/dev/null
}

normalize_platform() {
  case "$(uname -s)" in
    Linux*) printf 'linux' ;;
    Darwin*) printf 'macos' ;;
    MINGW*|MSYS*|CYGWIN*) printf 'windows' ;;
    *) uname -s | tr '[:upper:]' '[:lower:]' ;;
  esac
}

print_line() {
  local line="${1:-}"
  if [[ -n "${OUTPUT_PATH}" ]]; then
    printf '%s\n' "${line}" | tee -a "${REPORT}" "${OUTPUT_PATH}"
  else
    printf '%s\n' "${line}" | tee -a "${REPORT}"
  fi
}

# ITEM 1: byte-identical META block, mirroring the C multi engine
# (bindings/c/perf/run_comparison.py build_meta_items/print_meta_lines).
# Emits the same META,<key>,<value> lines and trailing blank line so the
# dotnet multi report head matches the C multi reference report exactly.
print_meta_block() {
  local meta_clients="${1:-}"
  local os_label cpu_label cores build_label commit_sha ts load_avg
  os_label="$(python3 -c 'import platform;s=platform.system();r=platform.release();print(f"{s} {r}" if s and r else platform.platform())')"
  cpu_label="$(python3 - <<'PY'
import platform, subprocess
def cpu():
    try:
        if platform.system() == "Linux":
            with open("/proc/cpuinfo", "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    if line.lower().startswith("model name"):
                        p = line.split(":", 1)
                        if len(p) == 2 and p[1].strip():
                            return p[1].strip()
        elif platform.system() == "Darwin":
            o = subprocess.check_output(["sysctl","-n","machdep.cpu.brand_string"],text=True,stderr=subprocess.DEVNULL).strip()
            if o:
                return o
        c = platform.processor().strip()
        if c:
            return c
    except Exception:
        pass
    return "unknown"
print(cpu())
PY
)"
  cores="$(python3 -c 'import os;print(os.cpu_count() or 0)')"
  build_label="Release"
  commit_sha="$(git -C "${REPO_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  ts="$(python3 -c 'import datetime;print(datetime.datetime.now().astimezone().isoformat(timespec="seconds"))')"
  load_avg="$(python3 -c 'import os;v=os.getloadavg();print(" ".join(f"{x:.2f}" for x in v))' 2>/dev/null || echo "")"
  print_line "META,os,${os_label}"
  print_line "META,cpu,${cpu_label}"
  print_line "META,cores,${cores}"
  print_line "META,build,${build_label}"
  print_line "META,commit,${commit_sha}"
  print_line "META,timestamp,${ts}"
  if [[ -n "${load_avg}" ]]; then
    print_line "META,load_avg,${load_avg}"
  fi
  print_line "META,runs,${RUNS}"
  if [[ -n "${meta_clients}" ]]; then
    print_line "META,clients,${meta_clients}"
  fi
  print_line ""
}

sleep_ms() {
  local ms="${1:-0}"
  sleep "$(printf '%d.%03d' "$(( ms / 1000 ))" "$(( ms % 1000 ))")"
}

validate_uint() {
  local label="${1:-value}"
  local value="${2:-}"
  if [[ ! "${value}" =~ ^[0-9]+$ || "${value}" -lt 1 ]]; then
    echo "${label} must be a positive integer." >&2
    exit 1
  fi
}

validate_nonnegative_uint() {
  local label="${1:-value}"
  local value="${2:-}"
  if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
    echo "${label} must be a non-negative integer." >&2
    exit 1
  fi
}

validate_byte_size_token() {
  local label="${1:-value}"
  local value="${2:-}"
  if [[ -n "${value}" && ! "${value}" =~ ^[0-9]+([bBkKmMgG])?$ ]]; then
    echo "${label} must be a byte size token such as 64b, 1k, or 64k." >&2
    exit 1
  fi
}

require_arg() {
  local option="${1:-option}"
  local value="${2:-}"
  if [[ -z "${value}" || "${value}" == --* ]]; then
    echo "Error: ${option} requires a value." >&2
    exit 1
  fi
}

normalize_multi_pattern_csv() {
  local raw="${1:-}"

  python3 - "${raw}" <<'PY'
import sys

raw = sys.argv[1].upper()
allowed = {
    "DEALER_DEALER",
    "DEALER_ROUTER",
    "DEALER_ROUTER_REQREP",
    "ROUTER_ROUTER",
    "ROUTER_ROUTER_REQREP",
    "PUBSUB",
    "STREAM",
}

if raw == "ALL":
    print(
        "MULTI_DEALER_DEALER,MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER,"
        "MULTI_DEALER_ROUTER_REQREP,MULTI_ROUTER_ROUTER_REQREP,"
        "MULTI_PUBSUB,MULTI_STREAM"
    )
    raise SystemExit(0)

items = []
for token in raw.split(","):
    value = token.strip()
    if not value:
        continue
    if value.startswith("MULTI_"):
        value = value[len("MULTI_") :]
    if value == "STREAMS":
        value = "STREAM"
    if value not in allowed:
        raise SystemExit(f"unsupported multi pattern: {value}")
    items.append(f"MULTI_{value}")

if not items:
    raise SystemExit("no valid multi pattern specified")

print(",".join(items))
PY
}

effective_msg_sizes_display() {
  local patterns_csv="${1:-}"
  local explicit_sizes="${2:-}"
  if [[ -n "${explicit_sizes}" ]]; then
    printf '%s' "${explicit_sizes}"
    return
  fi

  python3 - "${patterns_csv}" "${STREAM_MSG_SIZES:-${DEFAULT_MULTI_STREAM_MSG_SIZES}}" <<'PY'
import sys

patterns = [item.strip() for item in sys.argv[1].split(",") if item.strip()]
stream_sizes = [int(item.strip()) for item in sys.argv[2].split(",") if item.strip()]
sizes = set()
for pattern in patterns:
    if pattern == "MULTI_STREAM":
        sizes.update(stream_sizes)
    else:
        sizes.update([64, 256, 1024, 4096, 65536, 131072])
print(",".join(str(v) for v in sorted(sizes)))
PY
}

effective_clients_display() {
  local patterns_csv="${1:-}"
  local explicit_clients="${2:-}"
  if [[ -n "${explicit_clients}" ]]; then
    printf '%s' "${explicit_clients}"
    return
  fi

  python3 - "${patterns_csv}" "${EFFECTIVE_DEFAULT_CLIENTS}" "${EFFECTIVE_DEFAULT_STREAM_CLIENTS}" <<'PY'
import sys

patterns = [item.strip() for item in sys.argv[1].split(",") if item.strip()]
default_clients = sys.argv[2]
default_stream_clients = sys.argv[3]
if patterns and all(item == "MULTI_STREAM" for item in patterns):
    print(default_stream_clients)
elif any(item == "MULTI_STREAM" for item in patterns):
    print(f"{default_clients} (stream={default_stream_clients})")
else:
    print(default_clients)
PY
}

default_msg_sizes_for_pattern() {
  local pattern="${1:-}"
  if [[ "${pattern}" == "MULTI_STREAM" ]]; then
    printf '%s' "${STREAM_MSG_SIZES:-${DEFAULT_MULTI_STREAM_MSG_SIZES}}"
  else
    printf '%s' "64,256,1024,4096,65536,131072"
  fi
}

msg_sizes_for_pattern() {
  local pattern="${1:-}"
  local configured_sizes="${2:-}"
  if [[ -z "${configured_sizes}" ]]; then
    default_msg_sizes_for_pattern "${pattern}"
    return
  fi
  if [[ "${pattern}" != "MULTI_STREAM" ]]; then
    printf '%s' "${configured_sizes}"
    return
  fi

  python3 - "${configured_sizes}" "${STREAM_MSG_SIZES:-${DEFAULT_MULTI_STREAM_MSG_SIZES}}" <<'PY'
import sys

allowed = {item.strip() for item in sys.argv[2].split(",") if item.strip()}
items = []
for raw in sys.argv[1].split(","):
    value = raw.strip()
    if value in allowed and value not in items:
        items.append(value)
if not items:
    items = [item.strip() for item in sys.argv[2].split(",") if item.strip()]
print(",".join(items))
PY
}

default_clients_for_pattern() {
  local pattern="${1:-}"
  if [[ "${pattern}" == "MULTI_STREAM" ]]; then
    printf '%s' "${EFFECTIVE_DEFAULT_STREAM_CLIENTS}"
  else
    printf '%s' "${EFFECTIVE_DEFAULT_CLIENTS}"
  fi
}

pattern_uses_control_pipe() {
  local pattern="${1:-}"
  case "${pattern}" in
    MULTI_DEALER_DEALER|MULTI_PUBSUB|MULTI_STREAM)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

wait_for_ready_endpoint() {
  local log_path="$1"
  local timeout_ms="${2:-${SERVER_READY_TIMEOUT_MS}}"
  python3 - "${log_path}" "${timeout_ms}" <<'PY'
import pathlib
import sys
import time

path = pathlib.Path(sys.argv[1])
deadline = time.time() + max(0, int(sys.argv[2])) / 1000.0
while time.time() < deadline:
    if path.exists():
        text = path.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if line.startswith("READY,"):
                print(line.split(",", 1)[1].strip())
                raise SystemExit(0)
            if "multi_server_error:" in line:
                raise SystemExit(1)
    time.sleep(0.05)
raise SystemExit(1)
PY
}

wait_for_client_ready_line() {
  local log_path="$1"
  local timeout_ms="${2:-${READY_TIMEOUT_MS}}"
  python3 - "${log_path}" "${timeout_ms}" <<'PY'
import pathlib
import sys
import time

path = pathlib.Path(sys.argv[1])
deadline = time.time() + max(0, int(sys.argv[2])) / 1000.0
while time.time() < deadline:
    if path.exists():
        text = path.read_text(encoding="utf-8", errors="replace")
        if "CLIENT_READY," in text:
            raise SystemExit(0)
        if "multi_client_error:" in text:
            raise SystemExit(1)
    time.sleep(0.05)
raise SystemExit(1)
PY
}

wait_for_control_ready_endpoint() {
  local log_path="$1"
  local timeout_ms="${2:-${SERVER_READY_TIMEOUT_MS}}"
  python3 - "${log_path}" "${timeout_ms}" <<'PY'
import pathlib
import sys
import time

path = pathlib.Path(sys.argv[1])
deadline = time.time() + max(0, int(sys.argv[2])) / 1000.0
while time.time() < deadline:
    if path.exists():
        text = path.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if line.startswith("CONTROL_READY,"):
                print(line.split(",", 1)[1].strip())
                raise SystemExit(0)
            if "multi_server_error:" in line:
                raise SystemExit(1)
    time.sleep(0.05)
raise SystemExit(1)
PY
}

wait_for_client_control_endpoint() {
  local log_path="$1"
  local timeout_ms="${2:-${READY_TIMEOUT_MS}}"
  python3 - "${log_path}" "${timeout_ms}" <<'PY'
import pathlib
import sys
import time

path = pathlib.Path(sys.argv[1])
deadline = time.time() + max(0, int(sys.argv[2])) / 1000.0
while time.time() < deadline:
    if path.exists():
        text = path.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if line.startswith("CLIENT_CONTROL_ENDPOINT,"):
                print(line.split(",", 1)[1].strip())
                raise SystemExit(0)
        if "multi_client_error:" in text:
            raise SystemExit(1)
    time.sleep(0.05)
raise SystemExit(1)
PY
}

wait_for_control_connected() {
  local log_path="$1"
  local timeout_ms="${2:-${READY_TIMEOUT_MS}}"
  python3 - "${log_path}" "${timeout_ms}" <<'PY'
import pathlib
import sys
import time

path = pathlib.Path(sys.argv[1])
deadline = time.time() + max(0, int(sys.argv[2])) / 1000.0
while time.time() < deadline:
    if path.exists():
        text = path.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if line.startswith("CONTROL_CONNECTED,"):
                print(line.split(",", 1)[1].strip())
                raise SystemExit(0)
    time.sleep(0.05)
raise SystemExit(1)
PY
}

wait_for_results_from_logs() {
  local primary_log="${1:-}"
  local secondary_log="${2:-}"
  local pattern="${3:-}"
  local transport="${4:-}"
  local size="${5:-}"
  local timeout_seconds="${6:-${RESULT_TIMEOUT_SECONDS}}"
  local deadline=$((SECONDS + timeout_seconds))
  local extracted=""

  while (( SECONDS < deadline )); do
    if extracted="$(
      extract_results_from_logs \
        "${primary_log}" "${secondary_log}" "${pattern}" "${transport}" "${size}" \
        2>/dev/null
    )"; then
      printf '%s\n' "${extracted}"
      return 0
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

terminate_pid() {
  local pid="$1"
  if ! kill -0 "${pid}" 2>/dev/null; then
    return 0
  fi
  kill "${pid}" 2>/dev/null || true
  if wait_for_pid "${pid}" 2; then
    return 0
  fi
  kill -9 "${pid}" 2>/dev/null || true
  wait "${pid}" 2>/dev/null || true
}

wait_for_pid_exit_zero() {
  local pid="$1"
  local timeout_seconds="$2"
  local label="${3:-process}"
  if ! wait_for_pid "${pid}" "${timeout_seconds}"; then
    echo "${label} did not exit within ${timeout_seconds}s" >&2
    terminate_pid "${pid}"
    return 124
  fi

  local rc=0
  wait "${pid}" 2>/dev/null || rc=$?
  if [[ "${rc}" -ne 0 ]]; then
    echo "${label} exited with status ${rc}" >&2
    return "${rc}"
  fi
  return 0
}

terminate_running_pid_or_fail_if_exited() {
  local pid="$1"
  local timeout_seconds="$2"
  local label="${3:-process}"
  if ! wait_for_pid "${pid}" "${timeout_seconds}"; then
    terminate_pid "${pid}"
    return 0
  fi

  local rc=0
  wait "${pid}" 2>/dev/null || rc=$?
  if [[ "${rc}" -ne 0 ]]; then
    echo "${label} exited with status ${rc}" >&2
    return "${rc}"
  fi
  return 0
}

shutdown_timeout_seconds() {
  printf '%s' "$(( (SERVER_SHUTDOWN_TIMEOUT_MS + 999) / 1000 ))"
}

write_control_line() {
  local fd="$1"
  shift
  printf "$@" >&${fd} 2>/dev/null || true
}

extract_results_from_logs() {
  local primary_log="${1:-}"
  local secondary_log="${2:-}"
  local pattern="${3:-}"
  local transport="${4:-}"
  local size="${5:-}"

  python3 - "${primary_log}" "${secondary_log}" "${pattern}" "${transport}" "${size}" <<'PY'
import csv
import sys
from pathlib import Path

primary = Path(sys.argv[1])
secondary = Path(sys.argv[2])
expected = sys.argv[3]
transport = sys.argv[4]
size = sys.argv[5]
base = expected[len("MULTI_") :] if expected.startswith("MULTI_") else expected
required = ["throughput", "bandwidth", "latency", "latency_p95", "latency_p99"]
merged = {}

for path in (primary, secondary):
    if not path.exists():
        continue
    with path.open(encoding="utf-8", errors="replace") as handle:
        reader = csv.reader(handle)
        for row in reader:
            if len(row) != 7 or row[0] != "RESULT" or row[1] not in {"dotnet", "current"}:
                continue
            if row[2] not in {expected, base} or row[3] != transport or row[4] != size:
                continue
            metric = row[5]
            if metric in required:
                row[1] = "dotnet"
                row[2] = expected
                merged[metric] = row

missing = [metric for metric in required if metric not in merged]
if missing:
    raise SystemExit("missing required metrics: " + ",".join(missing))

for metric in required:
    print(",".join(merged[metric]))
PY
}

emit_result_row() {
  local metrics_file="${1:-}"
  local pattern="${2:-}"

  if [[ ! -s "${metrics_file}" ]]; then
    return
  fi

  python3 - "${metrics_file}" "${pattern}" <<'PY'
import csv
import sys

pattern = sys.argv[2].upper()
echo_patterns = {
    "MULTI_DEALER_ROUTER",
    "MULTI_DEALER_ROUTER_REQREP",
    "MULTI_ROUTER_ROUTER",
    "MULTI_ROUTER_ROUTER_REQREP",
    "MULTI_STREAM",
}
metrics = {}
size = ""
with open(sys.argv[1], encoding="utf-8") as handle:
    reader = csv.reader(handle)
    for row in reader:
        if len(row) != 7 or row[0] != "RESULT":
            continue
        size = row[4]
        metrics[row[5]] = row[6]

throughput_unit = "Kops/s" if pattern in echo_patterns else "Kmsg/s"
throughput = float(metrics["throughput"]) / 1000.0
bandwidth = float(metrics["bandwidth"])
latency_ms = float(metrics["latency"])
latency_p95_ms = float(metrics["latency_p95"])
latency_p99_ms = float(metrics["latency_p99"])
throughput_text = f"{throughput:8.3f} {throughput_unit}"
print(
    f"      | {size + 'B':<8} | {throughput_text:>16} | {bandwidth:>10.3f} MB/s |"
    f" {latency_ms:>9.3f} ms | {latency_p95_ms:>9.3f} ms | {latency_p99_ms:>9.3f} ms |"
)
PY
}

# C parity: bindings/c/perf/run_comparison.py:3082-3088. Override the live
# SPOT pass's latency/p95/p99 RESULT rows with the values measured by the
# clean (paced, latency-only) second pass; throughput/bandwidth keep the
# live (saturated) numbers. arg1: live block; arg2: clean block.
merge_spot_clean_latency() {
  local live_block="${1:-}"
  local clean_block="${2:-}"
  python3 - "${live_block}" "${clean_block}" <<'PY'
import csv
import io
import sys

live_text = sys.argv[1]
clean_text = sys.argv[2]

LATENCY_METRICS = {"latency", "latency_p95", "latency_p99"}


def parse(text):
    rows = []
    for row in csv.reader(io.StringIO(text)):
        if len(row) == 7 and row[0] == "RESULT":
            rows.append(row)
    return rows


clean_by_metric = {}
for row in parse(clean_text):
    if row[5] in LATENCY_METRICS:
        clean_by_metric[row[5]] = row[6]

out = io.StringIO()
writer = csv.writer(out, lineterminator="\n")
for row in parse(live_text):
    if row[5] in clean_by_metric:
        row = list(row)
        row[6] = clean_by_metric[row[5]]
    writer.writerow(row)
sys.stdout.write(out.getvalue())
PY
}

emit_failure_row() {
  local size="${1:-}"
  print_line "      | ${size}B      | FAIL               | FAIL           | FAIL          | FAIL          | FAIL          |"
}

extract_unsupported_line() {
  local pattern="${1:-}"
  local transport="${2:-}"
  shift 2

  python3 - "${pattern}" "${transport}" "$@" <<'PY'
import pathlib
import sys

expected = sys.argv[1]
transport = sys.argv[2]
base = expected[len("MULTI_"):] if expected.startswith("MULTI_") else expected
needles = {
    f"UNSUPPORTED,dotnet,{expected},{transport}",
    f"UNSUPPORTED,dotnet,{base},{transport}",
}
canonical = f"UNSUPPORTED,dotnet,{expected},{transport}"

for raw_path in sys.argv[3:]:
    path = pathlib.Path(raw_path)
    if not path.exists():
        continue
    text = path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        if line.strip() in needles:
            print(canonical)
            raise SystemExit(0)
    if transport in {"tcp", "tls", "ws", "wss", "ipc"}:
        lowered = text.lower()
        if "errno 98" in lowered or "address already in use" in lowered:
            raise SystemExit(1)
        if ("permission denied" in lowered
                or "operation not permitted" in lowered
                or "zlinkconnectexception" in lowered
                or "errno 1" in lowered
                or "errno 13" in lowered):
            print(canonical)
            raise SystemExit(0)

raise SystemExit(1)
PY
}

emit_auto_hwm_detail_table() {
  local pattern_name="${1:-}"
  shift || true

  python3 - "${pattern_name}" "$@" <<'PY'
import pathlib
import sys

SPOT_CONTROL_PATTERNS = {"SPOT", "SPOT_REQREP", "SPOT_SENDSEND"}


def normalize_pattern(name):
    value = (name or "").strip().upper()
    if value.startswith("MULTI_"):
        value = value[6:]
    return value


def parse_int(value, default=0):
    try:
        return int(str(value).strip())
    except (TypeError, ValueError):
        return default


def bytes_to_kb(value):
    parsed = parse_int(value, -1)
    return "" if parsed < 0 else str(parsed // 1024)


def parse_detail_line(line):
    stripped = (line or "").strip()
    if not stripped.startswith("AUTO_HWM_DETAIL,"):
        return None
    fields = {}
    for item in stripped.split(",")[1:]:
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        fields[key.strip()] = value.strip()
    return fields


def cell_widths(rows, columns):
    widths = []
    for header, key in columns:
        width = len(header)
        for row in rows:
            width = max(width, len(str(row.get(key, "?"))))
        widths.append(width)
    return widths


def emit_markdown_table(indent, columns, rows):
    widths = cell_widths(rows, columns)
    header_cells = [
        f" {header:<{widths[index]}} "
        for index, (header, _key) in enumerate(columns)
    ]
    sep_cells = ["-" * (width + 2) for width in widths]
    print(f"{indent}|" + "|".join(header_cells) + "|")
    print(f"{indent}|" + "|".join(sep_cells) + "|")
    for row in rows:
        cells = [
            f" {str(row.get(key, '?')):<{widths[index]}} "
            for index, (_header, key) in enumerate(columns)
        ]
        print(f"{indent}|" + "|".join(cells) + "|")


def active_hwm_fields(row):
    socket_type = (row.get("socket_type") or row.get("type") or "").lower()
    role = (row.get("role") or "").lower()
    send_active = True
    recv_active = True
    if socket_type in ("pub", "xpub") and role in ("spot_data", "control"):
        recv_active = False
    if socket_type in ("sub", "xsub") and role in ("recv_ingress", "control"):
        send_active = False
    return send_active, recv_active


def apply_active_hwm_display(row):
    display = dict(row)
    send_active, recv_active = active_hwm_fields(display)
    if not send_active:
        display["sndhwm"] = "-"
    if not recv_active:
        display["rcvhwm"] = "-"
    return display


def spot_snapshot_table(title, rows):
    if not rows:
        return False
    display_rows = []
    seen = set()
    for row in sorted(
        rows,
        key=lambda item: (
            parse_int(item.get("msg_size", "0")),
            parse_int(item.get("owner_id", "0")),
            item.get("socket", ""),
            item.get("role", ""),
        ),
    ):
        display = dict(row)
        display["type"] = row.get("socket_type", "")
        display = apply_active_hwm_display(display)
        key = tuple(
            display.get(name, "")
            for name in (
                "msg_size",
                "effective_message_bytes",
                "socket",
                "type",
                "role",
                "sndhwm",
                "rcvhwm",
                "effective_sndbuf",
                "effective_rcvbuf",
            )
        )
        if key in seen:
            continue
        seen.add(key)
        display_rows.append(display)
    if not display_rows:
        return False
    print(f"    {title}:")
    grouped = {}
    order = []
    for row in display_rows:
        key = (row.get("msg_size", ""), row.get("effective_message_bytes", ""))
        if key not in grouped:
            grouped[key] = []
            order.append(key)
        grouped[key].append(row)
    for index, key in enumerate(order):
        msg_size, msg_unit = key
        print(f"      - Size(B)={msg_size}, MsgUnit(B)={msg_unit}")
        emit_markdown_table(
            "      ",
            (
                ("Socket", "socket"),
                ("Type", "type"),
                ("Role", "role"),
                ("SNDHWM", "sndhwm"),
                ("RCVHWM", "rcvhwm"),
                ("SNDBUF", "effective_sndbuf"),
                ("RCVBUF", "effective_rcvbuf"),
            ),
            grouped[key],
        )
        if index + 1 < len(order):
            print("      ")
    return True


def emit_spot_tables(rows):
    snapshot_rows = [
        row
        for row in rows
        if row.get("source") == "spotnode_snapshot" and row.get("socket")
    ]
    if not snapshot_rows:
        return False
    emitted = False
    emitted = spot_snapshot_table(
        "Auto-HWM spotnode",
        [row for row in snapshot_rows if row.get("owner") == "node"],
    ) or emitted
    emitted = spot_snapshot_table(
        "Auto-HWM spot handles",
        [row for row in snapshot_rows if row.get("owner") == "spot"],
    ) or emitted
    return emitted


def expected_hwm(row):
    unit_budget = parse_int(row.get("unit_budget_bytes", ""), 0)
    msg_unit = parse_int(row.get("effective_message_bytes", ""), 0)
    size_cap = parse_int(row.get("size_cap", ""), 0)
    if unit_budget <= 0 or msg_unit <= 0:
        return None
    hwm = (unit_budget + msg_unit - 1) // msg_unit
    hwm = max(1, hwm)
    if size_cap > 0:
        hwm = min(hwm, size_cap)
    return hwm


def expected_match_score(row):
    expected = expected_hwm(row)
    if expected is None:
        return 2
    sndhwm = parse_int(row.get("sndhwm", ""), -1)
    rcvhwm = parse_int(row.get("rcvhwm", ""), -1)
    visible = 0
    matches = 0
    if sndhwm >= 0:
        visible += 1
        if sndhwm == expected:
            matches += 1
    if rcvhwm >= 0:
        visible += 1
        if rcvhwm == expected:
            matches += 1
    if visible == 0:
        return 2
    return 0 if matches == visible else 1 if matches > 0 else 2


def select_non_spot_rows(rows):
    selected = {}
    for index, row in enumerate(rows):
        key = (
            row.get("msg_size", ""),
            row.get("component", ""),
            row.get("socket_type", ""),
            row.get("unit_budget_bytes", ""),
            row.get("effective_message_bytes", ""),
        )
        score = expected_match_score(row)
        previous = selected.get(key)
        if previous is None or score < previous[0] or (
            score == previous[0] and index > previous[1]
        ):
            selected[key] = (score, index, row)
    return [item[2] for item in selected.values()]


pattern = normalize_pattern(sys.argv[1])
seen = set()
rows = []
for raw_path in sys.argv[2:]:
    path = pathlib.Path(raw_path)
    if not path.exists():
        continue
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        fields = parse_detail_line(line)
        if not fields or normalize_pattern(fields.get("pattern", "")) != pattern:
            continue
        key = (
            fields.get("pattern", ""),
            fields.get("transport", ""),
            fields.get("component", ""),
            fields.get("label", ""),
            fields.get("msg_size", ""),
            fields.get("source", ""),
            fields.get("role", ""),
            fields.get("scope", ""),
            fields.get("sndhwm", ""),
            fields.get("rcvhwm", ""),
            fields.get("effective_message_bytes", ""),
            fields.get("effective_sndbuf", ""),
            fields.get("effective_rcvbuf", ""),
            fields.get("socket_message_slots", ""),
            fields.get("unit_budget_bytes", ""),
        )
        if key in seen:
            continue
        seen.add(key)
        rows.append(fields)

if not rows:
    raise SystemExit(0)

if pattern in SPOT_CONTROL_PATTERNS:
    emit_spot_tables(rows)
    raise SystemExit(0)

rows = [
    row for row in rows
    if row.get("msg_size", "") and row.get("msg_size", "") != "0"
]
if not rows:
    raise SystemExit(0)
rows.sort(
    key=lambda row: (
        parse_int(row.get("msg_size", ""), 0),
        row.get("component", ""),
        row.get("socket_type", ""),
    )
)
display_rows = []
seen_display = set()
for fields in select_non_spot_rows(rows):
    display = dict(fields)
    display["type"] = fields.get("socket_type", "")
    msg_size = fields.get("msg_size", "")
    display["msg_size_display"] = msg_size if msg_size and msg_size != "0" else "?"
    display["unit_budget_kb"] = bytes_to_kb(fields.get("unit_budget_bytes", ""))
    display["effective_sndbuf_kb"] = bytes_to_kb(fields.get("effective_sndbuf", ""))
    display["effective_rcvbuf_kb"] = bytes_to_kb(fields.get("effective_rcvbuf", ""))
    key = tuple(
        display.get(name, "")
        for name in (
            "msg_size_display",
            "component",
            "type",
            "unit_budget_kb",
            "effective_message_bytes",
            "sndhwm",
            "rcvhwm",
            "effective_sndbuf_kb",
            "effective_rcvbuf_kb",
        )
    )
    if key in seen_display:
        continue
    seen_display.add(key)
    display_rows.append(display)
if not display_rows:
    raise SystemExit(0)
print("    Auto-HWM detail:")
emit_markdown_table(
    "      ",
    (
        ("Size(B)", "msg_size_display"),
        ("Component", "component"),
        ("Type", "type"),
        ("UnitBudget(KB)", "unit_budget_kb"),
        ("MsgUnit(B)", "effective_message_bytes"),
        ("SNDHWM", "sndhwm"),
        ("RCVHWM", "rcvhwm"),
        ("SNDBUF(KB)", "effective_sndbuf_kb"),
        ("RCVBUF(KB)", "effective_rcvbuf_kb"),
    ),
    display_rows,
)
PY
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pattern)
      require_arg "$1" "${2:-}"
      PATTERN="${2:-}"
      shift
      ;;
    --transports)
      require_arg "$1" "${2:-}"
      TRANSPORTS="${2:-}"
      shift
      ;;
    --msg-sizes)
      require_arg "$1" "${2:-}"
      MSG_SIZES="${2:-}"
      shift
      ;;
    --clients)
      require_arg "$1" "${2:-}"
      CLIENTS="${2:-}"
      shift
      ;;
    --build-dir)
      require_arg "$1" "${2:-}"
      BUILD_DIR="${2:-}"
      shift
      ;;
    --reuse-build)
      REUSE_BUILD=1
      ;;
    --clean-build)
      CLEAN_BUILD=1
      ;;
    --output)
      require_arg "$1" "${2:-}"
      OUTPUT_PATH="${2:-}"
      shift
      ;;
    --pin-cpu)
      PIN_CPU=1
      ;;
    --io-threads)
      require_arg "$1" "${2:-}"
      COMMON_IO_THREADS="${2:-}"
      shift
      ;;
    --server-io-threads)
      require_arg "$1" "${2:-}"
      SERVER_IO_THREADS="${2:-}"
      shift
      ;;
    --client-io-threads)
      require_arg "$1" "${2:-}"
      CLIENT_IO_THREADS="${2:-}"
      shift
      ;;
    --hwm)
      require_arg "$1" "${2:-}"
      HWM="${2:-}"
      shift
      ;;
    --send-hwm)
      require_arg "$1" "${2:-}"
      SNDHWM="${2:-}"
      shift
      ;;
    --recv-hwm)
      require_arg "$1" "${2:-}"
      RCVHWM="${2:-}"
      shift
      ;;
    --buf)
      require_arg "$1" "${2:-}"
      SNDBUF="${2:-}"
      RCVBUF="${2:-}"
      shift
      ;;
    --sndbuf)
      require_arg "$1" "${2:-}"
      SNDBUF="${2:-}"
      shift
      ;;
    --rcvbuf)
      require_arg "$1" "${2:-}"
      RCVBUF="${2:-}"
      shift
      ;;
    --sndtimeo|--send-timeout-ms)
      require_arg "$1" "${2:-}"
      SNDTIMEO_MS="${2:-}"
      shift
      ;;
    --rcvtimeo|--recv-timeout-ms)
      require_arg "$1" "${2:-}"
      RCVTIMEO_MS="${2:-}"
      shift
      ;;
    --connect-concurrency)
      require_arg "$1" "${2:-}"
      CONNECT_CONCURRENCY="${2:-}"
      shift
      ;;
    --transport-transition-ms)
      require_arg "$1" "${2:-}"
      TRANSPORT_TRANSITION_MS="${2:-}"
      shift
      ;;
    --pattern-transition-ms)
      require_arg "$1" "${2:-}"
      PATTERN_TRANSITION_MS="${2:-}"
      shift
      ;;
    --server-ready-timeout-ms)
      require_arg "$1" "${2:-}"
      SERVER_READY_TIMEOUT_MS="${2:-}"
      shift
      ;;
    --connect-ready-timeout-ms)
      require_arg "$1" "${2:-}"
      READY_TIMEOUT_MS="${2:-}"
      shift
      ;;
    --monitor-hwm)
      require_arg "$1" "${2:-}"
      MONITOR_HWM="${2:-}"
      shift
      ;;
    --server-shutdown-timeout-ms)
      require_arg "$1" "${2:-}"
      SERVER_SHUTDOWN_TIMEOUT_MS="${2:-}"
      shift
      ;;
    --server-bind-port)
      require_arg "$1" "${2:-}"
      SERVER_BIND_PORT="${2:-}"
      shift
      ;;
    --auto-hwm-profile)
      require_arg "$1" "${2:-}"
      CTX_AUTO_HWM_PROFILE="${2:-}"
      shift
      ;;
    --duration)
      require_arg "$1" "${2:-}"
      DURATION="${2:-}"
      shift
      ;;
    --runs)
      require_arg "$1" "${2:-}"
      RUNS="${2:-}"
      shift
      ;;
    --runs=*)
      RUNS="${1#--runs=}"
      ;;
    --results-dir)
      require_arg "$1" "${2:-}"
      RESULTS_ROOT="${2:-}"
      shift
      ;;
    --results-tag)
      require_arg "$1" "${2:-}"
      RESULTS_TAG="${2:-}"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
  shift
done

if [[ "${REUSE_BUILD}" -eq 1 && "${CLEAN_BUILD}" -eq 1 ]]; then
  echo "--reuse-build and --clean-build are mutually exclusive." >&2
  exit 1
fi

validate_uint "--duration" "${DURATION}"
validate_uint "--runs" "${RUNS}"
validate_uint "PERF_MULTI_CONNECT_READY_TIMEOUT_MS" "${READY_TIMEOUT_MS}"
validate_nonnegative_uint "PERF_MULTI_SERVER_READY_TIMEOUT_MS" "${SERVER_READY_TIMEOUT_MS}"
validate_nonnegative_uint "PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS" "${SERVER_SHUTDOWN_TIMEOUT_MS}"
validate_uint "PERF_MULTI_TIMEOUT_SECONDS" "${RESULT_TIMEOUT_SECONDS}"
validate_nonnegative_uint "PERF_MULTI_TRANSPORT_TRANSITION_MS" "${TRANSPORT_TRANSITION_MS}"
validate_nonnegative_uint "PERF_MULTI_PATTERN_TRANSITION_MS" "${PATTERN_TRANSITION_MS}"
validate_nonnegative_uint "PERF_MULTI_MONITOR_HWM" "${MONITOR_HWM}"
validate_nonnegative_uint "PERF_MULTI_SERVER_BIND_PORT" "${SERVER_BIND_PORT}"
if (( SERVER_BIND_PORT > 65535 )); then
  echo "PERF_MULTI_SERVER_BIND_PORT must be in range 0..65535." >&2
  exit 1
fi
validate_uint "PERF_MULTI_SNDTIMEO_MS" "${SNDTIMEO_MS}"
validate_uint "PERF_MULTI_RCVTIMEO_MS" "${RCVTIMEO_MS}"
validate_byte_size_token "PERF_MULTI_SNDBUF" "${SNDBUF}"
validate_byte_size_token "PERF_MULTI_RCVBUF" "${RCVBUF}"
case "${CTX_AUTO_HWM_PROFILE}" in
  ""|compact|low_latency|low-latency|balanced|throughput) ;;
  *)
    echo "PERF_CTX_AUTO_HWM_PROFILE must be compact, low_latency, balanced, or throughput." >&2
    exit 1
    ;;
esac
case "${CTX_AUTO_HWM_ENABLE}" in
  0|1) ;;
  *)
    echo "PERF_CTX_AUTO_HWM_ENABLE must be 0 or 1." >&2
    exit 1
    ;;
esac
if [[ -n "${COMMON_IO_THREADS}" ]]; then
  validate_uint "--io-threads" "${COMMON_IO_THREADS}"
fi
if [[ -n "${SERVER_IO_THREADS}" ]]; then
  validate_uint "--server-io-threads" "${SERVER_IO_THREADS}"
fi
if [[ -n "${CLIENT_IO_THREADS}" ]]; then
  validate_uint "--client-io-threads" "${CLIENT_IO_THREADS}"
fi
if [[ -n "${CONNECT_CONCURRENCY}" ]]; then
  validate_uint "--connect-concurrency" "${CONNECT_CONCURRENCY}"
fi
if [[ -n "${HWM}" ]]; then
  validate_uint "--hwm" "${HWM}"
fi
if [[ -n "${SNDHWM}" ]]; then
  validate_uint "--send-hwm" "${SNDHWM}"
fi
if [[ -n "${RCVHWM}" ]]; then
  validate_uint "--recv-hwm" "${RCVHWM}"
fi
if [[ -n "${HWM}${SNDHWM}${RCVHWM}${SNDBUF}${RCVBUF}" && "${ALLOW_MANUAL_SOCKET_OVERRIDES}" != "1" ]]; then
  echo "Error: manual HWM/SNDBUF/RCVBUF overrides are debug-only." >&2
  echo "Set PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1 to use --hwm/--send-hwm/--recv-hwm/--buf/--sndbuf/--rcvbuf." >&2
  exit 1
fi

if [[ -n "${CLIENTS}" ]]; then
  validate_uint "--clients" "${CLIENTS}"
fi

if [[ -n "${MSG_SIZES}" && ! "${MSG_SIZES}" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
  echo "--msg-sizes must be a comma-separated list of positive integers." >&2
  exit 1
fi

if [[ ! "${TRANSPORTS}" =~ ^[a-z]+(,[a-z]+)*$ ]]; then
  echo "--transports must be a comma-separated list of transport names." >&2
  exit 1
fi

PATTERN="$(normalize_multi_pattern_csv "${PATTERN}")"
EFFECTIVE_MSG_SIZES_DISPLAY="$(effective_msg_sizes_display "${PATTERN}" "${MSG_SIZES}")"
EFFECTIVE_CLIENTS_DISPLAY="$(effective_clients_display "${PATTERN}" "${CLIENTS}")"
if [[ -n "${SERVER_IO_THREADS}" ]]; then
  display_server_io_threads="${SERVER_IO_THREADS}"
elif [[ -n "${COMMON_IO_THREADS}" ]]; then
  display_server_io_threads="${COMMON_IO_THREADS} (from PERF_IO_THREADS)"
elif [[ -n "${EFFECTIVE_DEFAULT_IO_THREADS}" ]]; then
  display_server_io_threads="${EFFECTIVE_DEFAULT_IO_THREADS} (default)"
else
  display_server_io_threads="4 (default)"
fi
if [[ -n "${CLIENT_IO_THREADS}" ]]; then
  display_client_io_threads="${CLIENT_IO_THREADS}"
elif [[ -n "${COMMON_IO_THREADS}" ]]; then
  display_client_io_threads="${COMMON_IO_THREADS} (from PERF_IO_THREADS)"
elif [[ -n "${EFFECTIVE_DEFAULT_IO_THREADS}" ]]; then
  display_client_io_threads="${EFFECTIVE_DEFAULT_IO_THREADS} (default)"
else
  display_client_io_threads="4 (default)"
fi
if [[ "${ALLOW_MANUAL_SOCKET_OVERRIDES}" == "1" ]]; then
  DISPLAY_HWM="${HWM:-auto-hwm}"
  DISPLAY_SNDHWM="${SNDHWM:-${HWM:-auto-hwm}}"
  DISPLAY_RCVHWM="${RCVHWM:-${HWM:-auto-hwm}}"
  DISPLAY_SNDBUF="${SNDBUF:--1}"
  DISPLAY_RCVBUF="${RCVBUF:--1}"
else
  DISPLAY_HWM="auto-hwm"
  DISPLAY_SNDHWM="auto-hwm"
  DISPLAY_RCVHWM="auto-hwm"
  DISPLAY_SNDBUF="-1"
  DISPLAY_RCVBUF="-1"
fi
display_connect_concurrency="${CONNECT_CONCURRENCY:-}"
if [[ -z "${display_connect_concurrency}" ]]; then
  if [[ "${EFFECTIVE_CLIENTS_DISPLAY}" =~ ^[0-9]+$ && "${EFFECTIVE_CLIENTS_DISPLAY}" -ge 10000 ]]; then
    display_connect_concurrency="1024 (default)"
  else
    display_connect_concurrency="128 (default)"
  fi
fi
display_pin_cpu="off"
if [[ "${PIN_CPU}" -eq 1 ]]; then
  display_pin_cpu="on"
fi
mkdir -p "${RESULTS_ROOT}/multi/tmp" "${RESULTS_ROOT}/multi/report"

platform="$(normalize_platform)"
timestamp="$(date +%Y%m%d_%H%M%S)"
report_base="perf_dotnet_multi_${platform}_${timestamp}"
if [[ -n "${RESULTS_TAG}" ]]; then
  report_base="${report_base}_${RESULTS_TAG}"
fi
REPORT="${RESULTS_ROOT}/multi/report/${report_base}.txt"
: > "${REPORT}"
if [[ -n "${OUTPUT_PATH}" ]]; then
  mkdir -p "$(dirname "${OUTPUT_PATH}")"
  : > "${OUTPUT_PATH}"
fi
prune_report_dir "${RESULTS_ROOT}/multi/report" \
  "${PERF_RESULTS_MAX_FILES:-100}"
FAILURES_FILE="${RESULTS_ROOT}/multi/tmp/${report_base}.failures.csv"
RESULT_DATA_FILE="${RESULTS_ROOT}/multi/tmp/${report_base}.result_data.csv"
: > "${FAILURES_FILE}"
: > "${RESULT_DATA_FILE}"

record_failure() {
  local pattern="${1:-}"
  local transport="${2:-}"
  local size="${3:-}"
  local run_index="${4:-}"
  local reason="${5:-}"
  printf '%s,%s,%s,%s,%s\n' \
    "${pattern}" "${transport}" "${size}" "${run_index}" "${reason}" >> "${FAILURES_FILE}"
  failure_count=$(( ${failure_count:-0} + 1 ))
  print_line "    Testing ${transport} | ${size}B:"
  emit_failure_row "${size}"
  if [[ "${PERF_FAIL_FAST:-0}" == "1" ]]; then
    stop_early=1
    status=1
  fi
}

run_multi_process() {
  local role="$1"
  local log_path="$2"
  local endpoint="${3:-}"
  local control_fd="${4:-}"
  local background="${5:-0}"
  shift 5
  local extra_args=("$@")
  local shell_cmd="${PERF_BINARY@Q} --multi-${role} ${pattern@Q} ${transport@Q} ${size@Q}"
  local role_io_threads="${COMMON_IO_THREADS:-${EFFECTIVE_DEFAULT_IO_THREADS:-4}}"
  if [[ "${role}" == "server" && -n "${SERVER_IO_THREADS}" ]]; then
    role_io_threads="${SERVER_IO_THREADS}"
  elif [[ "${role}" == "client" && -n "${CLIENT_IO_THREADS}" ]]; then
    role_io_threads="${CLIENT_IO_THREADS}"
  fi
  local effective_ready_timeout="${READY_TIMEOUT_MS}"
  if [[ "${pattern}" == "MULTI_SPOT" || "${pattern}" == "MULTI_SPOT_REQREP" ]]; then
    if [[ "${transport}" == "tls" || "${transport}" == "wss" ]]; then
      if (( effective_ready_timeout < 12000 )); then
        effective_ready_timeout=12000
      fi
    fi
  fi
  local normalized_pattern="${pattern#MULTI_}"
  local env_prefix=(
    "PERF_PATTERN=${normalized_pattern}"
    "PERF_MULTI_PATTERN=${normalized_pattern}"
    "PERF_MULTI_TRANSPORT=${transport}"
    "PERF_MULTI_COMPONENT=${role}"
    "PERF_DOTNET_SERVER_STATS=${PERF_DOTNET_SERVER_STATS:-0}"
    "PERF_DOTNET_TIMING=${PERF_DOTNET_TIMING:-0}"
    # Match bindings/c/perf/multi/common/perf_multi_runtime.hpp:54:
    # bench_io_threads() default = 4. .NET default was 0 (no override =>
    # zlink ctx default 1), which capped per-process to single-core
    # throughput vs C's multi-core internal IO workers.
    "PERF_IO_THREADS=${role_io_threads}"
    "PERF_MULTI_CLIENTS=${pattern_clients}"
    "PERF_MULTI_DURATION_SECONDS=${DURATION}"
    "PERF_MULTI_CONNECT_READY_TIMEOUT_MS=${effective_ready_timeout}"
    "PERF_MULTI_SERVER_READY_TIMEOUT_MS=${SERVER_READY_TIMEOUT_MS}"
    "PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS=${SERVER_SHUTDOWN_TIMEOUT_MS}"
    "PERF_MULTI_SERVER_BIND_PORT=${SERVER_BIND_PORT}"
    "PERF_MULTI_SNDTIMEO_MS=${SNDTIMEO_MS}"
    "PERF_MULTI_RCVTIMEO_MS=${RCVTIMEO_MS}"
    "PERF_MULTI_MONITOR_HWM=${MONITOR_HWM}"
    "PERF_CTX_AUTO_HWM_ENABLE=${CTX_AUTO_HWM_ENABLE}"
    "PERF_CTX_AUTO_HWM_PROFILE=${CTX_AUTO_HWM_PROFILE}"
    "DOTNET_TieredCompilation=1"
    "DOTNET_TC_QuickJitForLoops=1"
    "DOTNET_ReadyToRun=1"
  )
  if [[ -n "${CONNECT_CONCURRENCY}" ]]; then
    env_prefix+=("PERF_MULTI_CONNECT_CONCURRENCY=${CONNECT_CONCURRENCY}")
  fi
  if [[ "${ALLOW_MANUAL_SOCKET_OVERRIDES}" == "1" ]]; then
    env_prefix+=("PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1")
    [[ -n "${HWM}" ]] && env_prefix+=("PERF_MULTI_HWM=${HWM}")
    [[ -n "${SNDHWM}" ]] && env_prefix+=("PERF_MULTI_SNDHWM=${SNDHWM}")
    [[ -n "${RCVHWM}" ]] && env_prefix+=("PERF_MULTI_RCVHWM=${RCVHWM}")
    [[ -n "${SNDBUF}" ]] && env_prefix+=("PERF_MULTI_SNDBUF=${SNDBUF}")
    [[ -n "${RCVBUF}" ]] && env_prefix+=("PERF_MULTI_RCVBUF=${RCVBUF}")
  fi

  if [[ -n "${endpoint}" ]]; then
    shell_cmd+=" --endpoint ${endpoint@Q}"
  fi

  local extra_arg
  for extra_arg in "${extra_args[@]}"; do
    shell_cmd+=" ${extra_arg@Q}"
  done

  if [[ "${PIN_CPU}" -eq 1 && "$(uname -s)" == Linux* ]] \
    && command -v taskset >/dev/null 2>&1; then
    shell_cmd="taskset -c 1 ${shell_cmd}"
  fi

  if [[ "${background}" == "1" ]]; then
    if [[ -n "${control_fd}" ]]; then
      if [[ "${control_fd}" =~ ^[0-9]+$ ]]; then
        env "${env_prefix[@]}" bash -lc "${shell_cmd}" <&${control_fd} > "${log_path}" 2>&1 &
      else
        env "${env_prefix[@]}" bash -lc "${shell_cmd}" < "${control_fd}" > "${log_path}" 2>&1 &
      fi
    else
      env "${env_prefix[@]}" bash -lc "${shell_cmd}" > "${log_path}" 2>&1 &
    fi
    return 0
  fi

  if [[ -n "${control_fd}" ]]; then
    env "${env_prefix[@]}" bash -lc "${shell_cmd}" <&${control_fd} > "${log_path}" 2>&1
  else
    if command -v timeout >/dev/null 2>&1; then
      env "${env_prefix[@]}" timeout "${RESULT_TIMEOUT_SECONDS}s" \
        bash -lc "${shell_cmd}" > "${log_path}" 2>&1
    else
      env "${env_prefix[@]}" bash -lc "${shell_cmd}" > "${log_path}" 2>&1
    fi
  fi
}

run_external_stream_client() {
  local endpoint="$1"
  ensure_stream_client
  local stream_clients="${pattern_clients}"
  local non_tcp_max="${PERF_STREAM_NON_TCP_CLIENTS_MAX:-${PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX:-10000}}"
  if [[ "${transport}" != "tcp" && "${stream_clients}" =~ ^[0-9]+$ \
        && "${non_tcp_max}" =~ ^[0-9]+$ \
        && "${stream_clients}" -gt "${non_tcp_max}" ]]; then
    stream_clients="${non_tcp_max}"
  fi
  local cmd=(
    "${STREAM_CLIENT}" --transport "${transport}" --pattern STREAM
    --sizes "${size}" --runs 1 --duration "${DURATION}"
    --ccu "${stream_clients}" --send-stop-token 1 --endpoint "${endpoint}"
    --io-threads "${CLIENT_IO_THREADS:-${COMMON_IO_THREADS:-${EFFECTIVE_DEFAULT_IO_THREADS:-4}}}"
  )
  if [[ "${PIN_CPU}" -eq 1 && "$(uname -s)" == Linux* ]] \
    && command -v taskset >/dev/null 2>&1; then
    cmd=(taskset -c 1 "${cmd[@]}")
  fi
  env \
    "PERF_PATTERN=${pattern#MULTI_}" \
    "PERF_MULTI_PATTERN=${pattern#MULTI_}" \
    "PERF_MULTI_TRANSPORT=${transport}" \
    "PERF_MULTI_COMPONENT=client" \
    "${cmd[@]}" > "${client_log}" 2>&1
}

# C parity: bindings/c/perf/run_comparison.py:3062-3088 +
# bindings/c/perf/multi/src/perf_multi_spot_server.cpp:197,334-373.
# After the live (saturated) SPOT size case, run a fully isolated second
# size case with PERF_MULTI_SPOT_LATENCY_ONLY=1 so the server publishes one
# paced probe per interval over an idle link. The clean pass's
# latency/p95/p99 are then merged over the live result; throughput keeps
# the live numbers. Echoes the extracted clean RESULT block on success.
run_spot_clean_latency_pass() {
  local _pattern="$1"
  local _transport="$2"
  local _size="$3"
  local _run_index="$4"
  local cl_server_log="${RESULTS_ROOT}/multi/tmp/${_pattern,,}_${_transport}_${_size}_clean_server_run${_run_index}.log"
  local cl_client_log="${RESULTS_ROOT}/multi/tmp/${_pattern,,}_${_transport}_${_size}_clean_client_run${_run_index}.log"
  local cl_server_fifo="${RESULTS_ROOT}/multi/tmp/${_pattern,,}_${_transport}_${_size}_clean_server_run${_run_index}.ctl"
  local cl_client_fifo="${RESULTS_ROOT}/multi/tmp/${_pattern,,}_${_transport}_${_size}_clean_client_run${_run_index}.ctl"
  rm -f "${cl_server_log}" "${cl_client_log}" "${cl_server_fifo}" "${cl_client_fifo}"

  # C parity (run_comparison.py:3069-3071): the second pass forces
  # PERF_MULTI_SPOT_LATENCY_ONLY=1 and PERF_MULTI_SPOT_CLEAN_LATENCY=0
  # (the latter prevents recursion). Exported so the server child inherits
  # them (run_multi_process's env_prefix does not clear inherited env).
  export PERF_MULTI_SPOT_LATENCY_ONLY=1
  export PERF_MULTI_SPOT_CLEAN_LATENCY=0

  local rc=1
  local cl_server_pid=0 cl_client_pid=0
  local cl_server_fd='' cl_client_fd=''
  local cl_server_ep='' cl_control_ep='' cl_client_ctrl_ep='' cl_connected_ep=''

  mkfifo "${cl_server_fifo}"
  run_multi_process "server" "${cl_server_log}" "" "${cl_server_fifo}" 1
  cl_server_pid=$!
  exec {cl_server_fd}>"${cl_server_fifo}"
  rm -f "${cl_server_fifo}"

  if cl_server_ep="$(wait_for_ready_endpoint "${cl_server_log}" "${SERVER_READY_TIMEOUT_MS}")" \
     && cl_control_ep="$(wait_for_control_ready_endpoint "${cl_server_log}" "${SERVER_READY_TIMEOUT_MS}")"; then
    mkfifo "${cl_client_fifo}"
    run_multi_process "client" "${cl_client_log}" "${cl_server_ep}" "${cl_client_fifo}" 1 "--control-endpoint" "${cl_control_ep}"
    cl_client_pid=$!
    exec {cl_client_fd}>"${cl_client_fifo}"
    rm -f "${cl_client_fifo}"

    if cl_client_ctrl_ep="$(wait_for_client_control_endpoint "${cl_client_log}" "${SPOT_READY_TIMEOUT_MS}")"; then
      write_control_line "${cl_server_fd}" 'CONNECT_CONTROL,%s\n' "${cl_client_ctrl_ep}"
      if cl_connected_ep="$(wait_for_control_connected "${cl_server_log}" "${SPOT_READY_TIMEOUT_MS}")"; then
        write_control_line "${cl_client_fd}" 'CONTROL_CONNECTED,%s\n' "${cl_connected_ep}"
        if wait_for_client_ready_line "${cl_client_log}" "${SPOT_READY_TIMEOUT_MS}"; then
          write_control_line "${cl_server_fd}" 'START,%s\n' "${_size}"
          write_control_line "${cl_client_fd}" 'START,%s\n' "${_size}"
          local cl_extracted=''
          if cl_extracted="$(wait_for_results_from_logs \
              "${cl_client_log}" "${cl_server_log}" "${_pattern}" \
              "${_transport}" "${_size}" "${RESULT_TIMEOUT_SECONDS}")"; then
            printf '%s\n' "${cl_extracted}"
            rc=0
          fi
        fi
      fi
    fi
  fi

  [[ -n "${cl_client_fd}" ]] && write_control_line "${cl_client_fd}" 'STOP\n'
  [[ -n "${cl_server_fd}" ]] && write_control_line "${cl_server_fd}" 'STOP\n'
  if [[ "${cl_client_pid}" -ne 0 ]]; then
    wait_for_pid_exit_zero "${cl_client_pid}" "$(shutdown_timeout_seconds)" \
      "SPOT clean latency client" || rc=1
  fi
  if [[ "${cl_server_pid}" -ne 0 ]]; then
    wait_for_pid_exit_zero "${cl_server_pid}" "$(shutdown_timeout_seconds)" \
      "SPOT clean latency server" || rc=1
  fi
  [[ -n "${cl_client_fd}" ]] && exec {cl_client_fd}>&-
  [[ -n "${cl_server_fd}" ]] && exec {cl_server_fd}>&-

  unset PERF_MULTI_SPOT_LATENCY_ONLY
  unset PERF_MULTI_SPOT_CLEAN_LATENCY
  return "${rc}"
}

ensure_build_output
prepare_core_runtime

if ! PERF_BINARY="$(resolve_perf_binary "${PROJECT_DIR}" "Zlink.BindingBench.Multi")"; then
  echo "multi benchmark binary not found under ${PROJECT_DIR}/bin/${CONFIGURATION}/net8.0." >&2
  echo "Gate 1 build output is required before smoke." >&2
  exit 1
fi

# ITEM 1: META clients value mirrors C resolve_clients_meta:
# env PERF_MULTI_CLIENTS/PERF_CLIENTS if numeric; otherwise the configured
# default clients, with the stream default when every selected pattern is stream.
META_CLIENTS=""
if [[ "${CLIENTS:-}" =~ ^[0-9]+$ ]]; then
  META_CLIENTS="${CLIENTS}"
elif [[ "${PERF_CLIENTS:-}" =~ ^[0-9]+$ ]]; then
  META_CLIENTS="${PERF_CLIENTS}"
else
  _all_stream=1
  for _p in "${PATTERN//,/ }"; do
    case "${_p}" in
      MULTI_STREAM|STREAM) ;;
      *) _all_stream=0 ;;
    esac
  done
  if [[ "${_all_stream}" -eq 1 ]]; then
    META_CLIENTS="${EFFECTIVE_DEFAULT_STREAM_CLIENTS}"
  else
    META_CLIENTS="${EFFECTIVE_DEFAULT_CLIENTS}"
  fi
fi
print_meta_block "${META_CLIENTS}"

ROUTED_ECHO_BORROW_PAYLOAD="none"
case ",${PATTERN}," in
  *,MULTI_DEALER_ROUTER,*|*,MULTI_DEALER_ROUTER_REQREP,*|*,MULTI_ROUTER_ROUTER,*|*,MULTI_ROUTER_ROUTER_REQREP,*)
    case ",${TRANSPORTS}," in
      *,tcp,*) ROUTED_ECHO_BORROW_PAYLOAD="tcp" ;;
    esac
    ;;
esac

# C multi engine (bindings/c/perf/run_comparison.py print_effective_options)
# emits this block twice with identical body: once labelled "(start)" before
# the patterns and once labelled "(result)" right before "## Result Data".
# Emit byte-identically for both labels.
print_effective_options() {
  local label="$1"
  print_line "## Effective Options (${label})"
  print_line "- lang: dotnet"
  print_line "- suite: multi"
  print_line "- runs: ${RUNS}"
  print_line "- patterns: ${PATTERN}"
  print_line "- transports: ${TRANSPORTS}"
  print_line "- msg_sizes: ${EFFECTIVE_MSG_SIZES_DISPLAY}"
  print_line "- routed_echo_borrow_payload: ${ROUTED_ECHO_BORROW_PAYLOAD}"
  print_line "- duration_seconds: ${DURATION}"
  print_line "- fail_fast: ${PERF_FAIL_FAST:-0}"
  print_line "- clients: ${EFFECTIVE_CLIENTS_DISPLAY}"
  print_line "- default_clients: ${EFFECTIVE_DEFAULT_CLIENTS}"
  print_line "- default_stream_clients: ${EFFECTIVE_DEFAULT_STREAM_CLIENTS}"
  print_line "- service_clients: auto"
  print_line "- server_io_threads: ${display_server_io_threads}"
  print_line "- client_io_threads: ${display_client_io_threads}"
  print_line "- hwm: ${DISPLAY_HWM}"
  print_line "- sndhwm: ${DISPLAY_SNDHWM}"
  print_line "- rcvhwm: ${DISPLAY_RCVHWM}"
  print_line "- sndbuf: ${DISPLAY_SNDBUF}"
  print_line "- rcvbuf: ${DISPLAY_RCVBUF}"
  print_line "- ctx_auto_hwm_enable: ${CTX_AUTO_HWM_ENABLE}"
  print_line "- ctx_auto_hwm_profile: ${CTX_AUTO_HWM_PROFILE}"
  print_line "- sndtimeo_ms: ${SNDTIMEO_MS}"
  print_line "- rcvtimeo_ms: ${RCVTIMEO_MS}"
  print_line "- connect_concurrency: ${display_connect_concurrency}"
  print_line "- connect_ready_timeout_ms: ${READY_TIMEOUT_MS}"
  print_line "- monitor_hwm: ${MONITOR_HWM}"
  print_line "- server_ready_timeout_ms: ${SERVER_READY_TIMEOUT_MS}"
  print_line "- server_shutdown_timeout_ms: ${SERVER_SHUTDOWN_TIMEOUT_MS}"
  print_line "- server_bind_port: ${SERVER_BIND_PORT}"
  print_line "- transport_transition_ms: ${TRANSPORT_TRANSITION_MS}"
  print_line "- pattern_transition_ms: ${PATTERN_TRANSITION_MS}"
  print_line "- lat_timeout_ms: ${PERF_MULTI_LAT_TIMEOUT_MS:-5000}"
  print_line "- stream_non_tcp_clients_max: ${PERF_STREAM_NON_TCP_CLIENTS_MAX:-${PERF_MULTI_STREAM_NON_TCP_CLIENTS_MAX:-10000}}"
  print_line "- disable_resource_metrics: ${PERF_DISABLE_RESOURCE_METRICS:-0}"
  print_line "- timeout_seconds: ${TIMEOUT_SECONDS_DISPLAY}"
}

print_effective_options "start"

IFS=',' read -r -a patterns <<< "${PATTERN}"
IFS=',' read -r -a transports <<< "${TRANSPORTS}"

status=0
result_lines=0
expected_result_lines=0
failure_count=0
success_count=0
unsupported_count=0
skip_count=0
stop_early=0
for (( run_index=1; run_index<=RUNS; run_index++ )); do
  if [[ "${stop_early}" -eq 1 ]]; then
    break
  fi
  for pattern_index in "${!patterns[@]}"; do
    if [[ "${stop_early}" -eq 1 ]]; then
      break
    fi
    pattern="${patterns[pattern_index]}"
    pattern="${pattern//[[:space:]]/}"
    [[ -n "${pattern}" ]] || continue

    pattern_msg_sizes="$(msg_sizes_for_pattern "${pattern}" "${MSG_SIZES}")"
    pattern_clients="${CLIENTS}"
    if [[ -z "${pattern_clients}" ]]; then
      pattern_clients="$(default_clients_for_pattern "${pattern}")"
    fi

    IFS=',' read -r -a msg_sizes <<< "${pattern_msg_sizes}"
    pattern_kind="one-way"
    case "${pattern}" in
      MULTI_DEALER_ROUTER|MULTI_DEALER_ROUTER_REQREP|MULTI_ROUTER_ROUTER|MULTI_ROUTER_ROUTER_REQREP|MULTI_STREAM)
        pattern_kind="echo"
        ;;
    esac
    # ITEM 1: C multi engine prints the pattern separator only BETWEEN
    # patterns (pattern_idx > 0), with no leading separator/blank before the
    # first pattern and no blank line between Effective Options and the
    # first "## PATTERN".
    if [[ "${FIRST_PATTERN_EMITTED:-0}" -eq 1 ]]; then
      print_line ""
      print_line "==============================================================================="
      print_line ""
    fi
    FIRST_PATTERN_EMITTED=1
    print_line "## PATTERN: ${pattern} (${pattern_kind})"
    print_line "  > Benchmarking current for ${pattern}..."
    pattern_auto_hwm_logs=()

    for transport_index in "${!transports[@]}"; do
      if [[ "${stop_early}" -eq 1 ]]; then
        break
      fi
      transport="${transports[transport_index]}"
      transport="${transport//[[:space:]]/}"
      [[ -n "${transport}" ]] || continue
      if [[ "${transport}" == "inproc" ]]; then
        print_line "UNSUPPORTED,dotnet,${pattern},${transport}"
        unsupported_count=$((unsupported_count + 1))
        continue
      fi

      print_line "    Testing ${transport}:"
      print_line "      | Size     |         Throughput |      Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) |"
      print_line "      |----------|--------------------|----------------|---------------|---------------|---------------|"

      for size in "${msg_sizes[@]}"; do
        if [[ "${stop_early}" -eq 1 ]]; then
          break
        fi
        size="${size//[[:space:]]/}"
        [[ -n "${size}" ]] || continue
        expected_result_lines=$((expected_result_lines + 5))

        metrics_file="${RESULTS_ROOT}/multi/tmp/${pattern,,}_${transport}_${size}_run${run_index}.metrics"
        : > "${metrics_file}"
        server_log="${RESULTS_ROOT}/multi/tmp/${pattern,,}_${transport}_${size}_server_run${run_index}.log"
        client_log="${RESULTS_ROOT}/multi/tmp/${pattern,,}_${transport}_${size}_client_run${run_index}.log"
        server_control_fifo="${RESULTS_ROOT}/multi/tmp/${pattern,,}_${transport}_${size}_server_run${run_index}.ctl"
        client_control_fifo="${RESULTS_ROOT}/multi/tmp/${pattern,,}_${transport}_${size}_client_run${run_index}.ctl"
        rm -f "${server_log}" "${client_log}" \
          "${server_control_fifo}" "${client_control_fifo}"

        server_control_fd=''
        client_control_fd=''
        server_endpoint=''
        server_pid=0
        server_started=0
        if pattern_uses_control_pipe "${pattern}"; then
          mkfifo "${server_control_fifo}"
          run_multi_process "server" "${server_log}" "" "${server_control_fifo}" 1
          server_pid=$!
          exec {server_control_fd}>"${server_control_fifo}"
          rm -f "${server_control_fifo}"
        else
          run_multi_process "server" "${server_log}" "" "" 1
          server_pid=$!
        fi
        if server_endpoint="$(wait_for_ready_endpoint "${server_log}" "${SERVER_READY_TIMEOUT_MS}")"; then
          server_started=1
        else
          terminate_pid "${server_pid}"
        fi

        if [[ "${server_started}" -ne 1 ]]; then
          if unsupported_line="$(extract_unsupported_line "${pattern}" "${transport}" "${server_log}" 2>/dev/null)"; then
            print_line "${unsupported_line}"
            unsupported_count=$((unsupported_count + 1))
            expected_result_lines=$((expected_result_lines - 5))
            if [[ -n "${server_control_fd}" ]]; then
              exec {server_control_fd}>&-
            fi
            continue
          fi
          cat "${server_log}" >&2 || true
          echo "server did not become ready for ${pattern} ${transport} ${size}" >&2
          record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "server_ready_timeout"
          if [[ -n "${server_control_fd}" ]]; then
            exec {server_control_fd}>&-
          fi
          status=1
          continue
        fi

        if [[ "${pattern}" == "MULTI_STREAM" ]]; then
          if run_external_stream_client "${server_endpoint}"; then
            write_control_line "${server_control_fd}" 'STOP\n'
            server_shutdown_ok=1
            if ! wait_for_pid_exit_zero "${server_pid}" "$(shutdown_timeout_seconds)" "${pattern} server"; then
              server_shutdown_ok=0
            fi
            if unsupported_line="$(extract_unsupported_line "${pattern}" "${transport}" "${client_log}" "${server_log}" 2>/dev/null)"; then
              print_line "${unsupported_line}"
              unsupported_count=$((unsupported_count + 1))
              expected_result_lines=$((expected_result_lines - 5))
              exec {server_control_fd}>&-
              continue
            fi
            if [[ "${server_shutdown_ok}" -ne 1 ]]; then
              cat "${server_log}" >&2 || true
              cat "${client_log}" >&2 || true
              record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "process_exit_nonzero"
              status=1
              exec {server_control_fd}>&-
              continue
            fi
            if ! extracted="$(extract_results_from_logs "${client_log}" "${server_log}" "${pattern}" "${transport}" "${size}")"; then
              record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "missing_required_result_lines"
              status=1
              exec {server_control_fd}>&-
              continue
            fi
          else
            if unsupported_line="$(extract_unsupported_line "${pattern}" "${transport}" "${client_log}" "${server_log}" 2>/dev/null)"; then
              print_line "${unsupported_line}"
              unsupported_count=$((unsupported_count + 1))
              expected_result_lines=$((expected_result_lines - 5))
              write_control_line "${server_control_fd}" 'STOP\n'
              terminate_pid "${server_pid}"
              exec {server_control_fd}>&-
              continue
            fi
            cat "${server_log}" >&2 || true
            cat "${client_log}" >&2 || true
            write_control_line "${server_control_fd}" 'STOP\n'
            terminate_pid "${server_pid}"
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "process_exit_nonzero"
            status=1
            exec {server_control_fd}>&-
            continue
          fi
          exec {server_control_fd}>&-
        elif [[ "${pattern}" == "MULTI_SPOT" || "${pattern}" == "MULTI_SPOT_REQREP" || "${pattern}" == "MULTI_SPOT_SENDSEND" ]]; then
          control_endpoint=''
          if ! control_endpoint="$(wait_for_control_ready_endpoint "${server_log}" "${SERVER_READY_TIMEOUT_MS}")"; then
            cat "${server_log}" >&2 || true
            write_control_line "${server_control_fd}" 'STOP\n'
            terminate_pid "${server_pid}"
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "server_ready_timeout"
            exec {server_control_fd}>&-
            status=1
            continue
          fi

          mkfifo "${client_control_fifo}"
          run_multi_process "client" "${client_log}" "${server_endpoint}" "${client_control_fifo}" 1 "--control-endpoint" "${control_endpoint}"
          client_pid=$!
          exec {client_control_fd}>"${client_control_fifo}"
          rm -f "${client_control_fifo}"

          client_ctrl_ep=''
          if ! client_ctrl_ep="$(wait_for_client_control_endpoint "${client_log}" "${SPOT_READY_TIMEOUT_MS}")"; then
            if unsupported_line="$(extract_unsupported_line "${pattern}" "${transport}" "${client_log}" "${server_log}" 2>/dev/null)"; then
              print_line "${unsupported_line}"
              unsupported_count=$((unsupported_count + 1))
              expected_result_lines=$((expected_result_lines - 5))
              terminate_pid "${client_pid}"
              write_control_line "${server_control_fd}" 'STOP\n'
              terminate_pid "${server_pid}"
              exec {server_control_fd}>&-
              exec {client_control_fd}>&-
              continue
            fi
            cat "${server_log}" >&2 || true
            cat "${client_log}" >&2 || true
            terminate_pid "${client_pid}"
            write_control_line "${server_control_fd}" 'STOP\n'
            terminate_pid "${server_pid}"
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "client_control_timeout"
            exec {server_control_fd}>&-
            exec {client_control_fd}>&-
            status=1
            continue
          fi

          write_control_line "${server_control_fd}" 'CONNECT_CONTROL,%s\n' "${client_ctrl_ep}"

          connected_ep=''
          if ! connected_ep="$(wait_for_control_connected "${server_log}" "${SPOT_READY_TIMEOUT_MS}")"; then
            cat "${server_log}" >&2 || true
            cat "${client_log}" >&2 || true
            terminate_pid "${client_pid}"
            write_control_line "${server_control_fd}" 'STOP\n'
            terminate_pid "${server_pid}"
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "control_connect_timeout"
            exec {server_control_fd}>&-
            exec {client_control_fd}>&-
            status=1
            continue
          fi

          write_control_line "${client_control_fd}" 'CONTROL_CONNECTED,%s\n' "${connected_ep}"

          if ! wait_for_client_ready_line "${client_log}" "${SPOT_READY_TIMEOUT_MS}"; then
            if unsupported_line="$(extract_unsupported_line "${pattern}" "${transport}" "${client_log}" "${server_log}" 2>/dev/null)"; then
             print_line "${unsupported_line}"
              unsupported_count=$((unsupported_count + 1))
              expected_result_lines=$((expected_result_lines - 5))
              terminate_pid "${client_pid}"
              write_control_line "${server_control_fd}" 'STOP\n'
              terminate_pid "${server_pid}"
              exec {server_control_fd}>&-
              exec {client_control_fd}>&-
              continue
            fi
            cat "${server_log}" >&2 || true
            cat "${client_log}" >&2 || true
            terminate_pid "${client_pid}"
            write_control_line "${server_control_fd}" 'STOP\n'
            terminate_pid "${server_pid}"
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "client_ready_timeout"
            exec {server_control_fd}>&-
            exec {client_control_fd}>&-
            status=1
            continue
          fi

          write_control_line "${server_control_fd}" 'START,%s\n' "${size}"
          write_control_line "${client_control_fd}" 'START,%s\n' "${size}"
          if ! extracted="$(wait_for_results_from_logs \
            "${client_log}" "${server_log}" "${pattern}" "${transport}" "${size}" \
            "${RESULT_TIMEOUT_SECONDS}")"; then
            if unsupported_line="$(extract_unsupported_line "${pattern}" "${transport}" "${client_log}" "${server_log}" 2>/dev/null)"; then
              print_line "${unsupported_line}"
              unsupported_count=$((unsupported_count + 1))
              expected_result_lines=$((expected_result_lines - 5))
              write_control_line "${server_control_fd}" 'STOP\n'
              terminate_pid "${client_pid}"
              terminate_pid "${server_pid}"
              exec {server_control_fd}>&-
              exec {client_control_fd}>&-
              continue
            fi
            cat "${server_log}" >&2 || true
            cat "${client_log}" >&2 || true
            write_control_line "${server_control_fd}" 'STOP\n'
            terminate_pid "${client_pid}"
            terminate_pid "${server_pid}"
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "result_timeout"
            exec {server_control_fd}>&-
            exec {client_control_fd}>&-
            status=1
            continue
          fi

          write_control_line "${server_control_fd}" 'STOP\n'
          write_control_line "${client_control_fd}" 'STOP\n'
          process_shutdown_ok=1
          if ! wait_for_pid_exit_zero "${client_pid}" "$(shutdown_timeout_seconds)" "${pattern} client"; then
            process_shutdown_ok=0
          fi
          if ! wait_for_pid_exit_zero "${server_pid}" "$(shutdown_timeout_seconds)" "${pattern} server"; then
            process_shutdown_ok=0
          fi
          if unsupported_line="$(extract_unsupported_line "${pattern}" "${transport}" "${client_log}" "${server_log}" 2>/dev/null)"; then
             print_line "${unsupported_line}"
              unsupported_count=$((unsupported_count + 1))
              expected_result_lines=$((expected_result_lines - 5))
            exec {server_control_fd}>&-
            exec {client_control_fd}>&-
            continue
          fi
          if [[ "${process_shutdown_ok}" -ne 1 ]]; then
            cat "${server_log}" >&2 || true
            cat "${client_log}" >&2 || true
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "process_exit_nonzero"
            status=1
            exec {server_control_fd}>&-
            exec {client_control_fd}>&-
            continue
          fi
          if ! extracted="$(extract_results_from_logs "${client_log}" "${server_log}" "${pattern}" "${transport}" "${size}")"; then
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "missing_required_result_lines"
            status=1
            exec {server_control_fd}>&-
            exec {client_control_fd}>&-
            continue
          fi
          exec {server_control_fd}>&-
          exec {client_control_fd}>&-
        elif [[ "${pattern}" == "MULTI_DEALER_DEALER" || "${pattern}" == "MULTI_PUBSUB" ]]; then
          mkfifo "${client_control_fifo}"
          run_multi_process "client" "${client_log}" "${server_endpoint}" "${client_control_fifo}" 1
          client_pid=$!
          exec {client_control_fd}>"${client_control_fifo}"
          rm -f "${client_control_fifo}"

          if ! wait_for_client_ready_line "${client_log}" "${READY_TIMEOUT_MS}"; then
            if unsupported_line="$(extract_unsupported_line "${pattern}" "${transport}" "${client_log}" "${server_log}" 2>/dev/null)"; then
             print_line "${unsupported_line}"
              unsupported_count=$((unsupported_count + 1))
              expected_result_lines=$((expected_result_lines - 5))
              terminate_pid "${client_pid}"
              write_control_line "${server_control_fd}" 'STOP\n'
              terminate_pid "${server_pid}"
              exec {server_control_fd}>&-
              exec {client_control_fd}>&-
              continue
            fi
            cat "${server_log}" >&2 || true
            cat "${client_log}" >&2 || true
            terminate_pid "${client_pid}"
            write_control_line "${server_control_fd}" 'STOP\n'
            terminate_pid "${server_pid}"
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "client_ready_timeout"
            exec {server_control_fd}>&-
            exec {client_control_fd}>&-
            status=1
            continue
          fi

          write_control_line "${server_control_fd}" 'START,%s\n' "${size}"
          write_control_line "${client_control_fd}" 'START,%s\n' "${size}"

          if ! extracted="$(wait_for_results_from_logs \
            "${client_log}" "${server_log}" "${pattern}" "${transport}" "${size}" \
            "${RESULT_TIMEOUT_SECONDS}")"; then
            if unsupported_line="$(extract_unsupported_line "${pattern}" "${transport}" "${client_log}" "${server_log}" 2>/dev/null)"; then
             print_line "${unsupported_line}"
              unsupported_count=$((unsupported_count + 1))
              expected_result_lines=$((expected_result_lines - 5))
              write_control_line "${server_control_fd}" 'STOP\n'
              terminate_pid "${client_pid}"
              terminate_pid "${server_pid}"
              exec {server_control_fd}>&-
              exec {client_control_fd}>&-
              continue
            fi
            cat "${server_log}" >&2 || true
            cat "${client_log}" >&2 || true
            write_control_line "${server_control_fd}" 'STOP\n'
            terminate_pid "${client_pid}"
            terminate_pid "${server_pid}"
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "result_timeout"
            exec {server_control_fd}>&-
            exec {client_control_fd}>&-
            status=1
            continue
          fi

          write_control_line "${server_control_fd}" 'STOP\n'
          write_control_line "${client_control_fd}" 'STOP\n'
          process_shutdown_ok=1
          if ! wait_for_pid_exit_zero "${client_pid}" "$(shutdown_timeout_seconds)" "${pattern} client"; then
            process_shutdown_ok=0
          fi
          if ! wait_for_pid_exit_zero "${server_pid}" "$(shutdown_timeout_seconds)" "${pattern} server"; then
            process_shutdown_ok=0
          fi
          if unsupported_line="$(extract_unsupported_line "${pattern}" "${transport}" "${client_log}" "${server_log}" 2>/dev/null)"; then
             print_line "${unsupported_line}"
              unsupported_count=$((unsupported_count + 1))
              expected_result_lines=$((expected_result_lines - 5))
            exec {server_control_fd}>&-
            exec {client_control_fd}>&-
            continue
          fi
          if [[ "${process_shutdown_ok}" -ne 1 ]]; then
            cat "${server_log}" >&2 || true
            cat "${client_log}" >&2 || true
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "process_exit_nonzero"
            status=1
            exec {server_control_fd}>&-
            exec {client_control_fd}>&-
            continue
          fi
          if ! extracted="$(extract_results_from_logs "${client_log}" "${server_log}" "${pattern}" "${transport}" "${size}")"; then
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "missing_required_result_lines"
            status=1
            exec {server_control_fd}>&-
            exec {client_control_fd}>&-
            continue
          fi
          exec {server_control_fd}>&-
          exec {client_control_fd}>&-
        else
          if run_multi_process "client" "${client_log}" "${server_endpoint}" "" 0; then
            server_shutdown_ok=1
            if ! terminate_running_pid_or_fail_if_exited \
                "${server_pid}" "$(shutdown_timeout_seconds)" "${pattern} server"; then
              server_shutdown_ok=0
            fi
            if unsupported_line="$(extract_unsupported_line "${pattern}" "${transport}" "${client_log}" "${server_log}" 2>/dev/null)"; then
           print_line "${unsupported_line}"
            unsupported_count=$((unsupported_count + 1))
            expected_result_lines=$((expected_result_lines - 5))
              continue
            fi
            if [[ "${server_shutdown_ok}" -ne 1 ]]; then
              cat "${server_log}" >&2 || true
              cat "${client_log}" >&2 || true
              record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "process_exit_nonzero"
              status=1
              continue
            fi
            if ! extracted="$(extract_results_from_logs "${client_log}" "${server_log}" "${pattern}" "${transport}" "${size}")"; then
              record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "missing_required_result_lines"
              status=1
              continue
            fi
          else
            if unsupported_line="$(extract_unsupported_line "${pattern}" "${transport}" "${client_log}" "${server_log}" 2>/dev/null)"; then
          print_line "${unsupported_line}"
          unsupported_count=$((unsupported_count + 1))
          expected_result_lines=$((expected_result_lines - 5))
              terminate_pid "${server_pid}"
              continue
            fi
            cat "${server_log}" >&2 || true
            cat "${client_log}" >&2 || true
            terminate_pid "${server_pid}"
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "process_exit_nonzero"
            status=1
            continue
          fi
        fi

        # C parity: bindings/c/perf/run_comparison.py:3062-3088. Only the
        # live MULTI_SPOT pass is saturated; its latency reflects queue
        # backlog (~2300ms here, like C's pre-fix bug). Defer the live
        # latency row by running a clean, paced latency-only second pass
        # and overriding latency/p95/p99 with its uncontended values.
        # REQREP/SENDSEND are echo (round-trip) patterns and are excluded,
        # matching C's normalized_pattern == "SPOT" guard.
        if [[ "${pattern}" == "MULTI_SPOT" \
              && "${PERF_MULTI_SPOT_CLEAN_LATENCY:-1}" != "0" ]]; then
          clean_extracted=''
          if clean_extracted="$(run_spot_clean_latency_pass \
              "${pattern}" "${transport}" "${size}" "${run_index}")"; then
            if merged_extracted="$(merge_spot_clean_latency \
                "${extracted}" "${clean_extracted}")"; then
              extracted="${merged_extracted}"
            fi
          else
            cat "${RESULTS_ROOT}/multi/tmp/${pattern,,}_${transport}_${size}_clean_server_run${run_index}.log" >&2 2>/dev/null || true
            cat "${RESULTS_ROOT}/multi/tmp/${pattern,,}_${transport}_${size}_clean_client_run${run_index}.log" >&2 2>/dev/null || true
            record_failure "${pattern}" "${transport}" "${size}" "${run_index}" "clean_latency_failed"
            status=1
            continue
          fi
        fi

        pattern_auto_hwm_logs+=("${server_log}" "${client_log}")
        while IFS= read -r result_line; do
          [[ -n "${result_line}" ]] || continue
          printf '%s\n' "${result_line}" >> "${metrics_file}"
          printf '%s\n' "${result_line}" >> "${RESULT_DATA_FILE}"
          result_lines=$((result_lines + 1))
        done <<< "${extracted}"
        print_line "    Testing ${transport} | ${size}B:"
        while IFS= read -r table_line; do
          print_line "${table_line}"
        done < <(emit_result_row "${metrics_file}" "${pattern}")
        success_count=$((success_count + 1))
      done

      print_line "    Testing ${transport}: Done"
      if [[ "${stop_early}" -ne 1 ]] \
          && (( transport_index + 1 < ${#transports[@]} && TRANSPORT_TRANSITION_MS > 0 )); then
        print_line "    [transport cooldown ${TRANSPORT_TRANSITION_MS}ms]"
        sleep_ms "${TRANSPORT_TRANSITION_MS}"
      fi
    done

    while IFS= read -r auto_hwm_line; do
      [[ -n "${auto_hwm_line}" ]] || continue
      print_line "${auto_hwm_line}"
    done < <(emit_auto_hwm_detail_table "${pattern}" "${pattern_auto_hwm_logs[@]}")

    if [[ "${stop_early}" -ne 1 ]] \
        && (( pattern_index + 1 < ${#patterns[@]} && PATTERN_TRANSITION_MS > 0 )); then
      print_line "[pattern cooldown ${PATTERN_TRANSITION_MS}ms]"
      sleep_ms "${PATTERN_TRANSITION_MS}"
    fi
  done
done

# ITEM 1: the C multi engine (bindings/c/perf/run_comparison.py) calls
# print_effective_options("result") unconditionally after the last pattern
# and before "## Result Data" (a leading blank line precedes the header).
# Match that exactly (C print() prefixes the header with one blank line).
print_line ""
print_effective_options "result"

if [[ -s "${RESULT_DATA_FILE}" ]]; then
  print_line ""
  print_line "## Result Data"
  # C emit_result_lines iterates sorted((pattern,transport,size,metric))
  # tuples and tags every row "current". Re-tag + re-sort the collected
  # dotnet RESULT lines so the block is byte-identical to C multi.
  while IFS= read -r result_line; do
    print_line "${result_line}"
  done < <(python3 - "${RESULT_DATA_FILE}" <<'PY'
import csv, sys
rows = {}
with open(sys.argv[1], encoding="utf-8", errors="replace") as fh:
    for row in csv.reader(fh):
        if len(row) != 7 or row[0] != "RESULT":
            continue
        pattern, transport, size, metric, value = (
            row[2], row[3], row[4], row[5], row[6]
        )
        try:
            size_i = int(size)
        except ValueError:
            size_i = 0
        rows[(pattern, transport, size_i, metric)] = (size, value)
for key in sorted(rows.keys()):
    pattern, transport, _size_i, metric = key
    size, value = rows[key]
    try:
        value = f"{float(value):.3f}"
    except ValueError:
        pass
    print(f"RESULT,current,{pattern},{transport},{size},{metric},{value}")
PY
)
fi
if [[ "${result_lines}" -eq "${expected_result_lines}" ]]; then
  completion_status="complete"
else
  completion_status="partial"
  status=1
fi
print_completion_section "${completion_status}" "${expected_result_lines}" "${result_lines}" \
  "${success_count}" "${unsupported_count}" "${skip_count}" "${failure_count}"
# ITEM 1: C multi "## Failures" lines are
#   - <PATTERN> current <transport> <size>B: <reason>
# sorted by (pattern, transport, size, reason), unique. Match that exactly
# instead of the generic key=value failures section.
if [[ "${failure_count}" -gt 0 && -s "${FAILURES_FILE}" ]]; then
  print_line ""
  print_line "## Failures"
  while IFS= read -r failure_line; do
    print_line "${failure_line}"
  done < <(python3 - "${FAILURES_FILE}" <<'PY'
import csv, sys
seen = set()
items = []
with open(sys.argv[1], encoding="utf-8", errors="replace") as fh:
    for row in csv.reader(fh):
        if len(row) < 5:
            continue
        pattern, transport, size, _run, reason = row[0], row[1], row[2], row[3], row[4]
        key = (pattern, transport, size, reason)
        if key in seen:
            continue
        seen.add(key)
        try:
            size_i = int(size)
        except ValueError:
            size_i = 0
        items.append((pattern, transport, size_i, size, reason))
for pattern, transport, _si, size, reason in sorted(
    items, key=lambda x: (x[0], x[1], x[2], x[4])
):
    print(f"- {pattern} current {transport} {size}B: {reason}")
PY
)
fi

# C prints "\nSaved result file:" (leading blank line before the line).
print_line ""
print_line "Saved result file: ${REPORT} (status=${completion_status})"
SHOW_TOTAL_TIME=1
exit "${status}"
