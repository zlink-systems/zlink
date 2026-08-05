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
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
readonly e2e_build_dir="${HOME}/.cache/zlink/java-e2e/PubSub"
readonly gradle_cache_dir="${HOME}/.cache/zlink/java-e2e/PubSub-gradle-cache"
redis_location_endpoint=""
location_key_prefix="zlink:e2e:pubsub:${run_id}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
if [[ "${LOCAL_READINESS_TIMEOUT_SECONDS}" != 3 \
   || "${LOCAL_READINESS_ATTEMPTS}" != 30 ]]; then
  echo "PubSub must use a 3s readiness limit" >&2
  exit 1
fi
if rg -n 'java\.net\.http\.HttpClient|HttpClient\.new' \
    "$(pwd)/Client/src/main/java" --glob '*.java'; then
  echo "PubSub client must use ZLinkHttpClient" >&2
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
    for _ in range(8):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
    print(" ".join(f"tcp://127.0.0.1:{port}" for port in ports[:3]), end=" ")
    print(" ".join(f"http://127.0.0.1:{port}" for port in ports[3:]))
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

wait_health() {
  local name="$1"
  local endpoint="$2"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if python3 - "${endpoint}/health" <<'PY'
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

wait_marker() {
  local file="$1"
  for _ in $(seq 1 600); do
    if [[ -f "${file}" ]]; then
      return 0
    fi
    sleep 0.1
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
    "zlink-redis-java-e2e" "redis:7.2-alpine"
  redis_location_endpoint="127.0.0.1:${redis_port}"
}

gradle_run() {
  ../../gradlew -PzlinkE2eBuildDir="${e2e_build_dir}" \
    --project-cache-dir "${gradle_cache_dir}" --no-daemon --no-parallel --max-workers=1 "$@" --quiet
}

client_bin() {
  echo "${e2e_build_dir}/Client/install/pub-sub-client/bin/pub-sub-client"
}

publisher_bin() {
  echo "${e2e_build_dir}/Server-Publisher/install/pub-sub-publisher/bin/pub-sub-publisher"
}

subscriber_bin() {
  echo "${e2e_build_dir}/Server-Subscriber/install/pub-sub-subscriber/bin/pub-sub-subscriber"
}

start_publisher() {
  local suffix="${1:-publisher}"
  local config="${config_dir}/${suffix}.properties"
  cat >"${config}" <<EOF
e2e.http-endpoint=${PUBLISHER_HTTP}
e2e.publisher-endpoint=${PUBLISHER_ENDPOINT}
e2e.redis-location-endpoint=${redis_location_endpoint}
e2e.location-key-prefix=${location_key_prefix}
e2e.log-dir=${log_dir}
EOF
  chmod 0600 "${config}"
  "$(publisher_bin)" --config "${config}" \
    >"${log_dir}/${suffix}.stdout.log" 2>"${log_dir}/${suffix}.stderr.log" &
  LAST_PID="$!"
  pids+=("${LAST_PID}")
  wait_port publisher-fanout "${PUBLISHER_ENDPOINT}"
  wait_health "${suffix}" "${PUBLISHER_HTTP}"
}

start_subscriber() {
  local rid="$1"
  local topics="$2"
  local http="$3"
  local delay="${4:-}"
  local config="${config_dir}/${rid}.properties"
  cat >"${config}" <<EOF
e2e.rid=${rid}
e2e.topics=${topics}
e2e.http-endpoint=${http}
e2e.redis-location-endpoint=${redis_location_endpoint}
e2e.location-key-prefix=${location_key_prefix}
e2e.log-dir=${log_dir}
e2e.delay.delay-millis=${delay:-0}
EOF
  chmod 0600 "${config}"
  "$(subscriber_bin)" --config "${config}" \
    >"${log_dir}/${rid}.stdout.log" 2>"${log_dir}/${rid}.stderr.log" &
  LAST_PID="$!"
  pids+=("${LAST_PID}")
  wait_health "${rid}" "${http}"
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
  "$(client_bin)" --config "${client_config}" --scenario "${mode}" \
    >"${log_dir}/client-${suffix}.stdout.log" 2>"${log_dir}/client-${suffix}.stderr.log"
  cat "${log_dir}/client-${suffix}.stdout.log"
}

read -r _UNUSED_PUB _UNUSED_ROUTER PUBLISHER_ENDPOINT _UNUSED_HTTP PUBLISHER_HTTP SUB1_HTTP SUB2_HTTP SUB3_HTTP <<<"$(reserve_ports)"

gradle_run installDist
start_redis_container

PUBLISHER_READY="${log_dir}/publisher-ready"
PRELATE_CONTINUE="${log_dir}/prelate-continue"
LATE_READY="${log_dir}/late-ready"
LATE_CONTINUE="${log_dir}/late-continue"
client_config="${config_dir}/client.properties"
cat >"${client_config}" <<EOF
publisherHttp=${PUBLISHER_HTTP}
publisherEndpoint=${PUBLISHER_ENDPOINT}
redisLocationEndpoint=${redis_location_endpoint}
locationKeyPrefix=${location_key_prefix}
sub1Http=${SUB1_HTTP}
sub2Http=${SUB2_HTTP}
sub3Http=${SUB3_HTTP}
publisherReadyFile=${PUBLISHER_READY}
prelateContinueFile=${PRELATE_CONTINUE}
lateReadyFile=${LATE_READY}
lateContinueFile=${LATE_CONTINUE}
buildDir=${e2e_build_dir}
logDir=${log_dir}
configDir=${config_dir}
EOF
chmod 0600 "${client_config}"

start_ordered_roles() {
  local sub1_delay_ms="${1:-}"
  shift
  local role
  mapfile -t ordered_roles < <(zlink_e2e_order_roles "$@")
  for role in "${ordered_roles[@]}"; do
    case "${role}" in
      publisher)
        start_publisher publisher
        PUBLISHER_PID="${LAST_PID}"
        ;;
      sub-1)
        start_subscriber sub-1 alpha "${SUB1_HTTP}" "${sub1_delay_ms}"
        SUB1_PID="${LAST_PID}"
        ;;
      sub-2)
        start_subscriber sub-2 beta "${SUB2_HTTP}"
        SUB2_PID="${LAST_PID}"
        ;;
      sub-3)
        start_subscriber sub-3 gamma "${SUB3_HTTP}"
        SUB3_PID="${LAST_PID}"
        ;;
    esac
  done
}

case "${SCENARIO}" in
  PS-A1|PS-A2|PS-C1)
    start_ordered_roles "" publisher sub-1 sub-2 sub-3
    run_client_mode "${SCENARIO}" "${SCENARIO}"
    grep -q "scenario ${SCENARIO} passed" "${log_dir}/client-${SCENARIO}.stdout.log"
    grep -Rq "message flow" "${log_dir}"/*-flow.log
    exit 0
    ;;
  PS-A3)
    start_ordered_roles "" publisher
    "$(client_bin)" --config "${client_config}" --scenario "${SCENARIO}" \
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
    grep -Rq "message flow" "${log_dir}"/*-flow.log
    exit 0
    ;;
  PS-A4)
    start_ordered_roles "" publisher sub-2
    run_client_mode "${SCENARIO}" "${SCENARIO}"
    grep -q "scenario ${SCENARIO} passed" "${log_dir}/client-${SCENARIO}.stdout.log"
    grep -Rq "message flow" "${log_dir}"/*-flow.log
    exit 0
    ;;
  PS-B1)
    start_ordered_roles 750 publisher sub-1 sub-2 sub-3
    run_client_mode "${SCENARIO}" "${SCENARIO}"
    grep -q "scenario ${SCENARIO} passed" "${log_dir}/client-${SCENARIO}.stdout.log"
    grep -Rq "message flow" "${log_dir}"/*-flow.log
    exit 0
    ;;
  PS-B2)
    start_ordered_roles "" publisher
    stop_pid "${PUBLISHER_PID}"
    start_subscriber sub-1 alpha "${SUB1_HTTP}"
    start_subscriber sub-2 beta "${SUB2_HTTP}"
    start_subscriber sub-3 gamma "${SUB3_HTTP}"
    run_client_mode "${SCENARIO}" "${SCENARIO}"
    grep -q "scenario ${SCENARIO} passed" "${log_dir}/client-${SCENARIO}.stdout.log"
    grep -Rq "message flow" "${log_dir}"/*-flow.log
    exit 0
    ;;
  all)
    ;;
  *)
    echo "Unknown PubSub scenario: ${SCENARIO}" >&2
    exit 1
    ;;
esac

start_ordered_roles "" publisher

"$(client_bin)" --config "${client_config}" --scenario default \
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

stop_pid "${SUB1_PID}"
run_client_mode subscriber-restarted ps-a4

start_subscriber sub-1 alpha "${SUB1_HTTP}" 750
SUB1_PID="${LAST_PID}"
run_client_mode slow-subscriber ps-b1

# Do not carry the deliberately slow handler's queued work into the
# independent publisher-restart scenario.
stop_pid "${SUB1_PID}"
start_subscriber sub-1 alpha "${SUB1_HTTP}"
SUB1_PID="${LAST_PID}"

stop_pid "${PUBLISHER_PID}"
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

grep -Rq "message flow" "${log_dir}"/*-flow.log
grep -q "HANDLER_MISSING/DROP/MissingEventMsg" "${log_dir}/sub-2-evidence.json"
echo "pub-sub e2e result=passed"
