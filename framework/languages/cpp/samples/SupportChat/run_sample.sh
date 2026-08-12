#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
CPP_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "$CPP_ROOT/samples/sample-build-common.sh"
zlink_cpp_sample_prepare_build "$CPP_ROOT"
if [[ ! -x "$BIN_DIR/sample_cpp_framework_supportchat_client" && -x "$BIN_DIR/linux-ninja-debug/sample_cpp_framework_supportchat_client" ]]; then
  BIN_DIR="$BIN_DIR/linux-ninja-debug"
fi

PIDS=()
RUN_DIR="$(mktemp -d)"
LOG_DIR="$RUN_DIR/logs"
FLOW_LOG_DIR="$RUN_DIR/flow-logs"
REDIS_CONTAINER_NAME=""
mkdir -p "$LOG_DIR" "$FLOW_LOG_DIR"
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
    zlink_redis_remove_by_id "$REDIS_CONTAINER_NAME" || true
  fi
  rm -rf "$RUN_DIR"
  if [[ "$cleanup_failed" -ne 0 && "$code" -eq 0 ]]; then
    code=1
  fi
  return "$code"
}
trap 'cleanup; status=$?; exit "$status"' EXIT

read -r -a SUPPORTCHAT_PORTS <<<"$(zlink_sample_allocate_ports 12)"
SUPPORTCHAT_RESERVED_PORT="${SUPPORTCHAT_PORTS[0]}"
SUPPORTCHAT_API_ROUTE="tcp://127.0.0.1:${SUPPORTCHAT_PORTS[1]}"
SUPPORTCHAT_SUPPORT_ROUTE="tcp://127.0.0.1:${SUPPORTCHAT_PORTS[2]}"
SUPPORTCHAT_SUPPORT_SPOT_ROUTER="tcp://127.0.0.1:${SUPPORTCHAT_PORTS[3]}"
SUPPORTCHAT_SUPPORT_SPOT="tcp://127.0.0.1:${SUPPORTCHAT_PORTS[4]}"
SUPPORTCHAT_SESSION_STREAM="tcp://127.0.0.1:${SUPPORTCHAT_PORTS[5]}"
SUPPORTCHAT_SESSION_SPOT_ROUTER="tcp://127.0.0.1:${SUPPORTCHAT_PORTS[6]}"
SUPPORTCHAT_SESSION_SPOT="tcp://127.0.0.1:${SUPPORTCHAT_PORTS[7]}"
SUPPORTCHAT_SUPPORT_HTTP_URL="http://127.0.0.1:${SUPPORTCHAT_PORTS[8]}"
SUPPORTCHAT_SESSION_ACTOR_ROUTE="tcp://127.0.0.1:${SUPPORTCHAT_PORTS[9]}"
SUPPORTCHAT_SUPPORT_ACTOR_ROUTE="tcp://127.0.0.1:${SUPPORTCHAT_PORTS[10]}"
SUPPORTCHAT_API_SPOT_ROUTE="tcp://127.0.0.1:${SUPPORTCHAT_PORTS[11]}"
if [[ -z "$SUPPORTCHAT_RESERVED_PORT" || -z "$SUPPORTCHAT_SESSION_SPOT" ]]; then
  echo "Failed to allocate local TCP ports for the SupportChat sample." >&2
  exit 1
fi

cmake --build "$BUILD_DIR" --parallel 2 --target \
  sample_cpp_framework_supportchat_api \
  sample_cpp_framework_supportchat_session \
  sample_cpp_framework_supportchat_support \
  sample_cpp_framework_supportchat_client >/dev/null

# 공통 sample spec: Redis가 필요한 실행은 전용 Docker container를 띄운다. 만들지 못하면
# host Redis나 다른 실행 endpoint로 대체하지 않고 즉시 실패한다.
zlink_redis_start_scoped_assign REDIS_CONTAINER_NAME redis_port \
  "zlink-redis-cpp-sample-supportchat" "redis:7-alpine"
SUPPORTCHAT_REDIS_ENDPOINT="tcp://127.0.0.1:${redis_port}"
SUPPORTCHAT_REDIS_KEY_PREFIX="supportchat:$$:"
CONFIG_DIR="$RUN_DIR/config"
mkdir -p "$CONFIG_DIR"

# 각 role은 자기 설정 파일 하나만 받는다(공통 정책 sample-e2e-configuration-policy.ko.md §2.1).
write_role_config() {
  python3 - "$CONFIG_DIR/$1.json" "$1" "$FLOW_LOG_DIR" "$SUPPORTCHAT_REDIS_ENDPOINT" \
    "$SUPPORTCHAT_REDIS_KEY_PREFIX" "$SUPPORTCHAT_API_ROUTE" "$SUPPORTCHAT_API_SPOT_ROUTE" "$SUPPORTCHAT_SUPPORT_ROUTE" \
    "$SUPPORTCHAT_SUPPORT_SPOT_ROUTER" "$SUPPORTCHAT_SUPPORT_SPOT" \
    "$SUPPORTCHAT_SUPPORT_HTTP_URL" "$SUPPORTCHAT_SUPPORT_ACTOR_ROUTE" \
    "$SUPPORTCHAT_SESSION_STREAM" "$SUPPORTCHAT_SESSION_SPOT_ROUTER" \
    "$SUPPORTCHAT_SESSION_SPOT" "$SUPPORTCHAT_SESSION_ACTOR_ROUTE" <<'CONFIG_PY'
import json
import os
import stat
import sys

(path, role_name, flow_log_dir, redis_endpoint, redis_key_prefix, api_route, api_spot_route,
 support_route, support_spot_router, support_spot, support_http_url,
 support_actor_route, session_stream, session_spot_router, session_spot,
 session_actor_route) = sys.argv[1:]

document = {
    "sample": {
        "role": {"name": role_name, "logDir": flow_log_dir},
        "topology": {
            "redisEndpoint": redis_endpoint,
            "redisKeyPrefix": redis_key_prefix,
            "apiRouteEndpoint": api_route,
            "apiSpotRouteEndpoint": api_spot_route,
            "supportRouteEndpoint": support_route,
            "supportSpotRouterEndpoint": support_spot_router,
            "supportSpotEndpoint": support_spot,
            "supportHttpUrl": support_http_url,
            "supportActorRouteEndpoint": support_actor_route,
            "sessionStreamEndpoint": session_stream,
            "sessionSpotRouterEndpoint": session_spot_router,
            "sessionSpotEndpoint": session_spot,
            "sessionActorRouteEndpoint": session_actor_route,
        },
    }
}

with open(path, "w", encoding="utf-8") as file:
    json.dump(document, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
CONFIG_PY
}

write_role_config api
write_role_config session
write_role_config support

port_of() {
  local endpoint="$1"
  echo "${endpoint##*:}"
}

wait_port() {
  local label="$1"
  local port="$2"
  for _ in $(seq 1 150); do
    if (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for ${label} on ${port}" >&2
  dump_logs
  return 1
}

if [[ "$SUPPORTCHAT_REDIS_ENDPOINT" == tcp://* || "$SUPPORTCHAT_REDIS_ENDPOINT" == 127.0.0.1:* ]]; then
  wait_port redis "$(port_of "$SUPPORTCHAT_REDIS_ENDPOINT")"
fi

start_role() {
  local name="$1"
  shift
  stdbuf -oL -eL "$@" >"$LOG_DIR/${name}.log" 2>&1 &
  PIDS+=("$!")
}

dump_logs() {
  for log in "$LOG_DIR"/*.log "$FLOW_LOG_DIR"/flow-*.log; do
    if [[ -f "$log" ]]; then
      echo "===== ${log}" >&2
      cat "$log" >&2
    fi
  done
}

start_role api "$BIN_DIR/sample_cpp_framework_supportchat_api" --config="$CONFIG_DIR/api.json"
start_role session "$BIN_DIR/sample_cpp_framework_supportchat_session" --config="$CONFIG_DIR/session.json"
start_role support "$BIN_DIR/sample_cpp_framework_supportchat_support" --config="$CONFIG_DIR/support.json"

wait_port session-object-route "$(port_of "$SUPPORTCHAT_SESSION_SPOT_ROUTER")"
wait_port support-object-route "$(port_of "$SUPPORTCHAT_SUPPORT_SPOT_ROUTER")"
wait_port support-http "$(port_of "$SUPPORTCHAT_SUPPORT_HTTP_URL")"

"$BIN_DIR/sample_cpp_framework_supportchat_client" --stream-endpoint "$SUPPORTCHAT_SESSION_STREAM" --support-http-url "$SUPPORTCHAT_SUPPORT_HTTP_URL" >"$LOG_DIR/client.log" 2>&1 || {
  dump_logs
  exit 1
}

grep -q "supportchat authentication=verified" "$LOG_DIR/client.log"
grep -q "supportchat conversation-assignment=verified" "$LOG_DIR/client.log"
grep -q "supportchat bound-push=verified" "$LOG_DIR/client.log"
grep -q "supportchat reconnect=verified" "$LOG_DIR/client.log"
grep -q "supportchat idle-resume=verified" "$LOG_DIR/client.log"
grep -q "supportchat idle-close=verified" "$LOG_DIR/client.log"
grep -q "supportchat closed-typing-ignore=verified" "$LOG_DIR/client.log"
grep -q "supportchat=completed" "$LOG_DIR/client.log"
grep -Rq "message flow" "$FLOW_LOG_DIR"
grep -q "message flow" "$FLOW_LOG_DIR/flow-api.log"
grep -q "message flow" "$FLOW_LOG_DIR/flow-session.log"
grep -q "message flow" "$FLOW_LOG_DIR/flow-support.log"

echo "PASS SupportChat.Cpp"
echo "supportchat sample result=passed"
