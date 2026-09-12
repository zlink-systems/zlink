#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${ROOT_DIR}/../../../.." && pwd)"

# README §7.2 formula 1은 `zlink-<lang> / zlink-c`다. 두 행이 같은 조건에서 잰 값이어야
# 그 비율이 binding 계층 비용이 된다. 언어 행은 모두 로컬 패키지의 Core를 로드하므로
# C 기준도 같은 바이너리를 써야 한다 — `core/build`의 개발 빌드와는 실측 6% 차이가
# 난다(#308: 600.2 대 636.6 KOPS, latency 13.1 대 8.1 ms).
ZLINK_LOCAL_PACKAGE_ROOT="${ZLINK_LOCAL_PACKAGE_ROOT:-${REPO_ROOT}/.artifacts/wsl}"
ZLINK_C_CORE_VERSION="${ZLINK_C_CORE_VERSION:-$(sed -n 's/^LIBZLINK_VERSION=//p' "${REPO_ROOT}/VERSION")}"
ZLINK_C_CORE_DEFAULT_PREFIX="${ZLINK_LOCAL_PACKAGE_ROOT}/install/zlink-core/${ZLINK_C_CORE_VERSION}"
if [[ ! -d "${ZLINK_C_CORE_DEFAULT_PREFIX}" ]]; then
  echo "C 기준 벤치가 쓸 Core 패키지가 없다: ${ZLINK_C_CORE_DEFAULT_PREFIX}" >&2
  echo "ZLINK_CORE_PACKAGE_PREFIX로 명시하거나 로컬 패키지를 먼저 만들어라." >&2
  exit 1
fi
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
RUN_STAMP="${RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
OUTPUT="${OUTPUT:-${ROOT_DIR}/../log/c/with_grpc_c_${RUN_STAMP}}"
REPORT_FILE="${REPORT_FILE:-with_grpc_c_${RUN_STAMP}.txt}"
REPORT_PATH="${OUTPUT}/${REPORT_FILE}"
PAYLOAD_SIZES="${PAYLOAD_SIZES:-1024,4096}"
DURATION_SECONDS="${DURATION_SECONDS:-3}"
WINDOW_SIZE="${WINDOW_SIZE:-100}"
MAX_OUTSTANDING="${MAX_OUTSTANDING:-4096}"
DRAIN_TIMEOUT_MS="${DRAIN_TIMEOUT_MS:-5000}"
ENABLE_ZMQ_SEND_SEND="${ENABLE_ZMQ_SEND_SEND:-0}"

# SKIP_BUILD=1 keeps the build out of a measurement window (plan 3.2: no build
# may overlap a measurement). Pre-build with the same targets, then measure.
if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DZLINK_C_CORE_BUILD_DIR="${ZLINK_CORE_PACKAGE_PREFIX:-${ZLINK_C_CORE_DEFAULT_PREFIX}}"
build_targets=(
  bench_c_with_grpc_zlink_server
  bench_c_with_grpc_zlink_client
  bench_c_with_grpc_grpc_server
  bench_c_with_grpc_grpc_client
)
if [[ "${ENABLE_ZMQ_SEND_SEND}" == "1" ]]; then
  build_targets+=(
    bench_c_with_grpc_zmq_server
    bench_c_with_grpc_zmq_client
  )
fi
cmake --build "${BUILD_DIR}" --target "${build_targets[@]}" -j"$(nproc)"
fi

mkdir -p "${OUTPUT}"
: >"${REPORT_PATH}"

zlink_server="${BUILD_DIR}/bench_c_with_grpc_zlink_server"
zlink_client="${BUILD_DIR}/bench_c_with_grpc_zlink_client"
grpc_server="${BUILD_DIR}/bench_c_with_grpc_grpc_server"
grpc_client="${BUILD_DIR}/bench_c_with_grpc_grpc_client"
zmq_server="${BUILD_DIR}/bench_c_with_grpc_zmq_server"
zmq_client="${BUILD_DIR}/bench_c_with_grpc_zmq_client"

export PAYLOAD_SIZES
export DURATION_SECONDS
export WINDOW_SIZE
export MAX_OUTSTANDING
export DRAIN_TIMEOUT_MS

# `setsid` forks when the caller is not already a process-group leader, so `$!`
# is a short-lived wrapper rather than the server. SERVER_PID is what the client
# samples server CPU/RSS from, and killing the wrapper's group does not reach the
# server, so each server records its own pid into a pidfile and we read that.
start_server() {
  local name="$1" binary="$2" pidfile="${OUTPUT}/$1.pid"
  rm -f "${pidfile}"
  setsid bash -c 'echo $$ >"$1"; exec "$2"' _ "${pidfile}" "${binary}" \
    >"${OUTPUT}/${name}.log" 2>&1 &
  for _ in $(seq 1 200); do
    if [[ -s "${pidfile}" ]]; then
      cat "${pidfile}"
      return 0
    fi
    sleep 0.05
  done
  echo "[bench] failed to start ${name}" >&2
  return 1
}

zlink_pid="$(start_server zlink-server "${zlink_server}")"
grpc_pid="$(start_server grpc-server "${grpc_server}")"
zmq_pid=""
if [[ "${ENABLE_ZMQ_SEND_SEND}" == "1" ]]; then
  zmq_pid="$(start_server zmq-server "${zmq_server}")"
fi

cleanup() {
  status=$?
  set +e
  kill -TERM -- "-${zlink_pid}" "-${grpc_pid}" >/dev/null 2>&1 || true
  if [[ -n "${zmq_pid}" ]]; then
    kill -TERM -- "-${zmq_pid}" >/dev/null 2>&1 || true
  fi
  sleep 1
  kill -KILL -- "-${zlink_pid}" "-${grpc_pid}" >/dev/null 2>&1 || true
  if [[ -n "${zmq_pid}" ]]; then
    kill -KILL -- "-${zmq_pid}" >/dev/null 2>&1 || true
  fi
  wait "${zlink_pid}" "${grpc_pid}" >/dev/null 2>&1 || true
  if [[ -n "${zmq_pid}" ]]; then
    wait "${zmq_pid}" >/dev/null 2>&1 || true
  fi
  exit "${status}"
}
trap cleanup EXIT

sleep 1
{
  echo "C zlink API vs gRPC local bench"
  echo
  echo "Effective Options:"
  echo "  generated_local: $(date '+%Y-%m-%dT%H:%M:%S%z')"
  echo "  payload_sizes: ${PAYLOAD_SIZES}"
  echo "  duration_seconds: ${DURATION_SECONDS}"
  echo "  window_size: ${WINDOW_SIZE}"
  echo "  max_outstanding: ${MAX_OUTSTANDING}"
  echo "  drain_timeout_ms: ${DRAIN_TIMEOUT_MS}"
  echo
  printf '| %-28s | %8s | %21s | %15s | %12s | %11s | %11s | %8s | %8s | %8s | %8s | %10s | %10s | %8s | %8s | %8s | %12s |\n' \
    "Scenario" "Size" "Throughput" "Bandwidth" "Lat.Mean(ms)" "Lat.P95(ms)" "Lat.P99(ms)" \
    "C.CPU%" "C.Mem MB" "S.CPU%" "S.Mem MB" "Submitted" "Completed" "Errors" "Blocked" \
    "MaxOut" "SubmitMs"
  printf '|%-30s|%-10s|%-23s|%-17s|%-14s|%-13s|%-13s|%-10s|%-10s|%-10s|%-10s|%-12s|%-12s|%-10s|%-10s|%-10s|%-14s|\n' \
    "------------------------------" "----------" "-----------------------" "-----------------" \
    "--------------" "-------------" "-------------" "----------" "----------" "----------" "----------" \
    "------------" "------------" "----------" "----------" "----------" "--------------"
  SERVER_PID="${grpc_pid}" "${grpc_client}"
  SERVER_PID="${zlink_pid}" "${zlink_client}"
  if [[ "${ENABLE_ZMQ_SEND_SEND}" == "1" ]]; then
    SERVER_PID="${zmq_pid}" "${zmq_client}"
  fi
} | tee "${REPORT_PATH}"

echo "[bench] report: ${REPORT_PATH}" >&2
