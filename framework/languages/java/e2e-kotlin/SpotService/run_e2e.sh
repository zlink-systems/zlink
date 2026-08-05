#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-kotlin-config.sh"
SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

cd "$(dirname "${BASH_SOURCE[0]}")"

if rg -n '@EnableZLinkFramework|ClientDriverSpot|\bZLink(SpotOutbound|RouteClient|SpotManager)\b' \
    Client/src/main/kotlin --glob '*.kt'; then
  echo "SpotService Kotlin Client must not host or call the framework runtime directly" >&2
  exit 1
fi
if rg -n 'receivedCount\([^)]*\)\s*==\s*0' \
    Client/src/main/kotlin --glob '*.kt'; then
  echo "SpotService Kotlin negative push assertions must use expectNone" >&2
  exit 1
fi

pids=()
PLAY_A_PID=""
PLAY_B_PID=""
SESSION_A_PID=""
SESSION_B_PID=""
GATEWAY_PID=""
MULTI_NODE_A_PID=""
MULTI_NODE_B_PID=""
redis_container=""
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
SCENARIO="${1:-all}"
BIND_RETRY_PATTERN="ZlinkBindException|BindException|Address already in use|EADDRINUSE|errno=98"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
export ZLINK_KOTLIN_E2E_BUILD_DIR="${ZLINK_KOTLIN_E2E_BUILD_DIR:-${HOME}/.cache/zlink/kotlin-e2e/SpotService}"
export ZLINK_KOTLIN_E2E_GRADLE_CACHE="${ZLINK_KOTLIN_E2E_GRADLE_CACHE:-${HOME}/.cache/zlink/kotlin-e2e/SpotService-gradle-cache}"
export ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX:-zlink:e2e:kotlin-spot-service:${run_id}}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
PROCESS_STOP_TIMEOUT_SECONDS=5
PROCESS_STOP_ATTEMPTS=50
MODE_RETRY_SETTLE_SECONDS=1

if [[ "${SCENARIO}" != "all" && "${ZLINK_SPOT_SERVICE_RETRY_CHILD:-0}" != "1" && "${ZLINK_SPOT_SERVICE_ALL_CHILD:-0}" != "1" ]]; then
  output="$(mktemp)"
  scenario_passed=0
  for attempt in 1 2 3; do
    : >"${output}"
    set +e
    timeout 900s env ZLINK_SPOT_SERVICE_RETRY_CHILD=1 "${SCRIPT_PATH}" "${SCENARIO}" 2>&1 | tee "${output}"
    status="${PIPESTATUS[0]}"
    set -e
    if [[ "${status}" == "0" ]]; then
      scenario_passed=1
      break
    fi
    if ! grep -Eq "${BIND_RETRY_PATTERN}" "${output}"; then
      break
    fi
    if [[ "${attempt}" == "3" ]]; then
      break
    fi
    echo "spot-service transient bind failure; retrying scenario=${SCENARIO} (${attempt}/3)" >&2
    sleep 1
  done
  rm -f "${output}"
  if [[ "${scenario_passed}" == "1" ]]; then
    exit 0
  fi
  echo "scenario=${SCENARIO} failed" >&2
  exit 1
fi

if [[ "${SCENARIO}" == "all" && "${ZLINK_SPOT_SERVICE_ALL_CHILD:-0}" != "1" ]]; then
  for child_group in default-batch SM-F6 SM-G2 SM-G3 SM-G4 SM-G1; do
    echo "child scenario=${child_group}"
    output="$(mktemp)"
    child_passed=0
    for attempt in 1 2 3; do
      : >"${output}"
      set +e
      timeout 900s env ZLINK_SPOT_SERVICE_ALL_CHILD=1 "${SCRIPT_PATH}" "${child_group}" 2>&1 | tee "${output}"
      status="${PIPESTATUS[0]}"
      set -e
      if [[ "${status}" == "0" ]]; then
        child_passed=1
        break
      fi
      if ! grep -Eq "${BIND_RETRY_PATTERN}" "${output}"; then
        break
      fi
      if [[ "${attempt}" == "3" ]]; then
        break
      fi
      echo "spot-service transient bind failure; retrying child scenario=${child_group} (${attempt}/3)" >&2
      sleep 1
    done
    rm -f "${output}"
    if [[ "${child_passed}" != "1" ]]; then
      echo "child scenario=${child_group} failed" >&2
      exit 1
    fi
  done
  echo "spot-service kotlin e2e result=passed"
  exit 0
fi

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
    for _ in $(seq 1 "${PROCESS_STOP_ATTEMPTS}"); do
      if ! kill -0 "${pid}" >/dev/null 2>&1; then
        break
      fi
      sleep "${LOCAL_READINESS_POLL_SECONDS}"
    done
    if kill -0 "${pid}" >/dev/null 2>&1; then
      for child in $(descendants "${pid}"); do
        kill -9 "${child}" >/dev/null 2>&1 || true
      done
      kill -9 "${pid}" >/dev/null 2>&1 || true
    fi
  done
  if [[ -n "${redis_container}" ]]; then
    docker rm -fv "${redis_container}" >/dev/null 2>&1 || true
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
    for _ in range(16):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
    print(" ".join(f"tcp://127.0.0.1:{port}" for port in ports[:14]), end=" ")
    print(" ".join(f"http://127.0.0.1:{port}" for port in ports[14:]))
finally:
    for sock in sockets:
        sock.close()
PY
}

reserve_client_endpoints() {
  python3 - <<'PY'
import socket
sockets = []
endpoints = []
try:
    for _ in range(2):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        endpoints.append(f"tcp://127.0.0.1:{sock.getsockname()[1]}")
    print(" ".join(endpoints))
finally:
    for sock in sockets:
        sock.close()
PY
}

reserve_session_endpoints() {
  python3 - <<'PY'
import socket
sockets = []
ports = []
try:
    for _ in range(3):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
    print(f"tcp://127.0.0.1:{ports[0]} tcp://127.0.0.1:{ports[1]} http://127.0.0.1:{ports[2]}")
finally:
    for sock in sockets:
        sock.close()
PY
}

reserve_gateway_endpoints() {
  python3 - <<'PY'
import socket
sockets = []
ports = []
try:
    for _ in range(2):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
    print(f"tcp://127.0.0.1:{ports[0]} http://127.0.0.1:{ports[1]}")
finally:
    for sock in sockets:
        sock.close()
PY
}

reserve_multinode_endpoints() {
  python3 - <<'PY'
import socket
sockets = []
ports = []
try:
    for _ in range(6):
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
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out after ${LOCAL_READINESS_TIMEOUT_SECONDS}s waiting for ${name} at ${endpoint}" >&2
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

generate_tls_cert() {
  local cert="$1"
  local key="$2"
  if ! command -v openssl >/dev/null 2>&1; then
    echo "openssl is required for SM-D14 TLS certificate generation" >&2
    return 1
  fi
  openssl req -x509 -newkey rsa:2048 -nodes -days 7 \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
    -keyout "${key}" \
    -out "${cert}" >/dev/null 2>&1
}

zlink_redis_start_scoped_assign redis_container redis_port \
  "zlink-redis-kotlin-e2e" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}" "127.0.0.1::6379"
redis_endpoint="127.0.0.1:${redis_port}"
export ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="${redis_endpoint}"
redis_host="${redis_endpoint%:*}"
redis_port="${redis_endpoint##*:}"
wait_tcp "${redis_host}" "${redis_port}" redis

gradle_run() {
  ../../gradlew --project-cache-dir "${ZLINK_KOTLIN_E2E_GRADLE_CACHE}" --no-daemon --no-parallel --max-workers=1 "$@" --quiet
}

role_bin() {
  case "$1" in
    client)
      echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Client/install/spot-service-kotlin-client/bin/spot-service-kotlin-client"
      ;;
    play)
      echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Server-Play/install/spot-service-kotlin-play/bin/spot-service-kotlin-play"
      ;;
    gateway)
      echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Server-Gateway/install/spot-service-kotlin-gateway/bin/spot-service-kotlin-gateway"
      ;;
    multi-node)
      echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Server-MultiNode/install/spot-service-kotlin-multi-node/bin/spot-service-kotlin-multi-node"
      ;;
    session)
      echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Server-Session/install/spot-service-kotlin-session/bin/spot-service-kotlin-session"
      ;;
    *)
      echo "unknown role $1" >&2
      return 1
      ;;
  esac
}

start_play() {
  local rid="$1"
  local route="$2"
  local spot="$3"
  local ingress="$4"
  local http="$5"
  local spot_pub="$6"
  local stream="$7"
  local tls_stream="$8"
  ZLINK_KOTLIN_E2E_NODE_RID="${rid}" \
  ZLINK_KOTLIN_E2E_ROUTE_ENDPOINT="${route}" \
  ZLINK_KOTLIN_E2E_INGRESS_ENDPOINT="${ingress}" \
  ZLINK_KOTLIN_E2E_INGRESS_A_ENDPOINT="${INGRESS_A}" \
  ZLINK_KOTLIN_E2E_INGRESS_B_ENDPOINT="${INGRESS_B}" \
  ZLINK_KOTLIN_E2E_ROUTE_A_ENDPOINT="${ROUTE_A}" \
  ZLINK_KOTLIN_E2E_ROUTE_B_ENDPOINT="${ROUTE_B}" \
  ZLINK_KOTLIN_E2E_SPOT_ENDPOINT="${spot}" \
  ZLINK_KOTLIN_E2E_SPOT_PUB_ENDPOINT="${spot_pub}" \
  ZLINK_KOTLIN_E2E_STREAM_ENDPOINT="${stream}" \
  ZLINK_KOTLIN_E2E_TLS_STREAM_ENDPOINT="${tls_stream}" \
  ZLINK_KOTLIN_E2E_TLS_CERTIFICATE_PATH="${TLS_CERTIFICATE_PATH:-}" \
  ZLINK_KOTLIN_E2E_TLS_KEY_PATH="${TLS_KEY_PATH:-}" \
  ZLINK_KOTLIN_E2E_SPOT_A_ENDPOINT="${SPOT_A}" \
  ZLINK_KOTLIN_E2E_SPOT_B_ENDPOINT="${SPOT_B}" \
  ZLINK_KOTLIN_E2E_HTTP_ENDPOINT="${http}" \
  ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
  ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
  ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
    zlink_kotlin_e2e_run "$(role_bin play)" >"${log_dir}/${rid}.stdout.log" 2>"${log_dir}/${rid}.stderr.log" &
  pids+=("$!")
  wait_port "${rid}-route" "${route}"
  if [[ -n "${stream}" ]]; then
    wait_port "${rid}-stream" "${stream}"
  fi
  if [[ -n "${tls_stream}" ]]; then
    wait_port "${rid}-tls-stream" "${tls_stream}"
  fi
  wait_port "${rid}-http" "${http}"
}

start_session() {
  read -r SPOT_SESSION_A STREAM_SESSION_A HTTP_SESSION_A <<<"$(reserve_session_endpoints)"
  read -r SPOT_SESSION_B STREAM_SESSION_B HTTP_SESSION_B <<<"$(reserve_session_endpoints)"
  ZLINK_KOTLIN_E2E_NODE_RID="session-a" \
  ZLINK_KOTLIN_E2E_ROUTE_A_ENDPOINT="${ROUTE_A}" \
  ZLINK_KOTLIN_E2E_ROUTE_B_ENDPOINT="${ROUTE_B}" \
  ZLINK_KOTLIN_E2E_SPOT_ENDPOINT="${SPOT_SESSION_A}" \
  ZLINK_KOTLIN_E2E_STREAM_ENDPOINT="${STREAM_SESSION_A}" \
  ZLINK_KOTLIN_E2E_HTTP_ENDPOINT="${HTTP_SESSION_A}" \
  ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
  ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
  ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
    zlink_kotlin_e2e_run "$(role_bin session)" >"${log_dir}/session-a.stdout.log" 2>"${log_dir}/session-a.stderr.log" &
  SESSION_A_PID="$!"
  pids+=("$!")
  ZLINK_KOTLIN_E2E_NODE_RID="session-b" \
  ZLINK_KOTLIN_E2E_ROUTE_A_ENDPOINT="${ROUTE_A}" \
  ZLINK_KOTLIN_E2E_ROUTE_B_ENDPOINT="${ROUTE_B}" \
  ZLINK_KOTLIN_E2E_SPOT_ENDPOINT="${SPOT_SESSION_B}" \
  ZLINK_KOTLIN_E2E_STREAM_ENDPOINT="${STREAM_SESSION_B}" \
  ZLINK_KOTLIN_E2E_HTTP_ENDPOINT="${HTTP_SESSION_B}" \
  ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
  ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
  ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
    zlink_kotlin_e2e_run "$(role_bin session)" >"${log_dir}/session-b.stdout.log" 2>"${log_dir}/session-b.stderr.log" &
  SESSION_B_PID="$!"
  pids+=("$!")
  wait_port session-a-spot "${SPOT_SESSION_A}"
  wait_port session-a-stream "${STREAM_SESSION_A}"
  wait_port session-a-http "${HTTP_SESSION_A}"
  wait_port session-b-spot "${SPOT_SESSION_B}"
  wait_port session-b-stream "${STREAM_SESSION_B}"
  wait_port session-b-http "${HTTP_SESSION_B}"
}

stop_session() {
  for pid in "${SESSION_B_PID}" "${SESSION_A_PID}"; do
    if [[ -n "${pid}" ]]; then
      for child in $(descendants "${pid}"); do
        kill "${child}" >/dev/null 2>&1 || true
      done
      kill "${pid}" >/dev/null 2>&1 || true
      for _ in $(seq 1 "${PROCESS_STOP_ATTEMPTS}"); do
        if ! kill -0 "${pid}" >/dev/null 2>&1; then
          break
        fi
        sleep "${LOCAL_READINESS_POLL_SECONDS}"
      done
      if kill -0 "${pid}" >/dev/null 2>&1; then
        kill -9 "${pid}" >/dev/null 2>&1 || true
      fi
      wait "${pid}" >/dev/null 2>&1 || true
    fi
  done
  SESSION_A_PID=""
  SESSION_B_PID=""
}

start_gateway() {
  read -r GATEWAY_SPOT_PUB GATEWAY_HTTP <<<"$(reserve_gateway_endpoints)"
  ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
  ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    zlink_kotlin_e2e_run "$(role_bin gateway)" \
    --rid gateway \
    --http-url "${GATEWAY_HTTP}" \
    --log-dir "${log_dir}" \
    --evidence-file "${log_dir}/gateway.evidence.log" \
    --spot-pub-endpoint "${GATEWAY_SPOT_PUB}" \
      >"${log_dir}/gateway.stdout.log" 2>"${log_dir}/gateway.stderr.log" &
  GATEWAY_PID="$!"
  pids+=("$!")
  wait_port gateway-spot-pub "${GATEWAY_SPOT_PUB}"
  wait_port gateway-http "${GATEWAY_HTTP}"
}

stop_gateway() {
  if [[ -z "${GATEWAY_PID}" ]]; then
    return
  fi
  for child in $(descendants "${GATEWAY_PID}"); do
    kill "${child}" >/dev/null 2>&1 || true
  done
  kill "${GATEWAY_PID}" >/dev/null 2>&1 || true
  wait "${GATEWAY_PID}" >/dev/null 2>&1 || true
  GATEWAY_PID=""
}

start_multi_nodes() {
  local spot_only="${1:-false}"
  read -r MULTI_ROUTE_A MULTI_ROUTE_B MULTI_SPOT_A MULTI_SPOT_B MULTI_HTTP_A MULTI_HTTP_B <<<"$(reserve_multinode_endpoints)"
  local node_a_args=()
  local node_b_args=()
  if [[ "${spot_only}" == "true" ]]; then
    node_a_args+=(--spot-only true)
    node_b_args+=(--spot-only true)
  fi
  ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
  ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    zlink_kotlin_e2e_run "$(role_bin multi-node)" \
    --rid multi-node-a \
    --http-url "${MULTI_HTTP_A}" \
    --log-dir "${log_dir}" \
    --evidence-file "${log_dir}/multi-node-a.evidence.log" \
    --multi-route-a-endpoint "${MULTI_ROUTE_A}" \
    --multi-spot-router-a-endpoint "${MULTI_SPOT_A}" \
    "${node_a_args[@]}" \
      >"${log_dir}/multi-node-a.stdout.log" 2>"${log_dir}/multi-node-a.stderr.log" &
  MULTI_NODE_A_PID="$!"
  pids+=("$!")
  if [[ "${spot_only}" != "true" ]]; then
    wait_port multi-node-a-route "${MULTI_ROUTE_A}"
  else
    wait_port multi-node-a-spot "${MULTI_SPOT_A}"
  fi
  wait_port multi-node-a-http "${MULTI_HTTP_A}"

  ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
  ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    zlink_kotlin_e2e_run "$(role_bin multi-node)" \
    --rid multi-node-b \
    --http-url "${MULTI_HTTP_B}" \
    --log-dir "${log_dir}" \
    --evidence-file "${log_dir}/multi-node-b.evidence.log" \
    --multi-route-b-endpoint "${MULTI_ROUTE_B}" \
    --multi-spot-router-b-endpoint "${MULTI_SPOT_B}" \
    "${node_b_args[@]}" \
      >"${log_dir}/multi-node-b.stdout.log" 2>"${log_dir}/multi-node-b.stderr.log" &
  MULTI_NODE_B_PID="$!"
  pids+=("$!")
  if [[ "${spot_only}" != "true" ]]; then
    wait_port multi-node-b-route "${MULTI_ROUTE_B}"
  else
    wait_port multi-node-b-spot "${MULTI_SPOT_B}"
  fi
  wait_port multi-node-b-http "${MULTI_HTTP_B}"
}

stop_multi_nodes() {
  for pid in "${MULTI_NODE_B_PID}" "${MULTI_NODE_A_PID}"; do
    if [[ -n "${pid}" ]]; then
      for child in $(descendants "${pid}"); do
        kill "${child}" >/dev/null 2>&1 || true
      done
      kill "${pid}" >/dev/null 2>&1 || true
      for _ in $(seq 1 "${PROCESS_STOP_ATTEMPTS}"); do
        if ! kill -0 "${pid}" >/dev/null 2>&1; then
          break
        fi
        sleep "${LOCAL_READINESS_POLL_SECONDS}"
      done
      if kill -0 "${pid}" >/dev/null 2>&1; then
        kill -9 "${pid}" >/dev/null 2>&1 || true
      fi
      wait "${pid}" >/dev/null 2>&1 || true
    fi
  done
  MULTI_NODE_A_PID=""
  MULTI_NODE_B_PID=""
}

fetch_evidence() {
  local name="$1"
  local endpoint="$2"
  python3 - "${endpoint}/evidence" >"${log_dir}/${name}-evidence.json" <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=5) as response:
    sys.stdout.write(response.read().decode("utf-8"))
PY
}

wait_file() {
  local name="$1"
  local path="$2"
  local deadline=$((SECONDS + 60))
  while (( SECONDS < deadline )); do
    if [[ -f "${path}" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${name}: ${path}" >&2
  return 1
}

crash_play_a() {
  local pid="${PLAY_A_PID}"
  if [[ -z "${pid}" ]]; then
    echo "play-a pid is not recorded" >&2
    return 1
  fi
  for child in $(descendants "${pid}"); do
    kill -9 "${child}" >/dev/null 2>&1 || true
  done
  kill -9 "${pid}" >/dev/null 2>&1 || true
  wait "${pid}" >/dev/null 2>&1 || true
  PLAY_A_PID=""
}

run_sm_g1() {
  local ready_file="${log_dir}/sm-g1-ready"
  local crashed_file="${log_dir}/sm-g1-crashed"
  local failed_file="${log_dir}/sm-g1-failed"
  local restarted_file="${log_dir}/sm-g1-restarted"
  local client_pid

  ZLINK_KOTLIN_E2E_CLIENT_MODE="play-crash-recovery" \
    ZLINK_KOTLIN_E2E_STREAM_A_ENDPOINT="${STREAM_A}" \
    ZLINK_KOTLIN_E2E_HTTP_A_ENDPOINT="${HTTP_A}" \
    ZLINK_KOTLIN_E2E_HTTP_B_ENDPOINT="${HTTP_B}" \
    ZLINK_KOTLIN_E2E_SM_G1_READY_FILE="${ready_file}" \
    ZLINK_KOTLIN_E2E_SM_G1_CRASHED_FILE="${crashed_file}" \
    ZLINK_KOTLIN_E2E_SM_G1_FAILED_FILE="${failed_file}" \
    ZLINK_KOTLIN_E2E_SM_G1_RESTARTED_FILE="${restarted_file}" \
    ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
      zlink_kotlin_e2e_run_timeout 180s "$(role_bin client)" >"${log_dir}/client-play-crash-recovery.stdout.log" 2>"${log_dir}/client-play-crash-recovery.stderr.log" &
  client_pid="$!"
  pids+=("${client_pid}")

  wait_file "SM-G1 ready signal" "${ready_file}"
  crash_play_a
  touch "${crashed_file}"
  wait_file "SM-G1 bounded failure signal" "${failed_file}"
  sleep "${ZLINK_KOTLIN_E2E_SM_G1_LEASE_WAIT_SECONDS:-20}"
  start_play play-a "${ROUTE_A}" "${SPOT_A}" "${INGRESS_A}" "${HTTP_A}" "${SPOT_PUB_A}" "${STREAM_A}" "${TLS_STREAM_A}"
  PLAY_A_PID="${pids[$((${#pids[@]} - 1))]}"
  sleep 2
  touch "${restarted_file}"

  wait "${client_pid}"
  grep -q "spot-service kotlin e2e mode=play-crash-recovery result=passed" "${log_dir}/client-play-crash-recovery.stdout.log"
  cat "${log_dir}/client-play-crash-recovery.stdout.log" >>"${log_dir}/client.stdout.log"
  cat "${log_dir}/client-play-crash-recovery.stderr.log" >>"${log_dir}/client.stderr.log"
}

close_spot() {
  local endpoint="$1"
  local rid="$2"
  python3 - "${endpoint}/admin/close?rid=${rid}" <<'PY'
import sys
import urllib.request
request = urllib.request.Request(sys.argv[1], method="POST")
with urllib.request.urlopen(request, timeout=5) as response:
    body = response.read().decode("utf-8")
    if '"closed":true' not in body:
        raise SystemExit("spot close did not report closed=true: " + body)
PY
}

create_timer_spot() {
  local endpoint="$1"
  local rid="$2"
  python3 - "${endpoint}/admin/create-timer?rid=${rid}" <<'PY'
import sys
import urllib.request
request = urllib.request.Request(sys.argv[1], method="POST")
with urllib.request.urlopen(request, timeout=5) as response:
    body = response.read().decode("utf-8")
    if '"created":true' not in body:
        raise SystemExit("timer spot create did not report created=true: " + body)
PY
}

assert_type_mismatch() {
  local endpoint="$1"
  local rid="$2"
  python3 - "${endpoint}/admin/type-mismatch?rid=${rid}" <<'PY'
import sys
import urllib.request
request = urllib.request.Request(sys.argv[1], method="POST")
with urllib.request.urlopen(request, timeout=5) as response:
    body = response.read().decode("utf-8")
    if '"mismatch":true' not in body:
        raise SystemExit("spot type mismatch did not report mismatch=true: " + body)
PY
}

set_placement_weight() {
  local endpoint="$1"
  local weight="$2"
  python3 - "${endpoint}/placement-weight" "${weight}" <<'PY'
import json
import sys
import urllib.request

request = urllib.request.Request(
    sys.argv[1],
    data=json.dumps({"weight": int(sys.argv[2])}).encode(),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request, timeout=5) as response:
    body = json.loads(response.read().decode())
    if body.get("weight") != int(sys.argv[2]):
        raise SystemExit(f"placement weight was not applied: {body}")
PY
}

prepare_default_spot_fixtures() {
  local room_a_response
  local room_b_response
  set_placement_weight "${HTTP_A}" 100
  set_placement_weight "${HTTP_B}" 0
  room_a_response="$(python3 - "${HTTP_A}/spot/create" room-a <<'PY'
import json
import sys
import urllib.request

request = urllib.request.Request(
    sys.argv[1],
    data=json.dumps({"spotRid": sys.argv[2]}).encode(),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request, timeout=5) as response:
    print(response.read().decode(), end="")
PY
)"
  python3 - "${room_a_response}" room-a play-a <<'PY'
import json
import sys

body = json.loads(sys.argv[1])
if body.get("spotRid") != sys.argv[2] or body.get("nodeRid") != sys.argv[3]:
    raise SystemExit(f"room-a fixture owner mismatch: {body}")
PY

  set_placement_weight "${HTTP_A}" 0
  set_placement_weight "${HTTP_B}" 100
  room_b_response="$(python3 - "${HTTP_B}/spot/create" room-b <<'PY'
import json
import sys
import urllib.request

request = urllib.request.Request(
    sys.argv[1],
    data=json.dumps({"spotRid": sys.argv[2]}).encode(),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request, timeout=5) as response:
    print(response.read().decode(), end="")
PY
)"
  python3 - "${room_b_response}" room-b play-b <<'PY'
import json
import sys

body = json.loads(sys.argv[1])
if body.get("spotRid") != sys.argv[2] or body.get("nodeRid") != sys.argv[3]:
    raise SystemExit(f"room-b fixture owner mismatch: {body}")
PY

  set_placement_weight "${HTTP_A}" 100
  set_placement_weight "${HTTP_B}" 100
}

read -r ROUTE_A ROUTE_B ROUTE_CLIENT SPOT_A SPOT_B SPOT_CLIENT INGRESS_A INGRESS_B SPOT_PUB_A SPOT_PUB_B _ STREAM_A STREAM_B TLS_STREAM_A_RAW HTTP_A HTTP_B <<<"$(reserve_ports)"
TLS_STREAM_A=""
if [[ "${SCENARIO}" == "all" || "${SCENARIO}" == "default-batch" || "${SCENARIO}" == "SM-D14" || "${SCENARIO}" == "sm-d14" || "${ZLINK_KOTLIN_E2E_MODES:-}" == *stream-tls* ]]; then
  TLS_STREAM_A="${TLS_STREAM_A_RAW/tcp:/tls:}"
  TLS_CERTIFICATE_PATH="${log_dir}/sm-d14-cert.pem"
  TLS_KEY_PATH="${log_dir}/sm-d14-key.pem"
  generate_tls_cert "${TLS_CERTIFICATE_PATH}" "${TLS_KEY_PATH}"
fi

gradle_run clean \
  :Client:installDist \
  :Server:Play:installDist \
  :Server:Gateway:installDist \
  :Server:MultiNode:installDist \
  :Server:Session:installDist

start_play play-a "${ROUTE_A}" "${SPOT_A}" "${INGRESS_A}" "${HTTP_A}" "${SPOT_PUB_A}" "${STREAM_A}" "${TLS_STREAM_A}"
PLAY_A_PID="${pids[$((${#pids[@]} - 1))]}"
start_play play-b "${ROUTE_B}" "${SPOT_B}" "${INGRESS_B}" "${HTTP_B}" "${SPOT_PUB_B}" "${STREAM_B}" ""
PLAY_B_PID="${pids[$((${#pids[@]} - 1))]}"
if [[ "${SCENARIO}" != "SM-F6" && "${SCENARIO}" != "sm-f6" ]]; then
  prepare_default_spot_fixtures
fi

run_client_mode() {
  local mode="$1"
  local route_client
  local spot_client
  local stream_a
  local stream_b
  local attempt
  local status
  for attempt in $(seq 1 5); do
    read -r route_client spot_client <<<"$(reserve_client_endpoints)"
    stream_a="${STREAM_A}"
    stream_b="${STREAM_B}"
    if [[ "${mode}" == "actor-session" || "${mode}" == "session-transfer" ]]; then
      stream_a="${STREAM_SESSION_A}"
      stream_b="${STREAM_SESSION_B}"
    fi
    set +e
    ZLINK_KOTLIN_E2E_CLIENT_MODE="${mode}" \
      ZLINK_KOTLIN_E2E_CLIENT_RID="client-${mode}" \
      ZLINK_KOTLIN_E2E_ROUTE_ENDPOINT="${route_client}" \
      ZLINK_KOTLIN_E2E_ROUTE_A_ENDPOINT="${ROUTE_A}" \
      ZLINK_KOTLIN_E2E_ROUTE_B_ENDPOINT="${ROUTE_B}" \
      ZLINK_KOTLIN_E2E_SPOT_ENDPOINT="${spot_client}" \
      ZLINK_KOTLIN_E2E_SPOT_A_ENDPOINT="${SPOT_A}" \
      ZLINK_KOTLIN_E2E_SPOT_B_ENDPOINT="${SPOT_B}" \
      ZLINK_KOTLIN_E2E_INGRESS_A_ENDPOINT="${INGRESS_A}" \
      ZLINK_KOTLIN_E2E_STREAM_A_ENDPOINT="${stream_a}" \
      ZLINK_KOTLIN_E2E_STREAM_B_ENDPOINT="${stream_b}" \
      ZLINK_KOTLIN_E2E_TLS_STREAM_A_ENDPOINT="${TLS_STREAM_A}" \
      ZLINK_KOTLIN_E2E_HTTP_A_ENDPOINT="${HTTP_A}" \
      ZLINK_KOTLIN_E2E_HTTP_B_ENDPOINT="${HTTP_B}" \
      ZLINK_KOTLIN_E2E_HTTP_SESSION_ENDPOINT="${HTTP_SESSION_A:-}" \
      ZLINK_KOTLIN_E2E_HTTP_SESSION_B_ENDPOINT="${HTTP_SESSION_B:-}" \
      ZLINK_KOTLIN_E2E_GATEWAY_HTTP_ENDPOINT="${GATEWAY_HTTP:-}" \
      ZLINK_KOTLIN_E2E_MULTI_HTTP_A_ENDPOINT="${MULTI_HTTP_A:-}" \
      ZLINK_KOTLIN_E2E_MULTI_HTTP_B_ENDPOINT="${MULTI_HTTP_B:-}" \
      ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
        zlink_kotlin_e2e_run_timeout 75s "$(role_bin client)" >"${log_dir}/client-${mode}.stdout.log" 2>"${log_dir}/client-${mode}.stderr.log"
    status="$?"
    set -e
    if [[ "${status}" == "0" ]] && grep -q "spot-service kotlin e2e mode=${mode} result=passed" "${log_dir}/client-${mode}.stdout.log"; then
      cat "${log_dir}/client-${mode}.stdout.log" >>"${log_dir}/client.stdout.log"
      cat "${log_dir}/client-${mode}.stderr.log" >>"${log_dir}/client.stderr.log"
      return 0
    fi
    sleep "${MODE_RETRY_SETTLE_SECONDS}"
  done
  return 1
}

scenario_modes() {
  case "$1" in
    all)
      echo "state1 state2 send normal worker missing timeout owner stage-wrapper spot-outbound spot-to-spot route-mesh route-lifecycle spot-only-mesh actor-session session-transfer stream-tls remote-actor-session owner-remap join-leave-race bound-push-load idle-timer timer-overrun"
      ;;
    default-batch)
      echo "state1 state2 send normal worker missing timeout owner stage-wrapper spot-outbound spot-to-spot route-mesh route-lifecycle actor-session session-transfer stream-tls remote-actor-session idle-timer timer-overrun"
      ;;
    SM-A1) echo "state1" ;;
    SM-A2) echo "state2" ;;
    SM-A3|SM-A4) echo "owner" ;;
    SM-A5) echo "stage-wrapper" ;;
    SM-A6) echo "lifecycle-close" ;;
    SM-A7) echo "type-mismatch" ;;
    SM-A8) echo "worker" ;;
    SM-B1|SM-B3|SM-B5|SM-B6|SM-B7|SM-B8|SM-D1|SM-D3|SM-D4|SM-D5|SM-D6|SM-D7|SM-D8|SM-D9|SM-D10|SM-D11|SM-D13) echo "actor-session" ;;
    SM-D12) echo "session-transfer" ;;
    SM-D14) echo "stream-tls" ;;
    SM-B2|SM-B4|SM-D2) echo "remote-actor-session" ;;
    SM-G2) echo "owner-remap" ;;
    SM-G3) echo "join-leave-race" ;;
    SM-G4) echo "bound-push-load" ;;
    SM-C1) echo "normal" ;;
    SM-C2) echo "spot-outbound" ;;
    SM-C3) echo "spot-to-spot" ;;
    SM-C4) echo "gateway-publish" ;;
    SM-E1) echo "missing" ;;
    SM-E2) echo "timer-basic" ;;
    SM-E3) echo "idle-timer" ;;
    SM-E4) echo "timer-overrun" ;;
    SM-F1|SM-F2|SM-F3|SM-F4) echo "route-mesh" ;;
    SM-F5) echo "route-lifecycle" ;;
    SM-F6|sm-f6) echo "spot-only-mesh" ;;
    SM-Q9) echo "multi-node" ;;
    *)
      echo "SpotService Kotlin scenario $1 is not mapped to an implemented client mode" >&2
      return 1
      ;;
  esac
}

: >"${log_dir}/client.stdout.log"
: >"${log_dir}/client.stderr.log"
if [[ "${SCENARIO}" == "SM-G1" || "${SCENARIO}" == "sm-g1" ]]; then
  run_sm_g1
  cat "${log_dir}/client.stdout.log"
  fetch_evidence play-a "${HTTP_A}"
  fetch_evidence play-b "${HTTP_B}"
  echo "spot-service kotlin e2e focused modes=play-crash-recovery result=passed"
  exit 0
fi
client_modes="${ZLINK_KOTLIN_E2E_MODES:-$(scenario_modes "${SCENARIO}")}"
for mode in ${client_modes}; do
  if [[ "${mode}" == "idle-timer" ]]; then
    create_timer_spot "${HTTP_A}" idle-close
    create_timer_spot "${HTTP_A}" idle-active
  fi
  if [[ "${mode}" == "timer-overrun" ]]; then
    create_timer_spot "${HTTP_A}" timer-overrun-skip
    create_timer_spot "${HTTP_A}" timer-overrun-catchup
    create_timer_spot "${HTTP_A}" timer-overrun-delay
  fi
  if [[ "${mode}" == "actor-session" || "${mode}" == "session-transfer" ]]; then
    if [[ -z "${SESSION_A_PID}" ]]; then
      start_session
      sleep "${MODE_RETRY_SETTLE_SECONDS}"
    fi
  fi
  if [[ "${mode}" == "gateway-publish" ]]; then
    start_gateway
    sleep "${MODE_RETRY_SETTLE_SECONDS}"
  fi
  if [[ "${mode}" == "spot-only-mesh" || "${mode}" == "multi-node" ]]; then
    start_multi_nodes "$([[ "${mode}" == "spot-only-mesh" ]] && echo true || echo false)"
    sleep "${MODE_RETRY_SETTLE_SECONDS}"
  fi
  run_client_mode "${mode}"
  if [[ "${mode}" == "spot-only-mesh" || "${mode}" == "multi-node" ]]; then
    fetch_evidence multi-node-a "${MULTI_HTTP_A}"
    fetch_evidence multi-node-b "${MULTI_HTTP_B}"
    stop_multi_nodes
  fi
  if [[ "${mode}" == "gateway-publish" ]]; then
    fetch_evidence gateway "${GATEWAY_HTTP}"
    stop_gateway
  fi
  if [[ "${mode}" == "session-transfer" ]]; then
    fetch_evidence session-a "${HTTP_SESSION_A}"
    fetch_evidence session-b "${HTTP_SESSION_B}"
    stop_session
  fi
  if [[ "${mode}" == "idle-timer" ]]; then
    close_spot "${HTTP_A}" idle-active
  fi
done
if [[ -n "${ZLINK_KOTLIN_E2E_MODES:-}" || ( "${SCENARIO}" != "all" && "${SCENARIO}" != "default-batch" ) ]]; then
  cat "${log_dir}/client.stdout.log"
  fetch_evidence play-a "${HTTP_A}"
  fetch_evidence play-b "${HTTP_B}"
  echo "spot-service kotlin e2e focused modes=${client_modes} result=passed"
  exit 0
fi
start_gateway
sleep "${MODE_RETRY_SETTLE_SECONDS}"
run_client_mode gateway-publish
fetch_evidence gateway "${GATEWAY_HTTP}"
stop_gateway
run_client_mode type-mismatch
run_client_mode timer-basic
run_client_mode lifecycle-close
start_multi_nodes
sleep "${MODE_RETRY_SETTLE_SECONDS}"
run_client_mode multi-node
fetch_evidence multi-node-a "${MULTI_HTTP_A}"
fetch_evidence multi-node-b "${MULTI_HTTP_B}"
stop_multi_nodes

cat "${log_dir}/client.stdout.log"
fetch_evidence play-a "${HTTP_A}"
fetch_evidence play-b "${HTTP_B}"
grep -Rq "message flow" "${log_dir}"/*-flow.log
grep -q "packet=RoutePingReq" "${log_dir}/play-a-flow.log"
grep -q '"marker":"RoutePingReq"' "${log_dir}/play-a-evidence.json"
grep -q '"value":"route-mesh-normal"' "${log_dir}/play-a-evidence.json"
grep -q '"marker":"ActorCreated"' "${log_dir}/session-a-evidence.json"
grep -q '"marker":"ActorCreatedPayload"' "${log_dir}/session-a-evidence.json"
grep -q '"value":"Player One/7/alpha,beta"' "${log_dir}/session-a-evidence.json"
grep -q '"marker":"ActorUserJoined"' "${log_dir}/session-a-evidence.json"
grep -q '"marker":"ActorUserRequest"' "${log_dir}/session-a-evidence.json"
grep -q 'ActorCreated.*ActorUserJoined.*ActorUserRequest' "${log_dir}/session-a-evidence.json"
grep -q 'user-echo-1.*user-echo-2.*user-echo-3' "${log_dir}/session-a-evidence.json"
if grep -q '"marker":"ActorUserJoined"' "${log_dir}/play-b-evidence.json"; then
  echo "unexpected play-b actor join evidence" >&2
  exit 1
fi
grep -q '"marker":"StreamInbound"' "${log_dir}/session-a-evidence.json"
grep -q "DispatchError" "${log_dir}/play-a-evidence.json"
grep -q "SpotInitialized" "${log_dir}/play-a-evidence.json"
grep -q "SpotClosing" "${log_dir}/play-a-evidence.json" "${log_dir}/play-b-evidence.json"
grep -q "SpotTypeMismatch" "${log_dir}/play-a-evidence.json"
grep -q "SpotTypeMismatchStateOk" "${log_dir}/play-a-evidence.json"
grep -q "SpotTimer" "${log_dir}/play-a-evidence.json"
grep -q "WorkerStarted" "${log_dir}/play-a-evidence.json"
grep -q "WorkerFollowUpBeforeComplete" "${log_dir}/play-a-evidence.json"
grep -q "WorkerCompleted" "${log_dir}/play-a-evidence.json"
grep -q "IngressRequest" "${log_dir}/play-a-evidence.json" "${log_dir}/play-b-evidence.json"
grep -q "IngressCommand" "${log_dir}/play-a-evidence.json" "${log_dir}/play-b-evidence.json"
grep -q "SpotOutbound" "${log_dir}/play-a-evidence.json"
grep -q "SpotToSpotSend" "${log_dir}/play-b-evidence.json"
grep -q "SpotMeshEvent" "${log_dir}/play-a-evidence.json"
grep -q "SpotMeshEvent" "${log_dir}/play-b-evidence.json"
grep -q "spot-publish|rid=gateway" "${log_dir}/gateway-evidence.json"
grep -q "IdleCloseRequested" "${log_dir}/play-a-evidence.json"
grep -q "IdleClosed" "${log_dir}/play-a-evidence.json"
grep -q "IdleKeptOpen" "${log_dir}/play-a-evidence.json"
grep -q "TimerOverrunConfigured" "${log_dir}/play-a-evidence.json"
grep -q "TimerOverrunTick" "${log_dir}/play-a-evidence.json"
grep -q "multi-state-request|node=multi-node-a" "${log_dir}/multi-node-a-evidence.json"
grep -q "multi-state-request|node=multi-node-b" "${log_dir}/multi-node-b-evidence.json"
echo "spot-service kotlin e2e result=passed"
