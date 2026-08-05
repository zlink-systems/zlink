#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/start-order-common.sh"

cd "$(dirname "${BASH_SOURCE[0]}")"

pids=()
REDIS_CONTAINER=""
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
config_dir="$(mktemp -d)"
chmod 700 "${config_dir}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
SCENARIO="${1:-all}"
e2e_start_order="$(zlink_e2e_start_order_mode "$@")"
echo "start_order=${e2e_start_order}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
readonly e2e_build_dir="${HOME}/.cache/zlink/java-e2e/RegistryMessaging"
readonly gradle_cache_dir="${HOME}/.cache/zlink/java-e2e/RegistryMessaging-gradle-cache"
redis_location_endpoint=""
location_key_prefix="zlink:e2e:registry-messaging:${run_id}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
if [[ "${LOCAL_READINESS_TIMEOUT_SECONDS}" != 3 \
   || "${LOCAL_READINESS_ATTEMPTS}" != 30 ]]; then
  echo "RegistryMessaging must use a 3s readiness limit" >&2
  exit 1
fi
if rg -n 'java\.net\.http\.HttpClient|HttpClient\.new' \
    "$(pwd)/Client/src/main/java" --glob '*.java'; then
  echo "RegistryMessaging client must use ZLinkHttpClient" >&2
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
  sleep 0.5
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    local pid="${pids[$i]}"
    for child in $(descendants "${pid}"); do
      kill -9 "${child}" >/dev/null 2>&1 || true
    done
    kill -9 "${pid}" >/dev/null 2>&1 || true
  done
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
  fi
  rm -rf "${config_dir}"
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
    for _ in range(18):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
    print(" ".join(f"tcp://127.0.0.1:{port}" for port in ports))
finally:
    for sock in sockets:
        sock.close()
PY
}

port_of() {
  local endpoint="$1"
  echo "${endpoint##*:}"
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

wait_health() {
  local name="$1"
  local endpoint="$2"
  local port
  port="$(port_of "${endpoint}")"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if python3 - "http://127.0.0.1:${port}/health" <<'PY'
import sys
import urllib.request

try:
    with urllib.request.urlopen(sys.argv[1], timeout=0.1) as response:
        sys.exit(0 if 200 <= response.status < 300 else 1)
except Exception:
    sys.exit(1)
PY
    then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${name} health at ${endpoint}" >&2
  return 1
}

wait_location_peers() {
  local http_endpoint="$1"
  local mesh_name="$2"
  local expected_count="$3"
  local timeout_seconds="${4:-5}"
  python3 - "$(port_of "${http_endpoint}")" "${mesh_name}" "${expected_count}" "${timeout_seconds}" <<'PY'
import json
import sys
import time
import urllib.error
import urllib.request

port, mesh_name, expected_count, timeout_text = sys.argv[1:]
expected_count = int(expected_count)
deadline = time.monotonic() + float(timeout_text)
last = None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/locations/peers",
                timeout=1) as response:
            last = json.load(response)
        ready = [peer for peer in last
                 if peer.get("meshName") == mesh_name
                 and peer.get("role") == "ROUTER"]
        if len(ready) >= expected_count:
            print(f"location readiness mesh={mesh_name} readyPeers={len(ready)}")
            raise SystemExit(0)
    except urllib.error.HTTPError as error:
        last = {
            "httpStatus": error.code,
            "body": error.read().decode("utf-8", errors="replace"),
        }
    except (OSError, ValueError, KeyError, TypeError):
        pass
    time.sleep(0.1)
raise SystemExit(
    f"timed out waiting for {mesh_name} ready peers={expected_count}; last={last!r}")
PY
}

wait_route_peer() {
  local http_endpoint="$1"
  local peer_rid="$2"
  local timeout_seconds="${3:-10}"
  python3 - "$(port_of "${http_endpoint}")" "${peer_rid}" "${timeout_seconds}" <<'PY'
import json
import sys
import time
import urllib.error
import urllib.request

port, peer_rid, timeout_text = sys.argv[1:]
deadline = time.monotonic() + float(timeout_text)
last = None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/route/status",
                timeout=1) as response:
            last = json.load(response)
        ready = [peer for peer in last.get("peers", [])
                 if peer.get("nodeRid") == peer_rid
                 and peer.get("state") == "ready"]
        if last.get("ready") and ready:
            print(f"route readiness peer={peer_rid}")
            raise SystemExit(0)
    except urllib.error.HTTPError as error:
        last = {
            "httpStatus": error.code,
            "body": error.read().decode("utf-8", errors="replace"),
        }
    except (OSError, ValueError, KeyError, TypeError):
        pass
    time.sleep(0.1)
raise SystemExit(f"timed out waiting for route peer={peer_rid}; last={last!r}")
PY
}

start_redis_container() {
  local redis_port
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required for ${SCENARIO}; it provisions a dedicated Redis location store." >&2
    exit 1
  fi
  zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
    "zlink-redis-java-e2e" "redis:7.2-alpine"
  redis_location_endpoint="127.0.0.1:${redis_port}"
}

gradle_run() {
  ../../gradlew -PzlinkE2eBuildDir="${e2e_build_dir}" \
    --project-cache-dir "${gradle_cache_dir}" --no-daemon "$@" --quiet
}

install_dist() {
  if [[ "${SCENARIO}" == "RM-A3" ]]; then
    gradle_run :Server:ObjectClient:installDist
  elif [[ "${SCENARIO}" == "all" || "${SCENARIO}" == "RM-A6" ]]; then
    gradle_run \
      :Client:installDist \
      :Server:Provider:installDist \
      :Server:Workflow:installDist \
      :Server:Consumer:installDist \
      :Server:ObjectClient:installDist
  else
    gradle_run \
      :Client:installDist \
      :Server:Provider:installDist \
      :Server:Consumer:installDist
  fi
}


client_bin() {
  echo "${e2e_build_dir}/Client/install/registry-messaging-client/bin/registry-messaging-client"
}

provider_bin() {
  echo "${e2e_build_dir}/Server-Provider/install/registry-messaging-provider/bin/registry-messaging-provider"
}

workflow_bin() {
  echo "${e2e_build_dir}/Server-Workflow/install/registry-messaging-workflow/bin/registry-messaging-workflow"
}

consumer_bin() {
  echo "${e2e_build_dir}/Server-Consumer/install/registry-messaging-consumer/bin/registry-messaging-consumer"
}

object_client_bin() {
  echo "${e2e_build_dir}/Server-ObjectClient/install/registry-messaging-object-client/bin/registry-messaging-object-client"
}

start_provider() {
  local rid="$1"
  local api="$2"
  local route="$3"
  local workflow="$4"
  local instance="${5:-$rid}"
  local weight="${6:-}"
  local http_port="${7:?http port is required}"
  local binary
  local route_peer=""
  local config_path="${config_dir}/${rid}-$(date +%s%N).properties"
  if [[ "${rid}" == "api-a" ]]; then
    route_peer="${ROUTE_B}"
  elif [[ "${rid}" == "api-b" ]]; then
    route_peer="${ROUTE_A}"
  fi
  if [[ "${SCENARIO}" == "RM-A1" || "${SCENARIO}" == "RM-A2" ]]; then
    route=""
  fi
  if [[ -n "${workflow}" && -z "${api}" && -z "${route}" ]]; then
    binary="$(workflow_bin)"
  else
    binary="$(provider_bin)"
  fi
  {
    echo "e2e.provider-rid=${rid}"
    echo "e2e.provider-instance=${instance}"
    echo "e2e.api-weight=${weight}"
    echo "e2e.api-endpoint=${api}"
    echo "e2e.route-endpoint=${route}"
    echo "e2e.route-peers=${route:+${route_peer}}"
    if [[ -n "${route}" ]]; then
      if [[ "${rid}" == "api-a" ]]; then
        echo "e2e.route-peer-rids=api-b"
      elif [[ "${rid}" == "api-b" ]]; then
        echo "e2e.route-peer-rids=api-a"
      else
        echo "e2e.route-peer-rids="
      fi
    else
      echo "e2e.route-peer-rids="
    fi
    echo "e2e.workflow-endpoint=${workflow}"
    echo "e2e.http-port=$(port_of "${http_port}")"
    echo 'server.port=${e2e.http-port}'
    echo "e2e.redis-location-endpoint=${redis_location_endpoint}"
    echo "e2e.location-key-prefix=${location_key_prefix}"
    echo "e2e.log-dir=${log_dir}"
  } >"${config_path}"
  chmod 600 "${config_path}"
  "${binary}" --config "${config_path}" >"${log_dir}/${rid}.stdout.log" 2>"${log_dir}/${rid}.stderr.log" &
  LAST_PID="$!"
  pids+=("${LAST_PID}")
  [[ -z "${api}" ]] || wait_port "${rid}-api" "${api}"
  [[ -z "${route}" ]] || wait_port "${rid}-route" "${route}"
  [[ -z "${workflow}" ]] || wait_port "${rid}-workflow" "${workflow}"
  wait_health "${rid}" "${http_port}"
}

start_consumer() {
  local name="$1"
  local mode="$2"
  local http_port="$3"
  local endpoints="${4:-}"
  local config_path="${config_dir}/${name}-$(date +%s%N).properties"
  {
    echo "e2e.consumer-name=${name}"
    echo "e2e.consumer-mode=${mode}"
    echo "e2e.provider-endpoints=${endpoints}"
    echo "e2e.http-port=$(port_of "${http_port}")"
    echo 'server.port=${e2e.http-port}'
    echo "e2e.redis-location-endpoint=${redis_location_endpoint}"
    echo "e2e.location-key-prefix=${location_key_prefix}"
    echo "e2e.log-dir=${log_dir}"
  } >"${config_path}"
  chmod 600 "${config_path}"
  "$(consumer_bin)" --config "${config_path}" >"${log_dir}/${name}.stdout.log" 2>"${log_dir}/${name}.stderr.log" &
  pids+=("$!")
  wait_health "${name}" "${http_port}"
}

start_object_client() {
  local rid="$1"
  local route_endpoint="$2"
  local http_endpoint="$3"
  local peer_connections="${4:-}"
  local server_weight="${5:-}"
  local config_path="${config_dir}/${rid}-$(date +%s%N).properties"
  {
    echo "e2e.client-rid=${rid}"
    echo "e2e.route-endpoint=${route_endpoint}"
    echo "e2e.peer-connections=${peer_connections}"
    echo "e2e.server-weight=${server_weight}"
    echo "e2e.http-port=$(port_of "${http_endpoint}")"
    echo 'server.port=${e2e.http-port}'
    echo "e2e.redis-location-endpoint=${redis_location_endpoint}"
    echo "e2e.location-key-prefix=${location_key_prefix}"
  } >"${config_path}"
  chmod 600 "${config_path}"
  "$(object_client_bin)" --config "${config_path}" \
    >"${log_dir}/${rid}.stdout.log" 2>"${log_dir}/${rid}.stderr.log" &
  LAST_PID="$!"
  pids+=("${LAST_PID}")
  wait_port "${rid}-route" "${route_endpoint}"
  wait_health "${rid}" "${http_endpoint}"
}

start_connection_proxy() {
  local name="$1"
  local listen_endpoint="$2"
  local upstream_endpoint="$3"
  local evidence="${log_dir}/${name}-connections.log"
  local ready="${log_dir}/${name}-ready"
  python3 Support/tcp_connection_proxy.py \
    --listen-port "$(port_of "${listen_endpoint}")" \
    --upstream-port "$(port_of "${upstream_endpoint}")" \
    --evidence "${evidence}" \
    --ready "${ready}" \
    >"${log_dir}/${name}.stdout.log" 2>"${log_dir}/${name}.stderr.log" &
  LAST_PID="$!"
  pids+=("${LAST_PID}")
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    [[ -f "${ready}" ]] && return 0
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${name} proxy" >&2
  return 1
}

wait_peer_state() {
  local http_endpoint="$1"
  local peer_rid="$2"
  local expected_state="$3"
  local expected_ready="$4"
  local timeout_seconds="${5:-10}"
  python3 - "$(port_of "${http_endpoint}")" "${peer_rid}" \
    "${expected_state}" "${expected_ready}" "${timeout_seconds}" <<'PY'
import json
import sys
import time
import urllib.request

port, peer_rid, expected_state, expected_ready, timeout_text = sys.argv[1:]
expected_ready = expected_ready == "true"
deadline = time.monotonic() + float(timeout_text)
last = None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/rm-a3/status",
                timeout=1) as response:
            last = json.load(response)
        matching = [peer for peer in last["peers"]
                    if peer["rid"] == peer_rid]
        if (len(matching) == 1
                and matching[0]["state"] == expected_state
                and matching[0]["ready"] is expected_ready):
            print(json.dumps(last, sort_keys=True))
            raise SystemExit(0)
    except (OSError, ValueError, KeyError):
        pass
    time.sleep(0.1)
raise SystemExit(
    f"timed out waiting for {peer_rid}={expected_state}/"
    f"{expected_ready}; last={last!r}")
PY
}

verify_not_required_stable() {
  local http_endpoint="$1"
  local peer_rid="$2"
  python3 - "$(port_of "${http_endpoint}")" "${peer_rid}" <<'PY'
import json
import sys
import time
import urllib.request

port, peer_rid = sys.argv[1:]
deadline = time.monotonic() + 20
observations = 0
while time.monotonic() < deadline:
    with urllib.request.urlopen(
            f"http://127.0.0.1:{port}/rm-a3/status",
            timeout=1) as response:
        status = json.load(response)
    matching = [peer for peer in status["peers"]
                if peer["rid"] == peer_rid]
    if len(matching) != 1:
        raise SystemExit(f"expected one monitored peer: {status!r}")
    peer = matching[0]
    if (peer["state"] != "not_required" or peer["ready"]
            or peer["lastFailure"] != ""
            or status["readyPeerCount"] != 0):
        raise SystemExit(
            "NotRequired entered reconnect/liveness accounting: "
            f"{status!r}")
    observations += 1
    time.sleep(0.25)
print(f"rm-a3 stable-not-required observations={observations}")
PY
}

verify_node_direct_not_found() {
  local http_endpoint="$1"
  local target_rid="$2"
  python3 - "$(port_of "${http_endpoint}")" "${target_rid}" <<'PY'
import json
import sys
import urllib.request

port, target_rid = sys.argv[1:]
request = urllib.request.Request(
    f"http://127.0.0.1:{port}/rm-a3/node-direct",
    data=json.dumps({"targetRid": target_rid}).encode(),
    headers={"Content-Type": "application/json"},
    method="POST")
with urllib.request.urlopen(request, timeout=3) as response:
    result = json.load(response)
if result.get("terminal") != "NotFound":
    raise SystemExit(f"Object Client Node direct was not NotFound: {result!r}")
for field in ("sendErrorKind", "requestErrorKind"):
    if result.get(field) != "REQUEST_TARGET_NOT_FOUND":
        raise SystemExit(f"Object Client lost typed {field}: {result!r}")
print("rm-a3 node-direct send=request=REQUEST_TARGET_NOT_FOUND")
PY
}

crash_pid() {
  local pid="$1"
  local label="$2"
  local status
  kill -9 "${pid}"
  set +e
  wait "${pid}"
  status="$?"
  set -e
  if [[ "${status}" != 137 ]]; then
    echo "${label} exited ${status} after SIGKILL; expected 137" >&2
    return 1
  fi
}

run_rm_a3() {
  echo "rm-a3 phase=automatic-not-required"
  start_object_client auto-client-a \
    "${RM_A3_ROUTE_A}" "${RM_A3_HTTP_A}"
  local auto_a_pid="${LAST_PID}"
  start_object_client auto-client-b \
    "${RM_A3_ROUTE_B}" "${RM_A3_HTTP_B}"
  local auto_b_pid="${LAST_PID}"
  wait_peer_state "${RM_A3_HTTP_A}" auto-client-b not_required false
  wait_peer_state "${RM_A3_HTTP_B}" auto-client-a not_required false
  verify_node_direct_not_found "${RM_A3_HTTP_A}" auto-client-b
  verify_not_required_stable "${RM_A3_HTTP_A}" auto-client-b
  stop_pid "${auto_a_pid}"
  stop_pid "${auto_b_pid}"

  echo "rm-a3 phase=manual-not-required"
  start_connection_proxy manual-proxy-a \
    "${RM_A3_PROXY_A}" "${RM_A3_ROUTE_A}"
  local proxy_a_pid="${LAST_PID}"
  start_connection_proxy manual-proxy-b \
    "${RM_A3_PROXY_B}" "${RM_A3_ROUTE_B}"
  local proxy_b_pid="${LAST_PID}"
  start_object_client manual-client-a \
    "${RM_A3_ROUTE_A}" "${RM_A3_HTTP_A}" \
    "manual-client-b@${RM_A3_PROXY_B}"
  local manual_a_pid="${LAST_PID}"
  start_object_client manual-client-b \
    "${RM_A3_ROUTE_B}" "${RM_A3_HTTP_B}" \
    "manual-client-a@${RM_A3_PROXY_A}"
  local manual_b_pid="${LAST_PID}"
  wait_peer_state "${RM_A3_HTTP_A}" manual-client-b not_required false
  wait_peer_state "${RM_A3_HTTP_B}" manual-client-a not_required false
  verify_not_required_stable "${RM_A3_HTTP_A}" manual-client-b
  [[ "$(wc -l <"${log_dir}/manual-proxy-a-connections.log")" == 1 ]]
  [[ "$(wc -l <"${log_dir}/manual-proxy-b-connections.log")" == 1 ]]
  stop_pid "${manual_a_pid}"
  stop_pid "${manual_b_pid}"
  stop_pid "${proxy_a_pid}"
  stop_pid "${proxy_b_pid}"

  echo "rm-a3 phase=weight-zero-server-required"
  start_object_client required-client-a \
    "${RM_A3_ROUTE_A}" "${RM_A3_HTTP_A}"
  local required_a_pid="${LAST_PID}"
  start_object_client required-client-b \
    "${RM_A3_ROUTE_B}" "${RM_A3_HTTP_B}" "" 0
  local required_b_pid="${LAST_PID}"
  wait_peer_state "${RM_A3_HTTP_A}" required-client-b ready true
  wait_peer_state "${RM_A3_HTTP_B}" required-client-a ready true
  crash_pid "${required_b_pid}" required-client-b
  wait_peer_state \
    "${RM_A3_HTTP_A}" required-client-b not_connected false 22
  stop_pid "${required_a_pid}"
  local pass_marker_tmp="${log_dir}/RM-A3.result.tmp"
  local pass_marker="${log_dir}/RM-A3.result"
  printf '%s\n' "result=passed" >"${pass_marker_tmp}"
  mv -f "${pass_marker_tmp}" "${pass_marker}"
  echo "scenario RM-A3 passed"
}

stop_pid() {
  local pid="$1"
  if kill -0 "${pid}" >/dev/null 2>&1; then
    kill "${pid}" >/dev/null 2>&1 || true
    for _ in $(seq 1 50); do
      if ! kill -0 "${pid}" >/dev/null 2>&1; then
        wait "${pid}" >/dev/null 2>&1 || true
        return
      fi
      sleep 0.1
    done
    for child in $(descendants "${pid}"); do
      kill -9 "${child}" >/dev/null 2>&1 || true
    done
    kill -9 "${pid}" >/dev/null 2>&1 || true
    wait "${pid}" >/dev/null 2>&1 || true
  fi
}

run_client() {
  local scenario="$1"
  local suffix="$2"
  shift 2
  local config_path="${config_dir}/client-${suffix}-$(date +%s%N).properties"
  {
    echo "providerAHttpUrl=http://127.0.0.1:$(port_of "${HTTP_API_A}")"
    echo "providerBHttpUrl=http://127.0.0.1:$(port_of "${HTTP_API_B}")"
    echo "workflowHttpUrl=http://127.0.0.1:$(port_of "${HTTP_WORKFLOW}")"
    echo "discoveryConsumerHttpUrl=http://127.0.0.1:$(port_of "${HTTP_DISCOVERY_CONSUMER}")"
    echo "directConsumerHttpUrl=http://127.0.0.1:$(port_of "${HTTP_DIRECT_CONSUMER}")"
    echo "singleConsumerHttpUrl=http://127.0.0.1:$(port_of "${HTTP_SINGLE_CONSUMER}")"
    echo "backpressureConsumerHttpUrl=http://127.0.0.1:$(port_of "${HTTP_BACKPRESSURE_CONSUMER}")"
    echo "redisLocationEndpoint=${redis_location_endpoint}"
    echo "locationKeyPrefix=${location_key_prefix}"
    echo "buildDir=${e2e_build_dir}"
    echo "logDir=${log_dir}"
    echo "configDir=${config_dir}"
  } >"${config_path}"
  chmod 600 "${config_path}"
  "$@" "$(client_bin)" --config "${config_path}" --scenario "${scenario}" \
    >"${log_dir}/client-${suffix}.stdout.log" 2>"${log_dir}/client-${suffix}.stderr.log"
}

is_common_scenario() {
  case "$1" in
    all|RM-A1|RM-A2|RM-A6|RM-C1|RM-C2|RM-C3|RM-C4|RM-C5|RM-C8|RM-C9)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

needs_workflow_role() {
  case "$1" in
    all|RM-A6)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

read -r API_A API_B ROUTE_A ROUTE_B WORKFLOW_A HTTP_API_A HTTP_API_B HTTP_WORKFLOW HTTP_DISCOVERY_CONSUMER HTTP_DIRECT_CONSUMER HTTP_SINGLE_CONSUMER HTTP_BACKPRESSURE_CONSUMER RM_A3_ROUTE_A RM_A3_ROUTE_B RM_A3_HTTP_A RM_A3_HTTP_B RM_A3_PROXY_A RM_A3_PROXY_B <<<"$(reserve_ports)"

start_redis_container
install_dist

if [[ "${SCENARIO}" == "RM-A3" ]]; then
  run_rm_a3
  exit 0
fi

if is_common_scenario "${SCENARIO}"; then
  SERVER_ROLES=(api-a api-b)
  if needs_workflow_role "${SCENARIO}"; then
    SERVER_ROLES+=(workflow-a)
  fi
  SERVER_ROLES+=(discovery-consumer direct-consumer single-consumer backpressure-consumer)
  mapfile -t ORDERED_SERVER_ROLES < <(zlink_e2e_order_roles "${SERVER_ROLES[@]}")
  for role in "${ORDERED_SERVER_ROLES[@]}"; do
    case "${role}" in
      api-a)
        start_provider api-a "${API_A}" "${ROUTE_A}" "" api-a "" "${HTTP_API_A}"
        API_A_PID="${LAST_PID}"
        ;;
      api-b)
        start_provider api-b "${API_B}" "${ROUTE_B}" "" api-b "" "${HTTP_API_B}"
        API_B_PID="${LAST_PID}"
        ;;
      workflow-a)
        start_provider workflow-a "" "" "${WORKFLOW_A}" workflow-a "" "${HTTP_WORKFLOW}"
        WORKFLOW_A_PID="${LAST_PID}"
        ;;
      discovery-consumer)
        start_consumer discovery-consumer discovery "${HTTP_DISCOVERY_CONSUMER}"
        ;;
      direct-consumer)
        start_consumer direct-consumer direct "${HTTP_DIRECT_CONSUMER}" "${API_A},${API_B}"
        ;;
      single-consumer)
        start_consumer single-consumer direct "${HTTP_SINGLE_CONSUMER}" "${API_A}"
        ;;
      backpressure-consumer)
        start_consumer backpressure-consumer direct "${HTTP_BACKPRESSURE_CONSUMER}" "${API_A}"
        ;;
    esac
  done

  if [[ "${SCENARIO}" != "RM-A1" && "${SCENARIO}" != "RM-A2" ]]; then
    wait_route_peer "${HTTP_API_A}" api-b
  fi
  wait_location_peers "${HTTP_DISCOVERY_CONSUMER}" "registry.messaging.api" 2

  common_client_scenario="${SCENARIO}"
  if [[ "${SCENARIO}" == "all" ]]; then
    common_client_scenario="common"
  fi
  run_client "${common_client_scenario}" "${common_client_scenario}" env
  cat "${log_dir}/client-${common_client_scenario}.stdout.log"

  stop_pid "${API_A_PID}"
  stop_pid "${API_B_PID}"
  if needs_workflow_role "${SCENARIO}"; then
    stop_pid "${WORKFLOW_A_PID}"
  fi
fi

if [[ "${SCENARIO}" == "all" || "${SCENARIO}" == "RM-C7" ]]; then
  start_provider api-a "${API_A}" "${ROUTE_A}" "" api-a 75 "${HTTP_API_A}"
  API_A_PID="${LAST_PID}"
  start_provider api-b "${API_B}" "${ROUTE_B}" "" api-b 25 "${HTTP_API_B}"
  API_B_PID="${LAST_PID}"
  weighted_client_scenario="${SCENARIO}"
  if [[ "${SCENARIO}" == "all" ]]; then
    weighted_client_scenario="weighted"
  fi
  run_client "${weighted_client_scenario}" rm-c7 env
  cat "${log_dir}/client-rm-c7.stdout.log"
  stop_pid "${API_A_PID}"
  stop_pid "${API_B_PID}"
fi

if [[ "${SCENARIO}" == "all" || "${SCENARIO}" == "RM-B1" ]]; then
  scale_out_client_scenario="${SCENARIO}"
  if [[ "${SCENARIO}" == "all" ]]; then
    scale_out_client_scenario="scale-out"
  fi
  run_client "${scale_out_client_scenario}" rm-b1 env
  cat "${log_dir}/client-rm-b1.stdout.log"
fi

if [[ "${SCENARIO}" == "all" || "${SCENARIO}" == "RM-B2" ]]; then
  scale_in_client_scenario="${SCENARIO}"
  if [[ "${SCENARIO}" == "all" ]]; then
    scale_in_client_scenario="scale-in"
  fi
  run_client "${scale_in_client_scenario}" rm-b2 env
  cat "${log_dir}/client-rm-b2.stdout.log"
fi

if [[ "${SCENARIO}" == "all" || "${SCENARIO}" == "RM-A4" ]]; then
  run_client "RM-A4" rm-a4 env
  cat "${log_dir}/client-rm-a4.stdout.log"
fi

if [[ "${SCENARIO}" == "all" ]]; then
  run_rm_a3
fi

grep -Rq "message flow" "${log_dir}"/*-flow.log
