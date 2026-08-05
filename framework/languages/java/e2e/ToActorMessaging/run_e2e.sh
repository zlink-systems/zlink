#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/start-order-common.sh"

cd "$(dirname "${BASH_SOURCE[0]}")"

pids=()
redis_container=""
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
e2e_start_order="$(zlink_e2e_start_order_mode "$@")"
SCENARIO="${1:-all}"
echo "start_order=${e2e_start_order}"
echo "scenario=${SCENARIO}"

if [[ ! -f "Server/Session/build.gradle.kts" ]]; then
  echo "ToActorMessaging requires the session gateway role" >&2
  exit 1
fi

if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi

location_key_prefix="zlink:e2e:toactor:${run_id}"
config_dir="$(mktemp -d)"
chmod 0700 "${config_dir}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
if [[ "${LOCAL_READINESS_TIMEOUT_SECONDS:-}" != 3 \
   || "${LOCAL_READINESS_ATTEMPTS:-}" != 30 ]]; then
  echo "ToActorMessaging must use a 3s readiness limit" >&2
  exit 1
fi
if rg -n 'java\.net\.http\.HttpClient|HttpClient\.new' \
    "$(pwd)/Client/src/main/java" "$(pwd)/Shared/src/main/java" --glob '*.java'; then
  echo "ToActorMessaging client must use ZLinkHttpClient" >&2
  exit 1
fi

reserve_ports() {
  python3 - <<'PY'
import socket
sockets = []
ports = []
try:
    for _ in range(10):
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

wait_tcp() {
  local host="$1"
  local port="$2"
  local name="$3"
  if python3 - "$host" "$port" "${LOCAL_READINESS_TIMEOUT_SECONDS}" "${LOCAL_READINESS_POLL_SECONDS}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
deadline = time.monotonic() + float(sys.argv[3])
while time.monotonic() < deadline:
    try:
        with socket.create_connection((host, port), timeout=1):
            sys.exit(0)
    except OSError:
        time.sleep(float(sys.argv[4]))
sys.exit(1)
PY
  then
    return 0
  fi
  echo "Timed out waiting for ${name} at ${host}:${port}" >&2
  return 1
}

zlink_redis_start_scoped_assign redis_container redis_port \
  "zlink-redis-java-e2e" "redis:7.2-alpine" "127.0.0.1::6379"
redis_endpoint="127.0.0.1:${redis_port}"
redis_host="${redis_endpoint%:*}"
redis_port="${redis_endpoint##*:}"
wait_tcp "${redis_host}" "${redis_port}" redis

read -r actor_http caller_http session_a_http session_b_http \
  actor_spot caller_spot session_a_spot session_b_spot \
  session_a_stream session_b_stream < <(reserve_ports)
actor_http_endpoint="http://127.0.0.1:${actor_http}"
caller_http_endpoint="http://127.0.0.1:${caller_http}"
session_a_http_endpoint="http://127.0.0.1:${session_a_http}"
session_b_http_endpoint="http://127.0.0.1:${session_b_http}"
actor_spot_endpoint="tcp://127.0.0.1:${actor_spot}"
caller_spot_endpoint="tcp://127.0.0.1:${caller_spot}"
session_a_stream_endpoint="tcp://127.0.0.1:${session_a_stream}"
session_b_stream_endpoint="tcp://127.0.0.1:${session_b_stream}"
actor_rid="actor-a"
caller_rid="aaa-caller"
echo "session_a_spot=tcp://127.0.0.1:${session_a_spot} session_a_stream=${session_a_stream_endpoint}"
echo "session_b_spot=tcp://127.0.0.1:${session_b_spot} session_b_stream=${session_b_stream_endpoint}"

write_config() {
  local path="$1"
  shift
  {
    printf 'redisLocationEndpoint=%s\n' "${redis_endpoint}"
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
    printf 'e2e.redis-location-endpoint=%s\n' "${redis_endpoint}"
    printf 'e2e.location-key-prefix=%s\n' "${location_key_prefix}"
    printf 'e2e.log-directory=%s\n' "${log_dir}"
    local property
    for property in "$@"; do printf 'e2e.%s\n' "${property}"; done
  } >"${path}"
  chmod 0600 "${path}"
}
actor_config="${config_dir}/actor.properties"
caller_config="${config_dir}/caller.properties"
session_a_config="${config_dir}/session-a.properties"
session_b_config="${config_dir}/session-b.properties"
client_config="${config_dir}/client.properties"
write_role_config "${actor_config}" \
  "actor-http-endpoint=${actor_http_endpoint}" "actor-spot-endpoint=${actor_spot_endpoint}" "actor-rid=${actor_rid}"
write_role_config "${caller_config}" \
  "caller-http-endpoint=${caller_http_endpoint}" "caller-spot-endpoint=${caller_spot_endpoint}" "caller-rid=${caller_rid}"
write_role_config "${session_a_config}" \
  "session-rid=session-a" "session-http-endpoint=${session_a_http_endpoint}" \
  "session-spot-endpoint=tcp://127.0.0.1:${session_a_spot}" "session-stream-endpoint=${session_a_stream_endpoint}"
write_role_config "${session_b_config}" \
  "session-rid=session-b" "session-http-endpoint=${session_b_http_endpoint}" \
  "session-spot-endpoint=tcp://127.0.0.1:${session_b_spot}" "session-stream-endpoint=${session_b_stream_endpoint}"
write_config "${client_config}" \
  "actorHttpEndpoint=${actor_http_endpoint}" \
  "callerHttpEndpoint=${caller_http_endpoint}" \
  "sessionAHttpEndpoint=${session_a_http_endpoint}" \
  "sessionBHttpEndpoint=${session_b_http_endpoint}" \
  "sessionAStreamEndpoint=${session_a_stream_endpoint}" \
  "sessionBStreamEndpoint=${session_b_stream_endpoint}"

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
    kill -9 "${pids[$i]}" >/dev/null 2>&1 || true
  done
  if [[ -n "${redis_container}" ]]; then
    docker rm -fv "${redis_container}" >/dev/null 2>&1 || true
  fi
  rm -rf "${config_dir}"
  wait >/dev/null 2>&1 || true
  exit "${status}"
}
trap cleanup EXIT

wait_http() {
  local endpoint="$1"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if python3 - "${endpoint}/health" >/dev/null 2>&1 <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    response.read()
PY
    then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${endpoint}" >&2
  return 1
}

start_role() {
  case "$1" in
    actor)
      ./Server/Actor/build/install/to-actor-actor/bin/to-actor-actor \
        --config "${actor_config}" >"${log_dir}/actor.log" 2>&1 &
      pids+=("$!")
      ;;
    caller)
      ./Server/Caller/build/install/to-actor-caller/bin/to-actor-caller \
        --config "${caller_config}" >"${log_dir}/caller.log" 2>&1 &
      pids+=("$!")
      ;;
    session-a)
      ./Server/Session/build/install/to-actor-session/bin/to-actor-session \
        --config "${session_a_config}" \
        >"${log_dir}/session-a.log" 2>&1 &
      pids+=("$!")
      ;;
    session-b)
      ./Server/Session/build/install/to-actor-session/bin/to-actor-session \
        --config "${session_b_config}" \
        >"${log_dir}/session-b.log" 2>&1 &
      pids+=("$!")
      ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

wait_role() {
  case "$1" in
    actor) wait_http "${actor_http_endpoint}" ;;
    caller) wait_http "${caller_http_endpoint}" ;;
    session-a) wait_http "${session_a_http_endpoint}" ;;
    session-b) wait_http "${session_b_http_endpoint}" ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

wait_log() {
  local log="$1"
  local pattern="$2"
  local description="$3"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if [[ -f "${log}" ]] && grep -q "${pattern}" "${log}"; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${description}" >&2
  return 1
}

wait_role_ready() {
  case "$1" in
    actor) wait_log "${log_dir}/actor.log" "\\[boot\\] role=actor step=baselineActors done" "actor baseline readiness" ;;
    caller) wait_log "${log_dir}/caller.log" "\\[boot\\] role=caller step=main run done" "caller application readiness" ;;
    session-a) wait_log "${log_dir}/session-a.log" "\[boot\] role=session rid=session-a step=main run done" "session-a application readiness" ;;
    session-b) wait_log "${log_dir}/session-b.log" "\[boot\] role=session rid=session-b step=main run done" "session-b application readiness" ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

../../gradlew --no-daemon --no-parallel --max-workers=1 \
  --gradle-user-home "${HOME}/.cache/zlink/java-e2e/toactor-gradle" -p . installDist

SERVER_ROLES=(actor caller session-a session-b)
mapfile -t ORDERED_SERVER_ROLES < <(zlink_e2e_order_roles "${SERVER_ROLES[@]}")
for role in "${ORDERED_SERVER_ROLES[@]}"; do
  start_role "$role"
done
for role in "${SERVER_ROLES[@]}"; do
  wait_role "$role"
done
for role in "${SERVER_ROLES[@]}"; do
  wait_role_ready "$role"
done

./Client/build/install/to-actor-client/bin/to-actor-client \
  --config "${client_config}" --scenario "${SCENARIO}" \
  > >(tee "${log_dir}/client.log") 2>"${log_dir}/client.stderr.log"
