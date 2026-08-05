#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
CPP_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "$CPP_ROOT/samples/sample-build-common.sh"
FLOW_LOG_DIR="${SCRIPT_DIR}/logs"
mkdir -p "$FLOW_LOG_DIR"
rm -f "$FLOW_LOG_DIR"/*.log
zlink_cpp_sample_prepare_build "$CPP_ROOT"
if [[ ! -x "$BIN_DIR/sample_cpp_framework_gamequest_client" && -x "$BIN_DIR/linux-ninja-debug/sample_cpp_framework_gamequest_client" ]]; then
  BIN_DIR="$BIN_DIR/linux-ninja-debug"
fi

PIDS=()
RUN_DIR="$(mktemp -d)"
RUN_ID="$(basename "$RUN_DIR")-$$-${RANDOM}"
LOG_DIR="$RUN_DIR/logs"
REDIS_CONTAINER_NAME=""
mkdir -p "$LOG_DIR"

cleanup() {
  local code=$?
  local cleanup_failed=0
  local status
  for pid in "${PIDS[@]}"; do
    if kill -0 "${pid}" >/dev/null 2>&1; then
      kill "${pid}" >/dev/null 2>&1 || true
      for _ in $(seq 1 300); do
        if ! kill -0 "${pid}" >/dev/null 2>&1; then
          break
        fi
        sleep 0.1
      done
      if kill -0 "${pid}" >/dev/null 2>&1; then
        echo "forced cleanup process ${pid}" >&2
        kill -9 "${pid}" >/dev/null 2>&1 || true
        cleanup_failed=1
      fi
    fi
    set +e
    wait "${pid}" 2>/dev/null
    status=$?
    set -e
    if [[ "$status" != "0" && "$status" != "127" && "$status" != "130" && "$status" != "143" ]]; then
      echo "cleanup process ${pid} exited unexpectedly with status ${status}" >&2
      cleanup_failed=1
    fi
  done
  if [[ -n "$REDIS_CONTAINER_NAME" ]]; then
    docker rm -fv "$REDIS_CONTAINER_NAME" >/dev/null 2>&1 || true
  fi
  rm -rf "$RUN_DIR"
  if [[ "$cleanup_failed" -ne 0 && "$code" -eq 0 ]]; then
    code=1
  fi
  return "$code"
}
trap 'cleanup; status=$?; exit "$status"' EXIT

PORT_ALLOCATION_OUTPUT="$(python3 - <<'PY'
import socket
sockets = []
try:
    for _ in range(17):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
    print(" ".join(str(sock.getsockname()[1]) for sock in sockets))
except OSError as error:
    print(f"SOCKETLESS {error.errno}:{error.strerror}")
finally:
    for sock in sockets:
        sock.close()
PY
)"
if [[ "$PORT_ALLOCATION_OUTPUT" == SOCKETLESS* ]]; then
  echo "Failed to allocate local TCP ports for the GameQuest sample: ${PORT_ALLOCATION_OUTPUT#SOCKETLESS }" >&2
  exit 1
fi
read -r GAMEQUEST_RESERVED_PORT GAMEQUEST_API_A_STREAM_PORT GAMEQUEST_API_B_STREAM_PORT GAMEQUEST_API_A_HTTP_PORT GAMEQUEST_API_B_HTTP_PORT GAMEQUEST_MISSION_A_ROUTE_PORT GAMEQUEST_MISSION_B_ROUTE_PORT GAMEQUEST_MISSION_A_SPOT_ROUTE_PORT GAMEQUEST_MISSION_B_SPOT_ROUTE_PORT GAMEQUEST_MISSION_A_SPOT_ROUTER_PORT GAMEQUEST_MISSION_B_SPOT_ROUTER_PORT GAMEQUEST_MISSION_A_SPOT_PORT GAMEQUEST_MISSION_B_SPOT_PORT GAMEQUEST_API_A_SPOT_ROUTER_PORT GAMEQUEST_API_B_SPOT_ROUTER_PORT GAMEQUEST_API_A_SPOT_ROUTE_PORT GAMEQUEST_API_B_SPOT_ROUTE_PORT <<<"$PORT_ALLOCATION_OUTPUT"
if [[ -z "$GAMEQUEST_RESERVED_PORT" || -z "$GAMEQUEST_API_B_SPOT_ROUTER_PORT" ]]; then
  echo "Failed to allocate local TCP ports for the GameQuest sample." >&2
  echo "This environment may block local socket creation." >&2
  exit 1
fi

cmake --build "$BUILD_DIR" --parallel 2 --target \
  sample_cpp_framework_gamequest_game_api \
  sample_cpp_framework_gamequest_quest_mission \
  sample_cpp_framework_gamequest_client >/dev/null

zlink_redis_start_scoped_assign REDIS_CONTAINER_NAME redis_port \
  "zlink-redis-cpp-sample-gamequest" "redis:7-alpine"
GAMEQUEST_REDIS_ENDPOINT="tcp://127.0.0.1:${redis_port}"
GAMEQUEST_REDIS_KEY_PREFIX_BASE="gamequest:cpp:"
GAMEQUEST_REDIS_KEY_PREFIX="${GAMEQUEST_REDIS_KEY_PREFIX_BASE%:}:${RUN_ID}:"
GAMEQUEST_API_A_STREAM_ENDPOINT="tcp://127.0.0.1:${GAMEQUEST_API_A_STREAM_PORT}"
GAMEQUEST_API_B_STREAM_ENDPOINT="tcp://127.0.0.1:${GAMEQUEST_API_B_STREAM_PORT}"
GAMEQUEST_API_A_HTTP_URL="http://127.0.0.1:${GAMEQUEST_API_A_HTTP_PORT}"
GAMEQUEST_API_B_HTTP_URL="http://127.0.0.1:${GAMEQUEST_API_B_HTTP_PORT}"
GAMEQUEST_MISSION_A_ROUTE_ENDPOINT="tcp://127.0.0.1:${GAMEQUEST_MISSION_A_ROUTE_PORT}"
GAMEQUEST_MISSION_B_ROUTE_ENDPOINT="tcp://127.0.0.1:${GAMEQUEST_MISSION_B_ROUTE_PORT}"
GAMEQUEST_MISSION_A_SPOT_ROUTE_ENDPOINT="tcp://127.0.0.1:${GAMEQUEST_MISSION_A_SPOT_ROUTE_PORT}"
GAMEQUEST_MISSION_B_SPOT_ROUTE_ENDPOINT="tcp://127.0.0.1:${GAMEQUEST_MISSION_B_SPOT_ROUTE_PORT}"
GAMEQUEST_MISSION_A_SPOT_ROUTER_ENDPOINT="tcp://127.0.0.1:${GAMEQUEST_MISSION_A_SPOT_ROUTER_PORT}"
GAMEQUEST_MISSION_B_SPOT_ROUTER_ENDPOINT="tcp://127.0.0.1:${GAMEQUEST_MISSION_B_SPOT_ROUTER_PORT}"
GAMEQUEST_MISSION_A_SPOT_ENDPOINT="tcp://127.0.0.1:${GAMEQUEST_MISSION_A_SPOT_PORT}"
GAMEQUEST_MISSION_B_SPOT_ENDPOINT="tcp://127.0.0.1:${GAMEQUEST_MISSION_B_SPOT_PORT}"
GAMEQUEST_API_A_SPOT_ROUTE="tcp://127.0.0.1:${GAMEQUEST_API_A_SPOT_ROUTE_PORT}"
GAMEQUEST_API_B_SPOT_ROUTE="tcp://127.0.0.1:${GAMEQUEST_API_B_SPOT_ROUTE_PORT}"
GAMEQUEST_API_A_SPOT_ROUTER_ENDPOINT="tcp://127.0.0.1:${GAMEQUEST_API_A_SPOT_ROUTER_PORT}"
GAMEQUEST_API_B_SPOT_ROUTER_ENDPOINT="tcp://127.0.0.1:${GAMEQUEST_API_B_SPOT_ROUTER_PORT}"

port_of() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  endpoint="${endpoint#http://}"
  echo "${endpoint##*:}"
}

wait_port() {
  local label="$1"
  local endpoint="$2"
  local port
  port="$(port_of "$endpoint")"
  for _ in $(seq 1 150); do
    if (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for ${label} at ${endpoint}" >&2
  dump_logs
  return 1
}

dump_logs() {
  for log in "$LOG_DIR"/*.log "$FLOW_LOG_DIR"/flow-*.log; do
    if [[ -f "$log" ]]; then
      echo "===== ${log}" >&2
      cat "$log" >&2
    fi
  done
}

start_role() {
  local name="$1"
  shift
  stdbuf -oL -eL "$@" >"$LOG_DIR/${name}.log" 2>&1 &
  PIDS+=("$!")
}

wait_port redis "$GAMEQUEST_REDIS_ENDPOINT"

CONFIG_DIR="$RUN_DIR/config"
mkdir -p "$CONFIG_DIR"

# 각 role은 자기 설정 파일 하나만 받는다(공통 정책 sample-e2e-configuration-policy.ko.md §2.1).
write_role_config() {
  python3 - "$CONFIG_DIR/$1.json" "$1" "$2" "$3" "$FLOW_LOG_DIR" \
    "$GAMEQUEST_REDIS_ENDPOINT" "$GAMEQUEST_REDIS_KEY_PREFIX" \
    "$GAMEQUEST_API_A_STREAM_ENDPOINT" "$GAMEQUEST_API_B_STREAM_ENDPOINT" \
    "$GAMEQUEST_API_A_HTTP_URL" "$GAMEQUEST_API_B_HTTP_URL" \
    "$GAMEQUEST_MISSION_A_ROUTE_ENDPOINT" "$GAMEQUEST_MISSION_B_ROUTE_ENDPOINT" \
    "$GAMEQUEST_MISSION_A_SPOT_ROUTE_ENDPOINT" "$GAMEQUEST_MISSION_B_SPOT_ROUTE_ENDPOINT" \
    "$GAMEQUEST_MISSION_A_SPOT_ROUTER_ENDPOINT" "$GAMEQUEST_MISSION_B_SPOT_ROUTER_ENDPOINT" \
    "$GAMEQUEST_MISSION_A_SPOT_ENDPOINT" "$GAMEQUEST_MISSION_B_SPOT_ENDPOINT" \
    "$GAMEQUEST_API_A_SPOT_ROUTER_ENDPOINT" "$GAMEQUEST_API_B_SPOT_ROUTER_ENDPOINT" \
    "$GAMEQUEST_API_A_SPOT_ROUTE" "$GAMEQUEST_API_B_SPOT_ROUTE" <<'CONFIG_PY'
import json
import os
import stat
import sys

(path, role_name, api_name, mission_name, flow_log_dir, redis_endpoint,
 redis_key_prefix, api_a_stream, api_b_stream, api_a_http, api_b_http,
 mission_a_route, mission_b_route, mission_a_spot_route, mission_b_spot_route,
 mission_a_spot_router, mission_b_spot_router, mission_a_spot, mission_b_spot,
 api_a_spot_router, api_b_spot_router, api_a_spot_route, api_b_spot_route) = sys.argv[1:]

document = {
    "sample": {
        "role": {"name": role_name, "logDir": flow_log_dir},
        "topology": {
            "redisEndpoint": redis_endpoint,
            "redisKeyPrefix": redis_key_prefix,
            "apiAStreamEndpoint": api_a_stream,
            "apiBStreamEndpoint": api_b_stream,
            "apiAHttpUrl": api_a_http,
            "apiBHttpUrl": api_b_http,
            "missionARouteEndpoint": mission_a_route,
            "missionBRouteEndpoint": mission_b_route,
            "missionASpotRouteEndpoint": mission_a_spot_route,
            "missionBSpotRouteEndpoint": mission_b_spot_route,
            "missionASpotRouterEndpoint": mission_a_spot_router,
            "missionBSpotRouterEndpoint": mission_b_spot_router,
            "missionASpotEndpoint": mission_a_spot,
            "missionBSpotEndpoint": mission_b_spot,
            "apiASpotRouterEndpoint": api_a_spot_router,
            "apiBSpotRouterEndpoint": api_b_spot_router,
            "apiName": api_name,
            "missionName": mission_name,
            "apiASpotRouteEndpoint": api_a_spot_route,
            "apiBSpotRouteEndpoint": api_b_spot_route,
        },
    }
}

with open(path, "w", encoding="utf-8") as file:
    json.dump(document, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
CONFIG_PY
}

write_role_config mission-a api-a mission-a
write_role_config mission-b api-a mission-b
write_role_config api-a api-a mission-a
write_role_config api-b api-b mission-a

start_role mission-a "$BIN_DIR/sample_cpp_framework_gamequest_quest_mission" --config="$CONFIG_DIR/mission-a.json"
start_role mission-b "$BIN_DIR/sample_cpp_framework_gamequest_quest_mission" --config="$CONFIG_DIR/mission-b.json"
start_role api-a "$BIN_DIR/sample_cpp_framework_gamequest_game_api" --config="$CONFIG_DIR/api-a.json"
start_role api-b "$BIN_DIR/sample_cpp_framework_gamequest_game_api" --config="$CONFIG_DIR/api-b.json"

wait_port mission-a-route "$GAMEQUEST_MISSION_A_SPOT_ROUTE_ENDPOINT"
wait_port mission-b-route "$GAMEQUEST_MISSION_B_SPOT_ROUTE_ENDPOINT"
wait_port api-a-route "$GAMEQUEST_API_A_SPOT_ROUTE"
wait_port api-b-route "$GAMEQUEST_API_B_SPOT_ROUTE"
wait_port api-a-stream "$GAMEQUEST_API_A_STREAM_ENDPOINT"
wait_port api-a-http "$GAMEQUEST_API_A_HTTP_URL"
wait_port api-b-stream "$GAMEQUEST_API_B_STREAM_ENDPOINT"
wait_port api-b-http "$GAMEQUEST_API_B_HTTP_URL"

wait_route_ready() {
  local api_url="$1"
  local target_rid="$2"
  for _ in $(seq 1 150); do
    if curl --connect-timeout 0.2 --max-time 0.5 -fsS \
      "$api_url/ready?targetRid=${target_rid}" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  curl --connect-timeout 0.2 --max-time 1 -sS \
    "$api_url/ready?targetRid=${target_rid}" >&2 || true
  echo "timed out waiting for GameQuest RouteMesh peer ${target_rid} from ${api_url}" >&2
  dump_logs
  return 1
}

# TCP listen readiness does not imply RouteMesh peer admission. Wait until both API
# nodes can route to both QuestMission owners before issuing gameplay messages.
wait_route_ready "$GAMEQUEST_API_A_HTTP_URL" "gamequest-mission-a-spot"
wait_route_ready "$GAMEQUEST_API_A_HTTP_URL" "gamequest-mission-b-spot"
wait_route_ready "$GAMEQUEST_API_A_HTTP_URL" "gamequest-api-b-spot"
wait_route_ready "$GAMEQUEST_API_B_HTTP_URL" "gamequest-mission-a-spot"
wait_route_ready "$GAMEQUEST_API_B_HTTP_URL" "gamequest-mission-b-spot"
wait_route_ready "$GAMEQUEST_API_B_HTTP_URL" "gamequest-api-a-spot"

echo "topology=ready"

"$BIN_DIR/sample_cpp_framework_gamequest_client" \
  --api-a-stream-endpoint "$GAMEQUEST_API_A_STREAM_ENDPOINT" \
  --api-b-stream-endpoint "$GAMEQUEST_API_B_STREAM_ENDPOINT" \
  --api-a-http-url "$GAMEQUEST_API_A_HTTP_URL" \
  --api-b-http-url "$GAMEQUEST_API_B_HTTP_URL" >"$LOG_DIR/client.log" 2>&1 || {
  cat "$LOG_DIR/client.log" >&2
  dump_logs
  exit 1
}

grep -q "gamequest-server-evidence=completed" "$LOG_DIR/client.log"
grep -q "gamequest=completed" "$LOG_DIR/client.log"
grep -q "gamequest api event routed" "$LOG_DIR/api-a.log"
grep -q "gamequest api event routed" "$LOG_DIR/api-b.log"
grep -q "gamequest mission processed" "$LOG_DIR/mission-a.log"
grep -q "gamequest mission processed" "$LOG_DIR/mission-b.log"
grep -Rq "message flow" "$FLOW_LOG_DIR"
grep -q "message flow" "$FLOW_LOG_DIR/flow-api-a.log"
grep -q "message flow" "$FLOW_LOG_DIR/flow-api-b.log"
grep -q "message flow" "$FLOW_LOG_DIR/flow-mission-a.log"
grep -q "message flow" "$FLOW_LOG_DIR/flow-mission-b.log"

echo "PASS GameQuest.Cpp"
echo "gamequest sample result=passed"
