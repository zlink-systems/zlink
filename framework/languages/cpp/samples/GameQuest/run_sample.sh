#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
CPP_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "$CPP_ROOT/samples/sample-build-common.sh"
zlink_cpp_sample_prepare_build "$CPP_ROOT"
if [[ ! -x "$BIN_DIR/sample_cpp_framework_gamequest_client" && -x "$BIN_DIR/linux-ninja-debug/sample_cpp_framework_gamequest_client" ]]; then
  BIN_DIR="$BIN_DIR/linux-ninja-debug"
fi

PIDS=()
WAIT_ATTEMPTS=300
WAIT_MILLISECONDS=100
WAIT_SECONDS="$(printf '0.%03d' "$WAIT_MILLISECONDS")"
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
        sleep "$WAIT_SECONDS"
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
    zlink_redis_remove_by_id "$REDIS_CONTAINER_NAME" || true
  fi
  rm -rf "$RUN_DIR"
  if [[ "$cleanup_failed" -ne 0 && "$code" -eq 0 ]]; then
    code=1
  fi
  return "$code"
}
trap 'cleanup; status=$?; exit "$status"' EXIT

PORT_ALLOCATION_OUTPUT="$(zlink_sample_allocate_ports 17)"
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
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    if (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep "$WAIT_SECONDS"
  done
  echo "timed out waiting for ${label} at ${endpoint}" >&2
  dump_logs
  return 1
}

dump_logs() {
  for log in "$LOG_DIR"/*.log; do
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
  python3 - "$CONFIG_DIR/$1.json" "$1" "$2" "$3" "$LOG_DIR" \
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
MISSION_A_PID="${PIDS[$(( ${#PIDS[@]} - 1 ))]}"
start_role mission-b "$BIN_DIR/sample_cpp_framework_gamequest_quest_mission" --config="$CONFIG_DIR/mission-b.json"
MISSION_B_PID="${PIDS[$(( ${#PIDS[@]} - 1 ))]}"

line_count () {
  local expected="$1"
  shift
  local count=0
  local log
  for log in "$@"; do
    [[ -f "$log" ]] || continue
    count=$((count + $(grep -Fxc "$expected" "$log" || true)))
  done
  echo "$count"
}

prefix_count () {
  local prefix="$1"
  shift
  local count=0
  local log
  for log in "$@"; do
    [[ -f "$log" ]] || continue
    count=$((count + $(grep -F -c "${prefix}" "$log" || true)))
  done
  echo "$count"
}

wait_for_exact_count () {
  local name="$1"
  local expected="$2"
  local line="$3"
  shift 3
  local actual=0
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    actual="$(line_count "$line" "$@")"
    [[ "$actual" == "$expected" ]] && return 0
    [[ "$actual" -gt "$expected" ]] && break
    sleep "$WAIT_SECONDS"
  done
  echo "expected ${name} exactly ${expected} time(s), found ${actual}" >&2
  dump_logs
  return 1
}

wait_for_prefix_minimum () {
  local name="$1"
  local minimum="$2"
  local prefix="$3"
  shift 3
  local actual=0
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    actual="$(prefix_count "$prefix" "$@")"
    [[ "$actual" -ge "$minimum" ]] && return 0
    sleep "$WAIT_SECONDS"
  done
  echo "expected ${name} at least ${minimum} time(s), found ${actual}" >&2
  dump_logs
  return 1
}

wait_for_prefix_exact_count () {
  local name="$1"
  local expected="$2"
  local prefix="$3"
  shift 3
  local actual=0
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    actual="$(prefix_count "$prefix" "$@")"
    [[ "$actual" == "$expected" ]] && return 0
    [[ "$actual" -gt "$expected" ]] && break
    sleep "$WAIT_SECONDS"
  done
  echo "expected ${name} exactly ${expected} time(s), found ${actual}" >&2
  dump_logs
  return 1
}

remove_pid () {
  local target="$1"
  local remaining=()
  local pid
  for pid in "${PIDS[@]}"; do
    [[ "$pid" == "$target" ]] || remaining+=("$pid")
  done
  PIDS=("${remaining[@]}")
}

wait_for_exact_count "mission-a instance factory readiness" 1 \
  "gamequest-ready kind=instance-factory node=mission-a" "$LOG_DIR/mission-a.log"
wait_for_exact_count "mission-b instance factory readiness" 1 \
  "gamequest-ready kind=instance-factory node=mission-b" "$LOG_DIR/mission-b.log"

start_role api-a "$BIN_DIR/sample_cpp_framework_gamequest_game_api" --config="$CONFIG_DIR/api-a.json"
start_role api-b "$BIN_DIR/sample_cpp_framework_gamequest_game_api" --config="$CONFIG_DIR/api-b.json"
wait_for_exact_count "api-a stream readiness" 1 \
  "gamequest-ready kind=stream node=api-a" "$LOG_DIR/api-a.log"
wait_for_exact_count "api-b stream readiness" 1 \
  "gamequest-ready kind=stream node=api-b" "$LOG_DIR/api-b.log"
wait_for_exact_count "api-a Mission spot route readiness" 1 \
  "gamequest-ready kind=spot-route node=api-a mesh=gamequest" "$LOG_DIR/api-a.log"
wait_for_exact_count "api-b Mission spot route readiness" 1 \
  "gamequest-ready kind=spot-route node=api-b mesh=gamequest" "$LOG_DIR/api-b.log"

OWNER_LOSS_RELEASE_FILE="$RUN_DIR/owner-loss-release"
start_role client "$BIN_DIR/sample_cpp_framework_gamequest_client" \
  --api-a-stream-endpoint "$GAMEQUEST_API_A_STREAM_ENDPOINT" \
  --api-b-stream-endpoint "$GAMEQUEST_API_B_STREAM_ENDPOINT" \
  --api-a-http-url "$GAMEQUEST_API_A_HTTP_URL" \
  --api-b-http-url "$GAMEQUEST_API_B_HTTP_URL" \
  --owner-loss-release-file "$OWNER_LOSS_RELEASE_FILE"
CLIENT_PID="${PIDS[$(( ${#PIDS[@]} - 1 ))]}"

wait_for_exact_count "owner-loss client stage" 1 \
  "gamequest-owner-loss-stage-ready player=player-owner-failure" "$LOG_DIR/client.log"

OWNER_NODE=""
for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
  if [[ "$(line_count "gamequest-owner ready player=player-owner-failure node=mission-a" "$LOG_DIR/mission-a.log")" == "1" ]]; then
    OWNER_NODE="mission-a"
    break
  fi
  if [[ "$(line_count "gamequest-owner ready player=player-owner-failure node=mission-b" "$LOG_DIR/mission-b.log")" == "1" ]]; then
    OWNER_NODE="mission-b"
    break
  fi
  sleep "$WAIT_SECONDS"
done
if [[ -z "$OWNER_NODE" ]]; then
  echo "owner-ready marker was not found for player-owner-failure" >&2
  dump_logs
  exit 1
fi

if [[ "$OWNER_NODE" == "mission-a" ]]; then
  OWNER_PID="$MISSION_A_PID"
else
  OWNER_PID="$MISSION_B_PID"
fi
kill -9 "$OWNER_PID"
set +e
wait "$OWNER_PID"
OWNER_STATUS=$?
set -e
if [[ "$OWNER_STATUS" != "137" ]]; then
  echo "owner Mission process exited with unexpected status ${OWNER_STATUS}" >&2
  exit 1
fi
remove_pid "$OWNER_PID"
: >"$OWNER_LOSS_RELEASE_FILE"

set +e
wait "$CLIENT_PID"
CLIENT_STATUS=$?
set -e
remove_pid "$CLIENT_PID"
if [[ "$CLIENT_STATUS" != "0" ]]; then
  cat "$LOG_DIR/client.log" >&2
  dump_logs
  exit 1
fi

wait_for_exact_count "client self-check completion marker" 1 \
  "gamequest=completed" "$LOG_DIR/client.log"
wait_for_exact_count "client server-evidence completion marker" 1 \
  "gamequest-server-evidence=completed" "$LOG_DIR/client.log"
wait_for_prefix_minimum "Api event routing across nodes" 4 \
  "gamequest-api event-routed player=" "$LOG_DIR/api-a.log" "$LOG_DIR/api-b.log"
wait_for_prefix_minimum "Mission event processing across nodes" 4 \
  "gamequest-mission processed player=" "$LOG_DIR/mission-a.log" "$LOG_DIR/mission-b.log"
wait_for_exact_count "player-alice reconcile" 1 \
  "gamequest-mission reconciled player=player-alice quest=first-hunt" \
  "$LOG_DIR/mission-a.log" "$LOG_DIR/mission-b.log"
wait_for_prefix_exact_count "player-alice replay" 1 \
  "gamequest-mission replayed player=player-alice generation=" \
  "$LOG_DIR/mission-a.log" "$LOG_DIR/mission-b.log"
wait_for_exact_count "owner unavailable" 1 \
  "gamequest-owner unavailable player=player-owner-failure" \
  "$LOG_DIR/api-a.log" "$LOG_DIR/api-b.log"
wait_for_exact_count "replacement handler absence" 0 \
  "gamequest-owner replacement-handler-invoked player=player-owner-failure" \
  "$LOG_DIR/mission-a.log" "$LOG_DIR/mission-b.log"

trap - EXIT
cleanup
echo "gamequest-placement=completed"
