#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/start-order-common.sh"

cd "$(dirname "${BASH_SOURCE[0]}")"

pids=()
REDIS_CONTAINER=""
BASE_REDIS_CONTAINER=""
REDIS_PROXY_PID=""
REDIS_DELAY_CONTROL_FILE=""
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
config_dir="$(mktemp -d)"
chmod 700 "${config_dir}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
SCENARIO="${1:-SF-A1}"
e2e_start_order="$(zlink_e2e_start_order_mode "$@")"
echo "start_order=${e2e_start_order}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
readonly e2e_build_dir="${HOME}/.cache/zlink/java-e2e/StoreFailure"
readonly gradle_cache_dir="${HOME}/.cache/zlink/java-e2e/StoreFailure-gradle-cache"
redis_command_timeout_ms=500
location_key_prefix="zlink:e2e:store-failure:${run_id}"
location_heartbeat_ms=1000
location_lease_ttl_ms=3000
location_polling_ms=500
location_store_failure_grace_ms=6000
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
SCENARIO_SETTLE_SECONDS=3
if rg -n 'java\.net\.http\.HttpClient|HttpClient\.new' \
    Client/src/main/java Shared/src/main/java --glob '*.java'; then
  echo "StoreFailure client must use ZLinkHttpClient" >&2
  exit 1
fi
if rg -n 'context\.runStoreFailure|runStoreFailure[A-Z]' \
    Client/src/main/java --glob '*.java'; then
  echo "StoreFailure scenarios must own their actions and assertions" >&2
  exit 1
fi

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
  for _ in $(seq 1 50); do
    local alive=0
    for pid in "${pids[@]}"; do
      if kill -0 "${pid}" >/dev/null 2>&1; then
        alive=1
        break
      fi
    done
    [[ "${alive}" == "0" ]] && break
    sleep 0.1
  done
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    local pid="${pids[$i]}"
    for child in $(descendants "${pid}"); do
      kill -9 "${child}" >/dev/null 2>&1 || true
    done
    kill -9 "${pid}" >/dev/null 2>&1 || true
  done
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    docker unpause "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
    docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${REDIS_PROXY_PID}" ]]; then
    kill "${REDIS_PROXY_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${BASE_REDIS_CONTAINER}" ]]; then
    docker rm -fv "${BASE_REDIS_CONTAINER}" >/dev/null 2>&1 || true
  fi
  rm -rf "${config_dir}"
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

redis_port="$(reserve_ports 1)"
zlink_redis_start_scoped_assign BASE_REDIS_CONTAINER redis_port \
  "zlink-redis-java-e2e" \
  "redis:7.2-alpine" \
  "127.0.0.1:${redis_port}:6379"
redis_location_endpoint="127.0.0.1:${redis_port}"
base_redis_location_endpoint="${redis_location_endpoint}"

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
  if [[ -n "${base_redis_location_endpoint}" ]]; then
    local proxy_port
    proxy_port="$(reserve_ports 1)"
    REDIS_DELAY_CONTROL_FILE="${log_dir}/redis-delay-ms"
    python3 - "${base_redis_location_endpoint}" "${proxy_port}" \
      "${REDIS_DELAY_CONTROL_FILE}" \
      >"${log_dir}/redis-proxy.stdout.log" 2>"${log_dir}/redis-proxy.stderr.log" <<'PY' &
import pathlib
import socket
import sys
import threading
import time

target = sys.argv[1]
listen_port = int(sys.argv[2])
delay_path = pathlib.Path(sys.argv[3])
delay_path.write_text("0", encoding="utf-8")
if target.startswith("tcp://"):
    target = target[len("tcp://"):]
host, port_text = target.rsplit(":", 1)
target_addr = (host, int(port_text))

class ConnectionDelay:
    def __init__(self):
        self.lock = threading.Lock()
        self.request_buffer = b""
        self.pending_peer_list_responses = 0

    def note_request(self, data):
        with self.lock:
            self.request_buffer = (self.request_buffer + data)[-4096:]
            upper = self.request_buffer.upper()
            if b"SMEMBERS" in upper and b":KEYS:PEER" in upper:
                self.pending_peer_list_responses += 1
                self.request_buffer = b""

    def take_peer_list_response(self):
        with self.lock:
            if self.pending_peer_list_responses == 0:
                return False
            self.pending_peer_list_responses -= 1
            return True

def pump(source, destination, connection_delay, request_direction=False):
    try:
        while True:
            data = source.recv(65536)
            if not data:
                return
            if request_direction:
                connection_delay.note_request(data)
            elif connection_delay.take_peer_list_response():
                delay = int(delay_path.read_text(encoding="utf-8").strip() or "0")
                if delay > 0:
                    print(f"redis proxy peer-list response delay milliseconds={delay}", flush=True)
                    time.sleep(delay / 1000.0)
            destination.sendall(data)
    except OSError:
        return
    finally:
        for sock in (source, destination):
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", listen_port))
    server.listen()
    print(f"redis proxy listening on 127.0.0.1:{listen_port} -> {target_addr[0]}:{target_addr[1]}", flush=True)
    while True:
        client, _ = server.accept()
        try:
            upstream = socket.create_connection(target_addr, timeout=5)
        except OSError:
            client.close()
            continue
        connection_delay = ConnectionDelay()
        threading.Thread(
            target=pump,
            args=(client, upstream, connection_delay, True),
            daemon=True).start()
        threading.Thread(
            target=pump,
            args=(upstream, client, connection_delay, False),
            daemon=True).start()
PY
    REDIS_PROXY_PID="$!"
    redis_location_endpoint="127.0.0.1:${proxy_port}"
    wait_port redis-proxy "127.0.0.1:${proxy_port}"
    return
  fi
  echo "StoreFailure requires the Redis container created by this runner." >&2
  return 1
}

pause_redis_container() {
  if [[ -n "${REDIS_PROXY_PID}" ]]; then
    kill -STOP "${REDIS_PROXY_PID}"
  else
    docker pause "${REDIS_CONTAINER}" >/dev/null
  fi
}

unpause_redis_container() {
  if [[ -n "${REDIS_PROXY_PID}" ]]; then
    kill -CONT "${REDIS_PROXY_PID}"
  else
    docker unpause "${REDIS_CONTAINER}" >/dev/null
  fi
}

stop_redis_process() {
  docker stop -t 1 "${BASE_REDIS_CONTAINER}" >/dev/null
}

restart_redis_process() {
  docker start "${BASE_REDIS_CONTAINER}" >/dev/null
  zlink_redis_wait_ready "${BASE_REDIS_CONTAINER}"
  wait_port redis-base "${base_redis_location_endpoint}"
}

stop_redis_container() {
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    docker unpause "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
    docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
    REDIS_CONTAINER=""
  fi
  if [[ -n "${REDIS_PROXY_PID}" ]]; then
    kill "${REDIS_PROXY_PID}" >/dev/null 2>&1 || true
    REDIS_PROXY_PID=""
  fi
  redis_location_endpoint="${base_redis_location_endpoint}"
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
  ../../gradlew -PzlinkE2eBuildDir="${e2e_build_dir}" \
    --project-cache-dir "${gradle_cache_dir}" --no-daemon "$@" --quiet
}

provider_bin() {
  echo "${e2e_build_dir}/Server-Provider/install/store-failure-provider/bin/store-failure-provider"
}

consumer_bin() {
  echo "${e2e_build_dir}/Server-Consumer/install/store-failure-consumer/bin/store-failure-consumer"
}

client_bin() {
  echo "${e2e_build_dir}/Client/install/store-failure-client/bin/store-failure-client"
}

start_provider() {
  local rid="$1"
  local endpoint="$2"
  local name="${3:-${rid}}"
  local http_endpoint="${4:-}"
  local config_path="${config_dir}/${name}-$(date +%s%N).properties"
  {
    echo "e2e.rid=${rid}"
    echo "e2e.http-endpoint=${http_endpoint}"
    echo "e2e.channel-endpoint=${endpoint}"
    echo "e2e.redis-location-endpoint=${redis_location_endpoint}"
    echo "e2e.location-key-prefix=${location_key_prefix}"
    echo "e2e.redis-command-timeout-millis=${redis_command_timeout_ms}"
    echo "e2e.heartbeat-millis=${location_heartbeat_ms}"
    echo "e2e.lease-ttl-millis=${location_lease_ttl_ms}"
    echo "e2e.polling-millis=${location_polling_ms}"
    echo "e2e.store-failure-grace-millis=${location_store_failure_grace_ms}"
    echo "e2e.evidence-file="
    echo "e2e.log-dir=${log_dir}"
  } >"${config_path}"
  chmod 600 "${config_path}"
  "$(provider_bin)" --config "${config_path}" >"${log_dir}/${name}.stdout.log" 2>"${log_dir}/${name}.stderr.log" &
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
  local store_mode="${3:-stamp}"
  local config_path="${config_dir}/${name}-$(date +%s%N).properties"
  {
    echo "e2e.rid=${name}"
    echo "e2e.http-endpoint=${http_endpoint}"
    echo "e2e.store-mode=${store_mode}"
    echo "e2e.redis-location-endpoint=${redis_location_endpoint}"
    echo "e2e.location-key-prefix=${location_key_prefix}"
    echo "e2e.redis-command-timeout-millis=${redis_command_timeout_ms}"
    echo "e2e.heartbeat-millis=${location_heartbeat_ms}"
    echo "e2e.lease-ttl-millis=${location_lease_ttl_ms}"
    echo "e2e.polling-millis=${location_polling_ms}"
    echo "e2e.store-failure-grace-millis=${location_store_failure_grace_ms}"
    echo "e2e.store-delay-control-file=${REDIS_DELAY_CONTROL_FILE}"
    echo "e2e.log-dir=${log_dir}"
  } >"${config_path}"
  chmod 600 "${config_path}"
  "$(consumer_bin)" --config "${config_path}" >"${log_dir}/${name}.stdout.log" 2>"${log_dir}/${name}.stderr.log" &
  LAST_PID="$!"
  pids+=("${LAST_PID}")
  wait_port "${name}-http" "${http_endpoint}"
}

run_client() {
  local scenario="$1"
  local expected_rids="$2"
  local suffix="$3"
  local expected_absent_rids="${4:-}"
  local dead_rid="${5:-api-b}"
  local background="${6:-false}"
  local config_path="${config_dir}/client-${suffix}-$(date +%s%N).properties"
  {
    echo "expectedRids=${expected_rids}"
    echo "consumerHttpEndpoint=${CONSUMER_HTTP}"
    echo "deadRid=${dead_rid}"
    echo "expectedAbsentRids=${expected_absent_rids}"
    echo "locationHeartbeatMillis=${location_heartbeat_ms}"
    echo "locationLeaseTtlMillis=${location_lease_ttl_ms}"
    echo "locationPollingMillis=${location_polling_ms}"
    echo "locationStoreFailureGraceMillis=${location_store_failure_grace_ms}"
  } >"${config_path}"
  chmod 600 "${config_path}"
  if [[ "${background}" == "true" ]]; then
    "$(client_bin)" --config "${config_path}" --scenario "${scenario}" \
      >"${log_dir}/client-${suffix}.stdout.log" 2>"${log_dir}/client-${suffix}.stderr.log" &
    LAST_CLIENT_PID="$!"
  else
    "$(client_bin)" --config "${config_path}" --scenario "${scenario}" \
      >"${log_dir}/client-${suffix}.stdout.log" 2>"${log_dir}/client-${suffix}.stderr.log"
  fi
}

start_initial_topology() {
  local consumer_name="$1"
  local consumer_mode="${2:-stamp}"
  local provider_http="${3:-true}"
  local -a roles
  mapfile -t roles < <(zlink_e2e_order_roles api-a api-b consumer)
  local role
  for role in "${roles[@]}"; do
    case "${role}" in
      api-a)
        if [[ "${provider_http}" == "true" ]]; then
          start_provider api-a "${API_A}" api-a "${HTTP_A}"
        else
          start_provider api-a "${API_A}" api-a
        fi
        API_A_PID="${LAST_PID}"
        ;;
      api-b)
        if [[ "${provider_http}" == "true" ]]; then
          start_provider api-b "${API_B}" api-b "${HTTP_B}"
        else
          start_provider api-b "${API_B}" api-b
        fi
        API_B_PID="${LAST_PID}"
        ;;
      consumer)
        start_consumer "${consumer_name}" "${CONSUMER_HTTP}" "${consumer_mode}"
        ;;
    esac
  done
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
  for pid in "${pids[@]}"; do
    wait "${pid}" >/dev/null 2>&1 || true
  done
  pids=()
  set -e
}

stop_pid() {
  local pid="$1"
  if kill -0 "${pid}" >/dev/null 2>&1; then
    kill "${pid}" >/dev/null 2>&1 || true
    wait "${pid}" >/dev/null 2>&1 || true
  fi
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

should_run() {
  local target="$1"
  [[ "${SCENARIO}" == "${target}" || ( "${SCENARIO}" == "all" && "${target}" == SF-* ) ]]
}

if [[ "${SCENARIO}" != "all" && "${SCENARIO}" != "SF-A1" && "${SCENARIO}" != "SF-A2" && "${SCENARIO}" != "SF-B1" && "${SCENARIO}" != "SF-B2" && "${SCENARIO}" != "SF-C1" && "${SCENARIO}" != "SF-C2" && "${SCENARIO}" != "SF-D1" && "${SCENARIO}" != "SF-D2" && "${SCENARIO}" != "SF-D3" && "${SCENARIO}" != "SF-E1" ]]; then
  echo "unknown StoreFailure scenario: ${SCENARIO}" >&2
  exit 1
fi

gradle_run installDist

if should_run SF-A1; then
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"
HTTP_A="$(http "${AH}")"; HTTP_B="$(http "${BH}")"; CONSUMER_HTTP="$(http "${CH}")"
start_initial_topology "consumer-SF-A1" stamp false
run_client "SF-A1" "api-a,api-b" "SF-A1" "" "api-b" "false"
cat "${log_dir}/client-SF-A1.stdout.log"
stop_all
fi

if should_run SF-A2; then
read -r AH BH CH CPH A B C <<<"$(reserve_ports 7)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"; API_C="$(tcp "${C}")"
HTTP_A="$(http "${AH}")"; HTTP_B="$(http "${BH}")"; HTTP_C="$(http "${CPH}")"; CONSUMER_HTTP="$(http "${CH}")"
start_initial_topology "consumer-SF-A2" polling
run_client "SF-A2" "api-a,api-b" "SF-A2-initial" "" "api-b" "false"
cat "${log_dir}/client-SF-A2-initial.stdout.log"
start_provider api-c "${API_C}" api-c "${HTTP_C}"
API_C_PID="${LAST_PID}"
run_client "SF-A2" "api-a,api-b,api-c" "SF-A2-added" "" "api-b" "false"
cat "${log_dir}/client-SF-A2-added.stdout.log"
shutdown_http "${HTTP_C}"
wait "${API_C_PID}" >/dev/null 2>&1 || true
run_client "SF-A2" "api-a,api-b" "SF-A2-removed" "api-c" "api-b" "false"
cat "${log_dir}/client-SF-A2-removed.stdout.log"
stop_all
fi

if should_run SF-B1; then
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"
HTTP_A="$(http "${AH}")"; HTTP_B="$(http "${BH}")"; CONSUMER_HTTP="$(http "${CH}")"
start_initial_topology "consumer-SF-B1"
run_client "SF-A1" "api-a,api-b" "SF-B1-baseline" "" "api-b" "false"
cat "${log_dir}/client-SF-B1-baseline.stdout.log"
pause_redis_container
run_client "SF-B1" "api-a,api-b" "SF-B1" "" "api-b" "false"
cat "${log_dir}/client-SF-B1.stdout.log"
unpause_redis_container
run_client "SF-B1-RECOVERED" "api-a,api-b" "SF-B1-recovered" "" "api-b" "false"
cat "${log_dir}/client-SF-B1-recovered.stdout.log"
stop_all
stop_redis_container
fi

if should_run SF-B2; then
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"
HTTP_A="$(http "${AH}")"; HTTP_B="$(http "${BH}")"; CONSUMER_HTTP="$(http "${CH}")"
start_initial_topology "consumer-SF-B2"
run_client "SF-A1" "api-a,api-b" "SF-B2-baseline" "" "api-b" "false"
cat "${log_dir}/client-SF-B2-baseline.stdout.log"
pause_redis_container
run_client "SF-B2" "api-a,api-b" "SF-B2" "" "api-b" "false"
cat "${log_dir}/client-SF-B2.stdout.log"
unpause_redis_container
run_client "SF-B2-RECOVERED" "api-a,api-b" "SF-B2-recovered" "" "api-b" "false"
cat "${log_dir}/client-SF-B2-recovered.stdout.log"
stop_all
stop_redis_container
fi

if should_run SF-C1; then
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"
HTTP_A="$(http "${AH}")"; HTTP_B="$(http "${BH}")"; CONSUMER_HTTP="$(http "${CH}")"
start_initial_topology "consumer-SF-C1" stamp false
run_client "SF-A1" "api-a,api-b" "SF-C1-baseline" "" "api-b" "false"
cat "${log_dir}/client-SF-C1-baseline.stdout.log"
kill -9 "${API_B_PID}" >/dev/null 2>&1 || true
wait "${API_B_PID}" >/dev/null 2>&1 || true
run_client "SF-C1" "api-a" "SF-C1" "" "api-b" "false"
cat "${log_dir}/client-SF-C1.stdout.log"
stop_all
fi

if should_run SF-C2; then
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"
HTTP_A="$(http "${AH}")"; HTTP_B="$(http "${BH}")"; CONSUMER_HTTP="$(http "${CH}")"
start_initial_topology "consumer-SF-C2"
run_client "SF-A1" "api-a,api-b" "SF-C2-baseline" "" "api-b" "false"
cat "${log_dir}/client-SF-C2-baseline.stdout.log"
shutdown_http "${HTTP_B}"
wait "${API_B_PID}" >/dev/null 2>&1 || true
run_client "SF-C2" "api-a" "SF-C2" "" "api-b" "false"
cat "${log_dir}/client-SF-C2.stdout.log"
stop_all
fi

if should_run SF-D1; then
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"
HTTP_A="$(http "${AH}")"; HTTP_B="$(http "${BH}")"; CONSUMER_HTTP="$(http "${CH}")"
start_initial_topology "consumer-SF-D1"
run_client "SF-A1" "api-a,api-b" "SF-D1-baseline" "" "api-b" "false"
cat "${log_dir}/client-SF-D1-baseline.stdout.log"
run_client "SF-D1" "api-a,api-b" "SF-D1" "" "api-b" "true"
SF_D1_CLIENT_PID="${LAST_CLIENT_PID}"
sleep 0.5
pause_redis_container
python3 - "${location_lease_ttl_ms}" <<'PY'
import sys
import time

time.sleep(int(sys.argv[1]) / 2000.0)
PY
unpause_redis_container
wait "${SF_D1_CLIENT_PID}"
cat "${log_dir}/client-SF-D1.stdout.log"
run_client "SF-D1-RECOVERED" "api-a,api-b" "SF-D1-recovered" "" "api-b" "false"
cat "${log_dir}/client-SF-D1-recovered.stdout.log"
stop_all
stop_redis_container
fi

if should_run SF-D2; then
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"
HTTP_A="$(http "${AH}")"; HTTP_B="$(http "${BH}")"; CONSUMER_HTTP="$(http "${CH}")"
start_initial_topology "consumer-SF-D2"
run_client "SF-A1" "api-a,api-b" "SF-D2-baseline" "" "api-b" "false"
cat "${log_dir}/client-SF-D2-baseline.stdout.log"
run_client "SF-D2" "api-a" "SF-D2" "" "api-b" "true"
SF_D2_CLIENT_PID="${LAST_CLIENT_PID}"
sleep 0.5
stop_redis_process
if [[ "$(docker inspect -f '{{.State.Running}}' "${BASE_REDIS_CONTAINER}")" != "false" ]]; then
  echo "SF-D2 Redis process did not stop" >&2
  exit 1
fi
kill -9 "${API_B_PID}" >/dev/null 2>&1 || true
wait "${API_B_PID}" >/dev/null 2>&1 || true
python3 - "${location_lease_ttl_ms}" "${location_heartbeat_ms}" <<'PY'
import sys
import time

time.sleep((int(sys.argv[1]) + int(sys.argv[2])) / 1000.0)
PY
restart_redis_process
wait "${SF_D2_CLIENT_PID}"
cat "${log_dir}/client-SF-D2.stdout.log"
stop_all
stop_redis_container
fi

if should_run SF-D3; then
start_redis_container
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"
HTTP_A="$(http "${AH}")"; HTTP_B="$(http "${BH}")"; CONSUMER_HTTP="$(http "${CH}")"
start_initial_topology "consumer-SF-D3"
run_client "SF-D3-HEALTHY" "api-a,api-b" "SF-D3-healthy" "" "api-b" "false"
cat "${log_dir}/client-SF-D3-healthy.stdout.log"
SF_D3_BEFORE_REFRESH="$(status_field "${CONSUMER_HTTP}" lastRefreshAt)"
pause_redis_container
run_client "SF-D3-OUTAGE" "api-a,api-b" "SF-D3-outage" "" "api-b" "false"
cat "${log_dir}/client-SF-D3-outage.stdout.log"
unpause_redis_container
run_client "SF-D3-RECOVERED" "api-a,api-b" "SF-D3-recovered" "" "api-b" "false"
cat "${log_dir}/client-SF-D3-recovered.stdout.log"
SF_D3_AFTER_REFRESH="$(status_field "${CONSUMER_HTTP}" lastRefreshAt)"
assert_instant_after "${SF_D3_BEFORE_REFRESH}" "${SF_D3_AFTER_REFRESH}" \
  "SF-D3 recovered status did not advance lastRefreshAt"
echo "scenario SF-D3 passed" | tee "${log_dir}/client-SF-D3.stdout.log"
stop_all
stop_redis_container
fi

if should_run SF-E1; then
start_redis_container
redis_command_timeout_ms=5000
read -r AH BH CH A B <<<"$(reserve_ports 5)"
API_A="$(tcp "${A}")"; API_B="$(tcp "${B}")"
HTTP_A="$(http "${AH}")"; HTTP_B="$(http "${BH}")"; CONSUMER_HTTP="$(http "${CH}")"
SF_E1_PROXY_ENDPOINT="${redis_location_endpoint}"
mapfile -t SF_E1_ROLES < <(zlink_e2e_order_roles api-a api-b consumer)
for role in "${SF_E1_ROLES[@]}"; do
  case "${role}" in
    api-a)
      redis_location_endpoint="${base_redis_location_endpoint}"
      start_provider api-a "${API_A}" api-a "${HTTP_A}"
      API_A_PID="${LAST_PID}"
      ;;
    api-b)
      redis_location_endpoint="${base_redis_location_endpoint}"
      start_provider api-b "${API_B}" api-b "${HTTP_B}"
      API_B_PID="${LAST_PID}"
      ;;
    consumer)
      redis_location_endpoint="${SF_E1_PROXY_ENDPOINT}"
      start_consumer "consumer-SF-E1" "${CONSUMER_HTTP}" delay
      ;;
  esac
done
redis_location_endpoint="${SF_E1_PROXY_ENDPOINT}"
run_client "SF-E1" "api-a,api-b" "SF-E1" "" "api-b" "false"
cat "${log_dir}/client-SF-E1.stdout.log"
grep -q "redis proxy peer-list response delay milliseconds=1200" \
  "${log_dir}/redis-proxy.stdout.log"
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
echo "store-failure e2e result=passed"
