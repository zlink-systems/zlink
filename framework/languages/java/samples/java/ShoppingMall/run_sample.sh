#!/usr/bin/env bash
set -euo pipefail
set +m

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../../runner-common.sh"
zlink_sample_configure_port_pool java
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)

RUN_DIR=""
LOG_DIR=""
REDIS_CONTAINER=""
pids=()

on_exit() {
  local status="$?"
  trap - EXIT
  if [[ "$status" != "0" && -n "${LOG_DIR}" ]]; then
    for log in "$LOG_DIR"/*.log; do
      [[ -f "$log" ]] || continue
      echo "===== $log =====" >&2
      tail -n 200 "$log" >&2 || true
    done
  fi
  cleanup
  [[ -z "${RUN_DIR}" ]] || rm -rf "$RUN_DIR"
  exit "$status"
}
trap on_exit EXIT

cd "$ROOT_DIR"
if rg -n -U '\.enableClient\(\s*[^)\s]|\.connect(?:Router|PeerPub)\(' Server --glob '*.java'; then
  echo "ShoppingMall server code must use location-store automatic connections" >&2
  exit 1
fi
RUN_DIR="$(mktemp -d)"
chmod 0700 "${RUN_DIR}"
LOG_DIR="$RUN_DIR/logs"
ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="workflow-a.log workflow-b.log api-a.log api-b.log"
mkdir -p "$LOG_DIR"

read -r api_a_http_port api_b_http_port workflow_a_http_port workflow_b_http_port \
  workflow_a_channel_port workflow_b_channel_port workflow_a_spot_port workflow_b_spot_port \
  workflow_a_router_port workflow_b_router_port <<<"$(zlink_sample_reserve_ports 10)"
api_a_http="http://127.0.0.1:$api_a_http_port"
api_b_http="http://127.0.0.1:$api_b_http_port"
workflow_a_http="http://127.0.0.1:$workflow_a_http_port"
workflow_b_http="http://127.0.0.1:$workflow_b_http_port"
workflow_a_channel="tcp://127.0.0.1:$workflow_a_channel_port"
workflow_b_channel="tcp://127.0.0.1:$workflow_b_channel_port"
workflow_a_spot="tcp://127.0.0.1:$workflow_a_spot_port"
workflow_b_spot="tcp://127.0.0.1:$workflow_b_spot_port"
workflow_a_router="tcp://127.0.0.1:$workflow_a_router_port"
workflow_b_router="tcp://127.0.0.1:$workflow_b_router_port"

zlink_redis_start_scoped_assign REDIS_CONTAINER REDIS_PORT \
  "zlink-redis-java-sample-shoppingmall" "redis:7.2-alpine"
redis_endpoint="127.0.0.1:$REDIS_PORT"
redis_key_prefix="shoppingmall:java:$(date +%s):$$:"

workflow_a_config="$RUN_DIR/workflow-a.properties"
workflow_b_config="$RUN_DIR/workflow-b.properties"
api_a_config="$RUN_DIR/api-a.properties"
api_b_config="$RUN_DIR/api-b.properties"
client_config="$RUN_DIR/client.properties"
write_common() {
  local path="$1"
  cat >>"$path" <<EOF
sample.logDirectory=${LOG_DIR}
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${redis_key_prefix}
EOF
}
cat >"$workflow_a_config" <<EOF
sample.instanceName=workflow-a
sample.httpUrl=${workflow_a_http}
sample.channelEndpoint=${workflow_a_channel}
sample.spotEndpoint=${workflow_a_spot}
sample.spotRouterEndpoint=${workflow_a_router}
EOF
cat >"$workflow_b_config" <<EOF
sample.instanceName=workflow-b
sample.httpUrl=${workflow_b_http}
sample.channelEndpoint=${workflow_b_channel}
sample.spotEndpoint=${workflow_b_spot}
sample.spotRouterEndpoint=${workflow_b_router}
EOF
cat >"$api_a_config" <<EOF
sample.instanceName=api-a
sample.httpUrl=${api_a_http}
EOF
cat >"$api_b_config" <<EOF
sample.instanceName=api-b
sample.httpUrl=${api_b_http}
EOF
for config in "$workflow_a_config" "$workflow_b_config" "$api_a_config" "$api_b_config"; do write_common "$config"; done
cat >"$client_config" <<EOF
sample.apiAHttpUrl=${api_a_http}
sample.apiBHttpUrl=${api_b_http}
EOF
chmod 0600 "$workflow_a_config" "$workflow_b_config" "$api_a_config" "$api_b_config" "$client_config"

gradle_run :Server:CommerceApi:installDist :Server:OrderWorkflow:installDist :Client:installDist >"$LOG_DIR/build.log" 2>&1
start_role() {
  local name="$1" binary="$2" config="$3"
  "$binary" --config "$config" >"$LOG_DIR/$name.log" 2>&1 &
  pids+=("$!")
}
json_field() {
  local name="$1"
  sed -nE "s/.*\"${name}\"[[:space:]]*:[[:space:]]*\"([^\"]+)\".*/\1/p" | head -n 1
}
json_number() {
  local name="$1"
  sed -nE "s/.*\"${name}\"[[:space:]]*:[[:space:]]*([0-9]+).*/\1/p" | head -n 1
}
post_json() {
  local url="$1" body="$2"
  curl --fail --silent --show-error --max-time 40 \
    -H 'content-type: application/json' --data "$body" "$url"
}
log_count() {
  local pattern="$1" file="$2"
  local count
  count="$(rg -c -- "$pattern" "$file" 2>/dev/null || true)"
  printf '%s\n' "${count:-0}"
}
wait_log_count() {
  local expected="$1" pattern="$2" file="$3"
  for _ in $(seq 1 300); do
    [[ "$(log_count "$pattern" "$file")" == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "Timed out waiting for ${expected} '${pattern}' in ${file}" >&2
  return 1
}
wait_log_at_least() {
  local expected="$1" pattern="$2" file="$3"
  for _ in $(seq 1 300); do
    (( $(log_count "$pattern" "$file") >= expected )) && return 0
    sleep 0.1
  done
  echo "Timed out waiting for ${expected}+ '${pattern}' in ${file}" >&2
  return 1
}
wait_replay_exactly_once() {
  local pattern="$1"
  for _ in $(seq 1 300); do
    local total=$(( $(log_count "$pattern" "$LOG_DIR/workflow-a.log") + $(log_count "$pattern" "$LOG_DIR/workflow-b.log") ))
    [[ "$total" == '1' ]] && return 0
    sleep 0.1
  done
  echo "Timed out waiting for one '${pattern}' across workflow logs" >&2
  return 1
}
client_order() {
  local name="$1"
  sed -nE "s/^shoppingmall-client-order name=${name} order=([^[:space:]]+)$/\1/p" \
    "$LOG_DIR/client.log" | head -n 1
}
start_role workflow-a "$(app_bin Server/OrderWorkflow OrderWorkflow)" "$workflow_a_config"
start_role workflow-b "$(app_bin Server/OrderWorkflow OrderWorkflow)" "$workflow_b_config"
wait_port "$workflow_a_router"
wait_port "$workflow_b_router"
wait_http "$workflow_a_http"
wait_http "$workflow_b_http"

start_role api-a "$(app_bin Server/CommerceApi CommerceApi)" "$api_a_config"
start_role api-b "$(app_bin Server/CommerceApi CommerceApi)" "$api_b_config"
wait_http "$api_a_http"
wait_http "$api_b_http"
wait_log_count 1 'shoppingmall-ready kind=http node=api-a' "$LOG_DIR/api-a.log"
wait_log_count 1 'shoppingmall-ready kind=http node=api-b' "$LOG_DIR/api-b.log"
wait_log_count 1 'shoppingmall-ready kind=object-route node=api-a target=workflow-a' "$LOG_DIR/api-a.log"
wait_log_count 1 'shoppingmall-ready kind=object-route node=api-a target=workflow-b' "$LOG_DIR/api-a.log"
wait_log_count 1 'shoppingmall-ready kind=object-route node=api-b target=workflow-a' "$LOG_DIR/api-b.log"
wait_log_count 1 'shoppingmall-ready kind=object-route node=api-b target=workflow-b' "$LOG_DIR/api-b.log"
"$(app_bin Client Client)" --config "$client_config" >"$LOG_DIR/client.log" 2>&1
cat "$LOG_DIR/client.log"
grep -Fxq 'shoppingmall=completed' "$LOG_DIR/client.log"

success_order="$(client_order success)"
concurrent_order="$(client_order concurrent)"
inventory_failure_order="$(client_order inventory-failure)"
payment_failure_order="$(client_order payment-failure)"
scale_out_order="$(client_order scale-out)"
for order in "$success_order" "$concurrent_order" "$inventory_failure_order" \
  "$payment_failure_order" "$scale_out_order"; do
  [[ -n "$order" ]] || { echo 'Client did not report its produced order ID.' >&2; exit 1; }
done

runner_pending_key="${redis_key_prefix}runner-pending"
runner_relocation_key="${redis_key_prefix}runner-relocation"
pending_json="$(post_json "$api_a_http/self-check/idempotency/pending" \
  "{\"cartId\":\"cart-success\",\"shippingAddressId\":\"addr-home\",\"paymentMethodId\":\"pm-ok\",\"idempotencyKey\":\"${runner_pending_key}\"}")"
pending_order="$(printf '%s' "$pending_json" | json_field orderId)"
[[ -n "$pending_order" ]] || { echo 'Runner pending fixture did not produce an order ID.' >&2; exit 1; }

for attempt in $(seq 1 20); do
  (( $(log_count 'shoppingmall-order started order=' "$LOG_DIR/workflow-a.log") >= 1 )) \
    && (( $(log_count 'shoppingmall-order started order=' "$LOG_DIR/workflow-b.log") >= 1 )) && break
  post_json "$api_a_http/orders/start" \
    "{\"cartId\":\"cart-success\",\"shippingAddressId\":\"addr-home\",\"paymentMethodId\":\"pm-ok\",\"idempotencyKey\":\"${redis_key_prefix}runner-witness-${attempt}\"}" >/dev/null
  sleep 0.1
done
wait_log_at_least 1 'shoppingmall-order started order=' "$LOG_DIR/workflow-a.log"
wait_log_at_least 1 'shoppingmall-order started order=' "$LOG_DIR/workflow-b.log"

checkpoint_json="$(post_json "$api_a_http/self-check/workflow/inventory-reserved" \
  "{\"cartId\":\"cart-success\",\"shippingAddressId\":\"addr-office\",\"paymentMethodId\":\"pm-ok\",\"idempotencyKey\":\"${runner_relocation_key}\"}")"
checkpoint_order="$(printf '%s' "$checkpoint_json" | json_field orderId)"
checkpoint_generation="$(printf '%s' "$checkpoint_json" | json_number objectGeneration)"
[[ -n "$checkpoint_order" ]] || { echo 'Runner relocation fixture did not produce an order ID.' >&2; exit 1; }
[[ -n "$checkpoint_generation" ]] || { echo 'Runner relocation fixture did not report ObjectGeneration.' >&2; exit 1; }

relocation_source=""
for _ in $(seq 1 300); do
  if (( $(log_count "shoppingmall-order started order=${checkpoint_order} " "$LOG_DIR/workflow-a.log") >= 1 )); then
    relocation_source=workflow-a
    relocation_http="$workflow_a_http"
    break
  fi
  if (( $(log_count "shoppingmall-order started order=${checkpoint_order} " "$LOG_DIR/workflow-b.log") >= 1 )); then
    relocation_source=workflow-b
    relocation_http="$workflow_b_http"
    break
  fi
  sleep 0.1
done
[[ -n "$relocation_source" ]] || { echo 'Runner could not locate the relocation source workflow.' >&2; exit 1; }
relocation_json="$(post_json "$relocation_http/self-check/relocate" '{}')"
grep -Fq '"outcome":"RELOCATED"' <<<"$relocation_json"

resume_json="$(post_json "$api_b_http/self-check/workflow/${checkpoint_order}/continue" '{}')"
grep -Fq '"status":"Confirmed"' <<<"$resume_json"

post_json "$api_a_http/self-check/projection/${success_order}/delete" '{}' >/dev/null
post_json "$api_b_http/self-check/projection/${success_order}/rebuild" '{}' | grep -Fq '"status":"Confirmed"'
assertion_json="$(post_json "$api_a_http/self-check/assert" "{\"successfulOrderId\":\"${success_order}\",\"pendingRecoveredOrderId\":\"${pending_order}\",\"concurrentOrderId\":\"${concurrent_order}\",\"resumedOrderId\":\"${checkpoint_order}\",\"inventoryFailureOrderId\":\"${inventory_failure_order}\",\"paymentFailureOrderId\":\"${payment_failure_order}\",\"scaleOutOrderId\":\"${scale_out_order}\"}")"
grep -Fq '"passed":true' <<<"$assertion_json"
wait_log_at_least 1 "shoppingmall-evidence order=${checkpoint_order} events=" "$LOG_DIR/api-a.log"
wait_replay_exactly_once "shoppingmall-order replayed order=${checkpoint_order} generation=${checkpoint_generation}"
wait_log_count 0 "shoppingmall-order external-effect-repeated order=${checkpoint_order}" "$LOG_DIR/workflow-a.log"
wait_log_count 0 "shoppingmall-order external-effect-repeated order=${checkpoint_order}" "$LOG_DIR/workflow-b.log"

trap - EXIT
true
cleanup
rm -rf "$RUN_DIR"
RUN_DIR=""
echo 'shoppingmall-placement=completed'
