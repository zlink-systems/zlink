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

WAIT_ATTEMPTS=300
WAIT_SECONDS=0.1
RUN_DIR="$(mktemp -d)"
RUN_ID="$(basename "$RUN_DIR")-$$-${RANDOM}"
LOG_DIR="$RUN_DIR/logs"
FLOW_LOG_DIR="$RUN_DIR/flow-logs"
CONFIG_DIR="$RUN_DIR/config"
mkdir -p "$LOG_DIR" "$FLOW_LOG_DIR" "$CONFIG_DIR"
PIDS=()
REDIS_CONTAINER_NAME=""

dump_logs() {
  for log in "$LOG_DIR"/*.log; do
    [[ -f "$log" ]] && { echo "===== $log" >&2; cat "$log" >&2; }
  done
}

cleanup() {
  local status=$?
  for ((i=${#PIDS[@]}-1; i>=0; i--)); do
    local pid="${PIDS[$i]}"
    kill -0 "$pid" >/dev/null 2>&1 && kill "$pid" >/dev/null 2>&1 || true
  done
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    local alive=0
    for pid in "${PIDS[@]}"; do
      kill -0 "$pid" >/dev/null 2>&1 && { alive=1; break; }
    done
    [[ "$alive" == 0 ]] && break
    sleep "$WAIT_SECONDS"
  done
  for pid in "${PIDS[@]}"; do
    kill -0 "$pid" >/dev/null 2>&1 && kill -9 "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
  done
  [[ -z "$REDIS_CONTAINER_NAME" ]] || zlink_redis_remove_by_id "$REDIS_CONTAINER_NAME" || true
  rm -rf "$RUN_DIR"
  return "$status"
}
trap 'cleanup; status=$?; exit "$status"' EXIT

count_exact_line() {
  local path="$1" line="$2"
  [[ -f "$path" ]] && grep -Fxc "$line" "$path" || true
}

count_prefix() {
  local path="$1" prefix="$2"
  [[ -f "$path" ]] && grep -Fc "$prefix" "$path" || true
}

wait_exact_line() {
  local label="$1" path="$2" line="$3" expected="$4" actual=0
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    actual="$(count_exact_line "$path" "$line")"
    [[ "$actual" == "$expected" ]] && return 0
    [[ "$actual" -gt "$expected" ]] && break
    sleep "$WAIT_SECONDS"
  done
  echo "expected $label exactly $expected time(s), found $actual" >&2
  return 1
}

wait_prefix_minimum() {
  local label="$1" path="$2" prefix="$3" minimum="$4" actual=0
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    actual="$(count_prefix "$path" "$prefix")"
    [[ "$actual" -ge "$minimum" ]] && return 0
    sleep "$WAIT_SECONDS"
  done
  echo "expected $label at least $minimum time(s), found $actual" >&2
  return 1
}

wait_pattern_minimum() {
  local label="$1" path="$2" pattern="$3" minimum="$4" actual=0
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    actual=$([[ -f "$path" ]] && grep -Ec "$pattern" "$path" || true)
    [[ "$actual" -ge "$minimum" ]] && return 0
    sleep "$WAIT_SECONDS"
  done
  echo "expected $label at least $minimum time(s), found $actual" >&2
  return 1
}

wait_prefix_exact_across() {
  local label="$1" prefix="$2" expected="$3" actual=0
  shift 3
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    actual=0
    for path in "$@"; do
      actual=$((actual + $(count_prefix "$path" "$prefix")))
    done
    [[ "$actual" == "$expected" ]] && return 0
    [[ "$actual" -gt "$expected" ]] && break
    sleep "$WAIT_SECONDS"
  done
  echo "expected $label exactly $expected time(s), found $actual" >&2
  return 1
}

json_field() {
  local body="$1" field="$2"
  python3 -c 'import json, sys; value = json.load(sys.stdin)[sys.argv[1]]; print(str(value).lower() if isinstance(value, bool) else value)' "$field" <<<"$body"
}

post_json() {
  local base_url="$1" path="$2" body="$3"
  curl --connect-timeout 1 --max-time 10 -fsS -H 'Content-Type: application/json' \
    --data "$body" "$base_url$path"
}

wait_order_status() {
  local base_url="$1" order_id="$2" expected="$3" status=""
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    if body="$(post_json "$base_url" /orders/get "{\"orderId\":\"$order_id\"}" 2>/dev/null)"; then
      status="$(python3 -c 'import json, sys; print(json.load(sys.stdin)["state"]["status"])' <<<"$body")"
      [[ "$status" == "$expected" ]] && return 0
    fi
    sleep "$WAIT_SECONDS"
  done
  echo "timed out waiting for order $order_id to reach $expected (last status=$status)" >&2
  return 1
}

start_role() {
  local name="$1"
  shift
  stdbuf -oL -eL "$@" >"$LOG_DIR/${name}.log" 2>&1 &
  PIDS+=("$!")
}

read -r -a PORTS <<<"$(zlink_sample_allocate_ports 17)"
API_A_HTTP_URL="http://127.0.0.1:${PORTS[1]}"
API_B_HTTP_URL="http://127.0.0.1:${PORTS[2]}"
API_A_ROUTE="tcp://127.0.0.1:${PORTS[3]}"
API_B_ROUTE="tcp://127.0.0.1:${PORTS[4]}"
WORKFLOW_A_HTTP_URL="http://127.0.0.1:${PORTS[5]}"
WORKFLOW_B_HTTP_URL="http://127.0.0.1:${PORTS[6]}"
WORKFLOW_A_ROUTE="tcp://127.0.0.1:${PORTS[7]}"
WORKFLOW_B_ROUTE="tcp://127.0.0.1:${PORTS[8]}"
WORKFLOW_A_SPOT_ROUTE="tcp://127.0.0.1:${PORTS[9]}"
WORKFLOW_B_SPOT_ROUTE="tcp://127.0.0.1:${PORTS[10]}"
WORKFLOW_A_SPOT="tcp://127.0.0.1:${PORTS[11]}"
WORKFLOW_A_SPOT_ROUTER="tcp://127.0.0.1:${PORTS[12]}"
WORKFLOW_B_SPOT="tcp://127.0.0.1:${PORTS[13]}"
WORKFLOW_B_SPOT_ROUTER="tcp://127.0.0.1:${PORTS[14]}"
API_A_SPOT_ROUTER="tcp://127.0.0.1:${PORTS[15]}"
API_B_SPOT_ROUTER="tcp://127.0.0.1:${PORTS[16]}"

cmake --build "$BUILD_DIR" --parallel 2 --target \
  sample_cpp_framework_shoppingmall_commerce_api \
  sample_cpp_framework_shoppingmall_order_workflow \
  sample_cpp_framework_shoppingmall_client >/dev/null

zlink_redis_start_scoped_assign REDIS_CONTAINER_NAME redis_port \
  "zlink-redis-cpp-sample-shoppingmall" "redis:7-alpine"
REDIS_ENDPOINT="tcp://127.0.0.1:${redis_port}"
REDIS_KEY_PREFIX="shoppingmall:cpp:${RUN_ID}:"

write_role_config() {
  python3 - "$CONFIG_DIR/$1.json" "$1" "$FLOW_LOG_DIR" "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" \
    "$API_A_HTTP_URL" "$API_B_HTTP_URL" "$API_A_ROUTE" "$API_B_ROUTE" \
    "$API_A_SPOT_ROUTER" "$API_B_SPOT_ROUTER" "$WORKFLOW_A_HTTP_URL" "$WORKFLOW_B_HTTP_URL" \
    "$WORKFLOW_A_ROUTE" "$WORKFLOW_B_ROUTE" "$WORKFLOW_A_SPOT_ROUTE" "$WORKFLOW_B_SPOT_ROUTE" \
    "$WORKFLOW_A_SPOT" "$WORKFLOW_A_SPOT_ROUTER" "$WORKFLOW_B_SPOT" "$WORKFLOW_B_SPOT_ROUTER" <<'PY'
import json
import os
import stat
import sys
(path, role_name, flow_log_dir, redis_endpoint, redis_key_prefix, api_a_http, api_b_http,
 api_a_route, api_b_route, api_a_spot_router, api_b_spot_router, workflow_a_http,
 workflow_b_http, workflow_a_route, workflow_b_route, workflow_a_spot_route,
 workflow_b_spot_route, workflow_a_spot, workflow_a_spot_router, workflow_b_spot,
 workflow_b_spot_router) = sys.argv[1:]
document = {"sample": {"role": {"name": role_name, "logDir": flow_log_dir}, "topology": {
    "redisEndpoint": redis_endpoint, "redisKeyPrefix": redis_key_prefix,
    "apiAHttpUrl": api_a_http, "apiBHttpUrl": api_b_http,
    "apiARouteEndpoint": api_a_route, "apiBRouteEndpoint": api_b_route,
    "apiASpotRouterEndpoint": api_a_spot_router, "apiBSpotRouterEndpoint": api_b_spot_router,
    "workflowAHttpUrl": workflow_a_http, "workflowBHttpUrl": workflow_b_http,
    "workflowARouteEndpoint": workflow_a_route, "workflowBRouteEndpoint": workflow_b_route,
    "workflowASpotRouteEndpoint": workflow_a_spot_route, "workflowBSpotRouteEndpoint": workflow_b_spot_route,
    "workflowASpotEndpoint": workflow_a_spot, "workflowASpotRouterEndpoint": workflow_a_spot_router,
    "workflowBSpotEndpoint": workflow_b_spot, "workflowBSpotRouterEndpoint": workflow_b_spot_router,
}}}
with open(path, "w", encoding="utf-8") as file:
    json.dump(document, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

write_role_config workflow-a
write_role_config workflow-b
write_role_config api-a
write_role_config api-b
start_role workflow-a "$BIN_DIR/sample_cpp_framework_shoppingmall_order_workflow" --config="$CONFIG_DIR/workflow-a.json"
start_role workflow-b "$BIN_DIR/sample_cpp_framework_shoppingmall_order_workflow" --config="$CONFIG_DIR/workflow-b.json"
start_role api-a "$BIN_DIR/sample_cpp_framework_shoppingmall_commerce_api" --config="$CONFIG_DIR/api-a.json"
start_role api-b "$BIN_DIR/sample_cpp_framework_shoppingmall_commerce_api" --config="$CONFIG_DIR/api-b.json"

# Readiness uses only sample-owned passive observations, never /ready probes.
wait_exact_line "api-a HTTP readiness" "$LOG_DIR/api-a.log" "shoppingmall-ready kind=http node=api-a" 1
wait_exact_line "api-b HTTP readiness" "$LOG_DIR/api-b.log" "shoppingmall-ready kind=http node=api-b" 1
wait_exact_line "api-a workflow-a object route" "$LOG_DIR/api-a.log" "shoppingmall-ready kind=object-route node=api-a target=workflow-a" 1
wait_exact_line "api-a workflow-b object route" "$LOG_DIR/api-a.log" "shoppingmall-ready kind=object-route node=api-a target=workflow-b" 1
wait_exact_line "api-b workflow-a object route" "$LOG_DIR/api-b.log" "shoppingmall-ready kind=object-route node=api-b target=workflow-a" 1
wait_exact_line "api-b workflow-b object route" "$LOG_DIR/api-b.log" "shoppingmall-ready kind=object-route node=api-b target=workflow-b" 1

# Runner-only fixture preparation. Returned order ids are allocated in this run.
pending_body="$(post_json "$API_A_HTTP_URL" /self-check/idempotency/pending '{"idempotencyKey":"order-pending-001","orderId":""}')"
pending_order_id="$(json_field "$pending_body" orderId)"
resume_body="$(post_json "$API_A_HTTP_URL" /self-check/workflow/inventory-reserved '{"cartId":"cart-success","shippingAddressId":"addr-home","paymentMethodId":"pm-ok","idempotencyKey":"order-resume-001"}')"
resume_order_id="$(json_field "$resume_body" orderId)"
projection_continue_body="$(post_json "$API_A_HTTP_URL" /orders/start '{"cartId":"cart-success","shippingAddressId":"addr-home","paymentMethodId":"pm-ok","idempotencyKey":"order-projection-continue-001"}')"
projection_continue_order_id="$(json_field "$projection_continue_body" orderId)"
wait_order_status "$API_A_HTTP_URL" "$projection_continue_order_id" Confirmed
post_json "$API_A_HTTP_URL" /self-check/projection/delete "{\"orderId\":\"$projection_continue_order_id\"}" >/dev/null
projection_rebuild_body="$(post_json "$API_B_HTTP_URL" /orders/start '{"cartId":"cart-success","shippingAddressId":"addr-home","paymentMethodId":"pm-ok","idempotencyKey":"order-projection-rebuild-001"}')"
projection_rebuild_order_id="$(json_field "$projection_rebuild_body" orderId)"
wait_order_status "$API_B_HTTP_URL" "$projection_rebuild_order_id" Confirmed
post_json "$API_B_HTTP_URL" /self-check/projection/delete "{\"orderId\":\"$projection_rebuild_order_id\"}" >/dev/null

"$BIN_DIR/sample_cpp_framework_shoppingmall_client" \
  --api-a-http-url "$API_A_HTTP_URL" --api-b-http-url "$API_B_HTTP_URL" \
  --resume-order-id "$resume_order_id" \
  --projection-continue-order-id "$projection_continue_order_id" \
  --projection-rebuild-order-id "$projection_rebuild_order_id" >"$LOG_DIR/client.log" 2>&1 || {
  dump_logs
  exit 1
}
wait_exact_line "Client completion marker" "$LOG_DIR/client.log" "shoppingmall=completed" 1

produced_order() {
  local name="$1"
  mapfile -t values < <(sed -n "s/^shoppingmall-produced name=${name} order=//p" "$LOG_DIR/client.log")
  [[ "${#values[@]}" == 1 && -n "${values[0]}" ]] || {
    echo "expected one Client-produced order for $name, found ${#values[@]}" >&2
    return 1
  }
  printf '%s' "${values[0]}"
}

success_order_id="$(produced_order success)"
concurrent_order_id="$(produced_order concurrent)"
client_pending_order_id="$(produced_order pending)"
inventory_failure_order_id="$(produced_order inventory-failure)"
payment_failure_order_id="$(produced_order payment-failure)"
scale_out_order_id="$(produced_order scale-out)"
[[ "$client_pending_order_id" == "$pending_order_id" ]] || {
  echo "Client pending order did not use this run's prepared identity" >&2
  exit 1
}

# The Client has already exercised only the public order API.  The runner now
# creates its own checkpoint fixture, asks its current workflow host to run the
# public planned-maintenance operation, and waits for the relocated order.
planned_relocation_body="$(post_json "$API_A_HTTP_URL" /self-check/workflow/inventory-reserved '{"cartId":"cart-success","shippingAddressId":"addr-home","paymentMethodId":"pm-ok","idempotencyKey":"order-planned-relocation-001"}')"
planned_relocation_order_id="$(json_field "$planned_relocation_body" orderId)"
relocation_trigger="$(post_json "$WORKFLOW_A_HTTP_URL" /self-check/relocation "{\"orderId\":\"$planned_relocation_order_id\"}")"
if [[ "$(json_field "$relocation_trigger" accepted)" != true ]]; then
  relocation_trigger="$(post_json "$WORKFLOW_B_HTTP_URL" /self-check/relocation "{\"orderId\":\"$planned_relocation_order_id\"}")"
fi
[[ "$(json_field "$relocation_trigger" accepted)" == true ]] || {
  echo "planned relocation trigger did not find the active workflow spot" >&2
  exit 1
}
# The relocated workflow fixture resumes from its target lifecycle and finishes
# the order itself; the runner does not issue a continuation after relocation.
wait_order_status "$API_A_HTTP_URL" "$planned_relocation_order_id" Confirmed
assertion_body="$(python3 - "$success_order_id" "$client_pending_order_id" "$concurrent_order_id" "$resume_order_id" "$inventory_failure_order_id" "$payment_failure_order_id" "$scale_out_order_id" <<'PY'
import json
import sys
keys = ("successfulOrderId", "pendingRecoveredOrderId", "concurrentOrderId", "resumedOrderId",
        "inventoryFailureOrderId", "paymentFailureOrderId", "scaleOutOrderId")
print(json.dumps(dict(zip(keys, sys.argv[1:]))))
PY
)"
assertion_result="$(post_json "$API_A_HTTP_URL" /self-check/assert "$assertion_body")"
[[ "$(json_field "$assertion_result" passed)" == true ]] || {
  echo "ShoppingMall server assertion failed: $assertion_result" >&2
  exit 1
}

# Each workflow log is checked separately; framework logs are not evidence.
wait_pattern_minimum "workflow-a order start" "$LOG_DIR/workflow-a.log" '^shoppingmall-order started order=.+ spot=.+$' 1
wait_pattern_minimum "workflow-b order start" "$LOG_DIR/workflow-b.log" '^shoppingmall-order started order=.+ spot=.+$' 1
wait_prefix_minimum "CommerceApi evidence" "$LOG_DIR/api-a.log" "shoppingmall-evidence order=$success_order_id events=" 1
wait_prefix_exact_across "planned-relocation replay" "shoppingmall-order replayed order=" 1 \
  "$LOG_DIR/workflow-a.log" "$LOG_DIR/workflow-b.log"
wait_prefix_exact_across "repeated external effect" "shoppingmall-order external-effect-repeated order=" 0 \
  "$LOG_DIR/workflow-a.log" "$LOG_DIR/workflow-b.log"

# Cleanup precedes the placement marker, which is the runner's final line.
trap - EXIT
cleanup
echo "shoppingmall-placement=completed"
