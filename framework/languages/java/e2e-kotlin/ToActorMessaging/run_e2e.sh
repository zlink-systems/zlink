#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"

cd "$(dirname "${BASH_SOURCE[0]}")"

pids=()
redis_container=""
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
E2E_START_ORDER="${E2E_START_ORDER:-forward}"
echo "start_order=${E2E_START_ORDER}"

if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi

location_key_prefix="zlink:e2e:toactor:${run_id}"
config_dir="$(mktemp -d)"
chmod 0700 "${config_dir}"

reserve_ports() {
  python3 - <<'PY'
import socket
sockets = []
ports = []
try:
    for _ in range(4):
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
  if python3 - "$host" "$port" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
deadline = time.monotonic() + 30
while time.monotonic() < deadline:
    try:
        with socket.create_connection((host, port), timeout=1):
            sys.exit(0)
    except OSError:
        time.sleep(0.2)
sys.exit(1)
PY
  then
    return 0
  fi
  echo "Timed out waiting for ${name} at ${host}:${port}" >&2
  return 1
}

zlink_redis_start_scoped_assign redis_container redis_port \
  "zlink-redis-kotlin-e2e" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}" "127.0.0.1::6379"
redis_endpoint="127.0.0.1:${redis_port}"
redis_host="${redis_endpoint%:*}"
redis_port="${redis_endpoint##*:}"
wait_tcp "${redis_host}" "${redis_port}" redis

read -r actor_http caller_http actor_spot caller_spot < <(reserve_ports)
actor_http_endpoint="http://127.0.0.1:${actor_http}"
caller_http_endpoint="http://127.0.0.1:${caller_http}"
actor_config="${config_dir}/actor.properties"
caller_config="${config_dir}/caller.properties"
client_config="${config_dir}/client.properties"
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
write_config "${actor_config}" \
  "actorHttpEndpoint=${actor_http_endpoint}" \
  "actorSpotEndpoint=tcp://127.0.0.1:${actor_spot}" \
  "actorRid=actor-a"
write_config "${caller_config}" \
  "callerHttpEndpoint=${caller_http_endpoint}" \
  "callerSpotEndpoint=tcp://127.0.0.1:${caller_spot}" \
  "callerRid=caller"
write_config "${client_config}" \
  "actorHttpEndpoint=${actor_http_endpoint}" \
  "callerHttpEndpoint=${caller_http_endpoint}"

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
  for _ in $(seq 1 300); do
    if python3 - "${endpoint}/health" >/dev/null 2>&1 <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    response.read()
PY
    then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${endpoint}" >&2
  return 1
}

ordered_roles() {
  python3 - "${E2E_START_ORDER}" "$@" <<'PY'
import random
import sys

mode = sys.argv[1]
roles = sys.argv[2:]
if mode in ("", "forward"):
    pass
elif mode == "reverse":
    roles.reverse()
elif mode.startswith("shuffle:"):
    seed_text = mode.split(":", 1)[1]
    if seed_text == "":
        raise SystemExit("E2E_START_ORDER shuffle requires a seed")
    random.Random(int(seed_text)).shuffle(roles)
else:
    raise SystemExit(f"unsupported E2E_START_ORDER={mode!r}")
for role in roles:
    print(role)
PY
}

start_role() {
  case "$1" in
    actor)
      ./Server/Actor/build/install/to-actor-kotlin-actor/bin/to-actor-kotlin-actor \
        --config "${actor_config}" >"${log_dir}/actor.log" 2>&1 &
      pids+=("$!")
      ;;
    caller)
      ./Server/Caller/build/install/to-actor-kotlin-caller/bin/to-actor-kotlin-caller \
        --config "${caller_config}" >"${log_dir}/caller.log" 2>&1 &
      pids+=("$!")
      ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

wait_role() {
  case "$1" in
    actor) wait_http "${actor_http_endpoint}" ;;
    caller) wait_http "${caller_http_endpoint}" ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

wait_log() {
  local log="$1"
  local pattern="$2"
  local description="$3"
  for _ in $(seq 1 300); do
    if [[ -f "${log}" ]] && grep -q "${pattern}" "${log}"; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${description}" >&2
  return 1
}

wait_role_ready() {
  case "$1" in
    actor) wait_log "${log_dir}/actor.log" "\\[boot\\] role=actor step=baselineActors done" "actor baseline readiness" ;;
    caller) wait_log "${log_dir}/caller.log" "\\[boot\\] role=caller step=main run done" "caller application readiness" ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

../../gradlew --no-daemon --gradle-user-home "${ZLINK_KOTLIN_E2E_GRADLE_CACHE:-${HOME}/.cache/zlink/java-e2e/toactor-gradle}" -p . installDist

SERVER_ROLES=(actor caller)
mapfile -t ORDERED_SERVER_ROLES < <(ordered_roles "${SERVER_ROLES[@]}")
for role in "${ORDERED_SERVER_ROLES[@]}"; do
  start_role "$role"
done
for role in "${SERVER_ROLES[@]}"; do
  wait_role "$role"
done
for role in "${SERVER_ROLES[@]}"; do
  wait_role_ready "$role"
done

./Client/build/install/to-actor-kotlin-client/bin/to-actor-kotlin-client \
  --config "${client_config}" > >(tee "${log_dir}/client.log") 2>"${log_dir}/client.stderr.log"
