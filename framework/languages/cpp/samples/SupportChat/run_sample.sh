#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
CPP_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "$CPP_ROOT/samples/sample-build-common.sh"
zlink_cpp_sample_prepare_build "$CPP_ROOT"
if [[ ! -x "$BIN_DIR/sample_cpp_framework_supportchat_client" && -x "$BIN_DIR/linux-ninja-debug/sample_cpp_framework_supportchat_client" ]]; then BIN_DIR="$BIN_DIR/linux-ninja-debug"; fi

WAIT_ATTEMPTS=300
WAIT_SECONDS=0.1
PIDS=()
RUN_DIR="$(mktemp -d)"
LOG_DIR="$RUN_DIR/logs"
FLOW_LOG_DIR="$RUN_DIR/flow-logs"
REDIS_CONTAINER_NAME=""
mkdir -p "$LOG_DIR" "$FLOW_LOG_DIR"

cleanup() {
  local code=$? cleanup_failed=0 status
  for pid in "${PIDS[@]}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
      for _ in $(seq 1 "$WAIT_ATTEMPTS"); do kill -0 "$pid" >/dev/null 2>&1 || break; sleep "$WAIT_SECONDS"; done
      if kill -0 "$pid" >/dev/null 2>&1; then kill -9 "$pid" >/dev/null 2>&1 || true; cleanup_failed=1; fi
    fi
    set +e; wait "$pid" 2>/dev/null; status=$?; set -e
    if [[ "$status" != "0" && "$status" != "127" && "$status" != "130" && "$status" != "143" ]]; then cleanup_failed=1; fi
  done
  if [[ -n "$REDIS_CONTAINER_NAME" ]]; then zlink_redis_remove_by_id "$REDIS_CONTAINER_NAME" || true; fi
  rm -rf "$RUN_DIR"
  [[ "$cleanup_failed" -eq 0 || "$code" -ne 0 ]] || code=1
  return "$code"
}
trap 'cleanup; status=$?; exit "$status"' EXIT

read -r -a PORTS <<<"$(zlink_sample_allocate_ports 12)"
API_ROUTE="tcp://127.0.0.1:${PORTS[1]}"
SUPPORT_ROUTE="tcp://127.0.0.1:${PORTS[2]}"
SUPPORT_SPOT_ROUTER="tcp://127.0.0.1:${PORTS[3]}"
SUPPORT_SPOT="tcp://127.0.0.1:${PORTS[4]}"
SESSION_STREAM="tcp://127.0.0.1:${PORTS[5]}"
SESSION_SPOT_ROUTER="tcp://127.0.0.1:${PORTS[6]}"
SESSION_SPOT="tcp://127.0.0.1:${PORTS[7]}"
SUPPORT_HTTP_URL="http://127.0.0.1:${PORTS[8]}"
SESSION_ACTOR_ROUTE="tcp://127.0.0.1:${PORTS[9]}"
SUPPORT_ACTOR_ROUTE="tcp://127.0.0.1:${PORTS[10]}"
API_SPOT_ROUTE="tcp://127.0.0.1:${PORTS[11]}"

cmake --build "$BUILD_DIR" --parallel 2 --target sample_cpp_framework_supportchat_api sample_cpp_framework_supportchat_session sample_cpp_framework_supportchat_support sample_cpp_framework_supportchat_client >/dev/null
zlink_redis_start_scoped_assign REDIS_CONTAINER_NAME redis_port "zlink-redis-cpp-sample-supportchat" "redis:7-alpine"
REDIS_ENDPOINT="tcp://127.0.0.1:${redis_port}"
REDIS_KEY_PREFIX="supportchat:$$:"
CONFIG_DIR="$RUN_DIR/config"
mkdir -p "$CONFIG_DIR"

write_role_config() {
  python3 - "$CONFIG_DIR/$1.json" "$1" "$FLOW_LOG_DIR" "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$API_ROUTE" "$API_SPOT_ROUTE" "$SUPPORT_ROUTE" "$SUPPORT_SPOT_ROUTER" "$SUPPORT_SPOT" "$SUPPORT_HTTP_URL" "$SUPPORT_ACTOR_ROUTE" "$SESSION_STREAM" "$SESSION_SPOT_ROUTER" "$SESSION_SPOT" "$SESSION_ACTOR_ROUTE" <<'CONFIG_PY'
import json, os, stat, sys
(path, role, log_dir, redis, prefix, api_route, api_spot_route, support_route, support_spot_router, support_spot, support_http, support_actor_route, session_stream, session_spot_router, session_spot, session_actor_route) = sys.argv[1:]
document = {"sample": {"role": {"name": role, "logDir": log_dir}, "topology": {
  "redisEndpoint": redis, "redisKeyPrefix": prefix, "apiRouteEndpoint": api_route, "apiSpotRouteEndpoint": api_spot_route,
  "supportRouteEndpoint": support_route, "supportSpotRouterEndpoint": support_spot_router, "supportSpotEndpoint": support_spot,
  "supportHttpUrl": support_http, "supportActorRouteEndpoint": support_actor_route, "sessionStreamEndpoint": session_stream,
  "sessionSpotRouterEndpoint": session_spot_router, "sessionSpotEndpoint": session_spot, "sessionActorRouteEndpoint": session_actor_route}}}
with open(path, "w", encoding="utf-8") as f: json.dump(document, f, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
CONFIG_PY
}

dump_logs() { for log in "$LOG_DIR"/*.log "$FLOW_LOG_DIR"/flow-*.log; do [[ -f "$log" ]] && { echo "===== $log" >&2; cat "$log" >&2; }; done; }
port_of() { echo "${1##*:}"; }
wait_port() {
  local label="$1" port="$2"
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do (echo >"/dev/tcp/127.0.0.1/$port") >/dev/null 2>&1 && return 0; sleep "$WAIT_SECONDS"; done
  echo "timed out waiting for $label" >&2; dump_logs; return 1
}
line_count() { local expected="$1" total=0 log; shift; for log in "$@"; do [[ -f "$log" ]] && total=$((total + $(grep -Fxc -- "$expected" "$log" || true))); done; echo "$total"; }
prefix_count() { local prefix="$1" total=0 log; shift; for log in "$@"; do [[ -f "$log" ]] && total=$((total + $(awk -v prefix="$prefix" 'index($0, prefix) == 1 { ++count } END { print count + 0 }' "$log"))); done; echo "$total"; }
wait_exact() {
  local name="$1" expected="$2" wanted="$3" actual=0; shift 3
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do actual="$(line_count "$expected" "$@")"; [[ "$actual" == "$wanted" ]] && return 0; [[ "$actual" -gt "$wanted" ]] && break; sleep "$WAIT_SECONDS"; done
  echo "expected $name exactly $wanted time(s), found $actual" >&2; dump_logs; return 1
}
wait_minimum() {
  local name="$1" prefix="$2" wanted="$3" actual=0; shift 3
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do actual="$(prefix_count "$prefix" "$@")"; [[ "$actual" -ge "$wanted" ]] && return 0; sleep "$WAIT_SECONDS"; done
  echo "expected $name at least $wanted time(s), found $actual" >&2; dump_logs; return 1
}
start_role() { local name="$1"; shift; stdbuf -oL -eL "$@" >"$LOG_DIR/$name.log" 2>&1 & PIDS+=("$!"); }
remove_pid() { local target="$1" remaining=() pid; for pid in "${PIDS[@]}"; do [[ "$pid" == "$target" ]] || remaining+=("$pid"); done; PIDS=("${remaining[@]}"); }

write_role_config api
write_role_config session
write_role_config support
wait_port redis "$(port_of "$REDIS_ENDPOINT")"
start_role api "$BIN_DIR/sample_cpp_framework_supportchat_api" --config="$CONFIG_DIR/api.json"
start_role session "$BIN_DIR/sample_cpp_framework_supportchat_session" --config="$CONFIG_DIR/session.json"
start_role support "$BIN_DIR/sample_cpp_framework_supportchat_support" --config="$CONFIG_DIR/support.json"

wait_exact "Api public readiness" "supportchat-ready kind=public node=api" 1 "$LOG_DIR/api.log"
wait_exact "Support public readiness" "supportchat-ready kind=public node=support" 1 "$LOG_DIR/support.log"
wait_exact "Session stream readiness" "supportchat-ready kind=stream node=session" 1 "$LOG_DIR/session.log"
wait_exact "Api Support spot route readiness" "supportchat-ready kind=spot-route node=api mesh=supportchat.support.spot" 1 "$LOG_DIR/api.log"
wait_exact "Session Support spot route readiness" "supportchat-ready kind=spot-route node=session mesh=supportchat.support.spot" 1 "$LOG_DIR/session.log"

start_role client "$BIN_DIR/sample_cpp_framework_supportchat_client" --stream-endpoint "$SESSION_STREAM"
CLIENT_PID="${PIDS[$(( ${#PIDS[@]} - 1 ))]}"
wait_exact "client authentication" "supportchat authentication=verified" 1 "$LOG_DIR/client.log"
wait_exact "client conversation assignment" "supportchat conversation-assignment=verified" 1 "$LOG_DIR/client.log"
wait_exact "client bound push" "supportchat bound-push=verified" 1 "$LOG_DIR/client.log"
wait_exact "client reconnect" "supportchat reconnect=verified" 1 "$LOG_DIR/client.log"
wait_exact "client idle resume" "supportchat idle-resume=verified" 1 "$LOG_DIR/client.log"
wait_exact "client idle close" "supportchat idle-close=verified" 1 "$LOG_DIR/client.log"
wait_exact "client closed typing ignore" "supportchat-closed-typing-ignore=verified" 1 "$LOG_DIR/client.log"
wait_exact "client completion" "supportchat=completed" 1 "$LOG_DIR/client.log"
SERVER_LOGS=("$LOG_DIR/api.log" "$LOG_DIR/support.log")
wait_minimum "conversation creation" "supportchat-conversation created conversation=" 1 "${SERVER_LOGS[@]}"
wait_minimum "agent conversation join" "supportchat-conversation agent-joined conversation=" 1 "${SERVER_LOGS[@]}"
wait_minimum "WaitingForAgent transition" "supportchat-conversation status=WaitingForAgent conversation=" 1 "${SERVER_LOGS[@]}"
wait_minimum "Active transition" "supportchat-conversation status=Active conversation=" 1 "${SERVER_LOGS[@]}"
wait_minimum "WaitingForClose transition" "supportchat-conversation status=WaitingForClose conversation=" 1 "${SERVER_LOGS[@]}"
wait_minimum "Closed transition" "supportchat-conversation status=Closed conversation=" 1 "${SERVER_LOGS[@]}"
set +e; wait "$CLIENT_PID"; CLIENT_STATUS=$?; set -e
remove_pid "$CLIENT_PID"
if [[ "$CLIENT_STATUS" -ne 0 ]]; then echo "SupportChat client exited with status $CLIENT_STATUS" >&2; dump_logs; exit 1; fi

cleanup
trap - EXIT
echo "supportchat-placement=completed"
