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
if [[ ! -x "$BIN_DIR/sample_cpp_framework_deliverydispatch_client" && -x "$BIN_DIR/linux-ninja-debug/sample_cpp_framework_deliverydispatch_client" ]]; then
  BIN_DIR="$BIN_DIR/linux-ninja-debug"
fi

PIDS=()
RUN_DIR="$(mktemp -d)"
LOG_DIR="$RUN_DIR/logs"
CONFIG_DIR="$RUN_DIR/config"
REDIS_CONTAINER_NAME=""
mkdir -p "$LOG_DIR" "$CONFIG_DIR"
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

read -r RESERVED_PORT API_HTTP_PORT DISPATCH_ROUTE DISPATCH_SPOT_ROUTER TRACKING_ROUTE TRACKING_SPOT_ROUTER TRACKING_SPOT DISPATCH_SPOT CUSTOMER_STREAM CUSTOMER_SPOT_ROUTER CUSTOMER_SPOT COURIER_STREAM COURIER_SESSION_SPOT_ROUTER COURIER_SESSION_SPOT COURIER_NODE1_ROUTE COURIER_NODE1_ROUTER COURIER_NODE1 COURIER_NODE2_ROUTE COURIER_NODE2_ROUTER COURIER_NODE2 <<<"$(python3 - <<'PY'
import socket
sockets = []
chosen = set()
while len(sockets) < 20:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    if port in chosen or port + 1000 in chosen:
        sock.close()
        continue
    chosen.add(port)
    chosen.add(port + 1000)
    sockets.append(sock)
ports = [sock.getsockname()[1] for sock in sockets]
endpoints = " ".join(f"tcp://127.0.0.1:{port}" for port in ports[2:])
print(f"{ports[0]} {ports[1]} {endpoints}")
for sock in sockets:
    sock.close()
PY
)"
if [[ -z "$RESERVED_PORT" || -z "$COURIER_NODE2" ]]; then
  echo "Failed to allocate local TCP ports for the DeliveryDispatch sample." >&2
  echo "This environment may block local socket creation." >&2
  exit 1
fi
cmake --build "$BUILD_DIR" --parallel 2 --target \
  sample_cpp_framework_deliverydispatch_dispatch \
  sample_cpp_framework_deliverydispatch_courier_actor_node \
  sample_cpp_framework_deliverydispatch_customer_gateway \
  sample_cpp_framework_deliverydispatch_courier_session \
  sample_cpp_framework_deliverydispatch_tracking \
  sample_cpp_framework_deliverydispatch_client >/dev/null

zlink_redis_start_scoped_assign REDIS_CONTAINER_NAME redis_port \
  "zlink-redis-cpp-sample-deliverydispatch" "redis:7-alpine"
REDIS_ENDPOINT="tcp://127.0.0.1:${redis_port}"
REDIS_KEY_PREFIX="deliverydispatch:$$:"
API_HTTP_URL="http://127.0.0.1:${API_HTTP_PORT}"

# 각 role은 자기 설정 파일 하나만 받는다(공통 정책 sample-e2e-configuration-policy.ko.md §2.1).
# 실행별 port와 Redis endpoint는 runner가 정하지만, 애플리케이션에는 환경 변수가 아니라 이
# 파일로만 전달한다.
write_role_config() {
  local role="$1"
  local instance_name="${2:-}"
  python3 - "$CONFIG_DIR/${role}.json" "$role" "$instance_name" "$FLOW_LOG_DIR" \
    "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$API_HTTP_URL" "$DISPATCH_ROUTE" \
    "$DISPATCH_SPOT_ROUTER" "$DISPATCH_SPOT" "$TRACKING_ROUTE" \
    "$TRACKING_SPOT_ROUTER" "$TRACKING_SPOT" "$CUSTOMER_STREAM" \
    "$CUSTOMER_SPOT_ROUTER" "$CUSTOMER_SPOT" "$COURIER_STREAM" \
    "$COURIER_SESSION_SPOT_ROUTER" "$COURIER_SESSION_SPOT" \
    "$COURIER_NODE1_ROUTE" "$COURIER_NODE1_ROUTER" "$COURIER_NODE1" \
    "$COURIER_NODE2_ROUTE" "$COURIER_NODE2_ROUTER" "$COURIER_NODE2" <<'PY'
import json
import os
import stat
import sys

(path, role_name, instance_name, flow_log_dir, redis_endpoint, redis_key_prefix,
 api_http_url, dispatch_route, dispatch_spot_router, dispatch_spot, tracking_route,
 tracking_spot_router, tracking_spot, customer_stream, customer_spot_router,
 customer_spot, courier_stream, courier_session_spot_router,
 courier_session_spot, courier_node1_route, courier_node1_router, courier_node1,
 courier_node2_route, courier_node2_router, courier_node2) = sys.argv[1:]

role = {"name": role_name, "logDir": flow_log_dir}
if instance_name:
    role["instanceName"] = instance_name

document = {
    "sample": {
        "role": role,
        "topology": {
            "redisEndpoint": redis_endpoint,
            "redisKeyPrefix": redis_key_prefix,
            "dispatchApiHttpUrl": api_http_url,
            "dispatchRouteEndpoint": dispatch_route,
            "dispatchSpotRouterEndpoint": dispatch_spot_router,
            "dispatchSpotEndpoint": dispatch_spot,
            "trackingRouteEndpoint": tracking_route,
            "trackingSpotRouterEndpoint": tracking_spot_router,
            "trackingSpotEndpoint": tracking_spot,
            "customerStreamEndpoint": customer_stream,
            "customerSpotRouterEndpoint": customer_spot_router,
            "customerSpotEndpoint": customer_spot,
            "courierStreamEndpoint": courier_stream,
            "courierSessionSpotRouterEndpoint": courier_session_spot_router,
            "courierSessionSpotEndpoint": courier_session_spot,
            "courierActorNode1RouteEndpoint": courier_node1_route,
            "courierActorNode1RouterEndpoint": courier_node1_router,
            "courierActorNode1Endpoint": courier_node1,
            "courierActorNode2RouteEndpoint": courier_node2_route,
            "courierActorNode2RouterEndpoint": courier_node2_router,
            "courierActorNode2Endpoint": courier_node2,
        },
    }
}

with open(path, "w", encoding="utf-8") as file:
    json.dump(document, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

write_role_config tracking
write_role_config customer-gateway
write_role_config courier-session
write_role_config dispatch
write_role_config delivery-courier-node-1 delivery-courier-node-1
write_role_config delivery-courier-node-2 delivery-courier-node-2

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
  for log in "$LOG_DIR"/*.log; do
    if [[ -f "$log" ]]; then
      echo "===== ${log}" >&2
      cat "$log" >&2
    fi
  done
  return 1
}

wait_port redis "$(port_of "$REDIS_ENDPOINT")"

start_role() {
  local name="$1"
  shift
  stdbuf -oL -eL "$@" >"$LOG_DIR/${name}.log" 2>&1 &
  PIDS+=("$!")
}

dump_logs() {
  for log in "$LOG_DIR"/*.log; do
    if [[ -f "$log" ]]; then
      echo "===== ${log}" >&2
      cat "$log" >&2
    fi
  done
  for log in "$FLOW_LOG_DIR"/flow-*.log; do
    if [[ -f "$log" ]]; then
      echo "===== ${log}" >&2
      cat "$log" >&2
    fi
  done
}

start_role tracking "$BIN_DIR/sample_cpp_framework_deliverydispatch_tracking" \
  --config="$CONFIG_DIR/tracking.json"
start_role customer-gateway "$BIN_DIR/sample_cpp_framework_deliverydispatch_customer_gateway" \
  --config="$CONFIG_DIR/customer-gateway.json"
start_role courier-session "$BIN_DIR/sample_cpp_framework_deliverydispatch_courier_session" \
  --config="$CONFIG_DIR/courier-session.json"
start_role courier-actor-node-1 \
  "$BIN_DIR/sample_cpp_framework_deliverydispatch_courier_actor_node" \
  --config="$CONFIG_DIR/delivery-courier-node-1.json"
start_role courier-actor-node-2 \
  "$BIN_DIR/sample_cpp_framework_deliverydispatch_courier_actor_node" \
  --config="$CONFIG_DIR/delivery-courier-node-2.json"
start_role dispatch "$BIN_DIR/sample_cpp_framework_deliverydispatch_dispatch" \
  --config="$CONFIG_DIR/dispatch.json"

wait_port tracking "$(port_of "$TRACKING_ROUTE")"
wait_port tracking-spot "$(port_of "$TRACKING_SPOT_ROUTER")"
wait_port customer-stream "$(port_of "$CUSTOMER_STREAM")"
wait_port customer-spot "$(port_of "$CUSTOMER_SPOT_ROUTER")"
wait_port courier-stream "$(port_of "$COURIER_STREAM")"
wait_port courier-session-spot "$(port_of "$COURIER_SESSION_SPOT_ROUTER")"
wait_port courier-actor-node-1-spot "$(port_of "$COURIER_NODE1_ROUTER")"
wait_port courier-actor-node-2-spot "$(port_of "$COURIER_NODE2_ROUTER")"
wait_port dispatch "$(port_of "$DISPATCH_ROUTE")"
wait_port dispatch-http "$API_HTTP_PORT"

wait_route_ready() {
  local target_rid="$1"
  for _ in $(seq 1 120); do
    if curl --connect-timeout 0.2 --max-time 0.5 -fsS \
      "$API_HTTP_URL/ready?targetRid=${target_rid}" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  curl --connect-timeout 0.2 --max-time 1 -sS \
    "$API_HTTP_URL/ready?targetRid=${target_rid}" >&2 || true
  echo "Timed out waiting for Dispatch route peer ${target_rid}" >&2
  return 1
}

wait_route_ready "delivery-courier-node-1"
wait_route_ready "delivery-courier-node-2"

"$BIN_DIR/sample_cpp_framework_deliverydispatch_client" \
  --api-url "$API_HTTP_URL" \
  --stream-endpoint "$CUSTOMER_STREAM" \
  --courier-stream-endpoint "$COURIER_STREAM" >"$LOG_DIR/client.log" 2>&1 || {
  cat "$LOG_DIR/client.log" >&2
  for log in "$LOG_DIR"/*.log; do
    echo "===== ${log}" >&2
    cat "$log" >&2
  done
  exit 1
}

grep -q "deliverydispatch-server-evidence=completed" "$LOG_DIR/client.log"
grep -q "deliverydispatch-reassignment=completed" "$LOG_DIR/client.log"
grep -q "deliverydispatch=completed" "$LOG_DIR/client.log"
grep -Rq "message flow" "$FLOW_LOG_DIR"
grep -q "message flow" "$FLOW_LOG_DIR/flow-dispatch.log"
grep -q "message flow" "$FLOW_LOG_DIR/flow-delivery-courier-node-1.log"
grep -q "message flow" "$FLOW_LOG_DIR/flow-delivery-courier-node-2.log"
grep -q "message flow" "$FLOW_LOG_DIR/flow-customer-gateway.log"
grep -q "message flow" "$FLOW_LOG_DIR/flow-courier-session.log"
echo "deliverydispatch sample result=passed"
