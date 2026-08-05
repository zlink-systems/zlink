#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/start-order-common.sh"
SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

cd "$(dirname "${BASH_SOURCE[0]}")"

pids=()
declare -A role_pids=()
REDIS_CONTAINER=""
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
SCENARIO="${1:-all}"
e2e_start_order="$(zlink_e2e_start_order_mode "$@")"
BIND_RETRY_PATTERN="ZlinkBindException|BindException|Address already in use|EADDRINUSE|errno=98"
echo "start_order=${e2e_start_order}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
readonly e2e_build_dir="${HOME}/.cache/zlink/java-e2e/SpotService"
readonly gradle_cache_dir="${HOME}/.cache/zlink/java-e2e/SpotService-gradle-cache"
if rg -n 'java\.net\.http\.HttpClient|HttpClient\.new' \
    "$(pwd)/Client/src/main/java" --glob '*.java'; then
  echo "SpotService client must use ZLinkHttpClient" >&2
  exit 1
fi
if rg -n 'runMode\(|/scenario/|class ClientScenario' \
    "$(pwd)/Client/src/main/java" "$(pwd)/Shared/src/main/java" --glob '*.java'; then
  echo "SpotService scenarios must run in Client scenario files" >&2
  exit 1
fi
if rg -n '@EnableZLinkFramework|\bZLink(SpotOutbound|RouteClient|ActorClient|SpotManager)\b' \
    "$(pwd)/Client/src/main/java" --glob '*.java'; then
  echo "SpotService Client must not host or call the framework runtime directly" >&2
  exit 1
fi
if rg -n 'receivedCount\([^)]*\)\s*==\s*0' \
    "$(pwd)/Client/src/main/java" --glob '*.java'; then
  echo "SpotService negative push assertions must use expectNone" >&2
  exit 1
fi

retry_child=0
all_child=0
for arg in "$@"; do
  case "${arg}" in
    --retry-child) retry_child=1 ;;
    --all-child) all_child=1 ;;
  esac
done

if [[ "${SCENARIO}" != "all" && "${retry_child}" != "1" && "${all_child}" != "1" ]]; then
  output="$(mktemp)"
  scenario_passed=0
  for attempt in 1 2 3; do
    : >"${output}"
    set +e
    timeout 900s "${SCRIPT_PATH}" "${SCENARIO}" --retry-child \
      --start-order "${e2e_start_order}" 2>&1 | tee "${output}"
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

if [[ "${SCENARIO}" == "all" && "${all_child}" != "1" ]]; then
  for child_group in default-batch SM-F6 SM-G2 SM-G3 SM-G4 SM-G1; do
    echo "child scenario=${child_group}"
    output="$(mktemp)"
    child_passed=0
    for attempt in 1 2 3; do
      : >"${output}"
      set +e
      timeout 900s "${SCRIPT_PATH}" "${child_group}" --all-child \
        --start-order "${e2e_start_order}" 2>&1 | tee "${output}"
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
  echo "spot-service e2e result=passed"
  exit 0
fi

config_dir="$(mktemp -d)"
chmod 700 "${config_dir}"
zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-java-e2e" "redis:7.2-alpine"
redis_location_endpoint="127.0.0.1:${redis_port}"
if [[ "${ZLINK_E2E_REDIS_MONITOR:-0}" == "1" ]]; then
  docker exec "${REDIS_CONTAINER}" redis-cli --csv monitor \
    >"${log_dir}/redis-monitor.log" 2>&1 &
  pids+=("$!")
fi
location_key_prefix="zlink:e2e:spot-service:${run_id}"
location_heartbeat_ms=500
location_lease_ttl_ms=5000
LOCATION_LEASE_POLL_MILLISECONDS=100
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
TOPOLOGY_READINESS_ATTEMPTS=100
SCENARIO_SETTLE_SECONDS=3

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
    docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
  fi
  rm -rf "${config_dir}"
  wait >/dev/null 2>&1 || true
  exit "${status}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ "${LOCAL_READINESS_TIMEOUT_SECONDS}" != 3 \
   || "${LOCAL_READINESS_ATTEMPTS}" != 30 \
   || "${SCENARIO_SETTLE_SECONDS:-}" != 3 ]]; then
  echo "SpotService must use 3s readiness and 3s scenario limits" >&2
  exit 1
fi

reserve_ports() {
  python3 - <<'PY'
import socket
sockets = []
ports = []
try:
    for _ in range(21):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
    print(" ".join(f"tcp://127.0.0.1:{port}" for port in ports[:16]), end=" ")
    print(" ".join(f"http://127.0.0.1:{port}" for port in ports[16:]))
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
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}

gradle_run() {
  ../../gradlew -PzlinkE2eBuildDir="${e2e_build_dir}" \
    --project-cache-dir "${gradle_cache_dir}" --no-daemon --no-parallel --max-workers=1 "$@" --quiet
}

client_bin() {
  echo "${e2e_build_dir}/Client/install/spot-service-client/bin/spot-service-client"
}

play_bin() {
  echo "${e2e_build_dir}/Server-Play/install/spot-service-play/bin/spot-service-play"
}

publisher_bin() {
  echo "${e2e_build_dir}/Server-Publisher/install/spot-service-publisher/bin/spot-service-publisher"
}

gateway_bin() {
  echo "${e2e_build_dir}/Server-Gateway/install/spot-service-gateway/bin/spot-service-gateway"
}

multi_node_bin() {
  echo "${e2e_build_dir}/Server-MultiNode/install/spot-service-multi-node/bin/spot-service-multi-node"
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
  local config_path="${config_dir}/${rid}-$(date +%s%N).properties"
  {
    echo "e2e.node-rid=${rid}"; echo "e2e.route-endpoint=${route}"
    echo "e2e.route-a-endpoint=${ROUTE_A}"; echo "e2e.route-b-endpoint=${ROUTE_B}"
    echo "e2e.ingress-endpoint=${ingress}"; echo "e2e.ingress-a-endpoint=${INGRESS_A}"
    echo "e2e.ingress-b-endpoint=${INGRESS_B}"; echo "e2e.spot-endpoint=${spot}"
    echo "e2e.spot-pub-endpoint=${spot_pub}"; echo "e2e.stream-endpoint=${stream}"
    echo "e2e.tls-stream-endpoint=${tls_stream}"
    echo "e2e.tls-certificate-path=${TLS_CERTIFICATE_PATH:-}"
    echo "e2e.tls-key-path=${TLS_KEY_PATH:-}"; echo "e2e.http-endpoint=${http}"
    echo "e2e.redis-location-endpoint=${redis_location_endpoint}"
    echo "e2e.location-key-prefix=${location_key_prefix}"
    echo "e2e.location-heartbeat-millis=${location_heartbeat_ms}"
    echo "e2e.location-lease-ttl-millis=${location_lease_ttl_ms}"
    echo "e2e.log-dir=${log_dir}"
  } >"${config_path}"
  chmod 600 "${config_path}"
  "$(play_bin)" --config "${config_path}" >"${log_dir}/${rid}.stdout.log" 2>"${log_dir}/${rid}.stderr.log" &
  pids+=("$!")
}

start_gateway() {
  local config_path="${config_dir}/gateway-$(date +%s%N).properties"
  {
    echo "e2e.gateway-rid=client-route-mesh"; echo "e2e.gateway-http-endpoint=${HTTP_GATEWAY}"
    echo "e2e.route-endpoint=${ROUTE_CLIENT}"; echo "e2e.route-a-endpoint=${ROUTE_A}"
    echo "e2e.route-b-endpoint=${ROUTE_B}"; echo "e2e.ingress-a-endpoint=${INGRESS_A}"
    echo "e2e.spot-endpoint=${SPOT_CLIENT}"; echo "e2e.spot-only=${SPOT_ONLY_MODE}"
    echo "e2e.redis-location-endpoint=${redis_location_endpoint}"
    echo "e2e.location-key-prefix=${location_key_prefix}"; echo "e2e.log-dir=${log_dir}"
  } >"${config_path}"
  chmod 600 "${config_path}"
  "$(gateway_bin)" --config "${config_path}" >"${log_dir}/gateway.stdout.log" 2>"${log_dir}/gateway.stderr.log" &
  pids+=("$!")
}

start_multi_node() {
  local rid="$1"
  local route="$2"
  local spot="$3"
  local http="$4"
  local config_path="${config_dir}/${rid}-$(date +%s%N).properties"
  {
    echo "e2e.node-rid=${rid}"; echo "e2e.route-endpoint=${route}"
    echo "e2e.route-a-endpoint=${ROUTE_A}"; echo "e2e.route-b-endpoint=${ROUTE_B}"
    echo "e2e.spot-endpoint=${spot}"; echo "e2e.http-endpoint=${http}"
    echo "e2e.spot-only=${SPOT_ONLY_MODE}"; echo "e2e.redis-location-endpoint=${redis_location_endpoint}"
    echo "e2e.location-key-prefix=${location_key_prefix}"; echo "e2e.log-dir=${log_dir}"
  } >"${config_path}"
  chmod 600 "${config_path}"
  "$(multi_node_bin)" --config "${config_path}" >"${log_dir}/${rid}.stdout.log" 2>"${log_dir}/${rid}.stderr.log" &
  pids+=("$!")
}

start_named_server() {
  case "$1" in
    play-a) start_play play-a "${ROUTE_A}" "${SPOT_A}" "${INGRESS_A}" "${HTTP_A}" "${SPOT_PUB_A}" "${STREAM_A}" "${TLS_STREAM_A}" ;;
    play-b) start_play play-b "${ROUTE_B}" "${SPOT_B}" "${INGRESS_B}" "${HTTP_B}" "${SPOT_PUB_B}" "${STREAM_B}" "" ;;
    gateway) start_gateway ;;
    multi-node-a) start_multi_node multi-node-a "${ROUTE_A}" "${MULTI_SPOT_A}" "${MULTI_HTTP_A}" ;;
    multi-node-b) start_multi_node multi-node-b "${ROUTE_B}" "${MULTI_SPOT_B}" "${MULTI_HTTP_B}" ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
  role_pids["$1"]="${pids[$((${#pids[@]} - 1))]}"
}

crash_named_server() {
  local role="$1"
  local pid="${role_pids[$role]:-}"
  if [[ -z "${pid}" ]]; then
    echo "No pid recorded for role '${role}'" >&2
    return 1
  fi
  for child in $(descendants "${pid}"); do
    kill -9 "${child}" >/dev/null 2>&1 || true
  done
  kill -9 "${pid}" >/dev/null 2>&1 || true
  wait "${pid}" >/dev/null 2>&1 || true
  unset "role_pids[$role]"
}

wait_named_server() {
  case "$1" in
    play-a)
      wait_port play-a-route "${ROUTE_A}"
      wait_port play-a-stream "${STREAM_A}"
      if [[ -n "${TLS_STREAM_A}" ]]; then
        wait_port play-a-tls-stream "${TLS_STREAM_A}"
      fi
      wait_port play-a-http "${HTTP_A}"
      ;;
    play-b)
      wait_port play-b-route "${ROUTE_B}"
      wait_port play-b-stream "${STREAM_B}"
      wait_port play-b-http "${HTTP_B}"
      ;;
    gateway)
      if [[ "${SPOT_ONLY_MODE}" != "true" ]]; then
        wait_port gateway-route "${ROUTE_CLIENT}"
      else
        wait_port gateway-spot "${SPOT_CLIENT}"
      fi
      wait_port gateway-http "${HTTP_GATEWAY}"
      ;;
    multi-node-a)
      if [[ "${SPOT_ONLY_MODE}" != "true" ]]; then
        wait_port multi-node-a-route "${ROUTE_A}"
      else
        wait_port multi-node-a-spot "${MULTI_SPOT_A}"
      fi
      wait_port multi-node-a-http "${MULTI_HTTP_A}"
      ;;
    multi-node-b)
      if [[ "${SPOT_ONLY_MODE}" != "true" ]]; then
        wait_port multi-node-b-route "${ROUTE_B}"
      else
        wait_port multi-node-b-spot "${MULTI_SPOT_B}"
      fi
      wait_port multi-node-b-http "${MULTI_HTTP_B}"
      ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

role_http_endpoint() {
  case "$1" in
    play-a) echo "${HTTP_A}" ;;
    play-b) echo "${HTTP_B}" ;;
    gateway) echo "${HTTP_GATEWAY}" ;;
    multi-node-a) echo "${MULTI_HTTP_A}" ;;
    multi-node-b) echo "${MULTI_HTTP_B}" ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

wait_topology_ready() {
  local role="$1"
  local expected="$2"
  local endpoint
  endpoint="$(role_http_endpoint "${role}")"
  for _ in $(seq 1 "${TOPOLOGY_READINESS_ATTEMPTS}"); do
    if curl -fsS "${endpoint}/topology/ready?expected=${expected}" >/dev/null 2>&1; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${role} RouteMesh topology expected=${expected}" >&2
  return 1
}

wait_location_ready() {
  local role="$1"
  local endpoint
  endpoint="$(role_http_endpoint "${role}")"
  for _ in $(seq 1 "${TOPOLOGY_READINESS_ATTEMPTS}"); do
    if curl -fsS "${endpoint}/location/ready" >/dev/null 2>&1; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${role} Location descriptors expected=${expected}" >&2
  return 1
}

spot_owner() {
  local endpoint="$1"
  local spot_rid="$2"
  curl -fsS "${endpoint}/location/spot-owner?spotRid=${spot_rid}" | tr -d '\r\n'
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
    print(f"placement endpoint={sys.argv[1]} weight={body['weight']}")
PY
}

prepare_default_spot_fixtures() {
  local room_a_response
  local room_b_response
  set_placement_weight "${HTTP_A}" 100
  set_placement_weight "${HTTP_B}" 0
  room_a_response="$(python3 - "${HTTP_A}/spot/create" room-a play-a <<'PY'
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
  room_b_response="$(python3 - "${HTTP_B}/spot/create" room-b play-b <<'PY'
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

wait_owner_lease_expired() {
  local owner_id="$1"
  local wait_budget_ms=$((location_lease_ttl_ms + location_heartbeat_ms))
  local attempts=$(((wait_budget_ms + LOCATION_LEASE_POLL_MILLISECONDS - 1) / LOCATION_LEASE_POLL_MILLISECONDS))
  local lease_key="${location_key_prefix}:lease:${owner_id}"
  local remaining_ms
  for _ in $(seq 1 "${attempts}"); do
    remaining_ms="$(docker exec "${REDIS_CONTAINER}" redis-cli --raw PTTL "${lease_key}")"
    if (( remaining_ms < 0 )); then
      echo "owner lease expired owner=${owner_id} evidence=redis-pttl"
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for owner lease expiry owner=${owner_id} budget_ms=${wait_budget_ms} remaining_ms=${remaining_ms}" >&2
  return 1
}

run_publisher() {
  local config_path="${config_dir}/publisher-$(date +%s%N).properties"
  {
    echo "e2e.spot-publisher-endpoint=${SPOT_PUBLISHER}"
    echo "e2e.redis-location-endpoint=${redis_location_endpoint}"
    echo "e2e.location-key-prefix=${location_key_prefix}"; echo "e2e.log-dir=${log_dir}"
  } >"${config_path}"
  chmod 600 "${config_path}"
  timeout -k 5s 30s "$(publisher_bin)" --config "${config_path}" >"${log_dir}/publisher.stdout.log" 2>"${log_dir}/publisher.stderr.log"
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

fetch_default_evidence() {
  fetch_evidence play-a "${HTTP_A}"
  fetch_evidence play-b "${HTTP_B}"
}

fetch_started_standard_evidence() {
  if [[ -n "${role_pids[play-a]:-}" ]]; then
    fetch_evidence play-a "${HTTP_A}"
  fi
  if [[ -n "${role_pids[play-b]:-}" ]]; then
    fetch_evidence play-b "${HTTP_B}"
  fi
}

assert_evidence_contains() {
  local pattern="$1"
  shift
  grep -q "${pattern}" "$@"
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

read -r ROUTE_A ROUTE_B ROUTE_CLIENT SPOT_A SPOT_B SPOT_CLIENT INGRESS_A INGRESS_B SPOT_PUB_A SPOT_PUB_B SPOT_PUBLISHER STREAM_A STREAM_B TLS_STREAM_A_RAW MULTI_SPOT_A MULTI_SPOT_B HTTP_A HTTP_B HTTP_GATEWAY MULTI_HTTP_A MULTI_HTTP_B <<<"$(reserve_ports)"
TLS_STREAM_A=""
if [[ "${SCENARIO}" == "SM-D14" || "${SCENARIO}" == "sm-d14" ]]; then
  TLS_STREAM_A="${TLS_STREAM_A_RAW/tcp:/tls:}"
  TLS_CERTIFICATE_PATH="${log_dir}/sm-d14-cert.pem"
  TLS_KEY_PATH="${log_dir}/sm-d14-key.pem"
  generate_tls_cert "${TLS_CERTIFICATE_PATH}" "${TLS_KEY_PATH}"
fi
SPOT_ONLY_MODE="false"
if [[ "${SCENARIO}" == "SM-F6" || "${SCENARIO}" == "sm-f6" ]]; then
  SPOT_ONLY_MODE="true"
fi

gradle_run installDist

if [[ "${SCENARIO}" == "SM-E1" || "${SCENARIO}" == "sm-e1" ]]; then
  SERVER_ROLES=(play-a gateway)
elif [[ "${SPOT_ONLY_MODE}" == "true" ]]; then
  SERVER_ROLES=(multi-node-a multi-node-b gateway)
else
  SERVER_ROLES=(play-a play-b gateway)
fi
mapfile -t ORDERED_SERVER_ROLES < <(zlink_e2e_order_roles "${SERVER_ROLES[@]}")
for role in "${ORDERED_SERVER_ROLES[@]}"; do
  start_named_server "$role"
  wait_named_server "$role"
done
expected_peers=$((${#SERVER_ROLES[@]} - 1))
for role in "${SERVER_ROLES[@]}"; do
  wait_topology_ready "${role}" "${expected_peers}"
done
for role in "${SERVER_ROLES[@]}"; do
  if [[ "${role}" != "gateway" ]]; then
    wait_location_ready "${role}"
  fi
done
if [[ "${SPOT_ONLY_MODE}" != "true" && "${SCENARIO}" != "SM-E1" && "${SCENARIO}" != "sm-e1" ]]; then
  prepare_default_spot_fixtures
fi
if [[ "${SPOT_ONLY_MODE}" == "true" ]]; then
  for role in gateway multi-node-a multi-node-b; do
    if ! grep -Fq "[topology] role=${role} route_mesh=disabled" "${log_dir}/${role}.stdout.log"; then
      echo "SM-F6 ${role} still registers RouteMesh" >&2
      exit 1
    fi
  done
fi

write_client_config() {
  local suffix="$1"
  LAST_CLIENT_CONFIG="${config_dir}/client-${suffix}-$(date +%s%N).properties"
  {
    echo "gatewayHttpEndpoint=${HTTP_GATEWAY}"; echo "streamAEndpoint=${STREAM_A}"
    echo "streamBEndpoint=${STREAM_B}"; echo "tlsStreamAEndpoint=${TLS_STREAM_A}"
    echo "httpAEndpoint=${HTTP_A}"; echo "httpBEndpoint=${HTTP_B}"
    echo "multiAHttpEndpoint=${MULTI_HTTP_A}"; echo "multiBHttpEndpoint=${MULTI_HTTP_B}"
    echo "readyFile=${log_dir}/sm-g1-ready"; echo "crashedFile=${log_dir}/sm-g1-crashed"
    echo "failedFile=${log_dir}/sm-g1-failed"; echo "restartedFile=${log_dir}/sm-g1-restarted"
    echo "secondCrashReadyFile=${log_dir}/sm-g1-second-crash-ready"
    echo "secondCrashedFile=${log_dir}/sm-g1-second-crashed"
  } >"${LAST_CLIENT_CONFIG}"
  chmod 600 "${LAST_CLIENT_CONFIG}"
}

run_client_mode() {
  local mode="$1"
  local timeout_seconds=90
  local attempt
  local client_scenario="${mode}"
  local status
  if [[ "${SCENARIO}" == SM-* ]]; then
    client_scenario="${SCENARIO}"
  fi
  write_client_config "${mode}"
  for attempt in $(seq 1 5); do
    set +e
      timeout -k 5s "${timeout_seconds}s" "$(client_bin)" \
        --config "${LAST_CLIENT_CONFIG}" --scenario "${client_scenario}" \
        >"${log_dir}/client-${mode}.stdout.log" 2>"${log_dir}/client-${mode}.stderr.log"
    status="$?"
    set -e
    if [[ "${status}" == "0" ]] && grep -q "spot-service e2e mode=${client_scenario} result=passed" "${log_dir}/client-${mode}.stdout.log"; then
      cat "${log_dir}/client-${mode}.stdout.log" >>"${log_dir}/client.stdout.log"
      cat "${log_dir}/client-${mode}.stderr.log" >>"${log_dir}/client.stderr.log"
      return 0
    fi
    sleep 1
  done
  return 1
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
  echo "Timed out waiting for ${name} at ${path}" >&2
  return 1
}

run_sm_g1() {
  local ready_file="${log_dir}/sm-g1-ready"
  local crashed_file="${log_dir}/sm-g1-crashed"
  local failed_file="${log_dir}/sm-g1-failed"
  local restarted_file="${log_dir}/sm-g1-restarted"
  local second_crash_ready_file="${log_dir}/sm-g1-second-crash-ready"
  local second_crashed_file="${log_dir}/sm-g1-second-crashed"
  local client_pid
  local play_a_owner

  play_a_owner="$(spot_owner "${HTTP_B}" room-a)"
  write_client_config play-crash-recovery
  timeout -k 5s 180s "$(client_bin)" --config "${LAST_CLIENT_CONFIG}" --scenario SM-G1 \
    >"${log_dir}/client-play-crash-recovery.stdout.log" 2>"${log_dir}/client-play-crash-recovery.stderr.log" &
  client_pid="$!"
  pids+=("${client_pid}")

  wait_file "SM-G1 ready signal" "${ready_file}"
  crash_named_server play-a
  touch "${crashed_file}"
  wait_file "SM-G1 bounded failure signal" "${failed_file}"
  wait_owner_lease_expired "${play_a_owner}"
  start_named_server play-a
  wait_named_server play-a
  wait_topology_ready play-a 2
  touch "${restarted_file}"
  wait_file "SM-G1 second crash ready signal" "${second_crash_ready_file}"
  crash_named_server play-a
  touch "${second_crashed_file}"

  wait "${client_pid}"
  grep -q "spot-service e2e mode=SM-G1 result=passed" "${log_dir}/client-play-crash-recovery.stdout.log"
  cat "${log_dir}/client-play-crash-recovery.stdout.log" >>"${log_dir}/client.stdout.log"
  cat "${log_dir}/client-play-crash-recovery.stderr.log" >>"${log_dir}/client.stderr.log"
}

scenario_modes() {
  case "$1" in
    all|default-batch)
      echo "state1 state2 send normal worker missing timeout owner spot-outbound spot-to-spot spot-mesh-cross-node route-mesh actor-session actor-missing actor-destroy actor-join-admission actor-push-chain actor-leave-disconnect actor-disconnect-notify idle-timer timer-overrun"
      ;;
    SM-A1) echo "state1" ;;
    SM-A2) echo "state2" ;;
    SM-A3|SM-A4) echo "owner" ;;
    SM-A5) echo "stage-wrapper" ;;
    SM-A6) echo "owner" ;;
    SM-A7) echo "owner" ;;
    SM-A8) echo "worker" ;;
    SM-B1|SM-B3|SM-B7|SM-D1|SM-D3|SM-D9) echo "actor-session" ;;
    SM-B2|SM-B4|SM-D2) echo "remote-actor-session" ;;
    SM-B5) echo "actor-missing" ;;
    SM-B6) echo "actor-leave-disconnect" ;;
    SM-B8) echo "actor-destroy" ;;
    SM-B9) echo "actor-join-admission" ;;
    SM-C1) echo "normal" ;;
    SM-C2) echo "spot-outbound" ;;
    SM-C3) echo "spot-to-spot" ;;
    SM-C4) echo "spot-mesh-cross-node" ;;
    SM-C5) echo "spot-mesh-cross-node" ;;
    SM-D5) echo "actor-disconnect-notify" ;;
    SM-D6) echo "bound-push-isolation" ;;
    SM-D7) echo "stream-auth" ;;
    SM-D8) echo "stream-reconnect" ;;
    SM-D4) echo "multi-actor-bind" ;;
    SM-D10) echo "stream-backpressure" ;;
    SM-D11) echo "mixed-stream-channel" ;;
    SM-D12) echo "stream-rebind-transfer" ;;
    SM-D13) echo "stream-heartbeat" ;;
    SM-D14) echo "stream-tls" ;;
    SM-D15) echo "actor-push-chain" ;;
    SM-E1) echo "missing" ;;
    SM-E2) echo "state1" ;;
    SM-E3) echo "idle-timer" ;;
    SM-E4) echo "timer-overrun" ;;
    SM-G2) echo "owner-remap" ;;
    SM-G3) echo "join-leave-race" ;;
    SM-G4) echo "bound-push-load" ;;
    SM-F1|SM-F2|SM-F3|SM-F4) echo "route-mesh" ;;
    SM-F5) echo "route-lifecycle" ;;
    SM-F6) echo "spot-only-mesh" ;;
    *)
      echo "SpotService Java scenario $1 is not mapped to an implemented client mode" >&2
      return 1
      ;;
  esac
}

: >"${log_dir}/client.stdout.log"
: >"${log_dir}/client.stderr.log"
if [[ "${SCENARIO}" == "SM-G1" || "${SCENARIO}" == "sm-g1" ]]; then
  run_sm_g1
  cat "${log_dir}/client.stdout.log"
  fetch_evidence play-b "${HTTP_B}"
  grep -q "sm-g1-second-crash-survivor" "${log_dir}/play-b-evidence.json"
  exit 0
fi
client_modes="$(scenario_modes "${SCENARIO}")"
for mode in ${client_modes}; do
  if [[ "${mode}" == "idle-timer" ]]; then
    create_timer_spot "${HTTP_A}" idle-close
    create_timer_spot "${HTTP_A}" idle-active
    sleep "${SCENARIO_SETTLE_SECONDS}"
  fi
  if [[ "${mode}" == "timer-overrun" ]]; then
    create_timer_spot "${HTTP_A}" timer-overrun-skip
    create_timer_spot "${HTTP_A}" timer-overrun-catchup
    create_timer_spot "${HTTP_A}" timer-overrun-delay
    sleep "${SCENARIO_SETTLE_SECONDS}"
  fi
  run_client_mode "${mode}"
  if [[ "${mode}" == "idle-timer" ]]; then
    close_spot "${HTTP_A}" idle-active
  fi
  sleep "${SCENARIO_SETTLE_SECONDS}"
done
if [[ "${SCENARIO}" != "all" && "${SCENARIO}" != "default-batch" ]]; then
  case "${SCENARIO}" in
    SM-C4|sm-c4)
      run_publisher
      cat "${log_dir}/publisher.stdout.log" >>"${log_dir}/client.stdout.log"
      cat "${log_dir}/publisher.stderr.log" >>"${log_dir}/client.stderr.log"
      ;;
    SM-A7|sm-a7)
      assert_type_mismatch "${HTTP_A}" room-a
      echo "scenario SM-A7 passed" >>"${log_dir}/client.stdout.log"
      ;;
    SM-E2|sm-e2)
      sleep "${SCENARIO_SETTLE_SECONDS}"
      echo "scenario SM-E2 passed" >>"${log_dir}/client.stdout.log"
      ;;
    SM-A6|sm-a6)
      close_spot "${HTTP_B}" room-b
      echo "scenario SM-A6 passed" >>"${log_dir}/client.stdout.log"
      ;;
  esac
  cat "${log_dir}/client.stdout.log"
  if [[ "${SPOT_ONLY_MODE}" == "true" ]]; then
    fetch_evidence multi-node-a "${MULTI_HTTP_A}"
    fetch_evidence multi-node-b "${MULTI_HTTP_B}"
  else
    fetch_started_standard_evidence
  fi
  case "${SCENARIO}" in
    SM-C4|sm-c4)
      assert_evidence_contains "SpotMeshMsg" "${log_dir}/play-a-evidence.json" "${log_dir}/play-b-evidence.json"
      ;;
    SM-A7|sm-a7)
      assert_evidence_contains "SpotTypeMismatch" "${log_dir}/play-a-evidence.json"
      assert_evidence_contains "SpotTypeMismatchStateOk" "${log_dir}/play-a-evidence.json"
      ;;
    SM-E2|sm-e2)
      assert_evidence_contains "SpotTimer" "${log_dir}/play-a-evidence.json"
      ;;
    SM-A6|sm-a6)
      assert_evidence_contains "SpotClosing" "${log_dir}/play-b-evidence.json"
      ;;
  esac
  exit 0
fi
run_publisher
cat "${log_dir}/publisher.stdout.log" >>"${log_dir}/client.stdout.log"
cat "${log_dir}/publisher.stderr.log" >>"${log_dir}/client.stderr.log"
sleep "${SCENARIO_SETTLE_SECONDS}"
assert_type_mismatch "${HTTP_A}" room-a
echo "scenario SM-A7 passed" >>"${log_dir}/client.stdout.log"
echo "scenario SM-E2 passed" >>"${log_dir}/client.stdout.log"
close_spot "${HTTP_B}" room-b
echo "scenario SM-A6 passed" >>"${log_dir}/client.stdout.log"

cat "${log_dir}/client.stdout.log"
fetch_default_evidence
grep -Rq "message flow" "${log_dir}"/*-flow.log
grep -q "packet=RouteReq" "${log_dir}/gateway-flow.log"
grep -q '"marker":"RouteReq"' "${log_dir}/play-a-evidence.json"
grep -q '"value":"route-mesh-normal"' "${log_dir}/play-a-evidence.json"
grep -q '"marker":"ActorCreated"' "${log_dir}/play-a-evidence.json"
grep -q '"marker":"ActorCreatedPayload"' "${log_dir}/play-a-evidence.json"
grep -q '"marker":"ActorDestroyed"' "${log_dir}/play-a-evidence.json"
grep -q '"value":"Player One/7/alpha,beta"' "${log_dir}/play-a-evidence.json"
grep -q '"marker":"ActorUserJoined"' "${log_dir}/play-a-evidence.json"
grep -q '"marker":"ActorUserLeft"' "${log_dir}/play-a-evidence.json"
grep -q '"marker":"ActorUserDisconnected"' "${log_dir}/play-a-evidence.json"
grep -q '"marker":"ActorUserReq"' "${log_dir}/play-a-evidence.json"
grep -q 'ActorCreated.*ActorUserJoined.*ActorUserReq' "${log_dir}/play-a-evidence.json"
grep -q 'user-echo-1.*user-echo-2.*user-echo-3' "${log_dir}/play-a-evidence.json"
if grep -q '"marker":"ActorCreated"' "${log_dir}/play-b-evidence.json"; then
  echo "unexpected play-b actor creation evidence" >&2
  exit 1
fi
grep -q '"marker":"StreamInbound"' "${log_dir}/play-a-evidence.json"
grep -q "DispatchError" "${log_dir}/play-a-evidence.json"
grep -q "MissingActorReq" "${log_dir}/play-a-evidence.json"
grep -q "SpotInitialized" "${log_dir}/play-a-evidence.json"
grep -q "SpotClosing" "${log_dir}/play-a-evidence.json" "${log_dir}/play-b-evidence.json"
grep -q "SpotTypeMismatch" "${log_dir}/play-a-evidence.json"
grep -q "SpotTypeMismatchStateOk" "${log_dir}/play-a-evidence.json"
grep -q "SpotTimer" "${log_dir}/play-a-evidence.json"
grep -q "WorkerStarted" "${log_dir}/play-a-evidence.json"
grep -q "WorkerFollowUpBeforeComplete" "${log_dir}/play-a-evidence.json"
grep -q "WorkerCompleted" "${log_dir}/play-a-evidence.json"
grep -q "IngressReq" "${log_dir}/play-a-evidence.json" "${log_dir}/play-b-evidence.json"
grep -q "IngressMsg" "${log_dir}/play-a-evidence.json" "${log_dir}/play-b-evidence.json"
grep -q "SpotOutbound" "${log_dir}/play-a-evidence.json"
grep -q "SpotToSpotSend" "${log_dir}/play-b-evidence.json"
grep -q "SpotMeshMsg" "${log_dir}/play-a-evidence.json"
grep -q "SpotMeshMsg" "${log_dir}/play-b-evidence.json"
grep -q "IdleCloseRequested" "${log_dir}/play-a-evidence.json"
grep -q "IdleClosed" "${log_dir}/play-a-evidence.json"
grep -q "IdleKeptOpen" "${log_dir}/play-a-evidence.json"
grep -q "TimerOverrunConfigured" "${log_dir}/play-a-evidence.json"
grep -q "TimerOverrunTick" "${log_dir}/play-a-evidence.json"
echo "spot-service e2e result=passed"
