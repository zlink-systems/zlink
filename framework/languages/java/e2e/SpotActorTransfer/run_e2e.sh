#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAVA_DIR="$(cd "${ROOT_DIR}/../.." && pwd)"
source "${JAVA_DIR}/e2e-redis-common.sh"
source "${ROOT_DIR}/../start-order-common.sh"

SCENARIO="${1:-all}"
e2e_start_order="$(zlink_e2e_start_order_mode "$@")"
echo "start_order=${e2e_start_order}"
if rg -q "observedAtNanos" "${ROOT_DIR}/Client" "${ROOT_DIR}/Server" "${ROOT_DIR}/Shared"; then
  echo "SpotActorTransfer evidence must not compare process-local nanoTime values" >&2
  exit 1
fi
if rg -n 'java\.net\.http\.HttpClient|HttpClient\.new' "${ROOT_DIR}/Client/src/main/java" --glob '*.java'; then
  echo "SpotActorTransfer client must use ZLinkHttpClient" >&2
  exit 1
fi
if [[ "${SCENARIO}" == "all" ]]; then
  for scenario in ST-A1 ST-A2 ST-A3 ST-B1 ST-B2 ST-B3 ST-B4 ST-C1 ST-C2 ST-C3 ST-D1 ST-D2 ST-E1 ST-E2 ST-F1 ST-F2 ST-F3 ST-F4 ST-F5 ST-F6 ST-R1; do
    passed=0
    for attempt in 1 2 3; do
      if "${BASH_SOURCE[0]}" "${scenario}" --start-order "${e2e_start_order}"; then
        passed=1
        break
      fi
      log_root="${ZLINK_JAVA_E2E_LOG_ROOT:-${ROOT_DIR}/log}"
      latest_log="$(find "${log_root}" -mindepth 1 -maxdepth 1 -type d | sort | tail -1)"
      if [[ "${attempt}" == "3" ]] \
         || ! rg --no-ignore -q "ZlinkBindException|BindException|Address already in use|errno=98" "${latest_log}"; then
        exit 1
      fi
      echo "spot-actor-transfer transient bind failure; retrying ${scenario} (${attempt}/3)" >&2
    done
    [[ "${passed}" == "1" ]]
  done
  echo "spot-actor-transfer Java e2e all result=passed"
  exit 0
fi
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
PROJECT_ROOT="${ZLINK_JAVA_E2E_PROJECT_ROOT:-${ROOT_DIR}}"
LOG_ROOT="${ZLINK_JAVA_E2E_LOG_ROOT:-${ROOT_DIR}/log}"
LOG_DIR="${LOG_ROOT}/${RUN_ID}"
CONFIG_DIR="$(mktemp -d)"
chmod 0700 "${CONFIG_DIR}"
REDIS_CONTAINER=""
PIDS=()
PID_A=""
PID_B=""
PID_C=""
LOCAL_READINESS_TIMEOUT_SECONDS=10
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=100
mkdir -p "${LOG_DIR}"

cleanup() {
  local pid
  for pid in "${PIDS[@]:-}"; do
    kill "${pid}" >/dev/null 2>&1 || true
  done
  for pid in "${PIDS[@]:-}"; do
    wait "${pid}" >/dev/null 2>&1 || true
  done
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
  fi
  rm -rf "${CONFIG_DIR}"
}
trap cleanup EXIT INT TERM

if [[ "${LOCAL_READINESS_TIMEOUT_SECONDS:-}" != 10 \
   || "${LOCAL_READINESS_ATTEMPTS}" != 100 ]]; then
  echo "SpotActorTransfer must use a 3s readiness limit" >&2
  exit 1
fi

read -r ROUTER_A_PORT ROUTER_B_PORT ROUTER_C_PORT HTTP_A_PORT HTTP_B_PORT HTTP_C_PORT STREAM_A_PORT STREAM_B_PORT STREAM_C_PORT <<<"$(python3 - <<'PY'
import socket
sockets=[]
ports=[]
for _ in range(9):
    s=socket.socket()
    s.bind(('127.0.0.1', 0))
    sockets.append(s)
    ports.append(str(s.getsockname()[1]))
print(' '.join(ports))
for s in sockets:
    s.close()
PY
)"

zlink_redis_start_scoped_assign \
  REDIS_CONTAINER REDIS_PORT \
  "zlink-redis-java-e2e-spot-transfer" \
  "redis:7.2-alpine" \
  "127.0.0.1::6379"
REDIS_LOCATION_ENDPOINT="127.0.0.1:${REDIS_PORT}"
if [[ "${ZLINK_E2E_REDIS_MONITOR:-0}" == "1" ]]; then
  docker exec "${REDIS_CONTAINER}" redis-cli --csv monitor \
    >"${LOG_DIR}/redis-monitor.log" 2>&1 &
  PIDS+=("$!")
fi

LOCATION_PREFIX="zlink:e2e:java:spot-transfer:${RUN_ID}:"
NODE_BIN="${ZLINK_JAVA_E2E_NODE_BIN:-${ROOT_DIR}/Server/ActorNode/build/install/spot-actor-transfer-actor-node/bin/spot-actor-transfer-actor-node}"
CLIENT_BIN="${ZLINK_JAVA_E2E_CLIENT_BIN:-${ROOT_DIR}/Client/build/install/spot-actor-transfer-client/bin/spot-actor-transfer-client}"

"${JAVA_DIR}/gradlew" \
  -p "${PROJECT_ROOT}" \
  --no-daemon \
  installDist

start_node() {
  local rid="$1"
  local router_port="$2"
  local http_port="$3"
  local stream_port="$4"
  local config_path="${CONFIG_DIR}/${rid}.properties"
  cat >"${config_path}" <<EOF
e2e.node-rid=${rid}
e2e.mesh-endpoint=tcp://127.0.0.1:${router_port}
e2e.mesh-peers=actor-a=tcp://127.0.0.1:${ROUTER_A_PORT},actor-b=tcp://127.0.0.1:${ROUTER_B_PORT},actor-c=tcp://127.0.0.1:${ROUTER_C_PORT}
e2e.http-endpoint=http://127.0.0.1:${http_port}
e2e.stream-endpoint=tcp://127.0.0.1:${stream_port}
e2e.redis-location-endpoint=${REDIS_LOCATION_ENDPOINT}
e2e.location-key-prefix=${LOCATION_PREFIX}
e2e.log-directory=${LOG_DIR}
e2e.scenario=${SCENARIO}
e2e.automatic-topology=$([[ "${SCENARIO}" == "ST-R1" ]] && echo true || echo false)
EOF
  chmod 0600 "${config_path}"
  "${NODE_BIN}" --config "${config_path}" \
    >"${LOG_DIR}/${rid}.stdout.log" 2>"${LOG_DIR}/${rid}.stderr.log" &
  PIDS+=("$!")
  case "${rid}" in
    actor-a) PID_A="$!" ;;
    actor-b) PID_B="$!" ;;
    actor-c) PID_C="$!" ;;
  esac
}

wait_http() {
  local url="$1"
  local pid="$2"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if curl --silent --fail --max-time 1 "${url}/health" >/dev/null; then
      return 0
    fi
    if ! kill -0 "${pid}" >/dev/null 2>&1; then
      echo "Node process ${pid} exited before ${url} became healthy" >&2
      return 1
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${url}" >&2
  curl --silent --show-error --max-time 1 "${url}/health" >&2 || true
  echo >&2
  return 1
}

wait_log_contains() {
  local log_file="$1"
  local pattern="$2"
  local deadline=$((SECONDS + 10))
  while (( SECONDS < deadline )); do
    if [[ -f "${log_file}" ]] && rg -q "${pattern}" "${log_file}"; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for '${pattern}' in ${log_file}" >&2
  return 1
}

mapfile -t ORDERED_SERVER_ROLES < <(zlink_e2e_order_roles actor-a actor-b actor-c)
for role in "${ORDERED_SERVER_ROLES[@]}"; do
  case "${role}" in
    actor-a) start_node actor-a "${ROUTER_A_PORT}" "${HTTP_A_PORT}" "${STREAM_A_PORT}" ;;
    actor-b) start_node actor-b "${ROUTER_B_PORT}" "${HTTP_B_PORT}" "${STREAM_B_PORT}" ;;
    actor-c) start_node actor-c "${ROUTER_C_PORT}" "${HTTP_C_PORT}" "${STREAM_C_PORT}" ;;
  esac
done
wait_http "http://127.0.0.1:${HTTP_A_PORT}" "${PID_A}"
wait_http "http://127.0.0.1:${HTTP_B_PORT}" "${PID_B}"
wait_http "http://127.0.0.1:${HTTP_C_PORT}" "${PID_C}"

run_client() {
  local config_path="${CONFIG_DIR}/client.properties"
  cat >"${config_path}" <<EOF
nodeAHttpEndpoint=http://127.0.0.1:${HTTP_A_PORT}
nodeBHttpEndpoint=http://127.0.0.1:${HTTP_B_PORT}
nodeCHttpEndpoint=http://127.0.0.1:${HTTP_C_PORT}
logDirectory=${LOG_DIR}
streamAEndpoint=tcp://127.0.0.1:${STREAM_A_PORT}
streamBEndpoint=tcp://127.0.0.1:${STREAM_B_PORT}
streamCEndpoint=tcp://127.0.0.1:${STREAM_C_PORT}
EOF
  chmod 0600 "${config_path}"
  timeout -k 5s 240s \
    "${CLIENT_BIN}" --config "${config_path}" --scenario "${SCENARIO}" \
    >"${LOG_DIR}/client.stdout.log" 2>"${LOG_DIR}/client.stderr.log"
}

run_client_or_report() {
  if run_client; then
    return 0
  else
    local status="$?"
    echo "SpotActorTransfer client failed with status ${status}" >&2
    cat "${LOG_DIR}/client.stdout.log" >&2 || true
    cat "${LOG_DIR}/client.stderr.log" >&2 || true
    return "${status}"
  fi
}

if [[ "${SCENARIO}" == "ST-B2" || "${SCENARIO}" == "ST-C1" \
   || "${SCENARIO}" == "ST-C2" || "${SCENARIO}" == "ST-D2" ]]; then
  run_client_or_report &
  CLIENT_PID="$!"
  deadline=$((SECONDS + 30))
  while [[ ! -f "${LOG_DIR}/${SCENARIO}.ready" ]]; do
    if (( SECONDS >= deadline )); then
      echo "Timed out waiting for ${SCENARIO} source shutdown gate" >&2
      exit 1
    fi
    sleep 0.1
  done
  kill -KILL "${PID_A}" >/dev/null 2>&1 || true
  touch "${LOG_DIR}/${SCENARIO}.killed"
  wait "${PID_A}" >/dev/null 2>&1 || true
  wait "${CLIENT_PID}"
else
  run_client_or_report
fi

grep -q "spot-actor-transfer e2e result=passed" "${LOG_DIR}/client.stdout.log"
if [[ "${SCENARIO}" == "ST-F6" ]]; then
  target_flow_found=0
  for flow_log in "${LOG_DIR}"/actor-*-flow.log; do
    if rg -q "outcome=RECEIVED.*kind=ACTOR_REQUEST.*packet=ProbeReq" \
        "${flow_log}"; then
      target_flow_found=1
      rg -q "outcome=REPLIED.*kind=ACTOR_REQUEST.*packet=ProbeReq" \
        "${flow_log}"
    fi
  done
  if [[ "${target_flow_found}" != "1" ]]; then
    echo "ST-F6 target did not receive a ProbeReq" >&2
    exit 1
  fi
  for evidence_log in "${LOG_DIR}"/actor-*.evidence.log; do
    flow_log="${evidence_log%.evidence.log}-flow.log"
    while IFS= read -r actor_id; do
      if rg -q "outcome=RECEIVED.*kind=ACTOR_REQUEST.*packet=ProbeReq.*actor=${actor_id}" \
          "${flow_log}"; then
        echo "ST-F6 transferred request was dispatched again on the source" >&2
        exit 1
      fi
    done < <(awk -F'|' '$3 == "transfer_out" { print $2 }' "${evidence_log}")
  done
  if ! rg -q "\\|request_timeout\\|" "${LOG_DIR}"/actor-*.evidence.log; then
    echo "ST-F6 request timeout evidence is missing" >&2
    exit 1
  fi
fi
cat "${LOG_DIR}/client.stdout.log"
