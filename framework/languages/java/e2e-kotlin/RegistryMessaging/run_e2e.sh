#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}"

RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="${ROOT_DIR}/logs/${RUN_ID}"
SCENARIO="${1:-all}"
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
HTTP_PROBE_TIMEOUT_SECONDS=3
mkdir -p "${LOG_DIR}"
echo "log_dir=${LOG_DIR}"

repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi

export ZLINK_KOTLIN_E2E_BUILD_DIR="${ZLINK_KOTLIN_E2E_BUILD_DIR:-${HOME}/.cache/zlink/kotlin-e2e/RegistryMessaging}"
export ZLINK_KOTLIN_E2E_GRADLE_CACHE="${ZLINK_KOTLIN_E2E_GRADLE_CACHE:-${HOME}/.cache/zlink/kotlin-e2e/RegistryMessaging-gradle-cache}"
ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT=""
export ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX:-zlink:e2e:kotlin-registry-messaging:${RUN_ID}}"

pids=()
REDIS_CONTAINER=""

print_logs() {
  local status="$1"
  if [[ "${status}" == "0" ]]; then
    return
  fi
  for log in "${LOG_DIR}"/*.log; do
    [[ -f "${log}" ]] || continue
    echo "===== ${log} =====" >&2
    tail -n 200 "${log}" >&2 || true
  done
}

run_all_scenarios() {
  local scenario
  for scenario in RM-A1 RM-A2 RM-A4 RM-A6 RM-B1 RM-B2 RM-C1 RM-C2 RM-C3 RM-C4 RM-C5 RM-C7 RM-C8 RM-C9; do
    echo "===== ${scenario} ====="
    "${ROOT_DIR}/run_e2e.sh" "${scenario}"
  done
  echo "registry-messaging kotlin e2e result=passed"
}

cleanup() {
  local status="$?"
  set +e
  print_logs "${status}"
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    kill "${pids[$i]}" >/dev/null 2>&1 || true
  done
  sleep 0.5
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    kill -9 "${pids[$i]}" >/dev/null 2>&1 || true
  done
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
  fi
  wait >/dev/null 2>&1 || true
  exit "${status}"
}
trap cleanup EXIT

if [[ "${SCENARIO}" == "all" ]]; then
  run_all_scenarios
  exit 0
fi

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

wait_health() {
  local url="$1"
  local name="$2"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS "${url}/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${name} at ${url}" >&2
  return 1
}

wait_location_peers() {
  local http_url="$1"
  local mesh_name="$2"
  local expected_count="$3"
  local timeout_seconds="${4:-5}"
  python3 - "${http_url}" "${mesh_name}" "${expected_count}" "${timeout_seconds}" <<'PY'
import json
import sys
import time
import urllib.error
import urllib.request

url, mesh_name, expected_count, timeout_text = sys.argv[1:]
expected_count = int(expected_count)
deadline = time.monotonic() + float(timeout_text)
last = None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(f"{url}/locations/peers", timeout=1) as response:
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
  local http_url="$1"
  local peer_rid="$2"
  local timeout_seconds="${3:-10}"
  python3 - "${http_url}" "${peer_rid}" "${timeout_seconds}" <<'PY'
import json
import sys
import time
import urllib.error
import urllib.request

url, peer_rid, timeout_text = sys.argv[1:]
deadline = time.monotonic() + float(timeout_text)
last = None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(f"{url}/route/status", timeout=1) as response:
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
    "zlink-redis-kotlin-e2e" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
  ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="127.0.0.1:${redis_port}"
  export ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT
}

gradle_run() {
  ../../gradlew --project-cache-dir "${ZLINK_KOTLIN_E2E_GRADLE_CACHE}" --no-daemon "$@" --quiet
}

bin_path() {
  local path="$1"
  local app="$2"
  echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/${path}/install/${app}/bin/${app}"
}

uses_common_roles() {
  case "$1" in
    all|RM-A1|RM-A2|RM-A6|RM-C1|RM-C2|RM-C3|RM-C4|RM-C5|RM-C8|RM-C9)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

CLIENT_BIN="$(bin_path Client registry-messaging-kotlin-client)"
CONSUMER_BIN="$(bin_path Server-Consumer registry-messaging-kotlin-consumer)"
PROVIDER_BIN="$(bin_path Server-Provider registry-messaging-kotlin-provider)"
WORKFLOW_BIN="$(bin_path Server-Workflow registry-messaging-kotlin-workflow)"

start_server() {
  local name="$1"
  local binary="$2"
  shift 2
  "${binary}" "$@" >"${LOG_DIR}/${name}.stdout.log" 2>"${LOG_DIR}/${name}.stderr.log" &
  pids+=("$!")
}

PROVIDER_A_HTTP_PORT="$(pick_port)"
PROVIDER_B_HTTP_PORT="$(pick_port)"
WORKFLOW_HTTP_PORT="$(pick_port)"
CONSUMER_HTTP_PORT="$(pick_port)"
SINGLE_CONSUMER_HTTP_PORT="$(pick_port)"
DISCOVERY_CONSUMER_HTTP_PORT="$(pick_port)"
BACKPRESSURE_CONSUMER_HTTP_PORT="$(pick_port)"
API_A_PORT="$(pick_port)"
API_B_PORT="$(pick_port)"
WORKFLOW_PORT="$(pick_port)"
ROUTE_A_PORT="$(pick_port)"
ROUTE_B_PORT="$(pick_port)"
CLIENT_ROUTE_PORT="$(pick_port)"

API_A="tcp://127.0.0.1:${API_A_PORT}"
API_B="tcp://127.0.0.1:${API_B_PORT}"
WORKFLOW="tcp://127.0.0.1:${WORKFLOW_PORT}"
ROUTE_A="tcp://127.0.0.1:${ROUTE_A_PORT}"
ROUTE_B="tcp://127.0.0.1:${ROUTE_B_PORT}"

gradle_run :Client:installDist :Server:Provider:installDist :Server:Consumer:installDist :Server:Workflow:installDist
start_redis_container

if uses_common_roles "${SCENARIO}"; then
  if [[ "${SCENARIO}" == "RM-A1" || "${SCENARIO}" == "RM-A2" ]]; then
    API_A_ROUTE_ARGS=()
    API_B_ROUTE_ARGS=()
  else
    API_A_ROUTE_ARGS=(--route-endpoint "${ROUTE_A}" --route-peer "${ROUTE_B}")
    API_B_ROUTE_ARGS=(--route-endpoint "${ROUTE_B}" --route-peer "${ROUTE_A}")
  fi
  start_server api-a "${PROVIDER_BIN}" \
    --rid api-a \
    --http-url "http://127.0.0.1:${PROVIDER_A_HTTP_PORT}" \
    --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --channel-endpoint "${API_A}" \
    "${API_A_ROUTE_ARGS[@]}" \
    --route-peer-rid api-b \
    --evidence-file "${LOG_DIR}/api-a.evidence.log" \
    --log-dir "${LOG_DIR}"
  wait_health "http://127.0.0.1:${PROVIDER_A_HTTP_PORT}" api-a

  start_server api-b "${PROVIDER_BIN}" \
    --rid api-b \
    --http-url "http://127.0.0.1:${PROVIDER_B_HTTP_PORT}" \
    --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --channel-endpoint "${API_B}" \
    "${API_B_ROUTE_ARGS[@]}" \
    --route-peer-rid api-a \
    --evidence-file "${LOG_DIR}/api-b.evidence.log" \
    --log-dir "${LOG_DIR}"
  wait_health "http://127.0.0.1:${PROVIDER_B_HTTP_PORT}" api-b

  start_server workflow-a "${WORKFLOW_BIN}" \
    --rid workflow-a \
    --http-url "http://127.0.0.1:${WORKFLOW_HTTP_PORT}" \
    --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --workflow-endpoint "${WORKFLOW}" \
    --evidence-file "${LOG_DIR}/workflow-a.evidence.log" \
    --log-dir "${LOG_DIR}"
  wait_health "http://127.0.0.1:${WORKFLOW_HTTP_PORT}" workflow-a

  start_server direct-consumer "${CONSUMER_BIN}" \
    --http-url "http://127.0.0.1:${CONSUMER_HTTP_PORT}" \
    --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --provider-endpoint "${API_A}" \
    --provider-endpoint "${API_B}" \
    --trace-label direct-consumer \
    --log-dir "${LOG_DIR}"
  wait_health "http://127.0.0.1:${CONSUMER_HTTP_PORT}" direct-consumer

  start_server single-consumer "${CONSUMER_BIN}" \
    --http-url "http://127.0.0.1:${SINGLE_CONSUMER_HTTP_PORT}" \
    --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --provider-endpoint "${API_A}" \
    --trace-label single-consumer \
    --log-dir "${LOG_DIR}"
  wait_health "http://127.0.0.1:${SINGLE_CONSUMER_HTTP_PORT}" single-consumer

  start_server discovery-consumer "${CONSUMER_BIN}" \
    --http-url "http://127.0.0.1:${DISCOVERY_CONSUMER_HTTP_PORT}" \
    --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --trace-label discovery-consumer \
    --log-dir "${LOG_DIR}"
  wait_health "http://127.0.0.1:${DISCOVERY_CONSUMER_HTTP_PORT}" discovery-consumer

  start_server backpressure-consumer "${CONSUMER_BIN}" \
    --http-url "http://127.0.0.1:${BACKPRESSURE_CONSUMER_HTTP_PORT}" \
    --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --provider-endpoint "${API_A}" \
    --trace-label backpressure-consumer \
    --log-dir "${LOG_DIR}"
  wait_health "http://127.0.0.1:${BACKPRESSURE_CONSUMER_HTTP_PORT}" backpressure-consumer

  if [[ "${SCENARIO}" != "RM-A1" && "${SCENARIO}" != "RM-A2" ]]; then
    wait_route_peer "http://127.0.0.1:${PROVIDER_A_HTTP_PORT}" api-b
  fi
  wait_location_peers "http://127.0.0.1:${DISCOVERY_CONSUMER_HTTP_PORT}" "profile" 2

fi

"${CLIENT_BIN}" \
  --provider-a-url "http://127.0.0.1:${PROVIDER_A_HTTP_PORT}" \
  --provider-b-url "http://127.0.0.1:${PROVIDER_B_HTTP_PORT}" \
  --workflow-url "http://127.0.0.1:${WORKFLOW_HTTP_PORT}" \
  --direct-consumer-url "http://127.0.0.1:${CONSUMER_HTTP_PORT}" \
  --single-consumer-url "http://127.0.0.1:${SINGLE_CONSUMER_HTTP_PORT}" \
  --discovery-consumer-url "http://127.0.0.1:${DISCOVERY_CONSUMER_HTTP_PORT}" \
  --backpressure-consumer-url "http://127.0.0.1:${BACKPRESSURE_CONSUMER_HTTP_PORT}" \
  --provider-bin "${PROVIDER_BIN}" \
  --consumer-bin "${CONSUMER_BIN}" \
  --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
  --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
  --log-dir "${LOG_DIR}" \
  --scenario "${SCENARIO}" \
  >"${LOG_DIR}/client.stdout.log" 2>"${LOG_DIR}/client.stderr.log"

cat "${LOG_DIR}/client.stdout.log"
