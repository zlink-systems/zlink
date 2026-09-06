#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
source "${ROOT_DIR}/bindings/tools/local_core_runtime.sh"
BUILD_DIR="${ROOT_DIR}/bindings/c/build"

LOCK_FILE="/tmp/bench_streamcompare.lock"
if [[ "${BENCH_STREAMCOMPARE_LOCKED:-0}" != "1" ]]; then
    export BENCH_STREAMCOMPARE_LOCKED=1
    # Hold lock in the flock parent process and close lock fd before exec.
    flock -n -E 200 -o "${LOCK_FILE}" "$0" "$@"
    rc="$?"
    if [[ "${rc}" != "0" ]]; then
        if [[ "${rc}" == "200" ]]; then
            echo "[$(date +'%F %T')] another stream benchmark is running" >&2
        fi
        exit "${rc}"
    fi
    exit 0
fi

usage()
{
    cat <<'USAGE'
Usage:
  run_benchmarks.sh [options]

Options:
  --stack <asio|cppserver|dotnet|netzlink|jvmzlink|jvmzlink-recv|jvmzmq|netty|zlink|zlink_packet|zmq|all|comma-list>
  --size <64|1024|65536|all|comma-list>
  --build-dir PATH            Build directory (default: bindings/c/build).
  --reuse-build               Reuse existing build directory as-is (skip configure/build).
  --clean-build               Remove build directory and do a clean build.
  --ccu <N>                    default: 1000
  --runs <N>                   default: 1
  --warmup <sec>               default: 3
  --duration <sec>             default: 5
  --client-io-threads <N>      default: 4
  --server-io-threads <N>      default: 4
  --resource-sample-ms <N>     default: 500
  --server-start-timeout <sec> default: 40
  --stack-gap <sec>            default: 5
  -h, --help

Examples:
  ./run_benchmarks.sh
  ./run_benchmarks.sh --stack zlink,zmq --size 1024 --runs 3
  ./run_benchmarks.sh --reuse-build
  ./run_benchmarks.sh --clean-build --build-dir ./bindings/c/build
USAGE
}

STACKS_ALL=(zlink zlink_packet netzlink jvmzlink jvmzlink-recv jvmzmq asio cppserver dotnet zmq netty)
SIZES_ALL=(64 1024 65536)

TARGET_STACK="all"
TARGET_SIZE="all"
BUILD_MODE="incremental"
BUILD_MODE_EXPLICIT=0
CCU="${BENCH_MULTI_CLIENTS:-1000}"
RUNS=1
WARMUP="${BENCH_MULTI_WARMUP_SECONDS:-3}"
DURATION="${BENCH_MULTI_DURATION_SECONDS:-5}"
CLIENT_IO_THREADS=4
SERVER_IO_THREADS=4
RESOURCE_SAMPLE_MS=500
SERVER_START_TIMEOUT=40
STACK_GAP_SEC=5

set_build_mode()
{
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
        --stack)
            TARGET_STACK="${2:-}"
            shift 2
            ;;
        --size)
            TARGET_SIZE="${2:-}"
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
        --build-dir)
            BUILD_DIR="${2:-}"
            shift 2
            ;;
        --ccu)
            CCU="${2:-}"
            shift 2
            ;;
        --runs)
            RUNS="${2:-}"
            shift 2
            ;;
        --warmup)
            WARMUP="${2:-}"
            shift 2
            ;;
        --duration)
            DURATION="${2:-}"
            shift 2
            ;;
        --client-io-threads)
            CLIENT_IO_THREADS="${2:-}"
            shift 2
            ;;
        --server-io-threads)
            SERVER_IO_THREADS="${2:-}"
            shift 2
            ;;
        --resource-sample-ms)
            RESOURCE_SAMPLE_MS="${2:-}"
            shift 2
            ;;
        --server-start-timeout)
            SERVER_START_TIMEOUT="${2:-}"
            shift 2
            ;;
        --stack-gap)
            STACK_GAP_SEC="${2:-}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

validate_int()
{
    local name="$1"
    local value="$2"
    if ! [[ "${value}" =~ ^[0-9]+$ ]] || (( value < 1 )); then
        echo "invalid ${name}: ${value}" >&2
        exit 2
    fi
}

validate_int "--ccu" "${CCU}"
validate_int "--runs" "${RUNS}"
validate_int "--warmup" "${WARMUP}"
validate_int "--duration" "${DURATION}"
validate_int "--client-io-threads" "${CLIENT_IO_THREADS}"
validate_int "--server-io-threads" "${SERVER_IO_THREADS}"
validate_int "--resource-sample-ms" "${RESOURCE_SAMPLE_MS}"
validate_int "--server-start-timeout" "${SERVER_START_TIMEOUT}"
validate_int "--stack-gap" "${STACK_GAP_SEC}"

BUILD_DIR="$(realpath -m "${BUILD_DIR}")"
ROOT_DIR="$(realpath -m "${ROOT_DIR}")"
if [[ "${BUILD_DIR}" != "${ROOT_DIR}/"* ]]; then
    echo "Build directory must be inside repo root: ${ROOT_DIR}" >&2
    exit 1
fi

RUN_STACKS=()
if [[ "${TARGET_STACK}" == "all" ]]; then
    RUN_STACKS=("${STACKS_ALL[@]}")
else
    IFS=',' read -r -a RUN_STACKS <<<"${TARGET_STACK}"
fi

RUN_SIZES=()
if [[ "${TARGET_SIZE}" == "all" ]]; then
    RUN_SIZES=("${SIZES_ALL[@]}")
else
    IFS=',' read -r -a RUN_SIZES <<<"${TARGET_SIZE}"
fi

if [[ ${#RUN_STACKS[@]} -eq 0 ]]; then
    echo "empty stack set" >&2
    exit 2
fi
if [[ ${#RUN_SIZES[@]} -eq 0 ]]; then
    echo "empty size set" >&2
    exit 2
fi

for s in "${RUN_STACKS[@]}"; do
    case "${s}" in
        asio|cppserver|dotnet|netzlink|jvmzlink|jvmzlink-recv|jvmzmq|netty|zlink|zlink_packet|zmq)
            ;;
        *)
            echo "invalid stack: ${s}" >&2
            exit 2
            ;;
    esac
done

for sz in "${RUN_SIZES[@]}"; do
    case "${sz}" in
        64|1024|65536)
            ;;
        *)
            echo "invalid size: ${sz}" >&2
            exit 2
            ;;
    esac
done

RESULT_DIR="${RESULT_DIR:-${SCRIPT_DIR}/results/$(date +%Y%m%d_%H%M%S)}"
LOG_DIR="${RESULT_DIR}/logs"
SCENARIO_LOG="${RESULT_DIR}/benchmark.log"
METRICS_CSV="${RESULT_DIR}/metrics.csv"
SUMMARY_JSON="${RESULT_DIR}/summary.json"
REPORT_MD="${RESULT_DIR}/comparison.md"
SKIP_CSV="${RESULT_DIR}/skipped_stacks.csv"

STREAMCOMPARE_BIN_DIR="${BUILD_DIR}/bench/with_stream"
CLIENT_BIN="${STREAMCOMPARE_BIN_DIR}/bench_streamcompare_client"
ASIO_BIN="${STREAMCOMPARE_BIN_DIR}/test_scenario_stream_asio"
ZLINK_BIN="${STREAMCOMPARE_BIN_DIR}/test_scenario_stream_zlink"
ZMQ_BIN="${STREAMCOMPARE_BIN_DIR}/test_scenario_stream_zmq"
ZLINK_PACKET_BIN="${STREAMCOMPARE_BIN_DIR}/test_scenario_stream_zlink_packet"

STACKS_ROOT_DIR="${STACKS_ROOT_DIR:-${ROOT_DIR}/bindings/c/bench/with_stream/stacks}"
if [[ ! -d "${STACKS_ROOT_DIR}" ]]; then
    echo "stream stacks directory not found: ${STACKS_ROOT_DIR}" >&2
    exit 2
fi

ZMQ_LIB_DIR="${STACKS_ROOT_DIR}/zmq/libzmq_dist/linux-x64/lib"

CPPSERVER_SRC_DIR="${STACKS_ROOT_DIR}/cppserver/upstream"
CPPSERVER_BUILD_DIR="${CPPSERVER_SRC_DIR}/build-stream"
CPPSERVER_BIN="${CPPSERVER_BUILD_DIR}/cppserver-performance-stream_fixed_server"
CPPSERVER_UPSTREAM_ENTRY="${CPPSERVER_SRC_DIR}/performance/stream_fixed_server.cpp"

DOTNET_PROJECT="${STACKS_ROOT_DIR}/dotnet/StreamServer.csproj"
DOTNET_OUT_DIR="${STACKS_ROOT_DIR}/dotnet/bin/Release/stream-bench"
DOTNET_DLL="${DOTNET_OUT_DIR}/StreamServer.dll"

NETZLINK_PROJECT="${STACKS_ROOT_DIR}/netzlink/NetZlinkStreamServer.csproj"
NETZLINK_OUT_DIR="${STACKS_ROOT_DIR}/netzlink/bin/Release/stream-bench"
NETZLINK_DLL="${NETZLINK_OUT_DIR}/NetZlinkStreamServer.dll"
NETZLINK_LEN32BE_PROJECT="${STACKS_ROOT_DIR}/netzlink-len32be/NetZlinkStreamLen32BeServer.csproj"
NETZLINK_LEN32BE_OUT_DIR="${STACKS_ROOT_DIR}/netzlink-len32be/bin/Release/stream-bench"
NETZLINK_LEN32BE_DLL="${NETZLINK_LEN32BE_OUT_DIR}/NetZlinkStreamLen32BeServer.dll"

JVMZLINK_PROJECT_DIR="${STACKS_ROOT_DIR}/jvmzlink"
JVMZLINK_BUILD_DIR="${JVMZLINK_PROJECT_DIR}/build"
JVMZLINK_BIN="${JVMZLINK_BUILD_DIR}/install/jvmzlink-stream-server/bin/jvmzlink-stream-server"
JVMZLINK_RECV_PROJECT_DIR="${STACKS_ROOT_DIR}/jvmzlink-recv"
JVMZLINK_RECV_BUILD_DIR="${JVMZLINK_RECV_PROJECT_DIR}/build"
JVMZLINK_RECV_BIN="${JVMZLINK_RECV_BUILD_DIR}/install/jvmzlink-recv-stream-server/bin/jvmzlink-recv-stream-server"
JVMZMQ_PROJECT_DIR="${STACKS_ROOT_DIR}/jvmzmq"
JVMZMQ_BUILD_DIR="${JVMZMQ_PROJECT_DIR}/build"
JVMZMQ_BIN="${JVMZMQ_BUILD_DIR}/install/jvmzmq-stream-server/bin/jvmzmq-stream-server"
JVMZLINK_LEN32BE_PROJECT_DIR="${STACKS_ROOT_DIR}/jvmzlink-len32be"
JVMZLINK_LEN32BE_BUILD_DIR="${JVMZLINK_LEN32BE_PROJECT_DIR}/build"
JVMZLINK_LEN32BE_BIN="${JVMZLINK_LEN32BE_BUILD_DIR}/install/jvmzlink-len32be-stream-server/bin/jvmzlink-len32be-stream-server"

BINDINGS_JAVA_PROJECT_DIR="${ROOT_DIR}/bindings/java"
ZLINK_CORE_LIBRARY="${ZLINK_LOCAL_CORE_RUNTIME}"

NETTY_PROJECT_DIR="${STACKS_ROOT_DIR}/netty"
NETTY_BUILD_DIR="${NETTY_PROJECT_DIR}/build"
NETTY_BIN="${NETTY_BUILD_DIR}/install/netty-stream-server/bin/netty-stream-server"
NETTY_JAVA_HOME="${NETTY_JAVA_HOME:-}"
NETTY_JAVA_BIN=""
NETTY_JAVA_VERSION=""
NETTY_GRADLE_BIN="${NETTY_GRADLE_BIN:-}"
NETTY_GRADLE_VERSION=""
NETTY_GRADLE_MIN_VERSION="8.8"
NETTY_GRADLE_FALLBACK_VERSION="8.10.2"
NETTY_GRADLE_TOOLS_DIR="${NETTY_PROJECT_DIR}/.gradle-tools"
NETTY_GRADLE_FALLBACK_DIR="${NETTY_GRADLE_TOOLS_DIR}/gradle-${NETTY_GRADLE_FALLBACK_VERSION}"

ACTIVE_SERVER_PID=""
ACTIVE_SERVER_STACK=""
HOST="${HOST:-127.0.0.1}"
PORT_CURSOR="${BASE_PORT:-22000}"
PORT_SCAN_LIMIT=5000
ALLOCATED_PORT=""

ACTIVE_STACKS=()
FAILED_CASES=0
MONITOR_PID=""
JAVA_BINDINGS_JAR_BUILT=0

resolve_stack_tuning()
{
    local stack="$1"

    STACK_SNDBUF=1048576
    STACK_RCVBUF=1048576
    STACK_BACKLOG=32768
    STACK_TCP_NODELAY=1
    STACK_IO_THREADS="${SERVER_IO_THREADS}"
    STACK_ENV_VARS=()

    case "${stack}" in
        dotnet)
            STACK_ENV_VARS+=("DOTNET_gcServer=1" "COMPlus_gcServer=1")
            ;;
        netzlink|netzlink-len32be)
            STACK_ENV_VARS+=("ZLINK_LIBRARY_PATH=${ZLINK_CORE_LIBRARY}")
            STACK_ENV_VARS+=("LD_LIBRARY_PATH=$(dirname "${ZLINK_CORE_LIBRARY}"):${LD_LIBRARY_PATH:-}")
            STACK_ENV_VARS+=("DOTNET_gcServer=1" "COMPlus_gcServer=1")
            ;;
        jvmzlink|jvmzlink-recv|jvmzlink-len32be)
            STACK_ENV_VARS+=("ZLINK_LIBRARY_PATH=${ZLINK_CORE_LIBRARY}")
            STACK_ENV_VARS+=("LD_LIBRARY_PATH=$(dirname "${ZLINK_CORE_LIBRARY}"):${LD_LIBRARY_PATH:-}")
            STACK_ENV_VARS+=(
              "JAVA_OPTS=${JAVA_OPTS:-} --enable-native-access=ALL-UNNAMED -XX:+UseG1GC -XX:MaxGCPauseMillis=200 -Xms4g -Xmx4g -XX:MaxDirectMemorySize=4g"
            )
            ;;
        netty)
            STACK_ENV_VARS+=(
              "JAVA_OPTS=${JAVA_OPTS:-} -XX:+UseG1GC -XX:MaxGCPauseMillis=200 -Xms4g -Xmx4g -XX:MaxDirectMemorySize=4g"
            )
            ;;
    esac
}

log()
{
    mkdir -p "${RESULT_DIR}" "${LOG_DIR}"
    echo "[$(date +'%F %T')] $*" | tee -a "${SCENARIO_LOG}"
}

start_process_resource_monitor()
{
    local target_pid="$1"
    local sample_ms="$2"
    local usage_csv="$3"
    local summary_file="$4"

    if ! [[ "${target_pid}" =~ ^[0-9]+$ ]] || (( target_pid < 1 )); then
        MONITOR_PID=""
        return 1
    fi

    python3 - "${target_pid}" "${sample_ms}" "${usage_csv}" "${summary_file}" <<'PY' >/dev/null 2>&1 &
import os
import sys
import time

pid = int(sys.argv[1])
sample_ms = max(100, int(sys.argv[2]))
usage_csv = sys.argv[3]
summary_file = sys.argv[4]

clk_tck = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
page_size = os.sysconf("SC_PAGE_SIZE")
interval_s = sample_ms / 1000.0


def proc_exists(proc_pid):
    return os.path.exists(f"/proc/{proc_pid}")


def read_proc(proc_pid):
    with open(f"/proc/{proc_pid}/stat", encoding="utf-8", errors="replace") as f:
        tokens = f.read().split()

    cpu_ticks = int(tokens[13]) + int(tokens[14])
    start_ticks = int(tokens[21])
    rss_kb = 0
    hwm_kb = 0

    try:
        with open(f"/proc/{proc_pid}/status", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    parts = line.split()
                    if len(parts) >= 2:
                        rss_kb = int(parts[1])
                elif line.startswith("VmHWM:"):
                    parts = line.split()
                    if len(parts) >= 2:
                        hwm_kb = int(parts[1])
    except Exception:
        pass

    if rss_kb <= 0 and len(tokens) > 23:
        rss_pages = int(tokens[23])
        if rss_pages > 0:
            rss_kb = (rss_pages * page_size) // 1024

    return cpu_ticks, rss_kb, hwm_kb, start_ticks


cpu_samples = []
rss_samples = []
peak_cpu_pct = 0.0
peak_rss_kb = 0
peak_hwm_kb = 0
prev_ticks = None
prev_time = None
origin_start_ticks = None

os.makedirs(os.path.dirname(usage_csv), exist_ok=True)
with open(usage_csv, "w", encoding="utf-8") as out:
    out.write("ts_ns,cpu_pct,rss_kb,hwm_kb\n")

    while proc_exists(pid):
        try:
            cpu_ticks, rss_kb, hwm_kb, start_ticks = read_proc(pid)
        except Exception:
            break

        if origin_start_ticks is None:
            origin_start_ticks = start_ticks
        elif start_ticks != origin_start_ticks:
            # PID has been recycled for another process.
            break

        now = time.monotonic()
        cpu_pct = 0.0
        if prev_ticks is not None and prev_time is not None and now > prev_time:
            cpu_sec = (cpu_ticks - prev_ticks) / float(clk_tck)
            elapsed = now - prev_time
            if cpu_sec >= 0.0 and elapsed > 0.0:
                cpu_pct = max(0.0, (cpu_sec / elapsed) * 100.0)

        prev_ticks = cpu_ticks
        prev_time = now

        cpu_samples.append(cpu_pct)
        rss_samples.append(rss_kb)
        if cpu_pct > peak_cpu_pct:
            peak_cpu_pct = cpu_pct
        if rss_kb > peak_rss_kb:
            peak_rss_kb = rss_kb
        if hwm_kb > peak_hwm_kb:
            peak_hwm_kb = hwm_kb

        out.write(f"{time.time_ns()},{cpu_pct:.4f},{rss_kb},{hwm_kb}\n")
        out.flush()
        time.sleep(interval_s)

avg_cpu_pct = (sum(cpu_samples) / len(cpu_samples)) if cpu_samples else 0.0
avg_rss_kb = (sum(rss_samples) / len(rss_samples)) if rss_samples else 0.0

with open(summary_file, "w", encoding="utf-8") as f:
    f.write(f"sample_count={len(cpu_samples)}\n")
    f.write(f"avg_cpu_pct={avg_cpu_pct:.4f}\n")
    f.write(f"peak_cpu_pct={peak_cpu_pct:.4f}\n")
    f.write(f"avg_rss_kb={avg_rss_kb:.2f}\n")
    f.write(f"peak_rss_kb={peak_rss_kb}\n")
    f.write(f"peak_hwm_kb={peak_hwm_kb}\n")
PY
    MONITOR_PID="$!"
    return 0
}

start_system_resource_monitor()
{
    local sample_ms="$1"
    local usage_csv="$2"
    local summary_file="$3"

    python3 - "${sample_ms}" "${usage_csv}" "${summary_file}" <<'PY' >/dev/null 2>&1 &
import os
import signal
import sys
import time

sample_ms = max(100, int(sys.argv[1]))
usage_csv = sys.argv[2]
summary_file = sys.argv[3]
interval_s = sample_ms / 1000.0

running = True


def on_signal(signum, frame):
    del signum, frame
    global running
    running = False


signal.signal(signal.SIGINT, on_signal)
signal.signal(signal.SIGTERM, on_signal)


def read_cpu_total_idle():
    with open("/proc/stat", encoding="utf-8", errors="replace") as f:
        head = f.readline().split()
    if len(head) < 5 or head[0] != "cpu":
        return 0, 0
    values = [int(x) for x in head[1:]]
    total = sum(values)
    idle = values[3]
    if len(values) > 4:
        idle += values[4]
    return total, idle


def read_mem_used():
    mem_total = 0
    mem_available = 0
    with open("/proc/meminfo", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("MemTotal:"):
                parts = line.split()
                if len(parts) >= 2:
                    mem_total = int(parts[1])
            elif line.startswith("MemAvailable:"):
                parts = line.split()
                if len(parts) >= 2:
                    mem_available = int(parts[1])
            if mem_total > 0 and mem_available > 0:
                break

    if mem_total <= 0:
        return 0, 0.0
    used_kb = max(0, mem_total - mem_available)
    used_pct = (used_kb / float(mem_total)) * 100.0
    return used_kb, used_pct


cpu_samples = []
mem_used_samples = []
mem_used_pct_samples = []
peak_cpu_pct = 0.0
peak_mem_used_kb = 0
peak_mem_used_pct = 0.0

prev_total, prev_idle = read_cpu_total_idle()

os.makedirs(os.path.dirname(usage_csv), exist_ok=True)
with open(usage_csv, "w", encoding="utf-8") as out:
    out.write("ts_ns,cpu_pct,mem_used_kb,mem_used_pct\n")

    while running:
        time.sleep(interval_s)
        total, idle = read_cpu_total_idle()
        dt = total - prev_total
        di = idle - prev_idle
        prev_total, prev_idle = total, idle

        cpu_pct = 0.0
        if dt > 0:
            busy = dt - di
            cpu_pct = max(0.0, min(100.0, (busy / float(dt)) * 100.0))

        mem_used_kb, mem_used_pct = read_mem_used()

        cpu_samples.append(cpu_pct)
        mem_used_samples.append(mem_used_kb)
        mem_used_pct_samples.append(mem_used_pct)
        if cpu_pct > peak_cpu_pct:
            peak_cpu_pct = cpu_pct
        if mem_used_kb > peak_mem_used_kb:
            peak_mem_used_kb = mem_used_kb
        if mem_used_pct > peak_mem_used_pct:
            peak_mem_used_pct = mem_used_pct

        out.write(f"{time.time_ns()},{cpu_pct:.4f},{mem_used_kb},{mem_used_pct:.4f}\n")
        out.flush()

avg_cpu_pct = (sum(cpu_samples) / len(cpu_samples)) if cpu_samples else 0.0
avg_mem_used_kb = (sum(mem_used_samples) / len(mem_used_samples)) if mem_used_samples else 0.0
avg_mem_used_pct = (sum(mem_used_pct_samples) / len(mem_used_pct_samples)) if mem_used_pct_samples else 0.0

with open(summary_file, "w", encoding="utf-8") as f:
    f.write(f"sample_count={len(cpu_samples)}\n")
    f.write(f"avg_cpu_pct={avg_cpu_pct:.4f}\n")
    f.write(f"peak_cpu_pct={peak_cpu_pct:.4f}\n")
    f.write(f"avg_mem_used_kb={avg_mem_used_kb:.2f}\n")
    f.write(f"peak_mem_used_kb={peak_mem_used_kb}\n")
    f.write(f"avg_mem_used_pct={avg_mem_used_pct:.4f}\n")
    f.write(f"peak_mem_used_pct={peak_mem_used_pct:.4f}\n")
PY
    MONITOR_PID="$!"
    return 0
}

java_major_version()
{
    local java_bin="$1"
    local major=""
    major="$("${java_bin}" -version 2>&1 | awk -F'[\".]' '/version/ { if ($2 == "1") print $3; else print $2; exit }')"
    if ! [[ "${major}" =~ ^[0-9]+$ ]]; then
        return 1
    fi
    printf '%s' "${major}"
}

version_ge()
{
    local have="$1"
    local need="$2"
    local top=""
    top="$(printf '%s\n%s\n' "${need}" "${have}" | sort -V | tail -n1)"
    [[ "${top}" == "${have}" ]]
}

gradle_version()
{
    local gradle_bin="$1"
    local version=""
    version="$("${gradle_bin}" -v 2>/dev/null | awk '/Gradle [0-9]/{print $2; exit}')"
    if ! [[ "${version}" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]]; then
        return 1
    fi
    printf '%s' "${version}"
}

resolve_netty_gradle()
{
    if [[ -n "${NETTY_GRADLE_BIN}" ]]; then
        if [[ ! -x "${NETTY_GRADLE_BIN}" ]]; then
            log "NETTY_GRADLE_BIN is not executable: ${NETTY_GRADLE_BIN}"
            return 1
        fi
        NETTY_GRADLE_VERSION="$(gradle_version "${NETTY_GRADLE_BIN}")" || {
            log "failed to detect version from NETTY_GRADLE_BIN=${NETTY_GRADLE_BIN}"
            return 1
        }
        if ! version_ge "${NETTY_GRADLE_VERSION}" "${NETTY_GRADLE_MIN_VERSION}"; then
            log "NETTY_GRADLE_BIN version=${NETTY_GRADLE_VERSION} is too old (need >=${NETTY_GRADLE_MIN_VERSION})"
            return 1
        fi
        return 0
    fi

    local system_gradle=""
    local system_gradle_version=""
    system_gradle="$(command -v gradle || true)"
    if [[ -n "${system_gradle}" ]]; then
        system_gradle_version="$(gradle_version "${system_gradle}")" || {
            log "failed to detect version from system gradle=${system_gradle}"
            return 1
        }
        if version_ge "${system_gradle_version}" "${NETTY_GRADLE_MIN_VERSION}"; then
            NETTY_GRADLE_BIN="${system_gradle}"
            NETTY_GRADLE_VERSION="${system_gradle_version}"
            return 0
        fi
    fi

    if ! command -v curl >/dev/null 2>&1; then
        log "curl is required to download Gradle ${NETTY_GRADLE_FALLBACK_VERSION} for netty stack"
        return 1
    fi
    if ! command -v unzip >/dev/null 2>&1; then
        log "unzip is required to install Gradle ${NETTY_GRADLE_FALLBACK_VERSION} for netty stack"
        return 1
    fi

    mkdir -p "${NETTY_GRADLE_TOOLS_DIR}"
    if [[ ! -x "${NETTY_GRADLE_FALLBACK_DIR}/bin/gradle" ]]; then
        local dist_zip="${NETTY_GRADLE_TOOLS_DIR}/gradle-${NETTY_GRADLE_FALLBACK_VERSION}-bin.zip"
        local dist_url="https://services.gradle.org/distributions/gradle-${NETTY_GRADLE_FALLBACK_VERSION}-bin.zip"
        if [[ ! -f "${dist_zip}" ]]; then
            log "download gradle version=${NETTY_GRADLE_FALLBACK_VERSION}"
            curl -fsSL -o "${dist_zip}" "${dist_url}"
        fi
        unzip -q -o "${dist_zip}" -d "${NETTY_GRADLE_TOOLS_DIR}"
    fi

    NETTY_GRADLE_BIN="${NETTY_GRADLE_FALLBACK_DIR}/bin/gradle"
    if [[ ! -x "${NETTY_GRADLE_BIN}" ]]; then
        log "fallback gradle not found: ${NETTY_GRADLE_BIN}"
        return 1
    fi

    NETTY_GRADLE_VERSION="$(gradle_version "${NETTY_GRADLE_BIN}")" || {
        log "failed to detect version from fallback gradle=${NETTY_GRADLE_BIN}"
        return 1
    }
    return 0
}

resolve_netty_java()
{
    if [[ -n "${NETTY_JAVA_BIN}" ]]; then
        return 0
    fi

    local candidate=""
    local candidate_home=""
    local major=""

    if [[ -n "${NETTY_JAVA_HOME}" ]]; then
        candidate="${NETTY_JAVA_HOME}/bin/java"
        if [[ ! -x "${candidate}" ]]; then
            log "netty java not found at NETTY_JAVA_HOME=${NETTY_JAVA_HOME}"
            return 1
        fi
        major="$(java_major_version "${candidate}")" || {
            log "failed to detect java version from ${candidate}"
            return 1
        }
        if (( major < 22 )); then
            log "NETTY_JAVA_HOME java major=${major} is too old (need >=22)"
            return 1
        fi
        NETTY_JAVA_BIN="${candidate}"
        NETTY_JAVA_VERSION="${major}"
        return 0
    fi

    if [[ -n "${JAVA_HOME:-}" ]]; then
        candidate="${JAVA_HOME}/bin/java"
        if [[ -x "${candidate}" ]]; then
            major="$(java_major_version "${candidate}")" || {
                log "failed to detect java version from JAVA_HOME=${JAVA_HOME}"
                return 1
            }
            if (( major >= 22 )); then
                NETTY_JAVA_HOME="${JAVA_HOME}"
                NETTY_JAVA_BIN="${candidate}"
                NETTY_JAVA_VERSION="${major}"
                return 0
            fi
        fi
    fi

    candidate="$(command -v java || true)"
    if [[ -z "${candidate}" ]]; then
        log "java not found in PATH (need JDK 22+ for netty stack)"
        return 1
    fi
    candidate="$(readlink -f "${candidate}" || printf '%s' "${candidate}")"

    major="$(java_major_version "${candidate}")" || {
        log "failed to detect java version from PATH java=${candidate}"
        return 1
    }
    if (( major < 22 )); then
        log "PATH java major=${major} is too old for netty (need >=22)"
        return 1
    fi

    candidate_home="$(cd "$(dirname "${candidate}")/.." && pwd)"
    NETTY_JAVA_HOME="${candidate_home}"
    NETTY_JAVA_BIN="${candidate}"
    NETTY_JAVA_VERSION="${major}"
    return 0
}

wait_for_port()
{
    local host="$1"
    local port="$2"
    local timeout_s="$3"
    local start_ts
    start_ts="$(date +%s)"

    while true; do
        if python3 - "${host}" "${port}" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(0.2)
try:
    rc = sock.connect_ex((host, port))
finally:
    sock.close()

sys.exit(0 if rc == 0 else 1)
PY
        then
            return 0
        fi

        local now_ts
        now_ts="$(date +%s)"
        if (( now_ts - start_ts >= timeout_s )); then
            return 1
        fi
        sleep 0.05
    done
}

can_bind_port()
{
    local host="$1"
    local port="$2"

    python3 - "${host}" "${port}" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.bind((host, port))
except OSError:
    sys.exit(1)
finally:
    s.close()
sys.exit(0)
PY
}

allocate_free_port()
{
    local host="$1"
    local scan_left="${PORT_SCAN_LIMIT}"
    ALLOCATED_PORT=""

    while (( scan_left > 0 )); do
        local candidate="${PORT_CURSOR}"
        PORT_CURSOR="$((PORT_CURSOR + 1))"
        if can_bind_port "${host}" "${candidate}"; then
            ALLOCATED_PORT="${candidate}"
            return 0
        fi
        scan_left="$((scan_left - 1))"
    done

    return 1
}

stop_active_server()
{
    if [[ -z "${ACTIVE_SERVER_PID}" ]]; then
        return
    fi

    if kill -0 "${ACTIVE_SERVER_PID}" >/dev/null 2>&1; then
        kill -INT "${ACTIVE_SERVER_PID}" >/dev/null 2>&1 || true
        sleep 0.5

        if kill -0 "${ACTIVE_SERVER_PID}" >/dev/null 2>&1; then
            kill "${ACTIVE_SERVER_PID}" >/dev/null 2>&1 || true
            sleep 0.5
        fi

        if kill -0 "${ACTIVE_SERVER_PID}" >/dev/null 2>&1; then
            kill -9 "${ACTIVE_SERVER_PID}" >/dev/null 2>&1 || true
        fi
    fi

    wait "${ACTIVE_SERVER_PID}" >/dev/null 2>&1 || true
    ACTIVE_SERVER_PID=""
    ACTIVE_SERVER_STACK=""
}

trap stop_active_server EXIT

record_skip()
{
    local stack="$1"
    local reason="$2"
    printf "%s,%s\n" "${stack}" "${reason}" >>"${SKIP_CSV}"
    log "skip stack=${stack} reason=${reason}"
}

build_core_targets()
{
    if [[ "${BUILD_MODE}" == "reuse" ]]; then
        log "reusing core build directory: ${BUILD_DIR} (skip configure/build)"
        if [[ ! -f "${CLIENT_BIN}" ]]; then
            echo "Error: --reuse-build requires an existing client binary: ${CLIENT_BIN}" >&2
            return 1
        fi
        return 0
    fi

    log "configure bindings/c build"
    cmake -S "${ROOT_DIR}/bindings/c" -B "${BUILD_DIR}" \
        -DZLINK_C_BUILD_BENCHES=ON \
        -DZLINK_C_BUILD_BENCH_STREAMCOMPARE=ON \
        -DZLINK_C_BUILD_BENCH_ZMQ=OFF \
        -DZLINK_C_BUILD_BENCH_ROUTER_COMPARE=OFF \
        -DZLINK_C_BUILD_BENCH_GRPC_COMPARE=OFF \
        -DZLINK_CORE_DIR="${ZLINK_CORE_PACKAGE_PREFIX:-${ROOT_DIR}/core}" \
        -DZLINK_C_CORE_BUILD_DIR="${ZLINK_CORE_PACKAGE_PREFIX:-${ROOT_DIR}/core/build}" >/dev/null

    log "build bench_streamcompare_client"
    cmake --build "${BUILD_DIR}" --target bench_streamcompare_client \
        -j"$(nproc)" >/dev/null
}

prepare_build_directory_policy()
{
    case "${BUILD_MODE}" in
        reuse)
            if [[ ! -d "${BUILD_DIR}" ]]; then
                echo "Error: --reuse-build requires an existing build directory: ${BUILD_DIR}" >&2
                return 1
            fi
            log "build_mode=reuse build_dir=${BUILD_DIR}"
            ;;
        clean)
            log "build_mode=clean build_dir=${BUILD_DIR}"
            log "cleaning build directory: ${BUILD_DIR}"
            rm -rf "${BUILD_DIR}"
            rm -rf "${CPPSERVER_BUILD_DIR}"
            rm -rf "${DOTNET_OUT_DIR}" "${NETZLINK_OUT_DIR}" "${NETZLINK_LEN32BE_OUT_DIR}"
            rm -rf "${JVMZLINK_BUILD_DIR}" "${JVMZLINK_LEN32BE_BUILD_DIR}" "${NETTY_BUILD_DIR}"
            ;;
        incremental)
            if [[ -d "${BUILD_DIR}" ]]; then
                log "build_mode=incremental build_dir=${BUILD_DIR} (reuse existing directory)"
            else
                log "build_mode=incremental build_dir=${BUILD_DIR} (create directory)"
            fi
            ;;
        *)
            echo "Error: invalid build mode: ${BUILD_MODE}" >&2
            return 1
            ;;
    esac

    if [[ "${BUILD_MODE}" != "reuse" && -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        local cache_cmake_source=""
        cache_cmake_source="$(
            sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt" \
                | tail -n 1
        )"
        if [[ -n "${cache_cmake_source}" && "${cache_cmake_source}" != "${ROOT_DIR}/bindings/c" ]]; then
            log "build cache source mismatch detected: cache=${cache_cmake_source} required=${ROOT_DIR}/bindings/c"
            log "resetting build directory: ${BUILD_DIR}"
            rm -rf "${BUILD_DIR}"
        fi
    fi
}

ensure_cppserver_build_dir()
{
    local cache_file="${CPPSERVER_BUILD_DIR}/CMakeCache.txt"
    if [[ ! -f "${cache_file}" ]]; then
        return 0
    fi

    local cache_home=""
    cache_home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${cache_file}" | head -n1)"
    if [[ -z "${cache_home}" ]]; then
        return 0
    fi

    local expected_home=""
    expected_home="$(cd "${CPPSERVER_SRC_DIR}" && pwd)"
    if [[ "${cache_home}" != "${expected_home}" ]]; then
        log "cppserver cache mismatch: cache_source=${cache_home} expected_source=${expected_home}; cleanup=${CPPSERVER_BUILD_DIR}"
        rm -rf "${CPPSERVER_BUILD_DIR}"
    fi
}

build_core_tests_stream_target()
{
    local target="$1"
    cmake --build "${BUILD_DIR}" --target "${target}" -j"$(nproc)" >/dev/null
}

try_build_stack()
{
    local stack="$1"

    case "${stack}" in
        asio)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                [[ -f "${ASIO_BIN}" ]]
            else
                build_core_tests_stream_target "test_scenario_stream_asio"
            fi
            ;;
        zlink)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                [[ -f "${ZLINK_BIN}" ]]
            else
                build_core_tests_stream_target "test_scenario_stream_zlink"
            fi
            ;;
        zlink_packet)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                [[ -f "${ZLINK_PACKET_BIN}" ]]
            else
                build_core_tests_stream_target "test_scenario_stream_zlink_packet"
            fi
            ;;
        zmq)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                [[ -f "${ZMQ_BIN}" ]]
            else
                build_core_tests_stream_target "test_scenario_stream_zmq"
            fi
            ;;
        cppserver)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                [[ -f "${CPPSERVER_BIN}" ]]
                return 0
            fi
            cat >"${CPPSERVER_UPSTREAM_ENTRY}" <<'CPP'
#include "../../test_scenario_stream_cppserver.cpp"
CPP
            ensure_cppserver_build_dir
            cmake -S "${CPPSERVER_SRC_DIR}" -B "${CPPSERVER_BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release >/dev/null
            cmake --build "${CPPSERVER_BUILD_DIR}" --target cppserver-performance-stream_fixed_server -j"$(nproc)" >/dev/null
            ;;
        dotnet)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                dotnet build "${DOTNET_PROJECT}" -c Release -o "${DOTNET_OUT_DIR}" --no-restore >/dev/null
            else
                dotnet build "${DOTNET_PROJECT}" -c Release -o "${DOTNET_OUT_DIR}" >/dev/null
            fi
            ;;
        netzlink)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                dotnet build "${NETZLINK_PROJECT}" -c Release -o "${NETZLINK_OUT_DIR}" --no-restore >/dev/null
            else
                dotnet build "${NETZLINK_PROJECT}" -c Release -o "${NETZLINK_OUT_DIR}" >/dev/null
            fi
            [[ -f "${NETZLINK_DLL}" ]]
            ;;
        netzlink-len32be)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                [[ -f "${NETZLINK_LEN32BE_DLL}" ]]
            else
                dotnet build "${NETZLINK_LEN32BE_PROJECT}" -c Release -o "${NETZLINK_LEN32BE_OUT_DIR}" >/dev/null
                [[ -f "${NETZLINK_LEN32BE_DLL}" ]]
            fi
            ;;
        jvmzlink)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                [[ -x "${JVMZLINK_BIN}" ]]
                return 0
            fi
            resolve_netty_java || return 1
            resolve_netty_gradle || return 1
            log "jvmzlink using java major=${NETTY_JAVA_VERSION} home=${NETTY_JAVA_HOME} gradle=${NETTY_GRADLE_VERSION}"
            if [[ "${JAVA_BINDINGS_JAR_BUILT}" == "0" ]]; then
                JAVA_HOME="${NETTY_JAVA_HOME}" PATH="${NETTY_JAVA_HOME}/bin:${PATH}" \
                    "${BINDINGS_JAVA_PROJECT_DIR}/gradlew" -p "${BINDINGS_JAVA_PROJECT_DIR}" --no-daemon jar >/dev/null
                JAVA_BINDINGS_JAR_BUILT=1
            fi
            JAVA_HOME="${NETTY_JAVA_HOME}" PATH="${NETTY_JAVA_HOME}/bin:${PATH}" \
                "${NETTY_GRADLE_BIN}" -p "${JVMZLINK_PROJECT_DIR}" --no-daemon installDist >/dev/null
            [[ -x "${JVMZLINK_BIN}" ]]
            ;;
        jvmzlink-recv)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                [[ -x "${JVMZLINK_RECV_BIN}" ]]
                return 0
            fi
            resolve_netty_java || return 1
            resolve_netty_gradle || return 1
            log "jvmzlink-recv using java major=${NETTY_JAVA_VERSION} home=${NETTY_JAVA_HOME} gradle=${NETTY_GRADLE_VERSION}"
            if [[ "${JAVA_BINDINGS_JAR_BUILT}" == "0" ]]; then
                JAVA_HOME="${NETTY_JAVA_HOME}" PATH="${NETTY_JAVA_HOME}/bin:${PATH}" \
                    "${BINDINGS_JAVA_PROJECT_DIR}/gradlew" -p "${BINDINGS_JAVA_PROJECT_DIR}" --no-daemon jar >/dev/null
                JAVA_BINDINGS_JAR_BUILT=1
            fi
            JAVA_HOME="${NETTY_JAVA_HOME}" PATH="${NETTY_JAVA_HOME}/bin:${PATH}" \
                "${NETTY_GRADLE_BIN}" -p "${JVMZLINK_RECV_PROJECT_DIR}" --no-daemon installDist >/dev/null
            [[ -x "${JVMZLINK_RECV_BIN}" ]]
            ;;
        jvmzmq)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                [[ -x "${JVMZMQ_BIN}" ]]
                return 0
            fi
            resolve_netty_java || return 1
            resolve_netty_gradle || return 1
            log "jvmzmq using java major=${NETTY_JAVA_VERSION} home=${NETTY_JAVA_HOME} gradle=${NETTY_GRADLE_VERSION}"
            JAVA_HOME="${NETTY_JAVA_HOME}" PATH="${NETTY_JAVA_HOME}/bin:${PATH}" \
                "${NETTY_GRADLE_BIN}" -p "${JVMZMQ_PROJECT_DIR}" --no-daemon installDist >/dev/null
            [[ -x "${JVMZMQ_BIN}" ]]
            ;;
        jvmzlink-len32be)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                [[ -x "${JVMZLINK_LEN32BE_BIN}" ]]
                return 0
            fi
            resolve_netty_java || return 1
            resolve_netty_gradle || return 1
            log "jvmzlink-len32be using java major=${NETTY_JAVA_VERSION} home=${NETTY_JAVA_HOME} gradle=${NETTY_GRADLE_VERSION}"
            if [[ "${JAVA_BINDINGS_JAR_BUILT}" == "0" ]]; then
                JAVA_HOME="${NETTY_JAVA_HOME}" PATH="${NETTY_JAVA_HOME}/bin:${PATH}" \
                    "${BINDINGS_JAVA_PROJECT_DIR}/gradlew" -p "${BINDINGS_JAVA_PROJECT_DIR}" --no-daemon jar >/dev/null
                JAVA_BINDINGS_JAR_BUILT=1
            fi
            JAVA_HOME="${NETTY_JAVA_HOME}" PATH="${NETTY_JAVA_HOME}/bin:${PATH}" \
                "${NETTY_GRADLE_BIN}" -p "${JVMZLINK_LEN32BE_PROJECT_DIR}" --no-daemon installDist >/dev/null
            [[ -x "${JVMZLINK_LEN32BE_BIN}" ]]
            ;;
        netty)
            if [[ "${BUILD_MODE}" == "reuse" ]]; then
                [[ -x "${NETTY_BIN}" ]]
                return 0
            fi
            resolve_netty_java || return 1
            resolve_netty_gradle || return 1
            log "netty using java major=${NETTY_JAVA_VERSION} home=${NETTY_JAVA_HOME} gradle=${NETTY_GRADLE_VERSION}"
            JAVA_HOME="${NETTY_JAVA_HOME}" PATH="${NETTY_JAVA_HOME}/bin:${PATH}" \
                "${NETTY_GRADLE_BIN}" -p "${NETTY_PROJECT_DIR}" --no-daemon installDist >/dev/null
            [[ -x "${NETTY_BIN}" ]]
            ;;
        *)
            return 1
            ;;
    esac
}

build_selected()
{
    build_core_targets

    local stack
    for stack in "${RUN_STACKS[@]}"; do
        log "build stack=${stack}"
        set +e
        try_build_stack "${stack}"
        local rc="$?"
        set -e
        if [[ "${rc}" == "0" ]]; then
            ACTIVE_STACKS+=("${stack}")
        else
            record_skip "${stack}" "build_failed"
        fi
    done
}

start_server()
{
    local stack="$1"
    local host="$2"
    local port="$3"
    local server_log="$4"
    local size_hint="$5"

    local -a cmd=()
    local -a env_cmd=()
    case "${stack}" in
        asio)
            cmd=("${ASIO_BIN}")
            ;;
        zlink)
            cmd=("${ZLINK_BIN}")
            ;;
        zlink_packet)
            cmd=("${ZLINK_PACKET_BIN}")
            ;;
        zmq)
            cmd=("${ZMQ_BIN}")
            ;;
        cppserver)
            cmd=("${CPPSERVER_BIN}")
            ;;
        dotnet)
            cmd=(dotnet "${DOTNET_DLL}")
            ;;
        netzlink)
            cmd=(dotnet "${NETZLINK_DLL}")
            ;;
        netzlink-len32be)
            cmd=(dotnet "${NETZLINK_LEN32BE_DLL}")
            ;;
        jvmzlink)
            cmd=("${JVMZLINK_BIN}")
            ;;
        jvmzlink-recv)
            cmd=("${JVMZLINK_RECV_BIN}")
            ;;
        jvmzmq)
            cmd=("${JVMZMQ_BIN}")
            ;;
        jvmzlink-len32be)
            cmd=("${JVMZLINK_LEN32BE_BIN}")
            ;;
        netty)
            cmd=("${NETTY_BIN}")
            ;;
        *)
            return 2
            ;;
    esac

    resolve_stack_tuning "${stack}"

    cmd+=(
        --host "${host}"
        --port "${port}"
        --size "${size_hint}"
        --sndbuf "${STACK_SNDBUF}"
        --rcvbuf "${STACK_RCVBUF}"
        --backlog "${STACK_BACKLOG}"
        --tcp-nodelay "${STACK_TCP_NODELAY}"
        --io-threads "${STACK_IO_THREADS}"
    )

    if [[ ${#STACK_ENV_VARS[@]} -gt 0 ]]; then
        env_cmd=(env "${STACK_ENV_VARS[@]}")
    fi

    log "stack_tuning stack=${stack} io_threads=${STACK_IO_THREADS} sndbuf=${STACK_SNDBUF} rcvbuf=${STACK_RCVBUF} backlog=${STACK_BACKLOG}"

    if [[ "${stack}" == "zmq" ]]; then
        (
            export LD_LIBRARY_PATH="${ZMQ_LIB_DIR}:${LD_LIBRARY_PATH:-}"
            if [[ ${#env_cmd[@]} -gt 0 ]]; then
                exec "${env_cmd[@]}" "${cmd[@]}" >"${server_log}" 2>&1
            else
                exec "${cmd[@]}" >"${server_log}" 2>&1
            fi
        ) &
    elif [[ "${stack}" == "netty" || "${stack}" == "jvmzlink" || "${stack}" == "jvmzlink-recv" || "${stack}" == "jvmzmq" || "${stack}" == "jvmzlink-len32be" ]]; then
        (
            export JAVA_HOME="${NETTY_JAVA_HOME}"
            export PATH="${NETTY_JAVA_HOME}/bin:${PATH}"
            if [[ ${#env_cmd[@]} -gt 0 ]]; then
                exec "${env_cmd[@]}" "${cmd[@]}" >"${server_log}" 2>&1
            else
                exec "${cmd[@]}" >"${server_log}" 2>&1
            fi
        ) &
    else
        (
            if [[ ${#env_cmd[@]} -gt 0 ]]; then
                exec "${env_cmd[@]}" "${cmd[@]}" >"${server_log}" 2>&1
            else
                exec "${cmd[@]}" >"${server_log}" 2>&1
            fi
        ) &
    fi

    ACTIVE_SERVER_PID="$!"
    ACTIVE_SERVER_STACK="${stack}"

    if ! wait_for_port "${host}" "${port}" "${SERVER_START_TIMEOUT}"; then
        return 1
    fi

    return 0
}

append_rows_from_client_log()
{
    local stack="$1"
    local client_rc="$2"
    local client_log="$3"
    local server_log="$4"
    local client_resource_summary="$5"
    local server_resource_summary="$6"
    local client_resource_log="$7"
    local server_resource_log="$8"
    local system_resource_summary="$9"
    local system_resource_log="${10}"
    local run_override="${11}"
    local phase_override="${12}"

    python3 - "${METRICS_CSV}" "${stack}" "${CCU}" "${client_rc}" \
        "${client_log}" "${server_log}" \
        "${client_resource_summary}" "${server_resource_summary}" \
        "${client_resource_log}" "${server_resource_log}" \
        "${system_resource_summary}" "${system_resource_log}" \
        "${run_override}" "${phase_override}" <<'PY'
import csv
import sys

(
    metrics_csv,
    stack,
    ccu,
    client_rc,
    client_log,
    server_log,
    client_resource_summary,
    server_resource_summary,
    client_resource_log,
    server_resource_log,
    system_resource_summary,
    system_resource_log,
    run_override,
    phase_override,
) = sys.argv[1:15]
ccu = int(ccu)

rows = []


def load_resource_summary(path):
    out = {
        "sample_count": "0",
        "avg_cpu_pct": "0.00",
        "peak_cpu_pct": "0.00",
        "avg_rss_kb": "0",
        "peak_rss_kb": "0",
        "peak_hwm_kb": "0",
        "avg_mem_used_kb": "0",
        "peak_mem_used_kb": "0",
        "avg_mem_used_pct": "0",
        "peak_mem_used_pct": "0",
    }
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw_line in f:
                line = raw_line.strip()
                if "=" not in line:
                    continue
                key, value = line.split("=", 1)
                if key in out:
                    out[key] = value.strip()
    except Exception:
        pass
    return out

def parse_float(text, fallback=0.0):
    try:
        return float(text)
    except Exception:
        return fallback


client_resource = load_resource_summary(client_resource_summary)
server_resource = load_resource_summary(server_resource_summary)
system_resource = load_resource_summary(system_resource_summary)

with open(client_log, encoding="utf-8", errors="replace") as f:
    csv_metrics = {}
    for raw_line in f:
        line = raw_line.strip()
        if not line.startswith("RESULT"):
            continue

        if "," in line:
            parts = [part.strip() for part in line.split(",")]
            if len(parts) == 7 and parts[0] == "RESULT":
                _, impl_name, phase_name, transport_name, size_text, metric_name, metric_value = parts
                if phase_name != "STREAM":
                    continue
                metric_row = csv_metrics.setdefault(size_text, {
                    "stack": stack,
                    "phase": phase_override if phase_override else "current",
                    "size": size_text,
                    "run": run_override if run_override and run_override != "0" else "1",
                    "ccu": str(ccu),
                    "client_rc": str(client_rc),
                    "throughput_bps": "0",
                    "throughput_mib_s": "0",
                    "throughput_tps": "0",
                    "p50_us": "0",
                    "p95_us": "0",
                    "p99_us": "0",
                    "connect_ok": str(ccu),
                    "connect_fail": "0",
                    "send_err": "0",
                    "recv_err": "0",
                    "timeout": "0",
                    "size_mismatch": "0",
                    "pass_fail": "PASS",
                    "client_avg_cpu_pct": client_resource["avg_cpu_pct"],
                    "client_peak_cpu_pct": client_resource["peak_cpu_pct"],
                    "client_avg_rss_kb": client_resource["avg_rss_kb"],
                    "client_peak_rss_kb": client_resource["peak_rss_kb"],
                    "client_peak_hwm_kb": client_resource["peak_hwm_kb"],
                    "server_avg_cpu_pct": server_resource["avg_cpu_pct"],
                    "server_peak_cpu_pct": server_resource["peak_cpu_pct"],
                    "server_avg_rss_kb": server_resource["avg_rss_kb"],
                    "server_peak_rss_kb": server_resource["peak_rss_kb"],
                    "server_peak_hwm_kb": server_resource["peak_hwm_kb"],
                    "system_avg_cpu_pct": system_resource["avg_cpu_pct"],
                    "system_peak_cpu_pct": system_resource["peak_cpu_pct"],
                    "system_avg_mem_used_kb": system_resource.get("avg_mem_used_kb", "0"),
                    "system_peak_mem_used_kb": system_resource.get("peak_mem_used_kb", "0"),
                    "system_avg_mem_used_pct": system_resource.get("avg_mem_used_pct", "0"),
                    "system_peak_mem_used_pct": system_resource.get("peak_mem_used_pct", "0"),
                    "client_resource_log": client_resource_log,
                    "server_resource_log": server_resource_log,
                    "system_resource_log": system_resource_log,
                    "client_log": client_log,
                    "server_log": server_log,
                })

                size_value = 0
                try:
                    size_value = int(size_text)
                except Exception:
                    size_value = 0

                numeric = parse_float(metric_value, 0.0)
                if metric_name == "throughput":
                    metric_row["throughput_tps"] = f"{numeric:.2f}"
                    if size_value > 0:
                        metric_row["throughput_bps"] = f"{numeric * float(size_value):.2f}"
                elif metric_name == "bandwidth":
                    metric_row["throughput_mib_s"] = metric_value
                elif metric_name == "latency":
                    metric_row["p50_us"] = f"{numeric * 1000.0:.2f}"
                elif metric_name == "latency_p95":
                    metric_row["p95_us"] = f"{numeric * 1000.0:.2f}"
                elif metric_name == "latency_p99":
                    metric_row["p99_us"] = f"{numeric * 1000.0:.2f}"
                continue

        fields = {}
        for token in line.split()[1:]:
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            fields[key] = value

        size_text = fields.get("size", "0")
        size = 0
        try:
            size = int(size_text)
        except Exception:
            size = 0
        throughput_bps = parse_float(fields.get("throughput_bps", "0"), 0.0)
        throughput_tps = 0.0
        if size > 0:
            throughput_tps = throughput_bps / float(size)

        row = {
            "stack": stack,
            "phase": phase_override if phase_override else fields.get("phase", ""),
            "size": size_text,
            "run": run_override if run_override and run_override != "0" else fields.get("run", "0"),
            "ccu": str(ccu),
            "client_rc": str(client_rc),
            "throughput_bps": fields.get("throughput_bps", "0"),
            "throughput_mib_s": fields.get("throughput_mib_s", "0"),
            "throughput_tps": f"{throughput_tps:.2f}",
            "p50_us": fields.get("p50_us", "0"),
            "p95_us": fields.get("p95_us", "0"),
            "p99_us": fields.get("p99_us", "0"),
            "connect_ok": fields.get("connect_ok", "0"),
            "connect_fail": fields.get("connect_fail", "0"),
            "send_err": fields.get("send_err", "0"),
            "recv_err": fields.get("recv_err", "0"),
            "timeout": fields.get("timeout", "0"),
            "size_mismatch": fields.get("size_mismatch", "0"),
            "pass_fail": fields.get("pass_fail", "FAIL"),
            "client_avg_cpu_pct": client_resource["avg_cpu_pct"],
            "client_peak_cpu_pct": client_resource["peak_cpu_pct"],
            "client_avg_rss_kb": client_resource["avg_rss_kb"],
            "client_peak_rss_kb": client_resource["peak_rss_kb"],
            "client_peak_hwm_kb": client_resource["peak_hwm_kb"],
            "server_avg_cpu_pct": server_resource["avg_cpu_pct"],
            "server_peak_cpu_pct": server_resource["peak_cpu_pct"],
            "server_avg_rss_kb": server_resource["avg_rss_kb"],
            "server_peak_rss_kb": server_resource["peak_rss_kb"],
            "server_peak_hwm_kb": server_resource["peak_hwm_kb"],
            "system_avg_cpu_pct": system_resource["avg_cpu_pct"],
            "system_peak_cpu_pct": system_resource["peak_cpu_pct"],
            "system_avg_mem_used_kb": system_resource.get("avg_mem_used_kb", "0"),
            "system_peak_mem_used_kb": system_resource.get("peak_mem_used_kb", "0"),
            "system_avg_mem_used_pct": system_resource.get("avg_mem_used_pct", "0"),
            "system_peak_mem_used_pct": system_resource.get("peak_mem_used_pct", "0"),
            "client_resource_log": client_resource_log,
            "server_resource_log": server_resource_log,
            "system_resource_log": system_resource_log,
            "client_log": client_log,
            "server_log": server_log,
        }
        rows.append(row)

for row in csv_metrics.values():
    rows.append(row)

with open(metrics_csv, "a", newline="", encoding="utf-8") as f:
    writer = csv.DictWriter(
        f,
        fieldnames=[
            "stack", "phase", "size", "run", "ccu", "client_rc",
            "throughput_bps", "throughput_mib_s", "throughput_tps",
            "p50_us", "p95_us",
            "p99_us", "connect_ok", "connect_fail", "send_err",
            "recv_err", "timeout", "size_mismatch", "pass_fail",
            "client_avg_cpu_pct", "client_peak_cpu_pct",
            "client_avg_rss_kb", "client_peak_rss_kb", "client_peak_hwm_kb",
            "server_avg_cpu_pct", "server_peak_cpu_pct",
            "server_avg_rss_kb", "server_peak_rss_kb", "server_peak_hwm_kb",
            "system_avg_cpu_pct", "system_peak_cpu_pct",
            "system_avg_mem_used_kb", "system_peak_mem_used_kb",
            "system_avg_mem_used_pct", "system_peak_mem_used_pct",
            "client_resource_log", "server_resource_log", "system_resource_log",
            "client_log", "server_log",
        ],
    )
    for row in rows:
        writer.writerow(row)

print(len(rows))
PY
}

run_stack_size_once()
{
    local stack="$1"
    local port="$2"
    local client_log="$3"
    local size_value="$4"

    local client_resource_log="${client_log%.log}_resource.csv"
    local client_resource_summary="${client_log%.log}_resource.summary"
    local client_pid=""
    local client_monitor_pid=""

    "${CLIENT_BIN}" \
        --host "${HOST}" \
        --port "${port}" \
        --ccu "${CCU}" \
        --sizes "${size_value}" \
        --runs "1" \
        --warmup "${WARMUP}" \
        --duration "${DURATION}" \
        --io-threads "${CLIENT_IO_THREADS}" \
        >"${client_log}" 2>&1 &
    client_pid="$!"

    MONITOR_PID=""
    start_process_resource_monitor "${client_pid}" "${RESOURCE_SAMPLE_MS}" "${client_resource_log}" "${client_resource_summary}"
    local client_monitor_rc="$?"
    client_monitor_pid="${MONITOR_PID:-}"
    if [[ "${client_monitor_rc}" != "0" || -z "${client_monitor_pid}" ]]; then
        client_monitor_pid=""
        log "warning: failed to start client resource monitor stack=${stack}"
    fi

    local client_rc=0
    wait "${client_pid}" || client_rc="$?"
    if [[ -n "${client_monitor_pid}" ]]; then
        wait "${client_monitor_pid}" >/dev/null 2>&1 || true
    fi

    return "${client_rc}"
}

main()
{
    mkdir -p "${RESULT_DIR}" "${LOG_DIR}"
    : >"${SCENARIO_LOG}"

    cat >"${METRICS_CSV}" <<'CSV'
stack,phase,size,run,ccu,client_rc,throughput_bps,throughput_mib_s,throughput_tps,p50_us,p95_us,p99_us,connect_ok,connect_fail,send_err,recv_err,timeout,size_mismatch,pass_fail,client_avg_cpu_pct,client_peak_cpu_pct,client_avg_rss_kb,client_peak_rss_kb,client_peak_hwm_kb,server_avg_cpu_pct,server_peak_cpu_pct,server_avg_rss_kb,server_peak_rss_kb,server_peak_hwm_kb,system_avg_cpu_pct,system_peak_cpu_pct,system_avg_mem_used_kb,system_peak_mem_used_kb,system_avg_mem_used_pct,system_peak_mem_used_pct,client_resource_log,server_resource_log,system_resource_log,client_log,server_log
CSV

    cat >"${SKIP_CSV}" <<'CSV'
stack,reason
CSV

    log "scope stacks=$(IFS=,; echo "${RUN_STACKS[*]}") sizes=$(IFS=,; echo "${RUN_SIZES[*]}")"
    log "settings mode=single-pass ccu=${CCU} runs=${RUNS} warmup=${WARMUP}s duration=${DURATION}s client_io_threads=${CLIENT_IO_THREADS} server_io_threads=${SERVER_IO_THREADS} resource_sample_ms=${RESOURCE_SAMPLE_MS}"
    log "build_settings build_dir=${BUILD_DIR} build_mode=${BUILD_MODE} reuse_build=$( [[ "${BUILD_MODE}" == "reuse" ]] && echo 1 || echo 0 ) clean_build=$( [[ "${BUILD_MODE}" == "clean" ]] && echo 1 || echo 0 )"

    prepare_build_directory_policy

    build_selected
    zlink_export_local_core_runtime
    log "core_runtime=$(readlink -f "${ZLINK_CORE_LIBRARY}")"

    if [[ ${#ACTIVE_STACKS[@]} -eq 0 ]]; then
        log "no active stacks after build"
        return 1
    fi

    local -A STACK_FAILED=()
    local size
    for size in "${RUN_SIZES[@]}"; do
        local run_idx
        for ((run_idx = 1; run_idx <= RUNS; ++run_idx)); do
            local stack
            for stack in "${ACTIVE_STACKS[@]}"; do
                if [[ -n "${STACK_FAILED[${stack}]:-}" ]]; then
                    continue
                fi

                if ! allocate_free_port "${HOST}"; then
                    record_skip "${stack}" "port_allocation_failed"
                    STACK_FAILED["${stack}"]="port_allocation_failed"
                    FAILED_CASES="$((FAILED_CASES + 1))"
                    continue
                fi

                local port="${ALLOCATED_PORT}"
                local stack_failed=0
                local log_tag="${stack}_run${run_idx}_size${size}"
                local server_log="${LOG_DIR}/${log_tag}_server.log"
                local client_log="${LOG_DIR}/${log_tag}_client.log"
                local client_resource_log="${LOG_DIR}/${log_tag}_client_resource.csv"
                local client_resource_summary="${LOG_DIR}/${log_tag}_client_resource.summary"
                local server_resource_log="${LOG_DIR}/${log_tag}_server_resource.csv"
                local server_resource_summary="${LOG_DIR}/${log_tag}_server_resource.summary"
                local system_resource_log="${LOG_DIR}/${log_tag}_system_resource.csv"
                local system_resource_summary="${LOG_DIR}/${log_tag}_system_resource.summary"
                local server_monitor_pid=""
                local system_monitor_pid=""

                log "start stack=${stack} size=${size} run=${run_idx}/${RUNS} port=${port}"
                if ! start_server "${stack}" "${HOST}" "${port}" "${server_log}" "${size}"; then
                    log "server start failed stack=${stack} size=${size} run=${run_idx}"
                    stop_active_server
                    FAILED_CASES="$((FAILED_CASES + 1))"
                    stack_failed=1
                else
                    set +e
                    MONITOR_PID=""
                    start_process_resource_monitor "${ACTIVE_SERVER_PID}" "${RESOURCE_SAMPLE_MS}" "${server_resource_log}" "${server_resource_summary}"
                    local server_monitor_rc="$?"
                    server_monitor_pid="${MONITOR_PID:-}"
                    set -e
                    if [[ "${server_monitor_rc}" != "0" || -z "${server_monitor_pid}" ]]; then
                        server_monitor_pid=""
                        log "warning: failed to start server resource monitor stack=${stack} size=${size} run=${run_idx}"
                    fi

                    set +e
                    MONITOR_PID=""
                    start_system_resource_monitor "${RESOURCE_SAMPLE_MS}" "${system_resource_log}" "${system_resource_summary}"
                    local system_monitor_rc="$?"
                    system_monitor_pid="${MONITOR_PID:-}"
                    set -e
                    if [[ "${system_monitor_rc}" != "0" || -z "${system_monitor_pid}" ]]; then
                        system_monitor_pid=""
                        log "warning: failed to start system resource monitor stack=${stack} size=${size} run=${run_idx}"
                    fi

                    set +e
                    run_stack_size_once "${stack}" "${port}" "${client_log}" "${size}"
                    local client_rc="$?"
                    set -e

                    stop_active_server
                    if [[ -n "${server_monitor_pid}" ]]; then
                        wait "${server_monitor_pid}" >/dev/null 2>&1 || true
                    fi
                    if [[ -n "${system_monitor_pid}" ]]; then
                        kill -TERM "${system_monitor_pid}" >/dev/null 2>&1 || true
                        wait "${system_monitor_pid}" >/dev/null 2>&1 || true
                    fi

                    local row_count_throughput
                    row_count_throughput="$(append_rows_from_client_log \
                        "${stack}" \
                        "${client_rc}" \
                        "${client_log}" \
                        "${server_log}" \
                        "${client_resource_summary}" \
                        "${server_resource_summary}" \
                        "${client_resource_log}" \
                        "${server_resource_log}" \
                        "${system_resource_summary}" \
                        "${system_resource_log}" \
                        "${run_idx}" \
                        "throughput")"
                    local row_count_latency
                    row_count_latency="$(append_rows_from_client_log \
                        "${stack}" \
                        "${client_rc}" \
                        "${client_log}" \
                        "${server_log}" \
                        "${client_resource_summary}" \
                        "${server_resource_summary}" \
                        "${client_resource_log}" \
                        "${server_resource_log}" \
                        "${system_resource_summary}" \
                        "${system_resource_log}" \
                        "${run_idx}" \
                        "latency")"
                    local row_count="$((row_count_throughput + row_count_latency))"

                    if [[ "${client_rc}" != "0" ]]; then
                        log "client failed stack=${stack} size=${size} run=${run_idx} rc=${client_rc}"
                        FAILED_CASES="$((FAILED_CASES + 1))"
                        stack_failed=1
                    elif [[ "${row_count}" == "0" ]]; then
                        log "no RESULT rows parsed stack=${stack} size=${size} run=${run_idx}"
                        FAILED_CASES="$((FAILED_CASES + 1))"
                        stack_failed=1
                    fi
                fi

                if (( stack_failed != 0 )); then
                    record_skip "${stack}" "run_failed"
                    STACK_FAILED["${stack}"]="run_failed"
                fi

                if (( STACK_GAP_SEC > 0 )); then
                    sleep "${STACK_GAP_SEC}"
                fi
            done
        done

        local size_summary_json="${RESULT_DIR}/summary_size${size}.json"
        local size_report_md="${RESULT_DIR}/comparison_size${size}.md"
        python3 "${SCRIPT_DIR}/run_comparison.py" \
            --metrics "${METRICS_CSV}" \
            --summary "${size_summary_json}" \
            --report "${size_report_md}" \
            --runs "${RUNS}" \
            --stacks "$(IFS=,; echo "${RUN_STACKS[*]}")" \
            --sizes "${size}" \
            --phases "throughput,latency" \
            --skip-file "${SKIP_CSV}"
        log "size_comparison size=${size} summary_json=${size_summary_json} comparison_md=${size_report_md}"
        if [[ -s "${size_summary_json}" ]]; then
            local size_console_summary
            size_console_summary="$(python3 - "${size_summary_json}" "${size_report_md}" <<'PY'
import json
import sys

summary_path = sys.argv[1]
report_path = sys.argv[2]

with open(summary_path, encoding="utf-8") as f:
    summary = json.load(f)

sizes = summary.get("config", {}).get("sizes", [])
size = str(sizes[0]) if sizes else "n/a"

def f2(v):
    if v is None:
        return "n/a"
    return f"{float(v):.2f}"

def us_to_ms(v):
    if v is None:
        return None
    return float(v) / 1000.0

print(f"===== SIZE {size} SUMMARY =====")
for phase in ("throughput", "latency"):
    key = f"{phase}:{size}"
    ranking = summary.get("ranking", {}).get(key, [])
    print(f"[{phase}] ranking(all)")
    print(" rank stack             kops       mean_ms  mismatch")
    if not ranking:
        print("  -   (no data)")
        continue
    for idx, entry in enumerate(ranking, 1):
        stack = str(entry.get("stack", "n/a"))[:16]
        kops = f2(entry.get("median_kops"))
        mean = us_to_ms(entry.get("median_mean_us"))
        mean_text = f2(mean)
        mismatch = int(entry.get("mismatch_total_all", 0))
        print(f" {idx:>2}   {stack:<16} {kops:>10} {mean_text:>10} {mismatch:>9}")
print(f"full_report={report_path}")
print(f"summary_json={summary_path}")
print(f"===== SIZE {size} SUMMARY END =====")
PY
)"
            echo "${size_console_summary}"
            echo "${size_console_summary}" >>"${SCENARIO_LOG}"
        fi
    done

    python3 "${SCRIPT_DIR}/run_comparison.py" \
        --metrics "${METRICS_CSV}" \
        --summary "${SUMMARY_JSON}" \
        --report "${REPORT_MD}" \
        --runs "${RUNS}" \
        --stacks "$(IFS=,; echo "${RUN_STACKS[*]}")" \
        --sizes "$(IFS=,; echo "${RUN_SIZES[*]}")" \
        --phases "throughput,latency" \
        --skip-file "${SKIP_CSV}"

    log "result_dir=${RESULT_DIR}"
    log "metrics_csv=${METRICS_CSV}"
    log "summary_json=${SUMMARY_JSON}"
    log "comparison_md=${REPORT_MD}"

    if (( FAILED_CASES > 0 )); then
        log "completed with failures failed_cases=${FAILED_CASES}"
        return 1
    fi

    log "completed"
    return 0
}

main "$@"
