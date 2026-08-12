#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CPP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$CPP_DIR/build"
source "$SCRIPT_DIR/../redis-common.sh"
zlink_cpp_e2e_acquire_run_lock "${BASH_SOURCE[0]}" "$@"
zlink_cpp_e2e_install_cleanup_trap

SCENARIO="${1:-all}"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$RUN_ID"
CONFIG_DIR="$LOG_DIR/config"
mkdir -p "$CONFIG_DIR"

ROLE_BIN="$BUILD_DIR/zlink_cpp_e2e_channel_egress_role"
CLIENT_BIN="$BUILD_DIR/zlink_cpp_e2e_channel_egress_client"
PIDS=()
LAST_PID=""
REDIS_CONTAINER=""
REDIS_CONTAINER_OWNED=0

cleanup() {
  local code=$?
  set +e
  for pid in "${PIDS[@]:-}"; do
    kill -TERM "$pid" >/dev/null 2>&1 || true
  done
  for pid in "${PIDS[@]:-}"; do
    wait "$pid" >/dev/null 2>&1 || true
  done
  rm -rf -- "$CONFIG_DIR"
  if [[ -n "$REDIS_CONTAINER" && "$REDIS_CONTAINER_OWNED" == "1" ]]; then
    zlink_redis_remove_by_id "$REDIS_CONTAINER" || true
  fi
  if [[ "$code" != "0" ]]; then
    echo "ChannelEgressRouting failed; logs=$LOG_DIR" >&2
    for log in "$LOG_DIR"/*.stderr.log; do
      [[ -f "$log" ]] || continue
      echo "===== $log =====" >&2
      tail -n 120 "$log" >&2 || true
    done
  fi
  exit "$code"
}
trap cleanup EXIT

zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-cpp-e2e-channel-egress" "redis:7-alpine"
REDIS_CONTAINER_OWNED=1
REDIS_ENDPOINT="127.0.0.1:${redis_port}"
REDIS_KEY_PREFIX="zlink:cpp:e2e:channel-egress:${RUN_ID}:"

VCPKG_PREFIX="$CPP_DIR/build/linux-ninja-vcpkg-debug/vcpkg_installed/x64-linux"
if [[ ! -f "$VCPKG_PREFIX/share/protobuf/protobuf-config.cmake" ]]; then
  echo "C++ framework dependency prefix is missing: $VCPKG_PREFIX" >&2
  exit 1
fi
cmake -S "$CPP_DIR" -B "$BUILD_DIR" \
  -Dprotobuf_DIR="$VCPKG_PREFIX/share/protobuf" \
  -Dabsl_DIR="$VCPKG_PREFIX/share/absl" \
  -Dutf8_range_DIR="$VCPKG_PREFIX/share/utf8_range" \
  -Dhiredis_DIR="$VCPKG_PREFIX/share/hiredis" \
  -Dlibuv_DIR="$VCPKG_PREFIX/share/libuv" \
  -Dredis++_DIR="$VCPKG_PREFIX/share/redis++" \
  -DCMAKE_PREFIX_PATH="$VCPKG_PREFIX" >/dev/null
cmake --build "$BUILD_DIR" --parallel 2 --target \
  zlink_cpp_e2e_channel_egress_role \
  zlink_cpp_e2e_channel_egress_client >/dev/null

alloc_port() {
  zlink_cpp_e2e_allocate_ports 1
}

wait_http() {
  local endpoint="$1"
  for _ in $(seq 1 200); do
    if curl --silent --show-error --max-time 1 "$endpoint/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.05
  done
  echo "Timed out waiting for $endpoint" >&2
  return 1
}

stop_role() {
  local endpoint="$1"
  curl --silent --show-error --max-time 2 -X POST "$endpoint/shutdown" >/dev/null 2>&1 || true
}

write_role_config() {
  local path="$1" role="$2" rid="$3" http_endpoint="$4" game_endpoint="$5" \
    audit_endpoint="${6:-}" workflow_endpoint="${7:-}" game_peers="${8:-}" audit_peers="${9:-}" \
    game_servers="${10:-}" game_clients="${11:-}" audit_servers="${12:-}" audit_clients="${13:-}" \
    workflow_servers="${14:-}" workflow_clients="${15:-}" invalid_mode="${16:-}" weight="${17:-100}" \
    hold_timeout_ms="${18:-0}"
  python3 - "$path" "$role" "$rid" "$http_endpoint" "$game_endpoint" "$audit_endpoint" \
    "$workflow_endpoint" "$game_peers" "$audit_peers" "$game_servers" "$game_clients" \
    "$audit_servers" "$audit_clients" "$workflow_servers" "$workflow_clients" "$invalid_mode" \
    "$weight" "$hold_timeout_ms" "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

(path, role, rid, http_endpoint, game_endpoint, audit_endpoint, workflow_endpoint,
 game_peers, audit_peers, game_servers, game_clients, audit_servers, audit_clients,
 workflow_servers, workflow_clients, invalid_mode, weight, hold_timeout_ms,
 redis_endpoint, redis_key_prefix, log_dir) = sys.argv[1:]
value = {"e2e": {
    "role": role, "rid": rid, "httpEndpoint": http_endpoint,
    "gameEndpoint": game_endpoint, "auditEndpoint": audit_endpoint,
    "workflowEndpoint": workflow_endpoint, "gamePeers": game_peers,
    "auditPeers": audit_peers, "gameServers": game_servers,
    "gameClients": game_clients, "auditServers": audit_servers,
    "auditClients": audit_clients, "workflowServers": workflow_servers,
    "workflowClients": workflow_clients, "invalidMode": invalid_mode,
    "workflowWeight": weight, "holdTimeoutMs": hold_timeout_ms, "instanceMarker": rid,
    "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
    "logDir": log_dir, "evidenceFile": os.path.join(log_dir, rid + ".evidence.jsonl")
}}
with open(path, "w", encoding="utf-8") as output:
    json.dump(value, output)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

write_client_config() {
  local path="$1" scenario="$2" session_url="$3" play_url="$4" audit_url="$5" caller_url="$6" \
    workflow_a_url="$7" workflow_b_url="$8" negative_url="$9" api_a_url="${10}" api_b_url="${11}" \
    spot_id="${12:-}" listener_url="${13:-}" expected_workflow_rid="${14:-}" \
    expected_workflow_lifecycle="${15:-}"
  python3 - "$path" "$scenario" "$session_url" "$play_url" "$audit_url" "$caller_url" \
    "$workflow_a_url" "$workflow_b_url" "$negative_url" "$api_a_url" "$api_b_url" "$spot_id" \
    "$listener_url" "$expected_workflow_rid" "$expected_workflow_lifecycle" <<'PY'
import json
import os
import stat
import sys

(path, scenario, session_url, play_url, audit_url, caller_url, workflow_a_url,
 workflow_b_url, negative_url, api_a_url, api_b_url, spot_id, listener_url,
 expected_workflow_rid, expected_workflow_lifecycle) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as output:
    json.dump({"e2e": {
        "scenario": scenario, "sessionUrl": session_url, "playUrl": play_url,
        "auditUrl": audit_url, "callerUrl": caller_url, "workflowAUrl": workflow_a_url,
        "workflowBUrl": workflow_b_url, "negativeUrl": negative_url,
        "apiAUrl": api_a_url, "apiBUrl": api_b_url, "spotId": spot_id,
        "listenerUrl": listener_url, "expectedWorkflowRid": expected_workflow_rid,
        "expectedWorkflowLifecycle": expected_workflow_lifecycle
    }}, output)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

start_role() {
  local config="$1" name="$2" endpoint="$3"
  "$ROLE_BIN" --config="$config" >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  PIDS+=("$!")
  LAST_PID="${PIDS[${#PIDS[@]}-1]}"
  wait_http "$endpoint"
}

run_client() {
  "$CLIENT_BIN" --config="$1" >>"$LOG_DIR/client.stdout.log" 2>>"$LOG_DIR/client.stderr.log"
}

run_ch01() {
  local session_http="http://127.0.0.1:$(alloc_port)"
  local play_http="http://127.0.0.1:$(alloc_port)"
  local session_game="tcp://127.0.0.1:$(alloc_port)"
  local play_game="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/session.json" session session "$session_http" "$session_game" "" "" \
    "$play_game" "" "game.session" "game.play" "" "" "" "" "" 100
  write_role_config "$CONFIG_DIR/play.json" play play "$play_http" "$play_game" "" "" \
    "$session_game" "" "game.play" "game.session" "" "" "" "" "" 100
  start_role "$CONFIG_DIR/session.json" session "$session_http"
  start_role "$CONFIG_DIR/play.json" play "$play_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-01 "$session_http" "$play_http" "" \
    "$session_http" "" "" "" "" "" ""
  run_client "$CONFIG_DIR/client.json"
  stop_role "$session_http"
  stop_role "$play_http"
}

run_ch02() {
  local client_scenario="${1:-CH-E2E-02}"
  local session_http="http://127.0.0.1:$(alloc_port)"
  local play_http="http://127.0.0.1:$(alloc_port)"
  local audit_http="http://127.0.0.1:$(alloc_port)"
  local workflow_a_http="http://127.0.0.1:$(alloc_port)"
  local workflow_b_http="http://127.0.0.1:$(alloc_port)"
  local session_game="tcp://127.0.0.1:$(alloc_port)"
  local play_game="tcp://127.0.0.1:$(alloc_port)"
  local play_audit="tcp://127.0.0.1:$(alloc_port)"
  local audit_game="tcp://127.0.0.1:$(alloc_port)"
  local workflow_a_port="tcp://127.0.0.1:$(alloc_port)"
  local workflow_b_port="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/session.json" session session "$session_http" "$session_game" "" "" \
    "$play_game" "" "game.session" "game.play" "" "" "" "" "" 100
  write_role_config "$CONFIG_DIR/play.json" play play "$play_http" "$play_game" "$play_audit" "" \
    "$session_game" "$audit_game" "game.play" "game.session" "" "audit.record" "" "workflow.command" "" 100
  write_role_config "$CONFIG_DIR/audit.json" audit audit "$audit_http" "" "$audit_game" "" \
    "" "$play_audit" "" "" "audit.record" "" "" "" "" 100
  write_role_config "$CONFIG_DIR/workflow-a.json" workflow-a workflow-a "$workflow_a_http" "" "" \
    "$workflow_a_port" "" "" "" "" "" "" "" "workflow.command" "" 100
  write_role_config "$CONFIG_DIR/workflow-b.json" workflow-b workflow-b "$workflow_b_http" "" "" \
    "$workflow_b_port" "" "" "" "" "" "" "" "workflow.command" "" 100
  start_role "$CONFIG_DIR/session.json" session "$session_http"
  start_role "$CONFIG_DIR/play.json" play "$play_http"
  start_role "$CONFIG_DIR/audit.json" audit "$audit_http"
  start_role "$CONFIG_DIR/workflow-a.json" workflow-a "$workflow_a_http"
  start_role "$CONFIG_DIR/workflow-b.json" workflow-b "$workflow_b_http"
  write_client_config "$CONFIG_DIR/client.json" "$client_scenario" "$session_http" "$play_http" "$audit_http" \
    "$session_http" "$workflow_a_http" "$workflow_b_http" "" "" "" ""
  run_client "$CONFIG_DIR/client.json"
  stop_role "$session_http"
  stop_role "$play_http"
  stop_role "$audit_http"
  stop_role "$workflow_a_http"
  stop_role "$workflow_b_http"
}

run_ch03() {
  local play_http="http://127.0.0.1:$(alloc_port)"
  local caller_http="http://127.0.0.1:$(alloc_port)"
  local workflow_http="http://127.0.0.1:$(alloc_port)"
  local play_game="tcp://127.0.0.1:$(alloc_port)"
  local caller_game="tcp://127.0.0.1:$(alloc_port)"
  local workflow_port="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/play.json" play play "$play_http" "$play_game" "" "" \
    "$caller_game" "" "game.play" "" "" "" "" "workflow.command" "" 100
  write_role_config "$CONFIG_DIR/caller.json" spot-caller spot-caller "$caller_http" "$caller_game" "" "" \
    "$play_game" "" "" "" "" "" "" "workflow.command" "" 100
  write_role_config "$CONFIG_DIR/workflow.json" workflow-a workflow-a "$workflow_http" "" "" \
    "$workflow_port" "" "" "" "" "" "" "workflow.command" "" "" 100
  start_role "$CONFIG_DIR/play.json" play "$play_http"
  start_role "$CONFIG_DIR/caller.json" caller "$caller_http"
  start_role "$CONFIG_DIR/workflow.json" workflow "$workflow_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-03 "" "$play_http" "" "$caller_http" \
    "$workflow_http" "" "" "" "" "ch-03-spot"
  run_client "$CONFIG_DIR/client.json"
  stop_role "$play_http"
  stop_role "$caller_http"
  stop_role "$workflow_http"
}

run_ch04a() {
  local caller_http="http://127.0.0.1:$(alloc_port)"
  local workflow_a_http="http://127.0.0.1:$(alloc_port)"
  local workflow_b_http="http://127.0.0.1:$(alloc_port)"
  local workflow_a_port="tcp://127.0.0.1:$(alloc_port)"
  local workflow_b_port="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/workflow-a.json" workflow-a workflow-a "$workflow_a_http" "" "" \
    "$workflow_a_port" "" "" "" "" "" "" "workflow.command" "" "" 100
  write_role_config "$CONFIG_DIR/workflow-b.json" workflow-b workflow-b "$workflow_b_http" "" "" \
    "$workflow_b_port" "" "" "" "" "" "" "workflow.command" "" "" 300
  write_role_config "$CONFIG_DIR/caller.json" caller caller "$caller_http" "" "" "" \
    "" "" "" "" "" "" "" "workflow.command" "" 100
  start_role "$CONFIG_DIR/workflow-a.json" workflow-a "$workflow_a_http"
  start_role "$CONFIG_DIR/workflow-b.json" workflow-b "$workflow_b_http"
  start_role "$CONFIG_DIR/caller.json" caller "$caller_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-04A "" "" "" "$caller_http" \
    "$workflow_a_http" "$workflow_b_http" "" "" "" "" "" "" ""
  run_client "$CONFIG_DIR/client.json"
  stop_role "$caller_http"
  stop_role "$workflow_a_http"
  stop_role "$workflow_b_http"
}

run_ch04b() {
  local caller_http="http://127.0.0.1:$(alloc_port)"
  local workflow_a_http="http://127.0.0.1:$(alloc_port)"
  local workflow_b_http="http://127.0.0.1:$(alloc_port)"
  local workflow_a_port="tcp://127.0.0.1:$(alloc_port)"
  local workflow_b_port="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/workflow-a.json" workflow-a workflow-a "$workflow_a_http" "" "" \
    "$workflow_a_port" "" "" "" "" "" "" "workflow.command" "" "" 100 2000
  write_role_config "$CONFIG_DIR/workflow-b.json" workflow-b workflow-b "$workflow_b_http" "" "" \
    "$workflow_b_port" "" "" "" "" "" "" "workflow.command" "" "" 100
  write_role_config "$CONFIG_DIR/caller.json" caller caller "$caller_http" "" "" "" \
    "" "" "" "" "" "" "" "workflow.command" "" 100
  start_role "$CONFIG_DIR/workflow-a.json" workflow-a "$workflow_a_http"
  start_role "$CONFIG_DIR/workflow-b.json" workflow-b "$workflow_b_http"
  start_role "$CONFIG_DIR/caller.json" caller "$caller_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-04B "" "" "" "$caller_http" \
    "$workflow_a_http" "$workflow_b_http" "" "" "" "" "" "" ""
  run_client "$CONFIG_DIR/client.json"
  stop_role "$caller_http"
  stop_role "$workflow_a_http"
  stop_role "$workflow_b_http"
}

run_ch04c() {
  local caller_http="http://127.0.0.1:$(alloc_port)"
  local workflow_http="http://127.0.0.1:$(alloc_port)"
  local workflow_port="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/workflow.json" workflow-a workflow-a "$workflow_http" "" "" \
    "$workflow_port" "" "" "" "" "" "" "workflow.command" "" "" 100
  write_role_config "$CONFIG_DIR/caller.json" caller caller "$caller_http" "" "" "" \
    "" "" "" "" "" "" "" "workflow.command" "" 100
  start_role "$CONFIG_DIR/workflow.json" workflow-a "$workflow_http"
  local workflow_pid="$LAST_PID"
  start_role "$CONFIG_DIR/caller.json" caller "$caller_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-04C "" "" "" "$caller_http" \
    "$workflow_http" "" "" "" "" "" "" "" ""
  run_client "$CONFIG_DIR/client.json"
  stop_role "$workflow_http"
  set +e
  wait "$workflow_pid"
  set -e
  write_role_config "$CONFIG_DIR/workflow.json" workflow-a workflow-a-restart "$workflow_http" "" "" \
    "$workflow_port" "" "" "" "" "" "" "workflow.command" "" "" 100
  start_role "$CONFIG_DIR/workflow.json" workflow-a-restart "$workflow_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-04C "" "" "" "$caller_http" \
    "$workflow_http" "" "" "" "" "" "" workflow-a-restart workflow-a-restart
  run_client "$CONFIG_DIR/client.json"
  stop_role "$caller_http"
  stop_role "$workflow_http"
}

run_ch05() {
  local caller_http="http://127.0.0.1:$(alloc_port)"
  local workflow_http="http://127.0.0.1:$(alloc_port)"
  local workflow_port="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/workflow.json" workflow-a workflow-a "$workflow_http" "" "" \
    "$workflow_port" "" "" "" "" "" "" "workflow.command" "" "" 100
  write_role_config "$CONFIG_DIR/caller.json" caller caller "$caller_http" "" "" "" \
    "" "" "" "" "" "" "" "workflow.command" "" 100
  start_role "$CONFIG_DIR/workflow.json" workflow-a "$workflow_http"
  start_role "$CONFIG_DIR/caller.json" caller "$caller_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-05 "" "" "" "$caller_http" \
    "$workflow_http" "" "$workflow_http" "" "" "" "" "" ""
  run_client "$CONFIG_DIR/client.json"
  stop_role "$workflow_http"
  write_client_config "$CONFIG_DIR/client.json" CPP-CONTRACT-ROLE-001 "" "" "" "$caller_http" \
    "" "" "" "" "" "" "" "" ""
  run_client "$CONFIG_DIR/client.json"
  stop_role "$caller_http"
}

run_ch06() {
  local negative_http="http://127.0.0.1:$(alloc_port)"
  local negative_port="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/negative.json" negative negative "$negative_http" "" "" \
    "$negative_port" "" "" "" "" "" "" "workflow.command" "workflow.command" \
    duplicate-workflow-client 100
  "$ROLE_BIN" --config="$CONFIG_DIR/negative.json" \
    >"$LOG_DIR/negative.stdout.log" 2>"$LOG_DIR/negative.stderr.log" &
  local negative_pid="$!"
  PIDS+=("$negative_pid")
  local exited=0
  for _ in $(seq 1 200); do
    if curl --silent --show-error --max-time 1 "$negative_http/health" >/dev/null 2>&1; then
      echo "CH-E2E-06 duplicate-registration process unexpectedly became healthy" >&2
      return 1
    fi
    if ! kill -0 "$negative_pid" >/dev/null 2>&1; then
      exited=1
      break
    fi
    sleep 0.05
  done
  if [[ "$exited" == "0" ]]; then
    kill -KILL "$negative_pid" >/dev/null 2>&1 || true
  fi
  set +e
  wait "$negative_pid"
  local code=$?
  set -e
  if [[ "$code" == "0" ]]; then
    echo "CH-E2E-06 duplicate-registration process exited successfully" >&2
    return 1
  fi
}

run_ch07a() {
  local caller_http="http://127.0.0.1:$(alloc_port)"
  local game_endpoint="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/caller.json" caller caller "$caller_http" "$game_endpoint" "" "" \
    "" "" "" "" "" "" "" "" "" 100
  start_role "$CONFIG_DIR/caller.json" caller "$caller_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-07A "" "" "" "$caller_http" \
    "" "" "" "" "" "" "" "" ""
  run_client "$CONFIG_DIR/client.json"
  stop_role "$caller_http"
}

run_ch07b() {
  local api_a_http="http://127.0.0.1:$(alloc_port)"
  local api_b_http="http://127.0.0.1:$(alloc_port)"
  local api_a_game="tcp://127.0.0.1:$(alloc_port)"
  local api_b_game="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/api-a.json" api-a api-a "$api_a_http" "$api_a_game" "" "" \
    "$api_b_game" "" "game.api" "" "" "" "" "" "" 100
  write_role_config "$CONFIG_DIR/api-b.json" api-b api-b "$api_b_http" "$api_b_game" "" "" \
    "$api_a_game" "" "game.api" "" "" "" "" "" "" 100
  start_role "$CONFIG_DIR/api-a.json" api-a "$api_a_http"
  start_role "$CONFIG_DIR/api-b.json" api-b "$api_b_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-07B "" "" "" "" \
    "" "" "" "$api_a_http" "$api_b_http" "" "" "" ""
  run_client "$CONFIG_DIR/client.json"
  stop_role "$api_a_http"
  stop_role "$api_b_http"
}

run_ch07c() {
  local caller_http="http://127.0.0.1:$(alloc_port)"
  local workflow_http="http://127.0.0.1:$(alloc_port)"
  local workflow_port_number="$(alloc_port)"
  local workflow_port="tcp://127.0.0.1:$workflow_port_number"
  write_role_config "$CONFIG_DIR/workflow.json" workflow-a workflow-a "$workflow_http" "" "" \
    "$workflow_port" "" "" "" "" "" "" "workflow.command" "" "" 100
  write_role_config "$CONFIG_DIR/caller.json" caller caller "$caller_http" "" "" "" \
    "" "" "" "" "" "" "" "workflow.command" "" 100
  start_role "$CONFIG_DIR/workflow.json" workflow-a "$workflow_http"
  local workflow_pid="$LAST_PID"
  start_role "$CONFIG_DIR/caller.json" caller "$caller_http"
  curl --silent --show-error --max-time 10 -X POST "$caller_http/request" \
    -H 'content-type: application/json' \
    --data '{"channel":"workflow.command","id":"ch-07c-ready"}' >/dev/null
  kill -STOP "$workflow_pid"
  ss -K dst 127.0.0.1 dport = "$workflow_port_number" >/dev/null 2>&1 || true
  sleep 0.2
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-07C "" "" "" "$caller_http" \
    "$workflow_http" "" "" "" "" "" "" "" ""
  if run_client "$CONFIG_DIR/client.json"; then
    kill -CONT "$workflow_pid" >/dev/null 2>&1 || true
  else
    local code=$?
    kill -CONT "$workflow_pid" >/dev/null 2>&1 || true
    return "$code"
  fi
  stop_role "$caller_http"
  stop_role "$workflow_http"
}

run_ch09() {
  local session_http="http://127.0.0.1:$(alloc_port)"
  local play_http="http://127.0.0.1:$(alloc_port)"
  local workflow_http="http://127.0.0.1:$(alloc_port)"
  local caller_http="http://127.0.0.1:$(alloc_port)"
  local play_game="tcp://127.0.0.1:0"
  local session_game="tcp://127.0.0.1:0"
  local workflow_port="tcp://127.0.0.1:0"
  write_role_config "$CONFIG_DIR/play.json" play play "$play_http" "$play_game" "" "" \
    "" "" "game.play" "" "" "" "" "" "" 100
  start_role "$CONFIG_DIR/play.json" play "$play_http"
  local play_endpoint
  play_endpoint="$(curl --silent --show-error --max-time 5 \
    "$play_http/listener-status?kind=route_mesh&name=channel-egress-game" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["endpoint"])')"
  write_role_config "$CONFIG_DIR/session.json" session session "$session_http" "$session_game" "" "" \
    "$play_endpoint" "" "game.session" "game.play" "" "" "" "" "" 100
  write_role_config "$CONFIG_DIR/workflow.json" workflow-a workflow-a "$workflow_http" "" "" \
    "$workflow_port" "" "" "" "" "" "" "workflow.command" "" "" 100
  write_role_config "$CONFIG_DIR/caller.json" caller caller "$caller_http" "" "" "" \
    "" "" "" "" "" "" "" "workflow.command" "" 100
  start_role "$CONFIG_DIR/session.json" session "$session_http"
  start_role "$CONFIG_DIR/workflow.json" workflow-a "$workflow_http"
  start_role "$CONFIG_DIR/caller.json" caller "$caller_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-09 "$session_http" "$play_http" "" "$caller_http" \
    "$workflow_http" "" "" "" "" "" "$play_http" "" ""
  run_client "$CONFIG_DIR/client.json"
  stop_role "$session_http"
  stop_role "$play_http"
  stop_role "$caller_http"
  stop_role "$workflow_http"
}

run_ch10() {
  local caller_http="http://127.0.0.1:$(alloc_port)"
  local workflow_http="http://127.0.0.1:$(alloc_port)"
  local workflow_port="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/workflow.json" workflow-a workflow-a "$workflow_http" "" "" \
    "$workflow_port" "" "" "" "" "" "" "workflow.command" "" "" 100
  write_role_config "$CONFIG_DIR/caller.json" caller caller "$caller_http" "" "" "" \
    "" "" "" "" "" "" "" "workflow.command" "" 100
  start_role "$CONFIG_DIR/workflow.json" workflow-a "$workflow_http"
  start_role "$CONFIG_DIR/caller.json" caller "$caller_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-10 "" "" "" "$caller_http" \
    "$workflow_http" "" "" "" "" "" "" "" ""
  run_client "$CONFIG_DIR/client.json"
  stop_role "$caller_http"
  stop_role "$workflow_http"
}

run_ch11() {
  local session_http="http://127.0.0.1:$(alloc_port)"
  local api_a_http="http://127.0.0.1:$(alloc_port)"
  local api_b_http="http://127.0.0.1:$(alloc_port)"
  local session_game="tcp://127.0.0.1:$(alloc_port)"
  local api_a_game="tcp://127.0.0.1:$(alloc_port)"
  local api_b_game="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/session.json" session session "$session_http" "$session_game" "" "" \
    "$api_a_game,$api_b_game" "" "game.session" "game.api" "" "" "" "" "" 100
  write_role_config "$CONFIG_DIR/api-a.json" api-a api-a "$api_a_http" "$api_a_game" "" "" \
    "$session_game,$api_b_game" "" "game.api" "" "" "" "" "" "" 100
  write_role_config "$CONFIG_DIR/api-b.json" api-b api-b "$api_b_http" "$api_b_game" "" "" \
    "$session_game,$api_a_game" "" "game.api" "" "" "" "" "" "" 100
  start_role "$CONFIG_DIR/session.json" session "$session_http"
  start_role "$CONFIG_DIR/api-a.json" api-a "$api_a_http"
  start_role "$CONFIG_DIR/api-b.json" api-b "$api_b_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-11 "$session_http" "" "" "" \
    "" "" "" "$api_a_http" "$api_b_http" "" "" "" ""
  run_client "$CONFIG_DIR/client.json"
  stop_role "$session_http"
  stop_role "$api_a_http"
  stop_role "$api_b_http"
}

run_ch12() {
  local workflow_a_http="http://127.0.0.1:$(alloc_port)"
  local workflow_b_http="http://127.0.0.1:$(alloc_port)"
  local workflow_a_port="tcp://127.0.0.1:$(alloc_port)"
  local workflow_b_port="tcp://127.0.0.1:$(alloc_port)"
  write_role_config "$CONFIG_DIR/workflow-a.json" workflow-a workflow-a "$workflow_a_http" "" "" \
    "$workflow_a_port" "" "" "" "" "" "" "workflow.command" "workflow.command" "" 100
  write_role_config "$CONFIG_DIR/workflow-b.json" workflow-b workflow-b "$workflow_b_http" "" "" \
    "$workflow_b_port" "" "" "" "" "" "" "workflow.command" "" "" 100
  start_role "$CONFIG_DIR/workflow-a.json" workflow-a "$workflow_a_http"
  start_role "$CONFIG_DIR/workflow-b.json" workflow-b "$workflow_b_http"
  write_client_config "$CONFIG_DIR/client.json" CH-E2E-12 "" "" "" "$workflow_a_http" \
    "$workflow_a_http" "$workflow_b_http" "" "" "" "" workflow-a workflow-a
  run_client "$CONFIG_DIR/client.json"
  stop_role "$workflow_a_http"
  stop_role "$workflow_b_http"
}

case "$SCENARIO" in
  CH-E2E-01|CH01) run_ch01 ;;
  CH-E2E-02|CH02) run_ch02 ;;
  CH-E2E-03|CH03) run_ch03 ;;
  CH-E2E-04A|CH04A) run_ch04a ;;
  CH-E2E-04B|CH04B) run_ch04b ;;
  CH-E2E-04C|CH04C) run_ch04c ;;
  CH-E2E-05|CH05) run_ch05 ;;
  CH-E2E-06|CH06) run_ch06 ;;
  CH-E2E-07A|CH07A) run_ch07a ;;
  CH-E2E-07B|CH07B) run_ch07b ;;
  CH-E2E-07C|CH07C) run_ch07c ;;
  CH-E2E-08|CH08) run_ch02 CH-E2E-08 ;;
  CH-E2E-09|CH09) run_ch09 ;;
  CH-E2E-10|CH10) run_ch10 ;;
  CH-E2E-11|CH11) run_ch11 ;;
  CH-E2E-12|CH12) run_ch12 ;;
  *) echo "scenario not implemented in C++ ChannelEgressRouting runner: $SCENARIO" >&2; exit 2 ;;
esac

echo "ChannelEgressRouting PASS scenario=$SCENARIO logs=$LOG_DIR"
