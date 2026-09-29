#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../../../.." && pwd)"
# shellcheck source=../runner_common.sh
source "${HERE}/../runner_common.sh"
bench_init c "grpc-c,zlink-c" "request-serial,request-backpressure,send-saturation" "1024,4096"

# BENCH_CORE_VERSION은 build.sh가 링크한 Core 패키지 버전이다(build.sh가 같은 기본값을 쓴다).
ZLINK_C_CORE_VERSION="${ZLINK_C_CORE_VERSION:-$(sed -n 's/^LIBZLINK_VERSION=//p' "${REPO_ROOT}/VERSION")}"
BUILD_DIR="${BUILD_DIR:-${HERE}/build}"

if [[ "${SKIP_BUILD}" != 1 ]]; then
  BUILD_DIR="${BUILD_DIR}" ZLINK_C_CORE_VERSION="${ZLINK_C_CORE_VERSION}" "${HERE}/build.sh"
fi

for binary in zlink_server zlink_client grpc_server grpc_client; do
  [[ -x "${BUILD_DIR}/bench_c_with_grpc_${binary}" ]] ||
    { echo "missing ${BUILD_DIR}/bench_c_with_grpc_${binary}; run without SKIP_BUILD=1" >&2; exit 1; }
done

check_ports_free 6071 6079
mkdir -p "${OUTPUT_DIR}"
a_pid=""
b_pid=""
trap cleanup_cell EXIT

# `setsid` forks when the caller is not already a process-group leader, so `$!` is a
# short-lived wrapper rather than the server. SERVER_PID is what the client samples server
# CPU/RSS from, so the server records its own pid into a pidfile and we read that.
start_server() {
  local pidfile="$1" log="$2" binary="$3" port="$4"
  rm -f "${pidfile}"
  setsid bash -c 'echo $$ >"$1"; exec "$2"' _ "${pidfile}" "${binary}" >"${log}" 2>&1 &
  for _ in $(seq 1 200); do
    [[ -s "${pidfile}" ]] && break
    sleep 0.05
  done
  [[ -s "${pidfile}" ]] || { echo "failed to start ${binary}" >&2; return 1; }
  b_pid="$(cat "${pidfile}")"
  for _ in $(seq 1 200); do
    ss -H -ltn "sport = :${port}" | grep -q . && return 0
    sleep 0.05
  done
  echo "server did not listen on ${port}: ${binary}" >&2
  return 1
}

# README §10.4: every cell (implementation x pattern x payload) starts a new server and a new
# client process, so no cell inherits another cell's connection, heap or scheduler state.
for run in $(seq 1 "${RUNS}"); do
  run_dir="${OUTPUT_DIR}/run${run}"
  for impl in "${implementations[@]}"; do
    case "${impl}" in
      grpc-c) server="${BUILD_DIR}/bench_c_with_grpc_grpc_server"
              client="${BUILD_DIR}/bench_c_with_grpc_grpc_client"; port=6071 ;;
      zlink-c) server="${BUILD_DIR}/bench_c_with_grpc_zlink_server"
               client="${BUILD_DIR}/bench_c_with_grpc_zlink_client"; port=6075 ;;
    esac
    for pattern in "${patterns[@]}"; do
      for payload in "${payloads[@]}"; do
        cell_dir="$(bench_cell_dir "${run}" "${impl}" "${pattern}" "${payload}")"
        mkdir -p "${cell_dir}"
        echo "[bench] run=${run} cell=${impl}-${pattern}-${payload}: start server, then client" >&2
        start_server "${cell_dir}/target.pid" "${cell_dir}/target.log" "${server}" "${port}"
        SERVER_PID="${b_pid}" BENCH_RUN_DIR="${run_dir}" BENCH_CORE_VERSION="${ZLINK_C_CORE_VERSION}" \
          PAYLOAD_SIZES="${payload}" PATTERNS="${pattern}" DURATION_SECONDS="${DURATION_SECONDS}" \
          "${client}" >"${cell_dir}/source.log" 2>&1 ||
          { echo "client failed: ${cell_dir}/source.log" >&2; exit 1; }
        cleanup_cell
        rm -f "${cell_dir}/target.pid"
        wait_for_ports_free 6071 6079
      done
    done
  done
done

echo "[bench] results=${OUTPUT_DIR}" >&2
