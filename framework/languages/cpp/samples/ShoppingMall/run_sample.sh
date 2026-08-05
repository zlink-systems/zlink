#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
CPP_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "$CPP_ROOT/samples/sample-build-common.sh"
zlink_cpp_sample_prepare_build "$CPP_ROOT"
if [[ ! -x "$BIN_DIR/sample_cpp_framework_shoppingmall_client" && -x "$BIN_DIR/linux-ninja-debug/sample_cpp_framework_shoppingmall_client" ]]; then
  BIN_DIR="$BIN_DIR/linux-ninja-debug"
fi

RUN_DIR="$(mktemp -d)"
RUN_ID="$(basename "$RUN_DIR")-$$-${RANDOM}"
LOG_DIR="$RUN_DIR/logs"
FLOW_LOG_DIR="$SCRIPT_DIR/logs"
mkdir -p "$LOG_DIR" "$FLOW_LOG_DIR"
rm -f "$FLOW_LOG_DIR"/*.log

PIDS=()
REDIS_CONTAINER_NAME=""
cleanup() {
  local code=$?
  local cleanup_failed=0
  local status
  for ((i=${#PIDS[@]}-1; i>=0; i--)); do
    local pid="${PIDS[$i]}"
    if kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
  done
  for _ in $(seq 1 300); do
    local any_alive=0
    for pid in "${PIDS[@]}"; do
      if kill -0 "$pid" >/dev/null 2>&1; then
        any_alive=1
        break
      fi
    done
    if [[ "$any_alive" == "0" ]]; then
      break
    fi
    sleep 0.1
  done
  for pid in "${PIDS[@]}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then
      echo "forced cleanup process $pid" >&2
      kill -9 "$pid" >/dev/null 2>&1 || true
      cleanup_failed=1
    fi
  done
  for pid in "${PIDS[@]}"; do
    set +e
    wait "$pid" >/dev/null 2>&1
    status=$?
    set -e
    if [[ "$status" != "0" && "$status" != "127" && "$status" != "130" && "$status" != "143" ]]; then
      echo "cleanup process $pid exited unexpectedly with status $status" >&2
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
    ports = [sock.getsockname()[1] for sock in sockets]
    print(
        f"{ports[0]} {ports[1]} {ports[2]} "
        f"tcp://127.0.0.1:{ports[3]} tcp://127.0.0.1:{ports[4]} "
        f"{ports[5]} {ports[6]} "
        f"tcp://127.0.0.1:{ports[7]} tcp://127.0.0.1:{ports[8]} "
        f"tcp://127.0.0.1:{ports[9]} tcp://127.0.0.1:{ports[10]} "
        f"tcp://127.0.0.1:{ports[11]} tcp://127.0.0.1:{ports[12]} "
        f"tcp://127.0.0.1:{ports[13]} tcp://127.0.0.1:{ports[14]} "
        f"tcp://127.0.0.1:{ports[15]} tcp://127.0.0.1:{ports[16]}"
    )
except OSError as error:
    print(f"SOCKETLESS {error.errno}:{error.strerror}")
finally:
    for sock in sockets:
        sock.close()
PY
)"
if [[ "$PORT_ALLOCATION_OUTPUT" == SOCKETLESS* ]]; then
  echo "ShoppingMall runner requires local TCP sockets; allocation failed: ${PORT_ALLOCATION_OUTPUT#SOCKETLESS }" >&2
  exit 1
fi
read -r SHOPPINGMALL_RESERVED_PORT SHOPPINGMALL_API_A_PORT SHOPPINGMALL_API_B_PORT SHOPPINGMALL_API_A_ROUTE SHOPPINGMALL_API_B_ROUTE SHOPPINGMALL_WORKFLOW_A_PORT SHOPPINGMALL_WORKFLOW_B_PORT SHOPPINGMALL_WORKFLOW_A_ROUTE SHOPPINGMALL_WORKFLOW_B_ROUTE SHOPPINGMALL_WORKFLOW_A_SPOT_ROUTE SHOPPINGMALL_WORKFLOW_B_SPOT_ROUTE SHOPPINGMALL_WORKFLOW_A_SPOT SHOPPINGMALL_WORKFLOW_A_SPOT_ROUTER SHOPPINGMALL_WORKFLOW_B_SPOT SHOPPINGMALL_WORKFLOW_B_SPOT_ROUTER SHOPPINGMALL_API_A_SPOT_ROUTER SHOPPINGMALL_API_B_SPOT_ROUTER <<<"$PORT_ALLOCATION_OUTPUT"

cmake --build "$BUILD_DIR" --parallel 2 --target \
  sample_cpp_framework_shoppingmall_commerce_api \
  sample_cpp_framework_shoppingmall_order_workflow \
  sample_cpp_framework_shoppingmall_client >/dev/null

zlink_redis_start_scoped_assign REDIS_CONTAINER_NAME redis_port \
  "zlink-redis-cpp-sample-shoppingmall" "redis:7-alpine"
SHOPPINGMALL_REDIS_ENDPOINT="tcp://127.0.0.1:${redis_port}"
SHOPPINGMALL_REDIS_KEY_PREFIX="shoppingmall:cpp:${RUN_ID}:"
SHOPPINGMALL_API_A_HTTP_URL="http://127.0.0.1:${SHOPPINGMALL_API_A_PORT}"
SHOPPINGMALL_API_B_HTTP_URL="http://127.0.0.1:${SHOPPINGMALL_API_B_PORT}"
SHOPPINGMALL_WORKFLOW_A_HTTP_URL="http://127.0.0.1:${SHOPPINGMALL_WORKFLOW_A_PORT}"
SHOPPINGMALL_WORKFLOW_B_HTTP_URL="http://127.0.0.1:${SHOPPINGMALL_WORKFLOW_B_PORT}"
SHOPPINGMALL_WORKFLOW_A_ROUTE_ENDPOINT="$SHOPPINGMALL_WORKFLOW_A_ROUTE"
SHOPPINGMALL_WORKFLOW_B_ROUTE_ENDPOINT="$SHOPPINGMALL_WORKFLOW_B_ROUTE"
SHOPPINGMALL_WORKFLOW_A_SPOT_ROUTE_ENDPOINT="$SHOPPINGMALL_WORKFLOW_A_SPOT_ROUTE"
SHOPPINGMALL_WORKFLOW_B_SPOT_ROUTE_ENDPOINT="$SHOPPINGMALL_WORKFLOW_B_SPOT_ROUTE"
SHOPPINGMALL_WORKFLOW_A_SPOT_ENDPOINT="$SHOPPINGMALL_WORKFLOW_A_SPOT"
SHOPPINGMALL_WORKFLOW_A_SPOT_ROUTER_ENDPOINT="$SHOPPINGMALL_WORKFLOW_A_SPOT_ROUTER"
SHOPPINGMALL_WORKFLOW_B_SPOT_ENDPOINT="$SHOPPINGMALL_WORKFLOW_B_SPOT"
SHOPPINGMALL_WORKFLOW_B_SPOT_ROUTER_ENDPOINT="$SHOPPINGMALL_WORKFLOW_B_SPOT_ROUTER"
SHOPPINGMALL_API_A_SPOT_ROUTER_ENDPOINT="$SHOPPINGMALL_API_A_SPOT_ROUTER"
SHOPPINGMALL_API_B_SPOT_ROUTER_ENDPOINT="$SHOPPINGMALL_API_B_SPOT_ROUTER"

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
  for log in "$LOG_DIR"/*.log "$FLOW_LOG_DIR"/*.log; do
    [[ -f "$log" ]] && { echo "===== $log" >&2; cat "$log" >&2; }
  done
  return 1
}

wait_http() {
  local label="$1"
  local endpoint="$2"
  for _ in $(seq 1 150); do
    if curl -fsS "${endpoint}/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for ${label} at ${endpoint}" >&2
  return 1
}

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
  echo "timed out waiting for ShoppingMall RouteMesh peer ${target_rid} from ${api_url}" >&2
  for log in "$LOG_DIR"/*.log "$FLOW_LOG_DIR"/*.log; do
    [[ -f "$log" ]] && { echo "===== $log" >&2; cat "$log" >&2; }
  done
  return 1
}

start_role() {
  local name="$1"
  shift
  stdbuf -oL -eL "$@" >"$LOG_DIR/${name}.log" 2>&1 &
  PIDS+=("$!")
}

wait_port redis "$SHOPPINGMALL_REDIS_ENDPOINT"


CONFIG_DIR="$RUN_DIR/config"
mkdir -p "$CONFIG_DIR"

# 각 role은 자기 설정 파일 하나만 받는다(공통 정책 sample-e2e-configuration-policy.ko.md §2.1).
write_role_config() {
  python3 - "$CONFIG_DIR/$1.json" "$1" "$FLOW_LOG_DIR" "$SHOPPINGMALL_REDIS_ENDPOINT" \
    "$SHOPPINGMALL_REDIS_KEY_PREFIX" "$SHOPPINGMALL_API_A_HTTP_URL" \
    "$SHOPPINGMALL_API_B_HTTP_URL" "$SHOPPINGMALL_API_A_ROUTE" "$SHOPPINGMALL_API_B_ROUTE" \
    "$SHOPPINGMALL_API_A_SPOT_ROUTER_ENDPOINT" "$SHOPPINGMALL_API_B_SPOT_ROUTER_ENDPOINT" \
    "$SHOPPINGMALL_WORKFLOW_A_HTTP_URL" "$SHOPPINGMALL_WORKFLOW_B_HTTP_URL" \
    "$SHOPPINGMALL_WORKFLOW_A_ROUTE_ENDPOINT" "$SHOPPINGMALL_WORKFLOW_B_ROUTE_ENDPOINT" \
    "$SHOPPINGMALL_WORKFLOW_A_SPOT_ROUTE_ENDPOINT" \
    "$SHOPPINGMALL_WORKFLOW_B_SPOT_ROUTE_ENDPOINT" "$SHOPPINGMALL_WORKFLOW_A_SPOT_ENDPOINT" \
    "$SHOPPINGMALL_WORKFLOW_A_SPOT_ROUTER_ENDPOINT" "$SHOPPINGMALL_WORKFLOW_B_SPOT_ENDPOINT" \
    "$SHOPPINGMALL_WORKFLOW_B_SPOT_ROUTER_ENDPOINT" <<'CONFIG_PY'
import json
import os
import stat
import sys

(path, role_name, flow_log_dir, redis_endpoint, redis_key_prefix, api_a_http,
 api_b_http, api_a_route, api_b_route, api_a_spot_router, api_b_spot_router,
 workflow_a_http, workflow_b_http, workflow_a_route, workflow_b_route,
 workflow_a_spot_route, workflow_b_spot_route, workflow_a_spot,
 workflow_a_spot_router, workflow_b_spot, workflow_b_spot_router) = sys.argv[1:]

document = {
    "sample": {
        "role": {"name": role_name, "logDir": flow_log_dir},
        "topology": {
            "redisEndpoint": redis_endpoint,
            "redisKeyPrefix": redis_key_prefix,
            "apiAHttpUrl": api_a_http,
            "apiBHttpUrl": api_b_http,
            "apiARouteEndpoint": api_a_route,
            "apiBRouteEndpoint": api_b_route,
            "apiASpotRouterEndpoint": api_a_spot_router,
            "apiBSpotRouterEndpoint": api_b_spot_router,
            "workflowAHttpUrl": workflow_a_http,
            "workflowBHttpUrl": workflow_b_http,
            "workflowARouteEndpoint": workflow_a_route,
            "workflowBRouteEndpoint": workflow_b_route,
            "workflowASpotRouteEndpoint": workflow_a_spot_route,
            "workflowBSpotRouteEndpoint": workflow_b_spot_route,
            "workflowASpotEndpoint": workflow_a_spot,
            "workflowASpotRouterEndpoint": workflow_a_spot_router,
            "workflowBSpotEndpoint": workflow_b_spot,
            "workflowBSpotRouterEndpoint": workflow_b_spot_router,
        },
    }
}

with open(path, "w", encoding="utf-8") as file:
    json.dump(document, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
CONFIG_PY
}

write_role_config workflow-a
write_role_config workflow-b
write_role_config api-a
write_role_config api-b

start_role workflow-a "$BIN_DIR/sample_cpp_framework_shoppingmall_order_workflow" --config="$CONFIG_DIR/workflow-a.json"
start_role workflow-b "$BIN_DIR/sample_cpp_framework_shoppingmall_order_workflow" --config="$CONFIG_DIR/workflow-b.json"
start_role api-a "$BIN_DIR/sample_cpp_framework_shoppingmall_commerce_api" --config="$CONFIG_DIR/api-a.json"
start_role api-b "$BIN_DIR/sample_cpp_framework_shoppingmall_commerce_api" --config="$CONFIG_DIR/api-b.json"

wait_port workflow-a-route "$SHOPPINGMALL_WORKFLOW_A_ROUTE_ENDPOINT"
wait_http workflow-a "$SHOPPINGMALL_WORKFLOW_A_HTTP_URL"
wait_port workflow-b-route "$SHOPPINGMALL_WORKFLOW_B_ROUTE_ENDPOINT"
wait_http workflow-b "$SHOPPINGMALL_WORKFLOW_B_HTTP_URL"
wait_port api-a-route "$SHOPPINGMALL_API_A_ROUTE"
wait_http api-a "$SHOPPINGMALL_API_A_HTTP_URL"
wait_port api-b-route "$SHOPPINGMALL_API_B_ROUTE"
wait_http api-b "$SHOPPINGMALL_API_B_HTTP_URL"

wait_route_ready "$SHOPPINGMALL_API_A_HTTP_URL" "shoppingmall-workflow-a-workflow"
wait_route_ready "$SHOPPINGMALL_API_A_HTTP_URL" "shoppingmall-workflow-b-workflow"
wait_route_ready "$SHOPPINGMALL_API_B_HTTP_URL" "shoppingmall-workflow-a-workflow"
wait_route_ready "$SHOPPINGMALL_API_B_HTTP_URL" "shoppingmall-workflow-b-workflow"

"$BIN_DIR/sample_cpp_framework_shoppingmall_client" \
  --api-a-http-url "$SHOPPINGMALL_API_A_HTTP_URL" \
  --api-b-http-url "$SHOPPINGMALL_API_B_HTTP_URL" >"$LOG_DIR/client.log" 2>&1 || {
  for log in "$LOG_DIR"/*.log "$FLOW_LOG_DIR"/*.log; do
    [[ -f "$log" ]] && { echo "===== $log" >&2; cat "$log" >&2; }
  done
  exit 1
}

grep -q "shoppingmall=completed" "$LOG_DIR/client.log"
grep -q "shoppingmall order: started.*spot=" "$LOG_DIR/workflow-a.log"
grep -q "shoppingmall order: started.*spot=" "$LOG_DIR/workflow-b.log"
grep -q "shoppingmall evidence:" "$LOG_DIR/api-a.log"
grep -Rq "message flow" "$FLOW_LOG_DIR"
grep -q "message flow" "$FLOW_LOG_DIR/flow-api-a.log"
grep -q "message flow" "$FLOW_LOG_DIR/flow-api-b.log"
grep -q "message flow" "$FLOW_LOG_DIR/flow-workflow-a.log"
grep -q "message flow" "$FLOW_LOG_DIR/flow-workflow-b.log"
echo "shoppingmall-server-evidence=completed"
echo "PASS ShoppingMall.Cpp"
