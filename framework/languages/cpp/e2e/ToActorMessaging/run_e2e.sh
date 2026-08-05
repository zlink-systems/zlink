#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../redis-common.sh"
BUILD_DIR="$SCRIPT_DIR/../../build"
SCENARIO="all"
E2E_START_ORDER="forward"
REDIS_ENDPOINT=""
REDIS_CONTAINER=""
for argument in "$@"; do
  case "$argument" in
    --scenario=*) SCENARIO="${argument#*=}" ;;
    --redis-endpoint=*) REDIS_ENDPOINT="${argument#*=}" ;;
    --redis-container=*) REDIS_CONTAINER="${argument#*=}" ;;
    --start-order=*) E2E_START_ORDER="${argument#*=}" ;;
    --*) echo "Unknown ToActorMessaging runner option: $argument" >&2; exit 2 ;;
    *)
      if [[ "$SCENARIO" == "all" ]]; then
        SCENARIO="$argument"
      else
        SCENARIO="$SCENARIO,$argument"
      fi
      ;;
  esac
done
SCENARIO="${SCENARIO#,}"
SCENARIO="${SCENARIO// /,}"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$RUN_ID"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
ROUTE_SETTLE_SECONDS=5
SCENARIO_SETTLE_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
LOCAL_READINESS_ATTEMPTS="$(
  python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"
pids=()
REDIS_CONTAINER_OWNED=0

mkdir -p "$LOG_DIR"
echo "log_dir=$LOG_DIR"
echo "start_order=$E2E_START_ORDER"

cmake -S "$SCRIPT_DIR/../.." -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" --target \
  zlink_cpp_e2e_to_actor_messaging_actor \
  zlink_cpp_e2e_to_actor_messaging_caller \
  zlink_cpp_e2e_to_actor_messaging_session \
  zlink_cpp_e2e_to_actor_messaging_client >/dev/null

LOCATION_KEY_PREFIX="zlink:e2e:toactor:$RUN_ID"
ACTOR_RID="actor-a"
ACTOR_B_RID="actor-b"
CALLER_RID="caller"
SESSION_A_RID="session-a"
SESSION_B_RID="session-b"

reserve_ports() {
  python3 - <<'PY'
import socket
sockets = []
ports = []
try:
    for _ in range(17):
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

read -r actor_http caller_http actor_spot caller_spot actor_pub caller_pub \
  session_a_http session_a_stream session_a_spot session_a_pub \
  session_b_http session_b_stream session_b_spot session_b_pub \
  actor_b_http actor_b_spot actor_b_pub < <(reserve_ports)
ACTOR_HTTP="http://127.0.0.1:${actor_http}"
CALLER_HTTP="http://127.0.0.1:${caller_http}"
ACTOR_SPOT="tcp://127.0.0.1:${actor_spot}"
CALLER_SPOT="tcp://127.0.0.1:${caller_spot}"
ACTOR_PUBSUB="tcp://127.0.0.1:${actor_pub}"
ACTOR_B_HTTP="http://127.0.0.1:${actor_b_http}"
ACTOR_B_SPOT="tcp://127.0.0.1:${actor_b_spot}"
ACTOR_B_PUBSUB="tcp://127.0.0.1:${actor_b_pub}"
CALLER_PUBSUB="tcp://127.0.0.1:${caller_pub}"
SESSION_A_HTTP="http://127.0.0.1:${session_a_http}"
SESSION_A_STREAM="tcp://127.0.0.1:${session_a_stream}"
SESSION_A_SPOT="tcp://127.0.0.1:${session_a_spot}"
SESSION_A_PUBSUB="tcp://127.0.0.1:${session_a_pub}"
SESSION_B_HTTP="http://127.0.0.1:${session_b_http}"
SESSION_B_STREAM="tcp://127.0.0.1:${session_b_stream}"
SESSION_B_SPOT="tcp://127.0.0.1:${session_b_spot}"
SESSION_B_PUBSUB="tcp://127.0.0.1:${session_b_pub}"

wait_tcp() {
  local host="$1"
  local port="$2"
  local name="$3"
  if python3 - "$host" "$port" "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
timeout = float(sys.argv[3])
poll = float(sys.argv[4])
deadline = time.monotonic() + timeout
while time.monotonic() < deadline:
    try:
        with socket.create_connection((host, port), timeout=1):
            sys.exit(0)
    except OSError:
        time.sleep(poll)
sys.exit(1)
PY
  then
    return 0
  fi
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name at $host:$port" >&2
  return 1
}

if [[ -n "$REDIS_CONTAINER" && -n "$REDIS_ENDPOINT" ]]; then
  echo "redis endpoint=$REDIS_ENDPOINT (existing owned container $REDIS_CONTAINER)"
elif [[ -n "$REDIS_ENDPOINT" ]]; then
  echo "External Redis endpoint is not supported by the C++ ToActorMessaging e2e runner." >&2
  exit 2
else
  zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
    "zlink-redis-cpp-e2e-toactormessaging" "redis:7-alpine"
  REDIS_CONTAINER_OWNED=1
  REDIS_ENDPOINT="127.0.0.1:${redis_port}"
  echo "redis endpoint=$REDIS_ENDPOINT (container $REDIS_CONTAINER)"
fi
REDIS_HOST="${REDIS_ENDPOINT%:*}"
REDIS_TCP_PORT="${REDIS_ENDPOINT##*:}"
wait_tcp "$REDIS_HOST" "$REDIS_TCP_PORT" redis
echo "redis key prefix=$LOCATION_KEY_PREFIX"

CONFIG_DIR="$LOG_DIR/config"
mkdir -p "$CONFIG_DIR"
python3 - "$CONFIG_DIR/actor.json" "$REDIS_ENDPOINT" "$LOCATION_KEY_PREFIX" "$LOG_DIR" \
  "$ACTOR_RID" "$ACTOR_HTTP" "$ACTOR_SPOT" "$ACTOR_PUBSUB" "$CALLER_RID" "$CALLER_SPOT" <<'PY'
import json
import os
import stat
import sys

path, redis_endpoint, key_prefix, log_dir, node_rid, http_endpoint, spot_endpoint, pub_sub_endpoint, peer_rid, peer_spot = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"redis": {"endpoint": redis_endpoint, "keyPrefix": key_prefix},
                       "logDir": log_dir, "nodeRid": node_rid,
                       "httpEndpoint": http_endpoint, "spotEndpoint": spot_endpoint,
                       "pubSubEndpoint": pub_sub_endpoint, "callerRid": peer_rid,
                       "callerSpotEndpoint": peer_spot}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
python3 - "$CONFIG_DIR/actor-b.json" "$REDIS_ENDPOINT" "$LOCATION_KEY_PREFIX" "$LOG_DIR" \
  "$ACTOR_B_RID" "$ACTOR_B_HTTP" "$ACTOR_B_SPOT" "$ACTOR_B_PUBSUB" "$CALLER_RID" "$CALLER_SPOT" <<'PY'
import json
import os
import stat
import sys

path, redis_endpoint, key_prefix, log_dir, node_rid, http_endpoint, spot_endpoint, pub_sub_endpoint, peer_rid, peer_spot = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"redis": {"endpoint": redis_endpoint, "keyPrefix": key_prefix},
                       "logDir": log_dir, "nodeRid": node_rid,
                       "httpEndpoint": http_endpoint, "spotEndpoint": spot_endpoint,
                       "pubSubEndpoint": pub_sub_endpoint, "callerRid": peer_rid,
                       "callerSpotEndpoint": peer_spot}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
python3 - "$CONFIG_DIR/caller.json" "$REDIS_ENDPOINT" "$LOCATION_KEY_PREFIX" "$LOG_DIR" \
  "$CALLER_RID" "$CALLER_HTTP" "$CALLER_SPOT" "$CALLER_PUBSUB" \
  "$ACTOR_RID" "$ACTOR_SPOT" "$ACTOR_B_RID" "$ACTOR_B_SPOT" <<'PY'
import json
import os
import stat
import sys

path, redis_endpoint, key_prefix, log_dir, node_rid, http_endpoint, spot_endpoint, pub_sub_endpoint, peer_rid, peer_spot, peer_b_rid, peer_b_spot = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"redis": {"endpoint": redis_endpoint, "keyPrefix": key_prefix},
                       "logDir": log_dir, "nodeRid": node_rid,
                       "httpEndpoint": http_endpoint, "spotEndpoint": spot_endpoint,
                       "pubSubEndpoint": pub_sub_endpoint, "actorRid": peer_rid,
                       "actorSpotEndpoint": peer_spot, "actorBRid": peer_b_rid,
                       "actorBSpotEndpoint": peer_b_spot}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
write_session_config() {
  local path="$1"
  local node_rid="$2"
  local http_endpoint="$3"
  local stream_endpoint="$4"
  local spot_endpoint="$5"
  local pub_sub_endpoint="$6"
  python3 - "$path" "$REDIS_ENDPOINT" "$LOCATION_KEY_PREFIX" "$LOG_DIR" \
    "$node_rid" "$http_endpoint" "$stream_endpoint" "$spot_endpoint" "$pub_sub_endpoint" \
    "$ACTOR_RID" "$ACTOR_SPOT" <<'PY'
import json
import os
import stat
import sys

path, redis_endpoint, key_prefix, log_dir, node_rid, http_endpoint, stream_endpoint, spot_endpoint, pub_sub_endpoint, actor_rid, actor_spot = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"redis": {"endpoint": redis_endpoint, "keyPrefix": key_prefix},
                       "logDir": log_dir, "nodeRid": node_rid,
                       "httpEndpoint": http_endpoint, "streamEndpoint": stream_endpoint,
                       "spotEndpoint": spot_endpoint, "pubSubEndpoint": pub_sub_endpoint,
                       "actorRid": actor_rid, "actorSpotEndpoint": actor_spot}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}
write_session_config "$CONFIG_DIR/session-a.json" "$SESSION_A_RID" "$SESSION_A_HTTP" \
  "$SESSION_A_STREAM" "$SESSION_A_SPOT" "$SESSION_A_PUBSUB"
write_session_config "$CONFIG_DIR/session-b.json" "$SESSION_B_RID" "$SESSION_B_HTTP" \
  "$SESSION_B_STREAM" "$SESSION_B_SPOT" "$SESSION_B_PUBSUB"

print_logs() {
  local status="$1"
  if [[ "$status" == "0" ]]; then
    return
  fi
  for log in "$LOG_DIR"/*.log; do
    [[ -f "$log" ]] || continue
    echo "===== $log =====" >&2
    tail -n 200 "$log" >&2 || true
  done
}

cleanup() {
  local status="$?"
  local cleanup_failed=0
  local wait_status
  set +e
  print_logs "$status"
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    kill "${pids[$i]}" >/dev/null 2>&1 || true
  done
  for pid in "${pids[@]:-}"; do
    wait "$pid" >/dev/null 2>&1
    wait_status=$?
    if [[ "$wait_status" != "0" && "$wait_status" != "127" && "$wait_status" != "130" && "$wait_status" != "143" ]]; then
      echo "cleanup process $pid exited unexpectedly with status $wait_status" >&2
      cleanup_failed=1
    fi
  done
  if [[ -n "$REDIS_CONTAINER" && "$REDIS_CONTAINER_OWNED" == "1" ]]; then
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  fi
  rm -rf "${CONFIG_DIR:-}"
  if [[ "$cleanup_failed" -ne 0 && "$status" -eq 0 ]]; then
    status=1
  fi
  exit "$status"
}
trap cleanup EXIT

ordered_roles() {
  python3 - "$E2E_START_ORDER" "$@" <<'PY'
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

wait_http() {
  local endpoint="$1"
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    if python3 - "${endpoint}/health" "$HTTP_PROBE_TIMEOUT_SECONDS" >/dev/null 2>&1 <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=float(sys.argv[2])) as response:
    response.read()
PY
    then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $endpoint" >&2
  return 1
}

start_role() {
  case "$1" in
    actor-a)
      "$BUILD_DIR/zlink_cpp_e2e_to_actor_messaging_actor" \
        --config="$CONFIG_DIR/actor.json" >"$LOG_DIR/actor.stdout.log" 2>"$LOG_DIR/actor.stderr.log" &
      pids+=("$!")
      ;;
    actor-b)
      "$BUILD_DIR/zlink_cpp_e2e_to_actor_messaging_actor" \
        --config="$CONFIG_DIR/actor-b.json" >"$LOG_DIR/actor-b.stdout.log" 2>"$LOG_DIR/actor-b.stderr.log" &
      pids+=("$!")
      ;;
    caller)
      "$BUILD_DIR/zlink_cpp_e2e_to_actor_messaging_caller" \
        --config="$CONFIG_DIR/caller.json" >"$LOG_DIR/caller.stdout.log" 2>"$LOG_DIR/caller.stderr.log" &
      pids+=("$!")
      ;;
    session-a|session-b)
      "$BUILD_DIR/zlink_cpp_e2e_to_actor_messaging_session" \
        --config="$CONFIG_DIR/$1.json" >"$LOG_DIR/$1.stdout.log" 2>"$LOG_DIR/$1.stderr.log" &
      pids+=("$!")
      ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

wait_role() {
  case "$1" in
    actor-a) wait_http "$ACTOR_HTTP" ;;
    actor-b) wait_http "$ACTOR_B_HTTP" ;;
    caller) wait_http "$CALLER_HTTP" ;;
    session-a) wait_http "$SESSION_A_HTTP" ;;
    session-b) wait_http "$SESSION_B_HTTP" ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

mapfile -t ORDERED_SERVER_ROLES < <(ordered_roles actor-a actor-b caller session-a session-b)
for role in "${ORDERED_SERVER_ROLES[@]}"; do
  start_role "$role"
done
for role in actor-a actor-b caller session-a session-b; do
  wait_role "$role"
done

"$BUILD_DIR/zlink_cpp_e2e_to_actor_messaging_client" \
  --actor-http="$ACTOR_HTTP" --caller-http="$CALLER_HTTP" \
  --actor-b-http="$ACTOR_B_HTTP" --route-control-http="$CALLER_HTTP" \
  --session-a-http="$SESSION_A_HTTP" --session-a-stream="$SESSION_A_STREAM" \
  --session-b-http="$SESSION_B_HTTP" --session-b-stream="$SESSION_B_STREAM" \
  --scenario="$SCENARIO" \
  > >(tee "$LOG_DIR/client.log") 2>"$LOG_DIR/client.stderr.log"
