#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../redis-common.sh"
CPP_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$CPP_ROOT/build}"

if [[ "$#" -eq 0 ]]; then
  SCENARIO="all"
else
  SCENARIO="$*"
  SCENARIO="${SCENARIO// /,}"
fi

RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/logs/$RUN_ID"
CONFIG_DIR="$LOG_DIR/config"
mkdir -p "$LOG_DIR"
mkdir -p "$CONFIG_DIR"

ACTOR_NODE_BIN="$BUILD_DIR/zlink_cpp_e2e_spot_actor_transfer_actor_node"
SESSION_BIN="$BUILD_DIR/zlink_cpp_e2e_spot_actor_transfer_session"
CLIENT_BIN="$BUILD_DIR/zlink_cpp_e2e_spot_actor_transfer_client"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
ROUTE_SETTLE_SECONDS=5
SCENARIO_SETTLE_SECONDS=3
REDIS_READINESS_TIMEOUT_SECONDS=60
HTTP_PROBE_TIMEOUT_SECONDS=3
SHUTDOWN_GRACE_SECONDS=0.5

pick_ports() {
  python3 - "$1" <<'PY'
import socket
import secrets
import sys
sockets = []
ports = []
candidates = list(range(10000, 30000))
secrets.SystemRandom().shuffle(candidates)
for port in candidates:
    try:
        current = socket.socket()
        current.bind(("127.0.0.1", port))
    except OSError:
        current.close()
        continue
    sockets.append(current)
    ports.append(port)
    if len(ports) == int(sys.argv[1]):
        break
if len(ports) != int(sys.argv[1]):
    raise SystemExit(f"cannot reserve {sys.argv[1]} listener ports")
print(" ".join(str(port) for port in ports))
for current in sockets:
    current.close()
PY
}

pids=()
REDIS_CONTAINER=""

cleanup() {
  local code=$?
  for pid in "${pids[@]:-}"; do
    kill -- "-$pid" >/dev/null 2>&1 || kill "$pid" >/dev/null 2>&1 || true
  done
  sleep "$SHUTDOWN_GRACE_SECONDS"
  for pid in "${pids[@]:-}"; do
    kill -KILL -- "-$pid" >/dev/null 2>&1 || kill -KILL "$pid" >/dev/null 2>&1 || true
  done
  wait "${pids[@]:-}" >/dev/null 2>&1 || true
  if [[ -n "$REDIS_CONTAINER" ]]; then
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  fi
  rm -rf "$CONFIG_DIR"
  if [[ "$code" -ne 0 ]]; then
    echo "E2E failed. Logs: $LOG_DIR" >&2
  fi
}
trap cleanup EXIT

wait_health() {
  local url="$1"
  local name="$2"
  local attempts
  attempts="$(
    python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys
print(max(1, math.ceil(float(sys.argv[1]) / float(sys.argv[2]))))
PY
  )"
  for _ in $(seq 1 "$attempts"); do
    if curl --max-time "$HTTP_PROBE_TIMEOUT_SECONDS" \
      --connect-timeout "$HTTP_PROBE_TIMEOUT_SECONDS" \
      -fsS "$url/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name at $url" >&2
  return 1
}

start_role() {
  local role="$1"
  local rid="$2"
  local url="$3"
  local router="$4"
  local stream="$5"
  local pub="$6"
  local binary
  if [[ "$role" == "actor" ]]; then
    binary="$ACTOR_NODE_BIN"
  else
    binary="$SESSION_BIN"
  fi
  local config_path="$CONFIG_DIR/$rid.json"
  python3 - "$config_path" "$rid" "$url" "$router" "$stream" "$pub" \
    "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

(path, rid, http_url, router, stream, pub, redis_endpoint,
 redis_key_prefix, log_dir) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"initialActorNode": "actor-a",
        "rid": rid, "httpUrl": http_url, "routerEndpoint": router,
        "streamEndpoint": stream, "pubEndpoint": pub,
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "logDir": log_dir, "evidenceFile": f"{log_dir}/{rid}.evidence.log"}},
        file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  setsid "$binary" --config="$config_path" \
    >"$LOG_DIR/${rid}.stdout.log" 2>"$LOG_DIR/${rid}.stderr.log" &
  pids+=("$!")
}

run_client() {
  local scenario="$1"
  local config_path="$CONFIG_DIR/client-${scenario//[^a-zA-Z0-9_-]/_}.json"
  python3 - "$config_path" "$NODE_A_URL" "$NODE_B_URL" "$SESSION_A_STREAM" \
    "$SESSION_B_STREAM" "$scenario" <<'PY'
import json
import os
import stat
import sys

path, node_a_url, node_b_url, node_a_stream, node_b_stream, scenario = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"nodeAUrl": node_a_url, "nodeBUrl": node_b_url,
        "nodeAStream": node_a_stream, "nodeBStream": node_b_stream,
        "scenario": scenario}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$CLIENT_BIN" --config="$config_path" \
    >>"$LOG_DIR/client.stdout.log" 2>>"$LOG_DIR/client.stderr.log"
}

zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-cpp-e2e-spot-actor-transfer" "redis:7-alpine"
REDIS_ENDPOINT="127.0.0.1:${redis_port}"
zlink_redis_wait_ready "$REDIS_CONTAINER" "$REDIS_READINESS_TIMEOUT_SECONDS"
REDIS_KEY_PREFIX="zlink:cpp-e2e:spot-actor-transfer:$(date +%s)-$$"

read -r NODE_A_HTTP_PORT NODE_B_HTTP_PORT SESSION_A_HTTP_PORT SESSION_B_HTTP_PORT \
  NODE_A_ROUTER_PORT NODE_B_ROUTER_PORT SESSION_A_ROUTER_PORT SESSION_B_ROUTER_PORT \
  NODE_A_STREAM_PORT NODE_B_STREAM_PORT SESSION_A_STREAM_PORT SESSION_B_STREAM_PORT \
  NODE_A_PUB_PORT NODE_B_PUB_PORT SESSION_A_PUB_PORT SESSION_B_PUB_PORT \
  < <(pick_ports 16)
NODE_A_PUB="tcp://127.0.0.1:$NODE_A_PUB_PORT"
NODE_B_PUB="tcp://127.0.0.1:$NODE_B_PUB_PORT"
SESSION_A_PUB="tcp://127.0.0.1:$SESSION_A_PUB_PORT"
SESSION_B_PUB="tcp://127.0.0.1:$SESSION_B_PUB_PORT"
NODE_A_URL="http://127.0.0.1:$NODE_A_HTTP_PORT"
NODE_B_URL="http://127.0.0.1:$NODE_B_HTTP_PORT"
SESSION_A_URL="http://127.0.0.1:$SESSION_A_HTTP_PORT"
SESSION_B_URL="http://127.0.0.1:$SESSION_B_HTTP_PORT"
NODE_A_ROUTER="tcp://127.0.0.1:$NODE_A_ROUTER_PORT"
NODE_B_ROUTER="tcp://127.0.0.1:$NODE_B_ROUTER_PORT"
SESSION_A_ROUTER="tcp://127.0.0.1:$SESSION_A_ROUTER_PORT"
SESSION_B_ROUTER="tcp://127.0.0.1:$SESSION_B_ROUTER_PORT"
NODE_A_STREAM="tcp://127.0.0.1:$NODE_A_STREAM_PORT"
NODE_B_STREAM="tcp://127.0.0.1:$NODE_B_STREAM_PORT"
SESSION_A_STREAM="tcp://127.0.0.1:$SESSION_A_STREAM_PORT"
SESSION_B_STREAM="tcp://127.0.0.1:$SESSION_B_STREAM_PORT"

echo "log_dir=$LOG_DIR"

start_role actor actor-a "$NODE_A_URL" "$NODE_A_ROUTER" "$NODE_A_STREAM" "$NODE_A_PUB"
wait_health "$NODE_A_URL" actor-a
if [[ "$SCENARIO" != "ST-A1" && "$SCENARIO" != "all" ]]; then
  start_role actor actor-b "$NODE_B_URL" "$NODE_B_ROUTER" "$NODE_B_STREAM" "$NODE_B_PUB"
  wait_health "$NODE_B_URL" actor-b
fi

start_role session session-a "$SESSION_A_URL" "$SESSION_A_ROUTER" "$SESSION_A_STREAM" "$SESSION_A_PUB"
wait_health "$SESSION_A_URL" session-a
start_role session session-b "$SESSION_B_URL" "$SESSION_B_ROUTER" "$SESSION_B_STREAM" "$SESSION_B_PUB"
wait_health "$SESSION_B_URL" session-b

sleep "$ROUTE_SETTLE_SECONDS"

: >"$LOG_DIR/client.stdout.log"
: >"$LOG_DIR/client.stderr.log"

restart_node_a() {
  read -r NODE_A_HTTP_PORT NODE_A_ROUTER_PORT NODE_A_STREAM_PORT NODE_A_PUB_PORT \
    < <(pick_ports 4)
  NODE_A_URL="http://127.0.0.1:$NODE_A_HTTP_PORT"
  NODE_A_ROUTER="tcp://127.0.0.1:$NODE_A_ROUTER_PORT"
  NODE_A_STREAM="tcp://127.0.0.1:$NODE_A_STREAM_PORT"
  NODE_A_PUB="tcp://127.0.0.1:$NODE_A_PUB_PORT"
  start_role actor actor-a "$NODE_A_URL" "$NODE_A_ROUTER" "$NODE_A_STREAM" "$NODE_A_PUB"
  wait_health "$NODE_A_URL" actor-a
  sleep "$ROUTE_SETTLE_SECONDS"
}

if [[ "$SCENARIO" == "all" ]]; then
  # ST-A1 measures an exact zero Relocation Store delta. Run it before any
  # remote relocation can leave asynchronous cleanup activity behind.
  run_client "ST-A1"
  start_role actor actor-b "$NODE_B_URL" "$NODE_B_ROUTER" "$NODE_B_STREAM" "$NODE_B_PUB"
  wait_health "$NODE_B_URL" actor-b
  sleep "$ROUTE_SETTLE_SECONDS"
  # Callback-failure cases intentionally hold transfer boundaries until their
  # caller deadline, so run them before other transfer work occupies the nodes.
  run_client "ST-C3"
  run_client "ST-A2,ST-A3,ST-B1,ST-B3,ST-B4,ST-D1,ST-D2,ST-E1,ST-E2,ST-F1,ST-F2,ST-F3,ST-F4,ST-F5"
  # ST-F6 exercises long in-flight delays (gate hold + 1s late-reply handler);
  # run it in a fresh client process, like the shutdown scenarios below.
  run_client "ST-F6"
  run_client "ST-C2"
  sleep "$SCENARIO_SETTLE_SECONDS"
  restart_node_a
  run_client "ST-B2"
  sleep "$SCENARIO_SETTLE_SECONDS"
  restart_node_a
  run_client "ST-C1"
else
  run_client "$SCENARIO"
fi

cat "$LOG_DIR/client.stdout.log"
