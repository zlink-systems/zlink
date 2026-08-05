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
chmod 0700 "${config_dir}"
SCENARIO="${1:-all}"
e2e_start_order="$(zlink_e2e_start_order_mode "$@")"
echo "start_order=${e2e_start_order}"

if [[ "${SCENARIO}" == "all" && "${ZLINK_RUNTIME_MONITORING_AGGREGATE_CHILD:-0}" != "1" ]]; then
  aggregate_status=0
  for scenario in MON-A1 MON-A2 MON-A3 MON-A4A MON-A4B MON-A5 MON-B1 MON-B2 MON-D1A MON-D1B; do
    echo "[runtime-monitoring] scenario=${scenario} start_order=${e2e_start_order}"
    if ! ZLINK_RUNTIME_MONITORING_AGGREGATE_CHILD=1 \
      "${BASH_SOURCE[0]}" "${scenario}" --start-order "${e2e_start_order}"; then
      aggregate_status=1
    fi
  done
  if [[ "${aggregate_status}" != "0" ]]; then
    exit "${aggregate_status}"
  fi
  echo "monitoring e2e result=passed"
  exit 0
fi

repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
readonly e2e_build_dir="${HOME}/.cache/zlink/java-e2e/RuntimeMonitoring"
readonly gradle_cache_dir="${HOME}/.cache/zlink/java-e2e/RuntimeMonitoring-gradle-cache"
redis_location_endpoint=""
location_key_prefix="zlink:e2e:runtime-monitoring:${run_id}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
if [[ "${LOCAL_READINESS_TIMEOUT_SECONDS}" != 3 \
   || "${LOCAL_READINESS_ATTEMPTS}" != 30 ]]; then
  echo "RuntimeMonitoring must use a 3s readiness limit" >&2
  exit 1
fi
if rg -n 'java\.net\.http\.HttpClient|HttpClient\.new' Client/src/main/java --glob '*.java'; then
  echo "RuntimeMonitoring client must use ZLinkHttpClient" >&2
  exit 1
fi
if rg -n 'runScenario\(|/scenario/|class TriggerScenario' \
    Client/src/main/java Server/Trigger/src/main/java --glob '*.java'; then
  echo "RuntimeMonitoring scenarios must run in Client files" >&2
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
    for _ in range(13):
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

wait_http_health() {
  local name="$1"
  local endpoint="$2"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if curl --max-time 1 -fsS "${endpoint}/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${name} health at ${endpoint}" >&2
  return 1
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

client_bin() {
  echo "${e2e_build_dir}/Client/install/runtime-monitoring-client/bin/runtime-monitoring-client"
}

service_bin() {
  echo "${e2e_build_dir}/Server-Service/install/runtime-monitoring-service/bin/runtime-monitoring-service"
}

filtered_service_bin() {
  echo "${e2e_build_dir}/Server-FilteredService/install/runtime-monitoring-filtered-service/bin/runtime-monitoring-filtered-service"
}

throwing_service_bin() {
  echo "${e2e_build_dir}/Server-ThrowingService/install/runtime-monitoring-throwing-service/bin/runtime-monitoring-throwing-service"
}

trigger_bin() {
  echo "${e2e_build_dir}/Server-Trigger/install/runtime-monitoring-trigger/bin/runtime-monitoring-trigger"
}

read -r API_PORT HANDSHAKE_PORT MESH_PORT MESH_B_PORT SVC_HTTP_PORT FILTER_API_PORT FILTER_HTTP_PORT THROW_API_PORT THROW_HTTP_PORT TRIGGER_HTTP_PORT _ _ _ <<<"$(reserve_ports)"
API_ENDPOINT="$(tcp "${API_PORT}")"
HANDSHAKE_ENDPOINT="$(tcp "${HANDSHAKE_PORT}")"
MESH_ENDPOINT="$(tcp "${MESH_PORT}")"
MESH_B_ENDPOINT="$(tcp "${MESH_B_PORT}")"
SERVICE_HTTP="$(http "${SVC_HTTP_PORT}")"
FILTER_API_ENDPOINT="$(tcp "${FILTER_API_PORT}")"
FILTER_HTTP="$(http "${FILTER_HTTP_PORT}")"
THROW_API_ENDPOINT="$(tcp "${THROW_API_PORT}")"
THROW_HTTP="$(http "${THROW_HTTP_PORT}")"
TRIGGER_HTTP="$(http "${TRIGGER_HTTP_PORT}")"

write_config() {
  local path="$1"
  shift
  {
    printf 'redisLocationEndpoint=%s\n' "${redis_location_endpoint}"
    printf 'locationKeyPrefix=%s\n' "${location_key_prefix}"
    printf 'logDirectory=%s\n' "${log_dir}"
    printf '%s\n' "$@"
  } >"${path}"
  chmod 0600 "${path}"
}
write_role_config() {
  local path="$1"
  shift
  {
    printf 'e2e.redis-location-endpoint=%s\n' "${redis_location_endpoint}"
    printf 'e2e.location-key-prefix=%s\n' "${location_key_prefix}"
    printf 'e2e.log-directory=%s\n' "${log_dir}"
    local property
    for property in "$@"; do printf 'e2e.%s\n' "${property}"; done
  } >"${path}"
  chmod 0600 "${path}"
}

service_config="${config_dir}/service.properties"
filtered_service_config="${config_dir}/filtered-service.properties"
throwing_service_config="${config_dir}/throwing-service.properties"
trigger_config="${config_dir}/trigger.properties"
client_config="${config_dir}/client.properties"

create_configs() {
  write_role_config "${service_config}" \
    "routing-id=svc-a" "api-endpoint=${API_ENDPOINT}" \
    "handshake-endpoint=${HANDSHAKE_ENDPOINT}" "mesh-endpoint=${MESH_ENDPOINT}" \
    "http-endpoint=${SERVICE_HTTP}" \
    "enable-handshake=true" "enable-spot=true"
  write_role_config "${filtered_service_config}" \
    "routing-id=svc-b" "api-endpoint=${FILTER_API_ENDPOINT}" \
    "mesh-endpoint=${MESH_B_ENDPOINT}" "mesh-peer-endpoint=${MESH_ENDPOINT}" \
    "http-endpoint=${FILTER_HTTP}" "enable-handshake=false" "enable-spot=true"
  write_role_config "${throwing_service_config}" \
    "routing-id=svc-throw" "api-endpoint=${THROW_API_ENDPOINT}" \
    "http-endpoint=${THROW_HTTP}" "enable-handshake=false" "enable-spot=false"
  write_role_config "${trigger_config}" \
    "api-endpoint=${API_ENDPOINT}" "service-b-api-endpoint=${FILTER_API_ENDPOINT}" \
    "trigger-http-endpoint=${TRIGGER_HTTP}"
  write_config "${client_config}" \
    "triggerHttpEndpoint=${TRIGGER_HTTP}" \
    "serviceHttpEndpoint=${SERVICE_HTTP}" \
    "serviceBHttpEndpoint=${FILTER_HTTP}" \
    "serviceBApiEndpoint=${FILTER_API_ENDPOINT}" \
    "handshakeEndpoint=${HANDSHAKE_ENDPOINT}" \
    "filteredServiceBinary=$(filtered_service_bin)" \
    "filteredServiceConfigPath=${filtered_service_config}" \
    "redisContainer=${REDIS_CONTAINER}"
}

start_initial_role() {
  case "$1" in
    service)
      "$(service_bin)" --config "${service_config}" >"${log_dir}/service.stdout.log" 2>"${log_dir}/service.stderr.log" &
      ;;
    filtered-service)
      "$(filtered_service_bin)" --config "${filtered_service_config}" >"${log_dir}/filtered-service.stdout.log" 2>"${log_dir}/filtered-service.stderr.log" &
      ;;
    throwing-service)
      "$(throwing_service_bin)" --config "${throwing_service_config}" >"${log_dir}/throwing-service.stdout.log" 2>"${log_dir}/throwing-service.stderr.log" &
      ;;
    trigger)
      "$(trigger_bin)" --config "${trigger_config}" >"${log_dir}/trigger.stdout.log" 2>"${log_dir}/trigger.stderr.log" &
      ;;
  esac
  pids+=("$!")
}

gradle_run installDist
start_redis_container
create_configs

mapfile -t ORDERED_SERVER_ROLES < <(zlink_e2e_order_roles service filtered-service throwing-service trigger)
for role in "${ORDERED_SERVER_ROLES[@]}"; do
  if [[ "${role}" == "filtered-service" ]]; then
    continue
  fi
  start_initial_role "${role}"
done

wait_port service-api "${API_ENDPOINT}"
wait_port service-http "${SERVICE_HTTP}"
wait_port throwing-service-api "${THROW_API_ENDPOINT}"
wait_port throwing-service-http "${THROW_HTTP}"
wait_port trigger-http "${TRIGGER_HTTP}"
wait_http_health service "${SERVICE_HTTP}"
wait_http_health throwing-service "${THROW_HTTP}"
wait_http_health trigger "${TRIGGER_HTTP}"

"$(client_bin)" --config "${client_config}" --scenario "${SCENARIO}" \
  >"${log_dir}/client.stdout.log" 2>"${log_dir}/client.stderr.log"

cat "${log_dir}/client.stdout.log"
if [[ "${SCENARIO}" == "all" ]]; then
  grep -q "scenario MON-A1 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario MON-A2 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario MON-A3 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario MON-A4A passed" "${log_dir}/client.stdout.log"
  grep -q "scenario MON-A4B passed" "${log_dir}/client.stdout.log"
  grep -q "scenario MON-A5 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario MON-D1A passed" "${log_dir}/client.stdout.log"
  grep -q "scenario MON-D1B passed" "${log_dir}/client.stdout.log"
else
  grep -q "scenario ${SCENARIO} passed" "${log_dir}/client.stdout.log"
fi
if compgen -G "${log_dir}/*-flow.log" >/dev/null; then
  grep -Rq "message flow" "${log_dir}"/*-flow.log
fi
