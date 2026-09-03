#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"
zlink_e2e_initialize kotlin "$0" "$@"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}"

pids=()
REDIS_CONTAINER=""
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="${ROOT_DIR}/logs/${run_id}"
SCENARIO="${1:-all}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/.artifacts/wsl/install/zlink-core/0.16.0/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
export ZLINK_KOTLIN_E2E_BUILD_DIR="${ZLINK_KOTLIN_E2E_BUILD_DIR:-${HOME}/.cache/zlink/kotlin-e2e/PubSub}"
export ZLINK_KOTLIN_E2E_GRADLE_CACHE="${ZLINK_KOTLIN_E2E_GRADLE_CACHE:-${HOME}/.cache/zlink/kotlin-e2e/PubSub-gradle-cache}"
ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT=""
export ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX:-zlink:e2e:kotlin:pubsub:${run_id}}"
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
HTTP_PROBE_TIMEOUT_SECONDS=3
SCENARIO_MARKER_ATTEMPTS=200
SCENARIO_MARKER_INTERVAL=0.1

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
    zlink_redis_remove_by_id "${REDIS_CONTAINER}" || true
  fi
  wait >/dev/null 2>&1 || true
  exit "${status}"
}
trap cleanup EXIT

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

wait_redis_port() {
  local endpoint="$1"
  local port
  port="$(port_of "${endpoint}")"
  for _ in $(seq 1 600); do
    if (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for Redis fixture at ${endpoint}" >&2
  return 1
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

wait_fanout_status_ready() {
  local url="$1"
  local name="$2"
  for _ in $(seq 1 200); do
    if curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS "${url}/status" \
      | grep -q '"isReady":true'; then
      return 0
    fi
    sleep "${SCENARIO_MARKER_INTERVAL}"
  done
  echo "Timed out waiting for ${name} fanout status" >&2
  return 1
}

wait_marker() {
  local file="$1"
  for _ in $(seq 1 "${SCENARIO_MARKER_ATTEMPTS}"); do
    if [[ -f "${file}" ]]; then
      return 0
    fi
    sleep "${SCENARIO_MARKER_INTERVAL}"
  done
  echo "Timed out waiting for marker ${file}" >&2
  return 1
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
  wait_redis_port "tcp://${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}"
}

gradle_run() {
  zlink_e2e_gradle_build_locked ../../gradlew \
    --project-cache-dir "${ZLINK_KOTLIN_E2E_GRADLE_CACHE}" --no-daemon "$@" --quiet
}

bin_path() {
  local path="$1"
  local app="$2"
  echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/${path}/install/${app}/bin/${app}"
}

CLIENT_BIN="$(bin_path Client pub-sub-kotlin-client)"
PUBLISHER_BIN="$(bin_path Server-Publisher pub-sub-kotlin-publisher)"
SUBSCRIBER_BIN="$(bin_path Server-Subscriber pub-sub-kotlin-subscriber)"

start_publisher() {
  local suffix="${1:-publisher}"
  local endpoint="${2:-${PUBLISHER_ENDPOINT}}"
  local http_endpoint="${3:-${PUBLISHER_HTTP}}"
  local rid="${4:-publisher-a}"
  local channel_name="${5:-pubsub.kotlin.events}"
  local redis_endpoint="${6-${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}}"
  local location_key_prefix="${7-${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}}"
  local advertise_host="${8:-}"
  local command=("${PUBLISHER_BIN}" \
    --publisher-endpoint "${endpoint}" \
    --http-endpoint "${http_endpoint}" \
    --rid "${rid}" \
    --channel-name "${channel_name}" \
    --redis-location-endpoint "${redis_endpoint}" \
    --location-key-prefix "${location_key_prefix}" \
    --log-dir "${log_dir}")
  if [[ -n "${advertise_host}" ]]; then
    command+=(--advertise-host "${advertise_host}")
  fi
  "${command[@]}" \
    >"${log_dir}/${suffix}.stdout.log" 2>"${log_dir}/${suffix}.stderr.log" &
  LAST_PID="$!"
  pids+=("${LAST_PID}")
  wait_port "${suffix}" "${endpoint}"
  wait_health "${http_endpoint}" "${suffix}"
}

start_publisher_port_zero() {
  local suffix="${1:-publisher}"
  local http_endpoint="${2:-${PUBLISHER_HTTP}}"
  local rid="${3:-publisher-a}"
  local channel_name="${4:-pubsub.kotlin.events}"
  "${PUBLISHER_BIN}" \
    --publisher-port 0 \
    --http-endpoint "${http_endpoint}" \
    --rid "${rid}" \
    --channel-name "${channel_name}" \
    --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --log-dir "${log_dir}" \
    >"${log_dir}/${suffix}.stdout.log" 2>"${log_dir}/${suffix}.stderr.log" &
  LAST_PID="$!"
  pids+=("${LAST_PID}")
  wait_health "${http_endpoint}" "${suffix}"
}

start_subscriber() {
  local rid="$1"
  local topics="$2"
  local http="$3"
  local delay="${4:-}"
  local include_all="${5:-true}"
  local manual_endpoint="${6:-}"
  local mixed_mode="${7:-false}"
  local no_store="${8:-false}"
  local redis_endpoint="${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}"
  local location_key_prefix="${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}"
  if [[ "${no_store}" == "true" ]]; then
    redis_endpoint=""
    location_key_prefix=""
  fi
  "${SUBSCRIBER_BIN}" \
    --rid "${rid}" \
    --topics "${topics}" \
    --include-all "${include_all}" \
    --http-endpoint "${http}" \
    --handler-delay-ms "${delay}" \
    --redis-location-endpoint "${redis_endpoint}" \
    --location-key-prefix "${location_key_prefix}" \
    --log-dir "${log_dir}" \
    --manual-endpoint "${manual_endpoint}" \
    --mixed-mode "${mixed_mode}" \
    >"${log_dir}/${rid}.stdout.log" 2>"${log_dir}/${rid}.stderr.log" &
  LAST_PID="$!"
  pids+=("${LAST_PID}")
  wait_health "${http}" "${rid}"
}

run_expected_subscriber_failure() {
  local suffix="$1"
  local redis_endpoint="$2"
  local manual_endpoint="$3"
  local mixed_mode="$4"
  set +e
  timeout 30s "${SUBSCRIBER_BIN}" \
    --rid "negative-${suffix}" \
    --topics all \
    --include-all true \
    --http-endpoint "${SUB4_HTTP}" \
    --handler-delay-ms "" \
    --redis-location-endpoint "${redis_endpoint}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --manual-endpoint "${manual_endpoint}" \
    --mixed-mode "${mixed_mode}" \
    --log-dir "${log_dir}" \
    >"${log_dir}/${suffix}.stdout.log" 2>"${log_dir}/${suffix}.stderr.log"
  local status="$?"
  set -e
  if [[ "${status}" == "0" ]]; then
    echo "${suffix} unexpectedly started successfully" >&2
    return 1
  fi
  grep -Eiq "automatic subscriber|cannot combine|requires" \
    "${log_dir}/${suffix}.stderr.log" "${log_dir}/${suffix}.stdout.log"
  echo "scenario ${suffix} passed (startup rejected with public configuration evidence)"
}

run_expected_publisher_failure() {
  local suffix="$1"
  shift
  set +e
  timeout 30s "${PUBLISHER_BIN}" \
    --publisher-endpoint "${PUBLISHER2_ENDPOINT}" \
    --http-endpoint "${PUBLISHER2_HTTP}" \
    --channel-name "pubsub.kotlin.events" \
    --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --log-dir "${log_dir}" \
    "$@" \
    >"${log_dir}/${suffix}.stdout.log" 2>"${log_dir}/${suffix}.stderr.log"
  local status="$?"
  set -e
  if [[ "${status}" == "0" || "${status}" == "124" ]]; then
    echo "${suffix} did not fail during startup" >&2
    return 1
  fi
  grep -Eiq "exactly one publisher identity|fixed routing ID|routing ID prefix" \
    "${log_dir}/${suffix}.stderr.log" "${log_dir}/${suffix}.stdout.log"
}

stop_pid() {
  local pid="$1"
  if kill -0 "${pid}" >/dev/null 2>&1; then
    kill "${pid}" >/dev/null 2>&1 || true
    wait "${pid}" >/dev/null 2>&1 || true
  fi
}

run_client_mode() {
  local mode="$1"
  local suffix="$2"
  "${CLIENT_BIN}" \
    --mode "${mode}" \
    --publisher-http "${PUBLISHER_HTTP}" \
    --publisher2-http "${PUBLISHER2_HTTP}" \
    --publisher2-endpoint "${PUBLISHER2_ENDPOINT}" \
    --publisher2-port "${PUBLISHER2_PORT}" \
    --publisher2-rid "${PUBLISHER2_RID}" \
    --publisher2-no-store "${PUBLISHER2_NO_STORE}" \
    --publisher2-advertise-host "${PUBLISHER2_ADVERTISE_HOST}" \
    --audit-publisher-http "${AUDIT_HTTP}" \
    --audit-publisher-endpoint "${AUDIT_ENDPOINT}" \
    --sub1-http "${SUB1_HTTP}" \
    --sub2-http "${SUB2_HTTP}" \
    --sub3-http "${SUB3_HTTP}" \
    --sub4-http "${SUB4_HTTP}" \
    --reconnect-http "${RECONNECT_HTTP}" \
    --publisher-bin "${PUBLISHER_BIN}" \
    --subscriber-bin "${SUBSCRIBER_BIN}" \
    --publisher-endpoint "${PUBLISHER_ENDPOINT}" \
    --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
    --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
    --log-dir "${log_dir}" \
    --late-continue-file "${LATE_CONTINUE}" \
    --publisher-pid "${PUBLISHER_PID:-}" \
    >"${log_dir}/client-${suffix}.stdout.log" 2>"${log_dir}/client-${suffix}.stderr.log"
  cat "${log_dir}/client-${suffix}.stdout.log"
}

assigned_ports=()
zlink_e2e_assign_unique_ports assigned_ports 11
PUBLISHER_ENDPOINT="tcp://127.0.0.1:${assigned_ports[0]}"
PUBLISHER_HTTP="http://127.0.0.1:${assigned_ports[1]}"
PUBLISHER2_ENDPOINT="tcp://127.0.0.1:${assigned_ports[2]}"
PUBLISHER2_HTTP="http://127.0.0.1:${assigned_ports[3]}"
AUDIT_ENDPOINT="tcp://127.0.0.1:${assigned_ports[4]}"
AUDIT_HTTP="http://127.0.0.1:${assigned_ports[5]}"
SUB1_HTTP="http://127.0.0.1:${assigned_ports[6]}"
SUB2_HTTP="http://127.0.0.1:${assigned_ports[7]}"
SUB3_HTTP="http://127.0.0.1:${assigned_ports[8]}"
SUB4_HTTP="http://127.0.0.1:${assigned_ports[9]}"
RECONNECT_HTTP="http://127.0.0.1:${assigned_ports[10]}"
PUBLISHER2_RID="publisher-b"
PUBLISHER2_NO_STORE="false"
PUBLISHER2_ADVERTISE_HOST=""
PUBLISHER2_PORT=""

if [[ "${SCENARIO}" == "all" ]]; then
  if [[ "${ZLINK_KOTLIN_E2E_SKIP_BUILD:-false}" != "true" ]]; then
    gradle_run installDist
  fi
  selectors=(
    PS-A1 PS-A2 PS-A3 PS-A4 PS-B1 PS-B2 PS-C1
    PS-D1 PS-D2 PS-D3 PS-D4 PS-D5 PS-D6 PS-D7A PS-D7B
    PS-E1 PS-E2A PS-E2B PS-E2C
    PS-F1 PS-F2 PS-F3 PS-F4 PS-F5
  )
  for selector in "${selectors[@]}"; do
    echo "=== Kotlin PubSub ${selector} ==="
    ZLINK_KOTLIN_E2E_SKIP_BUILD=true bash "${ROOT_DIR}/run_e2e.sh" "${selector}"
  done
  echo "pub-sub kotlin all result=passed selectors=${#selectors[@]}"
  exit 0
fi

if [[ "${ZLINK_KOTLIN_E2E_SKIP_BUILD:-false}" != "true" ]]; then
  gradle_run installDist
fi
start_redis_container

case "${SCENARIO}" in
  PS-E2A)
    run_expected_subscriber_failure PS-E2A "" "" false
    exit 0
    ;;
  PS-E2B)
    run_expected_subscriber_failure PS-E2B "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" "${PUBLISHER_ENDPOINT}" true
    exit 0
    ;;
  PS-E2C)
    run_expected_publisher_failure PS-E2C-missing
    run_expected_publisher_failure PS-E2C-duplicate \
      --rid publisher-fixed --routing-id-prefix publisher-allocated
    echo "scenario PS-E2C passed"
    exit 0
    ;;
esac

PUBLISHER_READY="${log_dir}/publisher-ready"
PRELATE_CONTINUE="${log_dir}/prelate-continue"
LATE_READY="${log_dir}/late-ready"
LATE_CONTINUE="${log_dir}/late-continue"

if [[ "${SCENARIO}" == "PS-D6" ]]; then
  start_publisher_port_zero publisher "${PUBLISHER_HTTP}" "publisher-a" "pubsub.kotlin.events"
else
  start_publisher publisher "${PUBLISHER_ENDPOINT}" "${PUBLISHER_HTTP}" "publisher-a" "pubsub.kotlin.events"
fi
PUBLISHER_PID="${LAST_PID}"

case "${SCENARIO}" in
  PS-A1|PS-A2|PS-C1)
    start_subscriber sub-1 alpha "${SUB1_HTTP}"
    start_subscriber sub-2 beta "${SUB2_HTTP}"
    start_subscriber sub-3 gamma "${SUB3_HTTP}"
    run_client_mode "${SCENARIO}" "${SCENARIO}"
    grep -q "scenario ${SCENARIO} passed" "${log_dir}/client-${SCENARIO}.stdout.log"
    grep -Rq "message flow" "${log_dir}"/*.stdout.log
    exit 0
    ;;
  PS-A3)
    "${CLIENT_BIN}" \
      --mode "${SCENARIO}" \
      --publisher-http "${PUBLISHER_HTTP}" \
      --sub1-http "${SUB1_HTTP}" \
      --sub2-http "${SUB2_HTTP}" \
      --sub3-http "${SUB3_HTTP}" \
      --reconnect-http "${RECONNECT_HTTP}" \
      --publisher-bin "${PUBLISHER_BIN}" \
      --subscriber-bin "${SUBSCRIBER_BIN}" \
      --publisher-endpoint "${PUBLISHER_ENDPOINT}" \
      --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
      --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
      --log-dir "${log_dir}" \
      --publisher-ready-file "${PUBLISHER_READY}" \
      --prelate-continue-file "${PRELATE_CONTINUE}" \
    --late-ready-file "${LATE_READY}" \
    --late-continue-file "${LATE_CONTINUE}" \
    --publisher-pid "${PUBLISHER_PID:-}" \
      >"${log_dir}/client-PS-A3.stdout.log" 2>"${log_dir}/client-PS-A3.stderr.log" &
    CLIENT_PID="$!"
    pids+=("${CLIENT_PID}")
    wait_marker "${PUBLISHER_READY}"
    start_subscriber sub-1 alpha "${SUB1_HTTP}"
    start_subscriber sub-2 beta "${SUB2_HTTP}"
    touch "${PRELATE_CONTINUE}"
    wait_marker "${LATE_READY}"
    start_subscriber sub-3 gamma "${SUB3_HTTP}"
    touch "${LATE_CONTINUE}"
    wait "${CLIENT_PID}"
    cat "${log_dir}/client-PS-A3.stdout.log"
    grep -q "scenario PS-A3 passed" "${log_dir}/client-PS-A3.stdout.log"
    grep -Rq "message flow" "${log_dir}"/*.stdout.log
    exit 0
    ;;
  PS-A4)
    start_subscriber sub-1 alpha "${SUB1_HTTP}"
    start_subscriber sub-2 beta "${SUB2_HTTP}"
    run_client_mode "${SCENARIO}" "${SCENARIO}"
    grep -q "scenario ${SCENARIO} passed" "${log_dir}/client-${SCENARIO}.stdout.log"
    grep -Rq "message flow" "${log_dir}"/*.stdout.log
    exit 0
    ;;
  PS-B1)
    start_subscriber sub-1 alpha "${SUB1_HTTP}" 750
    start_subscriber sub-2 beta "${SUB2_HTTP}"
    start_subscriber sub-3 gamma "${SUB3_HTTP}"
    run_client_mode "${SCENARIO}" "${SCENARIO}"
    grep -q "scenario ${SCENARIO} passed" "${log_dir}/client-${SCENARIO}.stdout.log"
    grep -Rq "message flow" "${log_dir}"/*.stdout.log
    exit 0
    ;;
  PS-B2)
    start_subscriber sub-1 alpha "${SUB1_HTTP}"
    start_subscriber sub-2 beta "${SUB2_HTTP}"
    start_subscriber sub-3 gamma "${SUB3_HTTP}"
    run_client_mode "${SCENARIO}" "${SCENARIO}"
    grep -q "scenario ${SCENARIO} passed" "${log_dir}/client-${SCENARIO}.stdout.log"
    grep -Rq "message flow" "${log_dir}"/*.stdout.log
    exit 0
    ;;
  PS-D1)
    start_subscriber sub-1 all "${SUB1_HTTP}"
    run_client_mode PS-D1 ps-d1
    grep -q "scenario PS-D1 passed" "${log_dir}/client-ps-d1.stdout.log"
    exit 0
    ;;
  PS-D2)
    start_publisher audit "${AUDIT_ENDPOINT}" "${AUDIT_HTTP}" "audit-publisher" "pubsub.kotlin.audit"
    start_subscriber sub-1 all "${SUB1_HTTP}"
    run_client_mode PS-D2 ps-d2
    grep -q "scenario PS-D2 passed" "${log_dir}/client-ps-d2.stdout.log"
    exit 0
    ;;
  PS-D3|PS-D4|PS-F4)
    start_subscriber sub-1 all "${SUB1_HTTP}"
    run_client_mode "${SCENARIO}" "${SCENARIO}"
    grep -q "scenario ${SCENARIO} passed" "${log_dir}/client-${SCENARIO}.stdout.log"
    exit 0
    ;;
  PS-D5)
    start_subscriber sub-1 all "${SUB1_HTTP}"
    wait_fanout_status_ready "${SUB1_HTTP}" sub-1
    timeout -k 2s 10s docker pause "${REDIS_CONTAINER}" >/dev/null
    if ! run_client_mode PS-D5 ps-d5-outage; then
      timeout -k 2s 10s docker unpause "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
      exit 1
    fi
    timeout -k 2s 10s docker unpause "${REDIS_CONTAINER}" >/dev/null
    run_client_mode PS-D5-RECOVERY ps-d5-recovery
    grep -q "scenario PS-D5 passed" "${log_dir}/client-ps-d5-outage.stdout.log"
    grep -q "scenario PS-D5-RECOVERY passed" "${log_dir}/client-ps-d5-recovery.stdout.log"
    exit 0
    ;;
  PS-D6)
    PUBLISHER2_PORT=0
    start_subscriber sub-1 all "${SUB1_HTTP}"
    run_client_mode PS-D6 ps-d6
    grep -q "scenario PS-D6 passed" "${log_dir}/client-ps-d6.stdout.log"
    exit 0
    ;;
  PS-D7A)
    start_subscriber sub-1 all "${SUB1_HTTP}"
    run_client_mode PS-D7A ps-d7a
    grep -q "scenario PS-D7A passed" "${log_dir}/client-ps-d7a.stdout.log"
    exit 0
    ;;
  PS-D7B)
    start_subscriber sub-1 all "${SUB1_HTTP}"
    start_subscriber sub-4 all "${SUB4_HTTP}" "" true "${PUBLISHER_ENDPOINT}" false true
    run_client_mode PS-D7B ps-d7b
    grep -q "scenario PS-D7B passed" "${log_dir}/client-ps-d7b.stdout.log"
    exit 0
    ;;
  PS-E1)
    start_subscriber sub-4 all "${SUB4_HTTP}" "" true "${PUBLISHER_ENDPOINT}" false true
    run_client_mode PS-E1 ps-e1
    grep -q "scenario PS-E1 passed" "${log_dir}/client-ps-e1.stdout.log"
    exit 0
    ;;
  PS-E2A)
    exit 2
    ;;
  PS-E2B)
    exit 2
    ;;
  PS-E2C)
    exit 2
    ;;
  PS-F1)
    PUBLISHER2_NO_STORE=true
    start_subscriber sub-1 all "${SUB1_HTTP}"
    start_subscriber sub-4 all "${SUB4_HTTP}" "" true "${PUBLISHER2_ENDPOINT}" false true
    run_client_mode PS-F1 ps-f1
    grep -q "scenario PS-F1 passed" "${log_dir}/client-ps-f1.stdout.log"
    exit 0
    ;;
  PS-F2)
    publisher2_proxy_port="$(port_of "${PUBLISHER2_ENDPOINT}")"
    PUBLISHER2_ENDPOINT="tcp://127.0.0.2:${publisher2_proxy_port}"
    PUBLISHER2_ADVERTISE_HOST="127.0.0.1"
    start_subscriber sub-1 all "${SUB1_HTTP}"
    run_client_mode PS-F2 PS-F2
    grep -q "scenario PS-F2 passed" "${log_dir}/client-PS-F2.stdout.log"
    exit 0
    ;;
  PS-F3)
    start_subscriber sub-1 '*' "${SUB1_HTTP}" "" false
    run_client_mode PS-F3 ps-f3
    grep -q "scenario PS-F3 passed" "${log_dir}/client-ps-f3.stdout.log"
    exit 0
    ;;
  PS-F5)
    start_subscriber sub-1 events.b "${SUB1_HTTP}" "" false
    run_client_mode PS-F5 ps-f5
    grep -q "scenario PS-F5 passed" "${log_dir}/client-ps-f5.stdout.log"
    exit 0
    ;;
  *)
    echo "Unknown PubSub scenario: ${SCENARIO}" >&2
    exit 1
    ;;
esac

"${CLIENT_BIN}" \
  --mode default \
  --publisher-http "${PUBLISHER_HTTP}" \
  --sub1-http "${SUB1_HTTP}" \
  --sub2-http "${SUB2_HTTP}" \
  --sub3-http "${SUB3_HTTP}" \
  --reconnect-http "${RECONNECT_HTTP}" \
  --publisher-bin "${PUBLISHER_BIN}" \
  --subscriber-bin "${SUBSCRIBER_BIN}" \
  --publisher-endpoint "${PUBLISHER_ENDPOINT}" \
  --redis-location-endpoint "${ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT}" \
  --location-key-prefix "${ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX}" \
  --log-dir "${log_dir}" \
  --publisher-ready-file "${PUBLISHER_READY}" \
  --prelate-continue-file "${PRELATE_CONTINUE}" \
  --late-ready-file "${LATE_READY}" \
  --late-continue-file "${LATE_CONTINUE}" \
  >"${log_dir}/client.stdout.log" 2>"${log_dir}/client.stderr.log" &
CLIENT_PID="$!"
pids+=("${CLIENT_PID}")

wait_marker "${PUBLISHER_READY}"
start_subscriber sub-1 alpha "${SUB1_HTTP}"
SUB1_PID="${LAST_PID}"
start_subscriber sub-2 beta "${SUB2_HTTP}"
SUB2_PID="${LAST_PID}"
touch "${PRELATE_CONTINUE}"

wait_marker "${LATE_READY}"
start_subscriber sub-3 gamma "${SUB3_HTTP}"
SUB3_PID="${LAST_PID}"
touch "${LATE_CONTINUE}"

wait "${CLIENT_PID}"
cat "${log_dir}/client.stdout.log"

run_client_mode subscriber-restarted ps-a4

stop_pid "${SUB1_PID}"
start_subscriber sub-1 alpha "${SUB1_HTTP}" 750
SUB1_PID="${LAST_PID}"
run_client_mode slow-subscriber ps-b1

run_client_mode publisher-restarted ps-b2

python3 - "${SUB1_HTTP}/evidence" >"${log_dir}/sub-1-evidence.json" <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=5) as response:
    sys.stdout.write(response.read().decode("utf-8"))
PY
python3 - "${SUB2_HTTP}/evidence" >"${log_dir}/sub-2-evidence.json" <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=5) as response:
    sys.stdout.write(response.read().decode("utf-8"))
PY
python3 - "${SUB3_HTTP}/evidence" >"${log_dir}/sub-3-evidence.json" <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=5) as response:
    sys.stdout.write(response.read().decode("utf-8"))
PY

grep -Rq "message flow" "${log_dir}"/*.stdout.log
grep -q "HANDLER_MISSING/DROP/MissingEvent" "${log_dir}/sub-2-evidence.json"
