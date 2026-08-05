#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"

cd "$(dirname "${BASH_SOURCE[0]}")"

pids=()
REDIS_CONTAINER=""
REDIS_CONTAINER_OWNED=0
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
SCENARIO="${1:-SF-A1}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
export ZLINK_KOTLIN_E2E_BUILD_DIR="${ZLINK_KOTLIN_E2E_BUILD_DIR:-${HOME}/.cache/zlink/kotlin-e2e/DiscoveryRegistryHa}"
export ZLINK_KOTLIN_E2E_GRADLE_CACHE="${ZLINK_KOTLIN_E2E_GRADLE_CACHE:-${HOME}/.cache/zlink/kotlin-e2e/DiscoveryRegistryHa-gradle-cache}"
export ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT=""
ZLINK_KOTLIN_E2E_BASE_REDIS_LOCATION_ENDPOINT=""
export ZLINK_KOTLIN_E2E_REDIS_COMMAND_TIMEOUT_MS="${ZLINK_KOTLIN_E2E_REDIS_COMMAND_TIMEOUT_MS:-500}"
export ZLINK_KOTLIN_E2E_LOCATION_HEARTBEAT_MS="${ZLINK_KOTLIN_E2E_LOCATION_HEARTBEAT_MS:-1000}"
export ZLINK_KOTLIN_E2E_LOCATION_LEASE_TTL_MS="${ZLINK_KOTLIN_E2E_LOCATION_LEASE_TTL_MS:-3000}"
export ZLINK_KOTLIN_E2E_LOCATION_POLLING_MS="${ZLINK_KOTLIN_E2E_LOCATION_POLLING_MS:-500}"
export ZLINK_KOTLIN_E2E_LOCATION_STORE_FAILURE_GRACE_MS="${ZLINK_KOTLIN_E2E_LOCATION_STORE_FAILURE_GRACE_MS:-6000}"
LOCAL_READINESS_ATTEMPTS=30
LOCAL_READINESS_POLL_SECONDS=0.1

print_logs() {
  local status="$1"
  if [[ "${status}" == "0" ]]; then
    return
  fi
  for log in "${log_dir}"/*.log; do
    [[ -f "${log}" ]] || continue
    echo "===== ${log} =====" >&2
    tail -n 160 "${log}" >&2 || true
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
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    docker unpause "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
    if [[ "${REDIS_CONTAINER_OWNED}" == "1" ]]; then
      docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
    fi
  fi
  wait >/dev/null 2>&1 || true
  exit "${status}"
}
trap cleanup EXIT

reserve_ports() {
  local count="$1"
  python3 - "${count}" <<'PY'
import socket
import sys
count = int(sys.argv[1])
sockets = []
ports = []
try:
    for _ in range(count):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
    print(" ".join(str(port) for port in ports))
finally:
    for sock in sockets:
        sock.close()
PY
}

tcp() {
  echo "tcp://127.0.0.1:$1"
}

http() {
  echo "http://127.0.0.1:$1"
}

port_of() {
  echo "${1##*:}"
}

start_redis_container() {
  local redis_port
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required for ${SCENARIO}; it provisions a dedicated Redis location store." >&2
    exit 1
  fi
  REDIS_CONTAINER_OWNED=1
  zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
    "zlink-redis-kotlin-e2e" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
  ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="127.0.0.1:${redis_port}"
  ZLINK_KOTLIN_E2E_BASE_REDIS_LOCATION_ENDPOINT="${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}"
  export ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT
}

pause_redis_container() {
  docker pause "${REDIS_CONTAINER}" >/dev/null
}

unpause_redis_container() {
  docker unpause "${REDIS_CONTAINER}" >/dev/null
}

stop_redis_container() {
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    docker unpause "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
    if [[ "${REDIS_CONTAINER_OWNED}" == "1" ]]; then
      docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
    fi
    REDIS_CONTAINER=""
    REDIS_CONTAINER_OWNED=0
  fi
  ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="${ZLINK_KOTLIN_E2E_BASE_REDIS_LOCATION_ENDPOINT}"
  export ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT
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
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}

gradle_run() {
  ../../gradlew --project-cache-dir "${ZLINK_KOTLIN_E2E_GRADLE_CACHE}" --no-daemon "$@" --quiet
}

bin_path() {
  local path="$1"
  local app="$2"
  echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/${path}/install/${app}/bin/${app}"
}

CLIENT_BIN="$(bin_path Client discovery-registry-ha-kotlin-client)"
PROVIDER_BIN="$(bin_path Server-Provider discovery-registry-ha-kotlin-provider)"
CONSUMER_BIN="$(bin_path Server-Consumer discovery-registry-ha-kotlin-consumer)"

start_provider() {
  local rid="$1"
  local endpoint="$2"
  local name="${3:-${rid}}"
  local http_endpoint="${4:-}"
  "${PROVIDER_BIN}" \
    --provider-rid "${rid}" \
    --http-endpoint "${http_endpoint}" \
    --api-endpoint "${endpoint}" \
    --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    --redis-command-timeout-ms "${ZLINK_KOTLIN_E2E_REDIS_COMMAND_TIMEOUT_MS}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --location-heartbeat-ms "${ZLINK_KOTLIN_E2E_LOCATION_HEARTBEAT_MS}" \
    --location-lease-ttl-ms "${ZLINK_KOTLIN_E2E_LOCATION_LEASE_TTL_MS}" \
    --location-polling-ms "${ZLINK_KOTLIN_E2E_LOCATION_POLLING_MS}" \
    --location-store-failure-grace-ms "${ZLINK_KOTLIN_E2E_LOCATION_STORE_FAILURE_GRACE_MS}" \
    --log-dir "${log_dir}" \
    >"${log_dir}/${name}.stdout.log" 2>"${log_dir}/${name}.stderr.log" &
  LAST_PID="$!"
  pids+=("${LAST_PID}")
  wait_port "${name}-api" "${endpoint}"
  if [[ -n "${http_endpoint}" ]]; then
    wait_port "${name}-http" "${http_endpoint}"
  fi
}

start_consumer() {
  local name="$1"
  local http_endpoint="$2"
  local store_mode="${3:-watch}"
  "${CONSUMER_BIN}" \
    --consumer-rid "${name}" \
    --http-endpoint "${http_endpoint}" \
    --location-store-mode "${store_mode}" \
    --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    --redis-command-timeout-ms "${ZLINK_KOTLIN_E2E_REDIS_COMMAND_TIMEOUT_MS}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --location-heartbeat-ms "${ZLINK_KOTLIN_E2E_LOCATION_HEARTBEAT_MS}" \
    --location-lease-ttl-ms "${ZLINK_KOTLIN_E2E_LOCATION_LEASE_TTL_MS}" \
    --location-polling-ms "${ZLINK_KOTLIN_E2E_LOCATION_POLLING_MS}" \
    --location-store-failure-grace-ms "${ZLINK_KOTLIN_E2E_LOCATION_STORE_FAILURE_GRACE_MS}" \
    --log-dir "${log_dir}" \
    >"${log_dir}/${name}.stdout.log" 2>"${log_dir}/${name}.stderr.log" &
  LAST_PID="$!"
  pids+=("${LAST_PID}")
  wait_port "${name}-http" "${http_endpoint}"
}

stop_all() {
  set +e
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    kill "${pids[$i]}" >/dev/null 2>&1 || true
  done
  for _ in $(seq 1 30); do
    local any_running=0
    for pid in "${pids[@]}"; do
      if kill -0 "${pid}" >/dev/null 2>&1; then
        any_running=1
        break
      fi
    done
    [[ "${any_running}" == "0" ]] && break
    sleep 0.1
  done
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    kill -9 "${pids[$i]}" >/dev/null 2>&1 || true
  done
  wait >/dev/null 2>&1 || true
  pids=()
  set -e
}

shutdown_http() {
  local endpoint="$1"
  python3 - "${endpoint}" <<'PY'
import sys
import urllib.request
endpoint = sys.argv[1]
request = urllib.request.Request(endpoint + "/shutdown", data=b"", method="POST")
with urllib.request.urlopen(request, timeout=5) as response:
    response.read()
PY
}

status_field() {
  local endpoint="$1"
  local field="$2"
  python3 - "${endpoint}" "${field}" <<'PY'
import json
import sys
import urllib.request
endpoint = sys.argv[1]
field = sys.argv[2]
with urllib.request.urlopen(endpoint + "/locations/status", timeout=5) as response:
    status = json.load(response)
print(status.get(field, ""))
PY
}

assert_instant_after() {
  local before="$1"
  local after="$2"
  local message="$3"
  python3 - "${before}" "${after}" "${message}" <<'PY'
import datetime
import sys
before = sys.argv[1]
after = sys.argv[2]
message = sys.argv[3]
def parse(value):
    if not value:
        raise ValueError("empty instant")
    return datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))
if parse(after) <= parse(before):
    raise SystemExit(message + f": before={before} after={after}")
PY
}

run_client() {
  local scenario="$1"
  local consumer_http="$2"
  local expected="$3"
  local suffix="${4:-${scenario}}"
  local dead_rid="${5:-api-b}"
  local absent="${6:-}"
  "${CLIENT_BIN}" \
    --scenario "${scenario}" \
    --consumer-http-endpoint "${consumer_http}" \
    --expected-rids "${expected}" \
    --dead-rid "${dead_rid}" \
    --expected-absent-rids "${absent}" \
    --location-heartbeat-ms "${ZLINK_KOTLIN_E2E_LOCATION_HEARTBEAT_MS}" \
    --location-lease-ttl-ms "${ZLINK_KOTLIN_E2E_LOCATION_LEASE_TTL_MS}" \
    --location-polling-ms "${ZLINK_KOTLIN_E2E_LOCATION_POLLING_MS}" \
    --location-store-failure-grace-ms "${ZLINK_KOTLIN_E2E_LOCATION_STORE_FAILURE_GRACE_MS}" \
    --log-dir "${log_dir}" \
    >"${log_dir}/client-${suffix}.stdout.log" 2>"${log_dir}/client-${suffix}.stderr.log"
  cat "${log_dir}/client-${suffix}.stdout.log"
}

should_run() {
  local target="$1"
  [[ "${SCENARIO}" == "${target}" || "${SCENARIO}" == "all" ]]
}

if [[ "${SCENARIO}" != "all" && "${SCENARIO}" != "SF-A1" && "${SCENARIO}" != "SF-A2" && "${SCENARIO}" != "SF-B1" && "${SCENARIO}" != "SF-B2" && "${SCENARIO}" != "SF-C1" && "${SCENARIO}" != "SF-C2" && "${SCENARIO}" != "SF-D1" && "${SCENARIO}" != "SF-D2" && "${SCENARIO}" != "SF-D3" && "${SCENARIO}" != "SF-E1" ]]; then
  echo "unknown scenario ${SCENARIO}" >&2
  exit 1
fi

gradle_run :Client:installDist :Server:Provider:installDist :Server:Consumer:installDist

if should_run SF-A1; then
ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="zlink:e2e:kotlin-store-failure:${run_id}:sf-a1"
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"; CONSUMER_HTTP="$(http "${CH}")"
start_provider api-a "${API_A}" api-a "$(http "${AH}")"
start_provider api-b "${API_B}" api-b "$(http "${BH}")"
start_consumer "consumer-SF-A1" "${CONSUMER_HTTP}"
sleep 2
run_client SF-A1 "${CONSUMER_HTTP}" "api-a,api-b"
stop_all
stop_redis_container
fi

if should_run SF-A2; then
ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="zlink:e2e:kotlin-store-failure:${run_id}:sf-a2"
start_redis_container
read -r AH BH CH CPH A B C <<<"$(reserve_ports 7)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"; API_C="$(tcp "${C}")"
HTTP_C="$(http "${CPH}")"; CONSUMER_HTTP="$(http "${CH}")"
start_provider api-a "${API_A}" api-a "$(http "${AH}")"
start_provider api-b "${API_B}" api-b "$(http "${BH}")"
start_consumer "consumer-SF-A2" "${CONSUMER_HTTP}" polling
sleep 2
run_client SF-A2 "${CONSUMER_HTTP}" "api-a,api-b" SF-A2-initial
start_provider api-c "${API_C}" api-c "${HTTP_C}"
API_C_PID="${LAST_PID}"
run_client SF-A2 "${CONSUMER_HTTP}" "api-a,api-b,api-c" SF-A2-added
shutdown_http "${HTTP_C}"
wait "${API_C_PID}" >/dev/null 2>&1 || true
run_client SF-A2 "${CONSUMER_HTTP}" "api-a,api-b" SF-A2-removed api-b api-c
stop_all
stop_redis_container
fi

if should_run SF-B1; then
ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="zlink:e2e:kotlin-store-failure:${run_id}:sf-b1"
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"; CONSUMER_HTTP="$(http "${CH}")"
start_provider api-a "${API_A}" api-a "$(http "${AH}")"
start_provider api-b "${API_B}" api-b "$(http "${BH}")"
start_consumer "consumer-SF-B1" "${CONSUMER_HTTP}"
sleep 2
run_client SF-A1 "${CONSUMER_HTTP}" "api-a,api-b" SF-B1-baseline
pause_redis_container
run_client SF-B1 "${CONSUMER_HTTP}" "api-a,api-b"
unpause_redis_container
run_client SF-B1-RECOVERED "${CONSUMER_HTTP}" "api-a,api-b" SF-B1-recovered
stop_all
stop_redis_container
fi

if should_run SF-B2; then
ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="zlink:e2e:kotlin-store-failure:${run_id}:sf-b2"
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"; CONSUMER_HTTP="$(http "${CH}")"
start_provider api-a "${API_A}" api-a "$(http "${AH}")"
start_provider api-b "${API_B}" api-b "$(http "${BH}")"
start_consumer "consumer-SF-B2" "${CONSUMER_HTTP}"
sleep 2
run_client SF-A1 "${CONSUMER_HTTP}" "api-a,api-b" SF-B2-baseline
pause_redis_container
run_client SF-B2 "${CONSUMER_HTTP}" "api-a,api-b"
unpause_redis_container
run_client SF-B2-RECOVERED "${CONSUMER_HTTP}" "api-a,api-b" SF-B2-recovered
stop_all
stop_redis_container
fi

if should_run SF-C1; then
ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="zlink:e2e:kotlin-store-failure:${run_id}:sf-c1"
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"; CONSUMER_HTTP="$(http "${CH}")"
start_provider api-a "${API_A}" api-a "$(http "${AH}")"
start_provider api-b "${API_B}" api-b "$(http "${BH}")"
API_B_PID="${LAST_PID}"
start_consumer "consumer-SF-C1" "${CONSUMER_HTTP}"
sleep 2
run_client SF-A1 "${CONSUMER_HTTP}" "api-a,api-b" SF-C1-baseline
kill -9 "${API_B_PID}" >/dev/null 2>&1 || true
wait "${API_B_PID}" >/dev/null 2>&1 || true
run_client SF-C1 "${CONSUMER_HTTP}" "api-a" SF-C1 api-b
stop_all
stop_redis_container
fi

if should_run SF-C2; then
ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="zlink:e2e:kotlin-store-failure:${run_id}:sf-c2"
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"; HTTP_B="$(http "${BH}")"; CONSUMER_HTTP="$(http "${CH}")"
start_provider api-a "${API_A}" api-a "$(http "${AH}")"
start_provider api-b "${API_B}" api-b "${HTTP_B}"
API_B_PID="${LAST_PID}"
start_consumer "consumer-SF-C2" "${CONSUMER_HTTP}"
sleep 2
run_client SF-A1 "${CONSUMER_HTTP}" "api-a,api-b" SF-C2-baseline
shutdown_http "${HTTP_B}"
wait "${API_B_PID}" >/dev/null 2>&1 || true
run_client SF-C2 "${CONSUMER_HTTP}" "api-a" SF-C2 api-b
stop_all
stop_redis_container
fi

if should_run SF-D1; then
ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="zlink:e2e:kotlin-store-failure:${run_id}:sf-d1"
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"; CONSUMER_HTTP="$(http "${CH}")"
start_provider api-a "${API_A}" api-a "$(http "${AH}")"
start_provider api-b "${API_B}" api-b "$(http "${BH}")"
start_consumer "consumer-SF-D1" "${CONSUMER_HTTP}"
sleep 2
run_client SF-A1 "${CONSUMER_HTTP}" "api-a,api-b" SF-D1-baseline
run_client SF-D1 "${CONSUMER_HTTP}" "api-a,api-b" SF-D1 &
SF_D1_CLIENT_PID="$!"
sleep 0.5
pause_redis_container
python3 - "${ZLINK_KOTLIN_E2E_LOCATION_LEASE_TTL_MS}" <<'PY'
import sys
import time
time.sleep(int(sys.argv[1]) / 2000.0)
PY
unpause_redis_container
wait "${SF_D1_CLIENT_PID}"
run_client SF-D1-RECOVERED "${CONSUMER_HTTP}" "api-a,api-b" SF-D1-recovered
stop_all
stop_redis_container
fi

if should_run SF-D2; then
ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="zlink:e2e:kotlin-store-failure:${run_id}:sf-d2"
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"; CONSUMER_HTTP="$(http "${CH}")"
start_provider api-a "${API_A}" api-a "$(http "${AH}")"
start_provider api-b "${API_B}" api-b "$(http "${BH}")"
API_B_PID="${LAST_PID}"
start_consumer "consumer-SF-D2" "${CONSUMER_HTTP}"
sleep 2
run_client SF-A1 "${CONSUMER_HTTP}" "api-a,api-b" SF-D2-baseline
run_client SF-D2 "${CONSUMER_HTTP}" "api-a" SF-D2 api-b &
SF_D2_CLIENT_PID="$!"
sleep 0.5
pause_redis_container
kill -9 "${API_B_PID}" >/dev/null 2>&1 || true
wait "${API_B_PID}" >/dev/null 2>&1 || true
python3 - "${ZLINK_KOTLIN_E2E_LOCATION_LEASE_TTL_MS}" "${ZLINK_KOTLIN_E2E_LOCATION_HEARTBEAT_MS}" <<'PY'
import sys
import time
time.sleep((int(sys.argv[1]) + int(sys.argv[2])) / 1000.0)
PY
unpause_redis_container
wait "${SF_D2_CLIENT_PID}"
stop_all
stop_redis_container
fi

if should_run SF-D3; then
ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="zlink:e2e:kotlin-store-failure:${run_id}:sf-d3"
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"; CONSUMER_HTTP="$(http "${CH}")"
start_provider api-a "${API_A}" api-a "$(http "${AH}")"
start_provider api-b "${API_B}" api-b "$(http "${BH}")"
start_consumer "consumer-SF-D3" "${CONSUMER_HTTP}"
sleep 2
run_client SF-D3-HEALTHY "${CONSUMER_HTTP}" "api-a,api-b" SF-D3-healthy
SF_D3_BEFORE_REFRESH="$(status_field "${CONSUMER_HTTP}" lastRefreshAt)"
pause_redis_container
run_client SF-D3-OUTAGE "${CONSUMER_HTTP}" "api-a,api-b" SF-D3-outage
unpause_redis_container
run_client SF-D3-RECOVERED "${CONSUMER_HTTP}" "api-a,api-b" SF-D3-recovered
SF_D3_AFTER_REFRESH="$(status_field "${CONSUMER_HTTP}" lastRefreshAt)"
assert_instant_after "${SF_D3_BEFORE_REFRESH}" "${SF_D3_AFTER_REFRESH}" \
  "SF-D3 recovered status did not advance lastRefreshAt"
echo "scenario SF-D3 passed" | tee "${log_dir}/client-SF-D3.stdout.log"
stop_all
stop_redis_container
fi

if should_run SF-E1; then
ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="zlink:e2e:kotlin-store-failure:${run_id}:sf-e1"
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"; CONSUMER_HTTP="$(http "${CH}")"
start_provider api-a "${API_A}" api-a "$(http "${AH}")"
start_provider api-b "${API_B}" api-b "$(http "${BH}")"
start_consumer "consumer-SF-E1" "${CONSUMER_HTTP}" delay
sleep 2
run_client SF-E1 "${CONSUMER_HTTP}" "api-a,api-b"
stop_all
stop_redis_container
fi

if [[ "${SCENARIO}" == "all" ]]; then
  for scenario in SF-A1 SF-A2 SF-B1 SF-B2 SF-C1 SF-C2 SF-D1 SF-D2 SF-D3 SF-E1; do
    grep -Rq "scenario ${scenario} " "${log_dir}"/client-*.stdout.log
  done
else
  grep -Rq "scenario ${SCENARIO} " "${log_dir}"/client-*.stdout.log
fi
grep -Rq "message flow" "${log_dir}"/*-flow.log
echo "discovery-registry-ha kotlin e2e result=passed"
