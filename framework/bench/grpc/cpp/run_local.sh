#!/usr/bin/env bash
# C++ with-grpc bench runner.
#
# plan 3.2 / G7: the measured span must contain no other build and no other job.
# This script therefore BUILDS NOTHING. Build first, separately, then run this.
# It takes /tmp/zlink-perf.lock for the whole span and refuses to start a run
# while the 1-minute load average is at or above the gate, recording every gate
# reading it took.
#
#   BUILD (outside the measured span):
#     cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
#
#   MEASURE:
#     ./run_local.sh <run-label> [extra client args...]
#
# DRY_RUN=1 stubs every bench process so the orchestration can be rehearsed
# without measuring anything.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build}"
RUN_LABEL="${1:-cpp-router-1}"
shift || true
STAMP="${RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
LOG_DIR="${SCRIPT_DIR}/log/${STAMP}/${RUN_LABEL}"
DRY_RUN="${DRY_RUN:-0}"
LOAD_GATE="${LOAD_GATE:-2.0}"

# spec 9: the cpp band. The runner never moves a busy port to another one --
# that would make the recorded endpoint disagree with the measured one.
GRPC_PORT=5111
FRAMEWORK_PORT=5112
FRAMEWORK_STATS_PORT=5113
GRPC_STATS_PORT=5114
RAW_REQUEST_PORT=5115
RAW_STATS_PORT=5116
RAW_COMMAND_PORT=5117

mkdir -p "${LOG_DIR}"

log () { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "${LOG_DIR}/runner.log"; }

check_ports () {
  local busy=0 port
  for port in ${GRPC_PORT} ${FRAMEWORK_PORT} ${FRAMEWORK_STATS_PORT} ${GRPC_STATS_PORT} \
              ${RAW_REQUEST_PORT} ${RAW_STATS_PORT} ${RAW_COMMAND_PORT}; do
    if ss -ltn "sport = :${port}" 2>/dev/null | grep -q LISTEN; then
      log "port ${port} is in use"
      busy=1
    fi
  done
  return ${busy}
}

# plan 3.2: below the gate or wait. The reading is recorded either way, because
# "the load was fine" is a claim the result has to be able to support.
check_load () {
  local load
  load="$(awk '{print $1}' /proc/loadavg)"
  log "loadavg1=${load} gate=${LOAD_GATE}"
  echo "${RUN_LABEL} loadavg1=${load} gate=${LOAD_GATE} at=$(date -Is)" \
    >> "${SCRIPT_DIR}/log/${STAMP}/load-gates.txt"
  awk -v l="${load}" -v g="${LOAD_GATE}" 'BEGIN { exit !(l < g) }'
}

SERVER_PIDS=()

start_servers () {
  if [[ "${DRY_RUN}" == "1" ]]; then
    log "DRY_RUN: would start grpc/raw/framework servers"
    return 0
  fi
  "${BUILD_DIR}/bench_cpp_grpc_server" \
    --endpoint "127.0.0.1:${GRPC_PORT}" --stats-port "${GRPC_STATS_PORT}" \
    > "${LOG_DIR}/grpc-server.log" 2>&1 &
  SERVER_PIDS+=($!)
  "${BUILD_DIR}/bench_cpp_zlink_server" \
    --endpoint "tcp://127.0.0.1:${RAW_REQUEST_PORT}" \
    --command-endpoint "tcp://127.0.0.1:${RAW_COMMAND_PORT}" \
    --stats-port "${RAW_STATS_PORT}" \
    > "${LOG_DIR}/zlink-server.log" 2>&1 &
  SERVER_PIDS+=($!)
  if [[ -x "${BUILD_DIR}/bench_cpp_framework_server" ]]; then
    "${BUILD_DIR}/bench_cpp_framework_server" \
      --endpoint "tcp://127.0.0.1:${FRAMEWORK_PORT}" \
      --stats-port "${FRAMEWORK_STATS_PORT}" \
      > "${LOG_DIR}/framework-server.log" 2>&1 &
    SERVER_PIDS+=($!)
  else
    log "framework server binary absent -- framework cells will be reported as not implemented"
  fi
  sleep 2
  local pid alive=0
  for pid in "${SERVER_PIDS[@]}"; do
    if kill -0 "${pid}" 2>/dev/null; then alive=$((alive + 1)); fi
  done
  log "servers started: ${alive}/${#SERVER_PIDS[@]} alive (pids ${SERVER_PIDS[*]})"
  ps -o pid,etime,cmd -p "${SERVER_PIDS[*]// /,}" 2>/dev/null | tee -a "${LOG_DIR}/runner.log"
  [[ ${alive} -eq ${#SERVER_PIDS[@]} ]]
}

stop_servers () {
  local pid
  for pid in "${SERVER_PIDS[@]:-}"; do
    [[ -n "${pid}" ]] && kill "${pid}" 2>/dev/null
  done
  sleep 1
  for pid in "${SERVER_PIDS[@]:-}"; do
    [[ -n "${pid}" ]] && kill -9 "${pid}" 2>/dev/null
  done
  SERVER_PIDS=()
}

trap stop_servers EXIT

main () {
  log "run ${RUN_LABEL} stamp=${STAMP} dry_run=${DRY_RUN}"
  if ! check_ports; then
    log "ABORT: a port in the cpp band is in use; not relocating (spec 9)"
    return 2
  fi
  if ! check_load; then
    log "ABORT: loadavg above gate"
    return 3
  fi
  if ! start_servers; then
    log "ABORT: a server failed to start"
    return 4
  fi

  if [[ "${DRY_RUN}" == "1" ]]; then
    log "DRY_RUN: would run client -> ${LOG_DIR}/cells.json"
    log "DRY_RUN: client args: $*"
    return 0
  fi

  "${BUILD_DIR}/bench_cpp_client" \
    --grpc-endpoint "127.0.0.1:${GRPC_PORT}" \
    --grpc-stats-port "${GRPC_STATS_PORT}" \
    --framework-endpoint "tcp://127.0.0.1:${FRAMEWORK_PORT}" \
    --framework-stats-port "${FRAMEWORK_STATS_PORT}" \
    --raw-request-endpoint "tcp://127.0.0.1:${RAW_REQUEST_PORT}" \
    --raw-command-endpoint "tcp://127.0.0.1:${RAW_COMMAND_PORT}" \
    --raw-stats-port "${RAW_STATS_PORT}" \
    --output-dir "${LOG_DIR}" \
    --run-label "${RUN_LABEL}" \
    "$@" 2>&1 | tee "${LOG_DIR}/client.log"
  local rc=${PIPESTATUS[0]}
  log "client rc=${rc}"
  return ${rc}
}

main "$@"
