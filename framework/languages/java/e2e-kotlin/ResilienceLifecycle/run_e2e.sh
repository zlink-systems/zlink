#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-kotlin-config.sh"

cd "$(dirname "${BASH_SOURCE[0]}")"

pids=()
redis_container=""
redis_container_owned=0
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
SCENARIO="${1:-all}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
export ZLINK_KOTLIN_E2E_BUILD_DIR="${ZLINK_KOTLIN_E2E_BUILD_DIR:-${HOME}/.cache/zlink/kotlin-e2e/ResilienceLifecycle}"
export ZLINK_KOTLIN_E2E_GRADLE_CACHE="${ZLINK_KOTLIN_E2E_GRADLE_CACHE:-${HOME}/.cache/zlink/kotlin-e2e/ResilienceLifecycle-gradle-cache}"
export ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX:-zlink:e2e:kotlin-resilience-lifecycle:${run_id}}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
PROCESS_STOP_ATTEMPTS=200
SCENARIO_SIGNAL_ATTEMPTS=300
POLL_INTERVAL=0.1
FLAP_SETTLE_SECONDS=2

print_logs() {
  local status="$1"
  if [[ "${status}" == "0" ]]; then
    return
  fi
  for log in "${log_dir}"/*.log; do
    [[ -f "${log}" ]] || continue
    echo "===== ${log} =====" >&2
    tail -n 200 "${log}" >&2 || true
  done
}

descendants() {
  local pid="$1"
  local child
  (pgrep -P "${pid}" 2>/dev/null || true) | while read -r child; do
    descendants "${child}"
    echo "${child}"
  done
}

cleanup() {
  local status="$?"
  set +e
  print_logs "${status}"
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    local pid="${pids[$i]}"
    for child in $(descendants "${pid}"); do
      kill "${child}" >/dev/null 2>&1 || true
    done
    kill "${pid}" >/dev/null 2>&1 || true
  done
  if [[ -n "${redis_container}" ]]; then
    docker unpause "${redis_container}" >/dev/null 2>&1 || true
    if [[ "${redis_container_owned}" == "1" ]]; then
      docker rm -fv "${redis_container}" >/dev/null 2>&1 || true
    fi
  fi
  wait >/dev/null 2>&1 || true
  exit "${status}"
}
trap cleanup EXIT

reserve_ports() {
  python3 - <<'PY'
import socket
sockets = []
ports = []
try:
    for _ in range(9):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
    print(" ".join(f"tcp://127.0.0.1:{port}" for port in ports[:4]), end=" ")
    print(" ".join(f"http://127.0.0.1:{port}" for port in ports[4:]))
finally:
    for sock in sockets:
        sock.close()
PY
}

reserve_http_endpoint() {
  python3 - <<'PY'
import socket
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    print(f"http://127.0.0.1:{sock.getsockname()[1]}")
PY
}

port_of() {
  echo "${1##*:}"
}

wait_port() {
  local name="$1"
  local endpoint="$2"
  local port
  port="$(port_of "${endpoint}")"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep "${POLL_INTERVAL}"
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}

wait_tcp() {
  local host="$1"
  local port="$2"
  local name="$3"
  if python3 - "$host" "$port" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
deadline = time.monotonic() + 30
while time.monotonic() < deadline:
    try:
        with socket.create_connection((host, port), timeout=1):
            sys.exit(0)
    except OSError:
        time.sleep(0.2)
sys.exit(1)
PY
  then
    return 0
  fi
  echo "Timed out waiting for ${name} at ${host}:${port}" >&2
  return 1
}

wait_port_down() {
  local name="$1"
  local endpoint="$2"
  local port
  port="$(port_of "${endpoint}")"
  for _ in $(seq 1 "${PROCESS_STOP_ATTEMPTS}"); do
    if ! (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep "${POLL_INTERVAL}"
  done
  echo "Timed out waiting for ${name} to stop at ${endpoint}" >&2
  return 1
}

wait_file() {
  local file="$1"
  for _ in $(seq 1 "${SCENARIO_SIGNAL_ATTEMPTS}"); do
    if [[ -f "${file}" ]]; then
      return 0
    fi
    sleep "${POLL_INTERVAL}"
  done
  echo "Timed out waiting for ${file}" >&2
  return 1
}

redis_container_owned=1
zlink_redis_start_scoped_assign redis_container redis_port \
  "zlink-redis-kotlin-e2e" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}" "127.0.0.1::6379"
redis_endpoint="127.0.0.1:${redis_port}"
export ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="${redis_endpoint}"
redis_host="${redis_endpoint%:*}"
redis_port="${redis_endpoint##*:}"
wait_tcp "${redis_host}" "${redis_port}" redis

pause_redis_container() {
  if [[ -z "${redis_container}" ]]; then
    echo "RL-C4 requires runner-owned Redis; unset external Redis endpoint variables for this scenario." >&2
    return 1
  fi
  docker pause "${redis_container}" >/dev/null
}

unpause_redis_container() {
  if [[ -z "${redis_container}" ]]; then
    echo "RL-C4 requires runner-owned Redis; unset external Redis endpoint variables for this scenario." >&2
    return 1
  fi
  docker unpause "${redis_container}" >/dev/null
  wait_tcp "${redis_host}" "${redis_port}" redis
}

should_run() {
  local target="$1"
  [[ "${SCENARIO}" == "all" || "${SCENARIO}" == "${target}" ]]
}

stop_pid() {
  local pid="$1"
  for child in $(descendants "${pid}"); do
    kill "${child}" >/dev/null 2>&1 || true
  done
  kill "${pid}" >/dev/null 2>&1 || true
  wait "${pid}" >/dev/null 2>&1 || true
}

kill_pid() {
  local pid="$1"
  for child in $(descendants "${pid}"); do
    kill -9 "${child}" >/dev/null 2>&1 || true
  done
  kill -9 "${pid}" >/dev/null 2>&1 || true
  wait "${pid}" >/dev/null 2>&1 || true
}

gradle_run() {
  ../../gradlew --project-cache-dir "${ZLINK_KOTLIN_E2E_GRADLE_CACHE}" --no-daemon --no-parallel --max-workers=1 "$@" --quiet
}

client_bin() {
  echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Client/install/resilience-lifecycle-kotlin-client/bin/resilience-lifecycle-kotlin-client"
}

provider_bin() {
  echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Server-Provider/install/resilience-lifecycle-kotlin-provider/bin/resilience-lifecycle-kotlin-provider"
}

consumer_bin() {
  echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Server-Consumer/install/resilience-lifecycle-kotlin-consumer/bin/resilience-lifecycle-kotlin-consumer"
}

start_provider() {
  local rid="$1"
  local api="$2"
  local http="$3"
  ZLINK_KOTLIN_E2E_PROVIDER_RID="${rid}" \
  ZLINK_KOTLIN_E2E_API_ENDPOINT="${api}" \
  ZLINK_KOTLIN_E2E_HTTP_ENDPOINT="${http}" \
  ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
  ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
  ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
    zlink_kotlin_e2e_run "$(provider_bin)" >"${log_dir}/${rid}.stdout.log" 2>"${log_dir}/${rid}.stderr.log" &
  pids+=("$!")
  wait_port "${rid}-api" "${api}"
  wait_port "${rid}-http" "${http}"
}

start_consumer() {
  ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
  ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
  ZLINK_KOTLIN_E2E_CONSUMER_HTTP_ENDPOINT="${CONSUMER_HTTP}" \
  ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
    zlink_kotlin_e2e_run "$(consumer_bin)" >"${log_dir}/consumer.stdout.log" 2>"${log_dir}/consumer.stderr.log" &
  pids+=("$!")
  wait_port consumer-http "${CONSUMER_HTTP}"
}

read -r API_A API_B API_A_REPLACEMENT API_B_GREEN HTTP_A HTTP_B HTTP_A_REPLACEMENT HTTP_B_GREEN CONSUMER_HTTP <<<"$(reserve_ports)"

gradle_run clean installDist

start_provider api-a "${API_A}" "${HTTP_A}"
PROVIDER_A_PID="${pids[-1]}"
CURRENT_API_A="${API_A}"
CURRENT_HTTP_A="${HTTP_A}"
start_provider api-b "${API_B}" "${HTTP_B}"
PROVIDER_B_PID="${pids[-1]}"
start_consumer
CONSUMER_PID="${pids[-1]}"

control_dir="${log_dir}/control"
mkdir -p "${control_dir}"

if should_run RL-A1 || should_run RL-C3; then
ZLINK_KOTLIN_E2E_CLIENT_MODE=restart \
ZLINK_KOTLIN_E2E_CONTROL_DIR="${control_dir}" \
ZLINK_KOTLIN_E2E_SCENARIO="${SCENARIO}" \
ZLINK_KOTLIN_E2E_CONSUMER_HTTP_ENDPOINT="${CONSUMER_HTTP}" \
ZLINK_KOTLIN_E2E_HTTP_A_ENDPOINT="${CURRENT_HTTP_A}" \
ZLINK_KOTLIN_E2E_HTTP_B_ENDPOINT="${HTTP_B}" \
ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
  zlink_kotlin_e2e_run "$(client_bin)" >"${log_dir}/client-restart.stdout.log" 2>"${log_dir}/client-restart.stderr.log" &
restart_client_pid="$!"
pids+=("${restart_client_pid}")

wait_file "${control_dir}/a1-ready"
stop_pid "${PROVIDER_A_PID}"
wait_port_down api-a "${CURRENT_API_A}"
touch "${control_dir}/a1-down"
wait_file "${control_dir}/a1-down-observed"
start_provider api-a "${API_A}" "${HTTP_A}"
PROVIDER_A_PID="${pids[-1]}"
CURRENT_API_A="${API_A}"
CURRENT_HTTP_A="${HTTP_A}"
touch "${control_dir}/a1-up"
wait "${restart_client_pid}"
fi

if should_run RL-A2; then
ZLINK_KOTLIN_E2E_CLIENT_MODE=reschedule \
ZLINK_KOTLIN_E2E_CONTROL_DIR="${control_dir}" \
ZLINK_KOTLIN_E2E_SCENARIO="${SCENARIO}" \
ZLINK_KOTLIN_E2E_CONSUMER_HTTP_ENDPOINT="${CONSUMER_HTTP}" \
ZLINK_KOTLIN_E2E_API_A_REPLACEMENT_ENDPOINT="${API_A_REPLACEMENT}" \
ZLINK_KOTLIN_E2E_HTTP_A_ENDPOINT="${CURRENT_HTTP_A}" \
ZLINK_KOTLIN_E2E_HTTP_B_ENDPOINT="${HTTP_B}" \
ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
  zlink_kotlin_e2e_run "$(client_bin)" >"${log_dir}/client-reschedule.stdout.log" 2>"${log_dir}/client-reschedule.stderr.log" &
reschedule_client_pid="$!"
pids+=("${reschedule_client_pid}")

wait_file "${control_dir}/a2-ready"
stop_pid "${PROVIDER_A_PID}"
wait_port_down api-a "${CURRENT_API_A}"
touch "${control_dir}/a2-down"
wait_file "${control_dir}/a2-down-observed"
start_provider api-a "${API_A_REPLACEMENT}" "${HTTP_A_REPLACEMENT}"
PROVIDER_A_PID="${pids[-1]}"
CURRENT_API_A="${API_A_REPLACEMENT}"
CURRENT_HTTP_A="${HTTP_A_REPLACEMENT}"
touch "${control_dir}/a2-up"
wait "${reschedule_client_pid}"
fi

if should_run RL-A4; then
ZLINK_KOTLIN_E2E_SCENARIO="RL-A4" \
ZLINK_KOTLIN_E2E_CONTROL_DIR="${control_dir}" \
ZLINK_KOTLIN_E2E_CONSUMER_HTTP_ENDPOINT="${CONSUMER_HTTP}" \
ZLINK_KOTLIN_E2E_API_B_ENDPOINT="${API_B}" \
ZLINK_KOTLIN_E2E_API_B_GREEN_ENDPOINT="${API_B_GREEN}" \
ZLINK_KOTLIN_E2E_HTTP_A_ENDPOINT="${CURRENT_HTTP_A}" \
ZLINK_KOTLIN_E2E_HTTP_B_ENDPOINT="${HTTP_B}" \
ZLINK_KOTLIN_E2E_HTTP_B_GREEN_ENDPOINT="${HTTP_B_GREEN}" \
ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
  zlink_kotlin_e2e_run "$(client_bin)" >"${log_dir}/client-a4.stdout.log" 2>"${log_dir}/client-a4.stderr.log" &
a4_client_pid="$!"
pids+=("${a4_client_pid}")

wait_file "${control_dir}/a4-drained"
stop_pid "${PROVIDER_B_PID}"
wait_port_down api-b "${API_B}"
start_provider api-b "${API_B_GREEN}" "${HTTP_B_GREEN}"
PROVIDER_B_PID="${pids[-1]}"
touch "${control_dir}/a4-green-up"
wait_file "${control_dir}/a4-green-served"
stop_pid "${PROVIDER_B_PID}"
wait_port_down api-b-green "${API_B_GREEN}"
touch "${control_dir}/a4-green-down"
wait_file "${control_dir}/a4-restore"
start_provider api-b "${API_B}" "${HTTP_B}"
PROVIDER_B_PID="${pids[-1]}"
touch "${control_dir}/a4-restored"
wait "${a4_client_pid}"
fi

if should_run RL-A5; then
ZLINK_KOTLIN_E2E_CLIENT_MODE=flapping \
ZLINK_KOTLIN_E2E_CONTROL_DIR="${control_dir}" \
ZLINK_KOTLIN_E2E_SCENARIO="${SCENARIO}" \
ZLINK_KOTLIN_E2E_CONSUMER_HTTP_ENDPOINT="${CONSUMER_HTTP}" \
ZLINK_KOTLIN_E2E_HTTP_A_ENDPOINT="${CURRENT_HTTP_A}" \
ZLINK_KOTLIN_E2E_HTTP_B_ENDPOINT="${HTTP_B}" \
ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
  zlink_kotlin_e2e_run "$(client_bin)" >"${log_dir}/client-flapping.stdout.log" 2>"${log_dir}/client-flapping.stderr.log" &
flapping_client_pid="$!"
pids+=("${flapping_client_pid}")

wait_file "${control_dir}/a5-ready"
for _ in $(seq 1 3); do
  stop_pid "${PROVIDER_A_PID}"
  wait_port_down api-a "${CURRENT_API_A}"
  sleep "${FLAP_SETTLE_SECONDS}"
  start_provider api-a "${API_A_REPLACEMENT}" "${HTTP_A_REPLACEMENT}"
  PROVIDER_A_PID="${pids[-1]}"
  CURRENT_API_A="${API_A_REPLACEMENT}"
  CURRENT_HTTP_A="${HTTP_A_REPLACEMENT}"
done
touch "${control_dir}/a5-stop"
wait "${flapping_client_pid}"
fi

if should_run RL-B2; then
ZLINK_KOTLIN_E2E_SCENARIO="RL-B2" \
ZLINK_KOTLIN_E2E_CONTROL_DIR="${control_dir}" \
ZLINK_KOTLIN_E2E_CONSUMER_HTTP_ENDPOINT="${CONSUMER_HTTP}" \
ZLINK_KOTLIN_E2E_HTTP_A_ENDPOINT="${CURRENT_HTTP_A}" \
ZLINK_KOTLIN_E2E_HTTP_B_ENDPOINT="${HTTP_B}" \
ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
  zlink_kotlin_e2e_run "$(client_bin)" >"${log_dir}/client-b2.stdout.log" 2>"${log_dir}/client-b2.stderr.log" &
b2_client_pid="$!"
pids+=("${b2_client_pid}")

wait_file "${control_dir}/b2-ready"
kill_pid "${PROVIDER_B_PID}"
wait_port_down api-b "${API_B}"
touch "${control_dir}/b2-crashed"
wait_file "${control_dir}/b2-restart"
start_provider api-b "${API_B}" "${HTTP_B}"
PROVIDER_B_PID="${pids[-1]}"
touch "${control_dir}/b2-restarted"
wait "${b2_client_pid}"
fi

if should_run RL-C2; then
ZLINK_KOTLIN_E2E_SCENARIO="RL-C2" \
ZLINK_KOTLIN_E2E_CONTROL_DIR="${control_dir}" \
ZLINK_KOTLIN_E2E_CONSUMER_HTTP_ENDPOINT="${CONSUMER_HTTP}" \
ZLINK_KOTLIN_E2E_HTTP_A_ENDPOINT="${CURRENT_HTTP_A}" \
ZLINK_KOTLIN_E2E_HTTP_B_ENDPOINT="${HTTP_B}" \
ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
  zlink_kotlin_e2e_run "$(client_bin)" >"${log_dir}/client-c2.stdout.log" 2>"${log_dir}/client-c2.stderr.log" &
c2_client_pid="$!"
pids+=("${c2_client_pid}")

wait_file "${control_dir}/c2-ready"
kill_pid "${PROVIDER_B_PID}"
wait_port_down api-b "${API_B}"
stop_pid "${CONSUMER_PID}"
wait_port_down consumer-http "${CONSUMER_HTTP}"
start_consumer
CONSUMER_PID="${pids[-1]}"
touch "${control_dir}/c2-crashed"
wait_file "${control_dir}/c2-restart"
start_provider api-b "${API_B}" "${HTTP_B}"
PROVIDER_B_PID="${pids[-1]}"
touch "${control_dir}/c2-restarted"
wait "${c2_client_pid}"
fi

if should_run RL-C4; then
ZLINK_KOTLIN_E2E_SCENARIO="RL-C4" \
ZLINK_KOTLIN_E2E_CONTROL_DIR="${control_dir}" \
ZLINK_KOTLIN_E2E_CONSUMER_HTTP_ENDPOINT="${CONSUMER_HTTP}" \
ZLINK_KOTLIN_E2E_HTTP_A_ENDPOINT="${CURRENT_HTTP_A}" \
ZLINK_KOTLIN_E2E_HTTP_B_ENDPOINT="${HTTP_B}" \
ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
  zlink_kotlin_e2e_run "$(client_bin)" >"${log_dir}/client-c4.stdout.log" 2>"${log_dir}/client-c4.stderr.log" &
c4_client_pid="$!"
pids+=("${c4_client_pid}")

wait_file "${control_dir}/c4-pause-store"
pause_redis_container
touch "${control_dir}/c4-store-paused"
wait_file "${control_dir}/c4-unpause-store"
unpause_redis_container
touch "${control_dir}/c4-store-unpaused"
wait "${c4_client_pid}"
fi

if should_run RL-B1 || should_run RL-B3 || should_run RL-B4 || should_run RL-B5 || should_run RL-B6 || should_run RL-D2 || should_run RL-D3 || should_run RL-D4; then
ZLINK_KOTLIN_E2E_SCENARIO="${SCENARIO}" \
ZLINK_KOTLIN_E2E_CONSUMER_HTTP_ENDPOINT="${CONSUMER_HTTP}" \
ZLINK_KOTLIN_E2E_HTTP_A_ENDPOINT="${CURRENT_HTTP_A}" \
ZLINK_KOTLIN_E2E_HTTP_B_ENDPOINT="${HTTP_B}" \
ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
  zlink_kotlin_e2e_run "$(client_bin)" >"${log_dir}/client-default.stdout.log" 2>"${log_dir}/client-default.stderr.log"
fi

if [[ "${SCENARIO}" == "all" ]]; then
  wait_port_down api-b "${API_B}"
  start_provider api-b "${API_B}" "${HTTP_B}"
  PROVIDER_B_PID="${pids[-1]}"
fi

if should_run RL-A3 || should_run RL-D1; then
for wave in 1 2; do
  storm_pids=()
  storm_consumer_pids=()
  for index in $(seq 1 6); do
    storm_log_dir="${log_dir}/storm-${wave}-${index}"
    mkdir -p "${storm_log_dir}"
    storm_consumer_http="$(reserve_http_endpoint)"
    ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    ZLINK_KOTLIN_E2E_CONSUMER_HTTP_ENDPOINT="${storm_consumer_http}" \
    ZLINK_KOTLIN_E2E_LOG_DIR="${storm_log_dir}" \
      zlink_kotlin_e2e_run "$(consumer_bin)" >"${storm_log_dir}/consumer.stdout.log" 2>"${storm_log_dir}/consumer.stderr.log" &
    storm_consumer_pids+=("$!")
    pids+=("$!")
    wait_port "storm-${wave}-${index}-consumer" "${storm_consumer_http}"
    ZLINK_KOTLIN_E2E_CLIENT_MODE=storm \
    ZLINK_KOTLIN_E2E_CONTROL_DIR="${control_dir}" \
    ZLINK_KOTLIN_E2E_SCENARIO="${SCENARIO}" \
    ZLINK_KOTLIN_E2E_CONSUMER_HTTP_ENDPOINT="${storm_consumer_http}" \
    ZLINK_KOTLIN_E2E_HTTP_A_ENDPOINT="${CURRENT_HTTP_A}" \
    ZLINK_KOTLIN_E2E_HTTP_B_ENDPOINT="${HTTP_B}" \
    ZLINK_KOTLIN_E2E_STORM_EXIT_DELAY_MS="$((index * 250))" \
    ZLINK_KOTLIN_E2E_LOG_DIR="${storm_log_dir}" \
      zlink_kotlin_e2e_run "$(client_bin)" >"${log_dir}/client-storm-${wave}-${index}.stdout.log" 2>"${log_dir}/client-storm-${wave}-${index}.stderr.log" &
    storm_pids+=("$!")
    pids+=("$!")
  done
  for pid in "${storm_pids[@]}"; do
    wait "${pid}"
  done
  for pid in "${storm_consumer_pids[@]}"; do
    stop_pid "${pid}"
  done
done
fi

if should_run RL-C1 || should_run RL-D5; then
ZLINK_KOTLIN_E2E_CLIENT_MODE=cleanup \
ZLINK_KOTLIN_E2E_CONTROL_DIR="${control_dir}" \
ZLINK_KOTLIN_E2E_SCENARIO="${SCENARIO}" \
ZLINK_KOTLIN_E2E_CONSUMER_HTTP_ENDPOINT="${CONSUMER_HTTP}" \
ZLINK_KOTLIN_E2E_HTTP_A_ENDPOINT="${CURRENT_HTTP_A}" \
ZLINK_KOTLIN_E2E_HTTP_B_ENDPOINT="${HTTP_B}" \
ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
  zlink_kotlin_e2e_run "$(client_bin)" >"${log_dir}/client-cleanup.stdout.log" 2>"${log_dir}/client-cleanup.stderr.log"
fi

for log in "${log_dir}"/client-*.stdout.log; do
  [[ -f "${log}" ]] && cat "${log}"
done
if [[ "${SCENARIO}" == "all" ]]; then
  grep -q "scenario RL-A1 passed" "${log_dir}/client-restart.stdout.log"
  grep -q "scenario RL-A2 passed" "${log_dir}/client-reschedule.stdout.log"
  grep -q "scenario RL-A4 passed" "${log_dir}/client-a4.stdout.log"
  grep -q "scenario RL-A3 passed" "${log_dir}"/client-storm-*.stdout.log
  grep -q "scenario RL-A5 passed" "${log_dir}/client-flapping.stdout.log"
  grep -q "scenario RL-B1 passed" "${log_dir}/client-default.stdout.log"
  grep -q "scenario RL-B2 passed" "${log_dir}/client-b2.stdout.log"
  grep -q "scenario RL-B3 passed" "${log_dir}/client-default.stdout.log"
  grep -q "scenario RL-B4 passed" "${log_dir}/client-default.stdout.log"
  grep -q "scenario RL-B5 passed" "${log_dir}/client-default.stdout.log"
  grep -q "scenario RL-B6 passed" "${log_dir}/client-default.stdout.log"
  grep -q "scenario RL-C1 passed" "${log_dir}/client-cleanup.stdout.log"
  grep -q "scenario RL-C2 passed" "${log_dir}/client-c2.stdout.log"
  grep -q "scenario RL-C3 passed" "${log_dir}/client-restart.stdout.log"
  grep -q "scenario RL-C4 passed" "${log_dir}/client-c4.stdout.log"
  grep -q "scenario RL-D1 passed" "${log_dir}"/client-storm-*.stdout.log
  grep -q "scenario RL-D2 passed" "${log_dir}/client-default.stdout.log"
  grep -q "scenario RL-D3 passed" "${log_dir}/client-default.stdout.log"
  grep -q "scenario RL-D4 passed" "${log_dir}/client-default.stdout.log"
  grep -q "scenario RL-D5 passed" "${log_dir}/client-cleanup.stdout.log"
else
  grep -Rq "scenario ${SCENARIO} passed" "${log_dir}"/client-*.stdout.log
fi
grep -Rq "message flow" "${log_dir}"/*-flow.log
