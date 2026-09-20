#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Port selection is a short hand-off because the runner only probes that a port
# is free before the sample processes bind it. Retry only a role startup
# that reports EADDRINUSE; functional sample failures keep their status.
if [[ "${1:-}" != "--zlink-bingo-retry-child" ]]; then
  for attempt in 1 2 3; do
    if bash "$SCRIPT_DIR/run_sample.sh" --zlink-bingo-retry-child "$@"; then
      exit 0
    else
      status=$?
    fi
    if [[ "$status" -ne 75 ]]; then
      exit "$status"
    fi
    echo "Bingo sample startup port collision; retrying with fresh ports (attempt $((attempt + 1))/3)." >&2
  done
  echo "Bingo sample startup port collision persisted after 3 attempts." >&2
  exit 75
fi

source "$SCRIPT_DIR/../redis-common.sh"
source "$SCRIPT_DIR/../sample-build-common.sh"
zlink_cpp_sample_prepare_build
cmake --build "$BUILD_DIR" --parallel 2 --target \
  sample_cpp_framework_bingo_api \
  sample_cpp_framework_bingo_matchmaking \
  sample_cpp_framework_bingo_play \
  sample_cpp_framework_bingo_session \
  sample_cpp_framework_bingo_client \
  $(zlink_cpp_sample_framework_test_targets \
    test_cpp_framework_sample_parity \
    zlink_cpp_framework_mesh_node_vertical_test \
    test_cpp_framework_actor_gateway) >/dev/null

if [[ ! -x "$BIN_DIR/sample_cpp_framework_bingo_api" && -x "$BIN_DIR/linux-ninja-debug/sample_cpp_framework_bingo_api" ]]; then
  BIN_DIR="$BIN_DIR/linux-ninja-debug"
fi

API_BIN="$BIN_DIR/sample_cpp_framework_bingo_api"
MATCHMAKING_BIN="$BIN_DIR/sample_cpp_framework_bingo_matchmaking"
PLAY_BIN="$BIN_DIR/sample_cpp_framework_bingo_play"
SESSION_BIN="$BIN_DIR/sample_cpp_framework_bingo_session"
CLIENT_BIN="$BIN_DIR/sample_cpp_framework_bingo_client"

for binary in "$API_BIN" "$MATCHMAKING_BIN" "$PLAY_BIN" "$SESSION_BIN" "$CLIENT_BIN"; do
  if [[ ! -x "$binary" ]]; then
    echo "Missing executable: $binary" >&2
    echo "CMake build did not produce the expected Bingo sample executable." >&2
    exit 1
  fi
done

zlink_cpp_sample_run_framework_tests \
  'test_cpp_framework_sample_parity|zlink_cpp_framework_mesh_node_vertical_test|test_cpp_framework_actor_gateway'

read -r -a PORTS <<<"$(zlink_sample_allocate_paired_ports 24)"

if [[ ${#PORTS[@]} -lt 24 ]]; then
  echo "Failed to allocate 24 local TCP ports for the Bingo sample." >&2
  echo "This environment may block local socket creation." >&2
  exit 1
fi

API_A_CHANNEL_ENDPOINT="tcp://127.0.0.1:${PORTS[2]}"
PLAY_A_CHANNEL_ENDPOINT="tcp://127.0.0.1:${PORTS[3]}"
SESSION_A_SPOT_ENDPOINT="tcp://127.0.0.1:${PORTS[4]}"
SESSION_A_ROUTER_ENDPOINT="tcp://127.0.0.1:${PORTS[5]}"
SESSION_B_SPOT_ENDPOINT="tcp://127.0.0.1:${PORTS[6]}"
SESSION_B_ROUTER_ENDPOINT="tcp://127.0.0.1:${PORTS[7]}"
PLAY_B_CHANNEL_ENDPOINT="tcp://127.0.0.1:${PORTS[8]}"
PLAY_A_SPOT_ENDPOINT="tcp://127.0.0.1:${PORTS[9]}"
PLAY_A_SPOT_ROUTER_ENDPOINT="tcp://127.0.0.1:${PORTS[10]}"
SESSION_A_STREAM_ENDPOINT="tcp://127.0.0.1:${PORTS[11]}"
SESSION_B_STREAM_ENDPOINT="tcp://127.0.0.1:${PORTS[12]}"
PLAY_B_SPOT_ENDPOINT="tcp://127.0.0.1:${PORTS[13]}"
PLAY_B_SPOT_ROUTER_ENDPOINT="tcp://127.0.0.1:${PORTS[14]}"
API_B_CHANNEL_ENDPOINT="tcp://127.0.0.1:${PORTS[15]}"
PLAY_A_ROUTE_ENDPOINT="tcp://127.0.0.1:${PORTS[0]}"
PLAY_B_ROUTE_ENDPOINT="tcp://127.0.0.1:${PORTS[1]}"
API_A_PLAY_ROUTE_ENDPOINT="tcp://127.0.0.1:${PORTS[16]}"
API_B_PLAY_ROUTE_ENDPOINT="tcp://127.0.0.1:${PORTS[17]}"
API_A_MATCHMAKING_ROUTE_ENDPOINT="tcp://127.0.0.1:${PORTS[18]}"
API_B_MATCHMAKING_ROUTE_ENDPOINT="tcp://127.0.0.1:${PORTS[19]}"
MATCHMAKING_ROUTE_ENDPOINT="tcp://127.0.0.1:${PORTS[20]}"
SESSION_A_PLAY_ROUTE_ENDPOINT="tcp://127.0.0.1:${PORTS[21]}"
SESSION_B_PLAY_ROUTE_ENDPOINT="tcp://127.0.0.1:${PORTS[22]}"

endpoint_host() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  endpoint="${endpoint#http://}"
  echo "${endpoint%:*}"
}

endpoint_port() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  endpoint="${endpoint#http://}"
  echo "${endpoint##*:}"
}

wait_port() {
  local name="$1"
  local endpoint="$2"
  local host
  local port
  host="$(endpoint_host "$endpoint")"
  port="$(endpoint_port "$endpoint")"
  for _ in $(seq 1 600); do
    if (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
      return 0
    fi
    for log in "$LOG_DIR"/*.log; do
      [[ -f "$log" ]] || continue
      if grep -Eq 'errno=98|Address already in use' "$log"; then
        echo "${name} failed because a sample port was already in use: ${log}" >&2
        return 75
      fi
    done
    sleep 0.1
  done
  for log in "$LOG_DIR"/*.log; do
    [[ -f "$log" ]] || continue
    if grep -Eq 'errno=98|Address already in use' "$log"; then
      echo "${name} failed because a sample port was already in use: ${log}" >&2
      return 75
    fi
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}

wait_log_contains() {
  local description="$1"
  local pattern="$2"
  shift 2
  for _ in $(seq 1 300); do
    if grep -Fq "$pattern" "$@" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${description}." >&2
  return 1
}

log_line_count() {
  local expected_line="$1"
  shift
  awk -v expected="$expected_line" 'index($0, expected) { ++count } END { print count + 0 }' \
    "$@" 2>/dev/null
}

wait_log_count() {
  local description="$1"
  local expected_count="$2"
  local expected_line="$3"
  shift 3
  local actual_count
  for _ in $(seq 1 300); do
    actual_count="$(log_line_count "$expected_line" "$@")"
    if [[ "$actual_count" -eq "$expected_count" ]]; then
      return 0
    fi
    if [[ "$actual_count" -gt "$expected_count" ]]; then
      break
    fi
    sleep 0.1
  done
  echo "Expected ${description} exactly ${expected_count} time(s), found ${actual_count}." >&2
  return 1
}

RUN_DIR="$(mktemp -d)"
LOG_DIR="$RUN_DIR/logs"
FLOW_LOG_DIR="$RUN_DIR/flow-logs"
mkdir -p "$LOG_DIR" "$FLOW_LOG_DIR"
PIDS=()
REDIS_CONTAINER=""
cleanup_done=false
BINGO_REDIS_KEY_PREFIX="bingo:cpp:${RANDOM}:$$:"

cleanup() {
  local code=$?
  set +e
  if [[ "$cleanup_done" == true ]]; then
    return
  fi
  cleanup_done=true
  zlink_cpp_sample_stop_processes "${PIDS[@]}"
  if [[ -n "$REDIS_CONTAINER" ]]; then
    zlink_redis_remove_by_id "$REDIS_CONTAINER" || true
  fi
  if [[ "$code" -ne 0 ]]; then
    echo "Bingo sample process logs (failure evidence):" >&2
    for log in "$LOG_DIR"/*.log; do
      [[ -f "$log" ]] || continue
      echo "--- $log" >&2
      sed -n '1,240p' "$log" >&2
    done
  fi
  zlink_sample_close_run_dir "$RUN_DIR" "$code" "Bingo"
  return "$code"
}
trap zlink_cpp_sample_exit_trap EXIT

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run the Bingo sample." >&2
  exit 1
fi
zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-cpp-sample-bingo" "redis:7-alpine"
BINGO_REDIS_ENDPOINT="127.0.0.1:${redis_port}"
wait_port redis "tcp://${BINGO_REDIS_ENDPOINT}"

CONFIG_DIR="$LOG_DIR/config"
mkdir -p "$CONFIG_DIR"

# 각 role은 자기 설정 파일 하나만 받는다(공통 정책 sample-e2e-configuration-policy.ko.md §2.1).
write_role_config() {
  local api_node="$2" play_node="$3" session_node="$4" stream_endpoint="$5"
  local session_spot_endpoint="$6" session_router_endpoint="$7"
  zlink_sample_write_private_file "$CONFIG_DIR/$1.json" <<CONFIG_JSON
{
  "sample": {
    "host": {"keepRunning": true},
    "topology": {
      "logDir": "$FLOW_LOG_DIR",
      "apiNode": "$api_node",
      "playNode": "$play_node",
      "sessionNode": "$session_node",
      "apiChannelEndpoint": "$API_A_CHANNEL_ENDPOINT",
      "apiAChannelEndpoint": "$API_A_CHANNEL_ENDPOINT",
      "apiBChannelEndpoint": "$API_B_CHANNEL_ENDPOINT",
      "playChannelEndpoint": "$PLAY_A_CHANNEL_ENDPOINT",
      "playAChannelEndpoint": "$PLAY_A_CHANNEL_ENDPOINT",
      "playBChannelEndpoint": "$PLAY_B_CHANNEL_ENDPOINT",
      "playARouteEndpoint": "$PLAY_A_ROUTE_ENDPOINT",
      "playBRouteEndpoint": "$PLAY_B_ROUTE_ENDPOINT",
      "apiAPlayRouteEndpoint": "$API_A_PLAY_ROUTE_ENDPOINT",
      "apiBPlayRouteEndpoint": "$API_B_PLAY_ROUTE_ENDPOINT",
      "apiAMatchmakingRouteEndpoint": "$API_A_MATCHMAKING_ROUTE_ENDPOINT",
      "apiBMatchmakingRouteEndpoint": "$API_B_MATCHMAKING_ROUTE_ENDPOINT",
      "matchmakingRouteEndpoint": "$MATCHMAKING_ROUTE_ENDPOINT",
      "playASpotEndpoint": "$PLAY_A_SPOT_ENDPOINT",
      "playBSpotEndpoint": "$PLAY_B_SPOT_ENDPOINT",
      "playASpotRouterEndpoint": "$PLAY_A_SPOT_ROUTER_ENDPOINT",
      "playBSpotRouterEndpoint": "$PLAY_B_SPOT_ROUTER_ENDPOINT",
      "sessionSpotEndpoint": "$session_spot_endpoint",
      "sessionRouterEndpoint": "$session_router_endpoint",
      "streamEndpoint": "$stream_endpoint",
      "sessionAStreamEndpoint": "$SESSION_A_STREAM_ENDPOINT",
      "sessionBStreamEndpoint": "$SESSION_B_STREAM_ENDPOINT",
      "sessionAPlayRouteEndpoint": "$SESSION_A_PLAY_ROUTE_ENDPOINT",
      "sessionBPlayRouteEndpoint": "$SESSION_B_PLAY_ROUTE_ENDPOINT",
      "redisEndpoint": "$BINGO_REDIS_ENDPOINT",
      "redisKeyPrefix": "$BINGO_REDIS_KEY_PREFIX"
    }
  }
}
CONFIG_JSON
}

write_role_config play-a a a a "$SESSION_A_STREAM_ENDPOINT" "$SESSION_A_SPOT_ENDPOINT" "$SESSION_A_ROUTER_ENDPOINT"
write_role_config play-b a b a "$SESSION_A_STREAM_ENDPOINT" "$SESSION_A_SPOT_ENDPOINT" "$SESSION_A_ROUTER_ENDPOINT"
write_role_config api-a a a a "$SESSION_A_STREAM_ENDPOINT" "$SESSION_A_SPOT_ENDPOINT" "$SESSION_A_ROUTER_ENDPOINT"
write_role_config api-b b a a "$SESSION_A_STREAM_ENDPOINT" "$SESSION_A_SPOT_ENDPOINT" "$SESSION_A_ROUTER_ENDPOINT"
write_role_config matchmaking a a a "$SESSION_A_STREAM_ENDPOINT" "$SESSION_A_SPOT_ENDPOINT" "$SESSION_A_ROUTER_ENDPOINT"
write_role_config session-a a a a "$SESSION_A_STREAM_ENDPOINT" "$SESSION_A_SPOT_ENDPOINT" "$SESSION_A_ROUTER_ENDPOINT"
write_role_config session-b a a b "$SESSION_B_STREAM_ENDPOINT" "$SESSION_B_SPOT_ENDPOINT" "$SESSION_B_ROUTER_ENDPOINT"

start_server() {
  local name="$1"
  local binary="$2"
  shift 2
  # Keep application readiness/cleanup markers separate from diagnostic
  # trace. This prevents concurrent stdout/stderr writes from splitting a
  # marker while preserving both streams in the failure bundle.
  stdbuf -oL -eL "$binary" "$@" >"$LOG_DIR/${name}.stdout.log" \
    2>"$LOG_DIR/${name}.trace.log" &
  PIDS+=("$!")
}

start_server matchmaking "$MATCHMAKING_BIN" --config="$CONFIG_DIR/matchmaking.json"
wait_port matchmaking "$MATCHMAKING_ROUTE_ENDPOINT"

start_server api-a "$API_BIN" --config="$CONFIG_DIR/api-a.json"
start_server api-b "$API_BIN" --config="$CONFIG_DIR/api-b.json"
wait_port api-a "$API_A_CHANNEL_ENDPOINT"
wait_port api-b "$API_B_CHANNEL_ENDPOINT"
wait_port api-a-play-route "$API_A_PLAY_ROUTE_ENDPOINT"
wait_port api-b-play-route "$API_B_PLAY_ROUTE_ENDPOINT"
wait_port api-a-matchmaking-route "$API_A_MATCHMAKING_ROUTE_ENDPOINT"
wait_port api-b-matchmaking-route "$API_B_MATCHMAKING_ROUTE_ENDPOINT"

start_server play-a "$PLAY_BIN" --config="$CONFIG_DIR/play-a.json"
start_server play-b "$PLAY_BIN" --config="$CONFIG_DIR/play-b.json"
wait_port play-a-spot-router "$PLAY_A_SPOT_ROUTER_ENDPOINT"
wait_port play-b-spot-router "$PLAY_B_SPOT_ROUTER_ENDPOINT"

start_server session-a "$SESSION_BIN" --config="$CONFIG_DIR/session-a.json"
start_server session-b "$SESSION_BIN" --config="$CONFIG_DIR/session-b.json"
wait_port session-a-stream "$SESSION_A_STREAM_ENDPOINT"
wait_port session-a-play-route "$SESSION_A_PLAY_ROUTE_ENDPOINT"
wait_port session-b-stream "$SESSION_B_STREAM_ENDPOINT"
wait_port session-b-play-route "$SESSION_B_PLAY_ROUTE_ENDPOINT"

# A remote observer User Spot creation may select the other Play node; do not
# start the client until the Play-to-Play peer connection is admitted on both
# sides (spec 07 §7 — a peer is usable only after handshake and admission).
wait_log_contains "play-a peer route readiness" \
  "bingo-ready kind=peer-route node=play-a peer=play-b" "$LOG_DIR/play-a.stdout.log"
wait_log_contains "play-b peer route readiness" \
  "bingo-ready kind=peer-route node=play-b peer=play-a" "$LOG_DIR/play-b.stdout.log"
wait_log_contains "api-a matchmaking route readiness" \
  "bingo-ready kind=mesh-route node=api-a mesh=matchmaking" "$LOG_DIR/api-a.stdout.log"
wait_log_contains "api-a room route readiness" \
  "bingo-ready kind=mesh-route node=api-a mesh=room" "$LOG_DIR/api-a.stdout.log"
wait_log_contains "api-b matchmaking route readiness" \
  "bingo-ready kind=mesh-route node=api-b mesh=matchmaking" "$LOG_DIR/api-b.stdout.log"
wait_log_contains "api-b room route readiness" \
  "bingo-ready kind=mesh-route node=api-b mesh=room" "$LOG_DIR/api-b.stdout.log"
wait_log_contains "session-a room route readiness" \
  "bingo-ready kind=mesh-route node=session-a mesh=room" "$LOG_DIR/session-a.stdout.log"
wait_log_contains "session-b room route readiness" \
  "bingo-ready kind=mesh-route node=session-b mesh=room" "$LOG_DIR/session-b.stdout.log"

"$CLIENT_BIN" \
  --session-a-stream-endpoint "$SESSION_A_STREAM_ENDPOINT" \
  --session-b-stream-endpoint "$SESSION_B_STREAM_ENDPOINT" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.trace.log" || {
  echo "=== bingo client ===" >&2
  for log in "$LOG_DIR"/*.log; do
    [[ -f "$log" ]] && cat "$log" >&2
  done
  exit 1
}

grep -q "bingo=completed" "$LOG_DIR/client.stdout.log"
grep -q "stream-result sample=Bingo client=player1 operation=authenticate" "$LOG_DIR/client.stdout.log"
grep -q "stream-handler sample=Bingo client=player1 message=PlayerJoinedNotify" "$LOG_DIR/client.stdout.log"
PLAY_LOGS=("$LOG_DIR/play-a.stdout.log" "$LOG_DIR/play-b.stdout.log")
SESSION_LOGS=("$LOG_DIR/session-a.stdout.log" "$LOG_DIR/session-b.stdout.log")

wait_log_count "player-1 record fetch" 1 \
  "bingo-record fetched actor=player-1 wins=0 losses=0" "${PLAY_LOGS[@]}"
wait_log_count "player-2 record fetch" 1 \
  "bingo-record fetched actor=player-2 wins=0 losses=0" "${PLAY_LOGS[@]}"
wait_log_count "player-1 record report" 1 \
  "bingo-record reported actor=player-1 wins=1 losses=0" "${PLAY_LOGS[@]}"
wait_log_count "player-2 record report" 1 \
  "bingo-record reported actor=player-2 wins=0 losses=1" "${PLAY_LOGS[@]}"
for actor in player-1 player-2 observer; do
  wait_log_count "${actor} room leave" 1 \
    "bingo-lifecycle room-leave actor=${actor}" "${PLAY_LOGS[@]}"
  wait_log_count "${actor} Entry Spot leave" 1 \
    "bingo-lifecycle entry-leave actor=${actor}" "${PLAY_LOGS[@]}"
done
wait_log_count "player-1 Entry Spot destroy completion" 1 \
  "bingo-lifecycle entry-destroy-complete actor=player-1" "${PLAY_LOGS[@]}"
wait_log_count "player-2 Entry Spot destroy completion" 1 \
  "bingo-lifecycle entry-destroy-complete actor=player-2" "${PLAY_LOGS[@]}"
wait_log_count "player-1 session disconnect" 1 \
  "bingo-lifecycle session-disconnect actor=player-1 destroy=false" "${SESSION_LOGS[@]}"
wait_log_count "player-2 session disconnect" 1 \
  "bingo-lifecycle session-disconnect actor=player-2 destroy=false" "${SESSION_LOGS[@]}"
wait_log_count "observer record report" 0 \
  "bingo-record reported actor=observer" "${PLAY_LOGS[@]}"
wait_log_count "observer Entry Spot destroy completion" 0 \
  "bingo-lifecycle entry-destroy-complete actor=observer" "${PLAY_LOGS[@]}"
grep -Rq "message flow" "$FLOW_LOG_DIR"

cleanup
trap - EXIT
zlink_cpp_sample_assert_graceful_teardown

echo "bingo full client/server self-check completed"
echo "bingo-placement=completed"
