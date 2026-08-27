#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
source "../../runner-common.sh"
zlink_sample_configure_port_pool kotlin
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(-c standalone.settings.gradle.kts)

readonly WAIT_ATTEMPTS=300
readonly WAIT_INTERVAL_SECONDS=0.1
pids=()
redis_container_id=""
log_dir="build/sample-logs"
store_dir="build/sample-store"
config_dir="build/sample-config"
ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="workflow-a.log workflow-b.log api-a.log api-b.log"

print_logs() {
  [[ "$1" == 0 ]] && return
  for log in "$log_dir"/*.log; do [[ -f "$log" ]] && tail -n 200 "$log" >&2; done
}
trap cleanup EXIT

for host in Server/CommerceApi/src/main/kotlin Server/OrderWorkflow/src/main/kotlin; do
  rg -q 'useCoroutineHandlers\(Dispatchers\.Default\)' "$host" --glob '*.kt' || {
    echo "ShoppingMall framework host must configure coroutine handlers: $host" >&2; exit 1; }
done
mkdir -p "$log_dir" "$store_dir" "$config_dir"
rm -f "$log_dir"/*.log "$store_dir"/* "$config_dir"/*.properties

read -r -a ports <<<"$(zlink_sample_reserve_ports 8)"
api_a_channel="tcp://127.0.0.1:${ports[0]}"; api_b_channel="tcp://127.0.0.1:${ports[1]}"
workflow_a_channel="tcp://127.0.0.1:${ports[2]}"; workflow_b_channel="tcp://127.0.0.1:${ports[3]}"
api_a_http="http://127.0.0.1:${ports[4]}"; api_b_http="http://127.0.0.1:${ports[5]}"
workflow_a_http="http://127.0.0.1:${ports[6]}"; workflow_b_http="http://127.0.0.1:${ports[7]}"

zlink_redis_start_scoped_assign redis_container_id redis_port \
  "zlink-redis-kotlin-sample-shoppingmall" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
redis_endpoint="127.0.0.1:${redis_port}"
redis_key_prefix="shoppingmall:kotlin:$(date +%s):$$:"

write_role_config() {
  local path="$1" id="$2" channel="$3" http="$4"
  cat >"$path" <<EOF
sample.instanceId=$id
sample.httpEndpoint=$http
sample.logDirectory=$PWD/$log_dir
sample.channelEndpoint=$channel
sample.redisEndpoint=$redis_endpoint
sample.redisKeyPrefix=$redis_key_prefix
sample.storeDirectory=$PWD/$store_dir
EOF
}
workflow_a_config="$config_dir/workflow-a.properties"; workflow_b_config="$config_dir/workflow-b.properties"
api_a_config="$config_dir/api-a.properties"; api_b_config="$config_dir/api-b.properties"; client_config="$config_dir/client.properties"
write_role_config "$workflow_a_config" workflow-a "$workflow_a_channel" "$workflow_a_http"
write_role_config "$workflow_b_config" workflow-b "$workflow_b_channel" "$workflow_b_http"
write_role_config "$api_a_config" api-a "$api_a_channel" "$api_a_http"
write_role_config "$api_b_config" api-b "$api_b_channel" "$api_b_http"
cat >"$client_config" <<EOF
sample.apiAHttpUrl=$api_a_http
sample.apiBHttpUrl=$api_b_http
EOF

(
  cd ../../..
  zlink_sample_gradle_locked ./gradlew --no-daemon --no-parallel --max-workers=1 \
    :zlink-framework-core:jar :zlink-framework-spring-boot-starter:jar \
    :zlink-framework-kotlin:jar :zlink-framework-locations-redis:jar --quiet
)
gradle_run :Server:OrderWorkflow:installDist :Server:CommerceApi:installDist :Client:installDist

start_role() { "$2" --config "$3" >"$log_dir/$1.log" 2>&1 & pids+=("$!"); }
# rg -c prints nothing and exits 1 when there is no match, so `|| true` alone leaves an empty
# string that breaks (( ... >= 1 )). Always emit a number.
count() { local n; n="$(rg -c -- "$1" "$2" 2>/dev/null || true)"; printf %s "${n:-0}"; }
wait_count() {
  local expected="$1" line="$2" log="$3"
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do [[ "$(count "$line" "$log")" == "$expected" ]] && return; sleep "$WAIT_INTERVAL_SECONDS"; done
  echo "Timed out waiting for $expected '$line' in $log" >&2; return 1
}
wait_at_least() {
  local expected="$1" line="$2" log="$3"
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do (( $(count "$line" "$log") >= expected )) && return; sleep "$WAIT_INTERVAL_SECONDS"; done
  echo "Timed out waiting for $expected+ '$line' in $log" >&2; return 1
}
wait_replay_once() {
  local line="$1"
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    local total=$(( $(count "$line" "$log_dir/workflow-a.log") + $(count "$line" "$log_dir/workflow-b.log") ))
    [[ "$total" == 1 ]] && return; (( total > 1 )) && return 1; sleep "$WAIT_INTERVAL_SECONDS"
  done
  echo "Timed out waiting for one '$line' across workflow logs" >&2; return 1
}
wait_total_at_least() {
  local expected="$1" line="$2"; shift 2
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    local total=0; for log in "$@"; do total=$(( total + $(count "$line" "$log") )); done
    (( total >= expected )) && return; sleep "$WAIT_INTERVAL_SECONDS"
  done
  echo "Timed out waiting for $expected+ '$line' across logs" >&2; return 1
}
post() { curl --fail --silent --show-error --max-time 40 -H 'content-type: application/json' --data "$2" "$1"; }
field() { sed -nE "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"([^\"]+)\".*/\1/p" | head -n 1; }
number() { sed -nE "s/.*\"$1\"[[:space:]]*:[[:space:]]*([0-9]+).*/\1/p" | head -n 1; }
client_order() { sed -nE "s/^shoppingmall-client-order name=$1 order=([^[:space:]]+)$/\1/p" "$log_dir/client.log" | head -n 1; }
wait_order_status() {
  local url="$1" order="$2" status="$3"
  for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
    curl --fail --silent --show-error --max-time 5 "$url/orders/$order" | grep -Fq "\"status\":\"$status\"" && return
    sleep "$WAIT_INTERVAL_SECONDS"
  done
  echo "Timed out waiting for $order to reach $status" >&2; return 1
}

start_role workflow-a "$(app_bin Server/OrderWorkflow OrderWorkflow)" "$workflow_a_config"
start_role workflow-b "$(app_bin Server/OrderWorkflow OrderWorkflow)" "$workflow_b_config"
wait_port "$workflow_a_channel"; wait_port "$workflow_b_channel"; wait_http "$workflow_a_http"; wait_http "$workflow_b_http"
start_role api-a "$(app_bin Server/CommerceApi CommerceApi)" "$api_a_config"
start_role api-b "$(app_bin Server/CommerceApi CommerceApi)" "$api_b_config"
wait_port "$api_a_channel"; wait_port "$api_b_channel"; wait_http "$api_a_http"; wait_http "$api_b_http"

wait_count 1 'shoppingmall-ready kind=http node=api-a' "$log_dir/api-a.log"
wait_count 1 'shoppingmall-ready kind=http node=api-b' "$log_dir/api-b.log"
for api in a b; do for workflow in a b; do
  wait_count 1 "shoppingmall-ready kind=object-route node=api-$api target=workflow-$workflow" "$log_dir/api-$api.log"
done; done

pending_key="${redis_key_prefix}runner-pending"
pending_json="$(post "$api_a_http/self-check/idempotency/pending" "{\"cartId\":\"cart-success\",\"shippingAddressId\":\"addr-home\",\"paymentMethodId\":\"pm-ok\",\"idempotencyKey\":\"$pending_key\"}")"
pending_order="$(printf %s "$pending_json" | field orderId)"; [[ -n "$pending_order" ]]

for attempt in $(seq 1 20); do
  (( $(count 'shoppingmall-order started order=' "$log_dir/workflow-a.log") >= 1 )) && (( $(count 'shoppingmall-order started order=' "$log_dir/workflow-b.log") >= 1 )) && break
  post "$api_a_http/orders/start" "{\"cartId\":\"cart-inventory-fail\",\"shippingAddressId\":\"addr-home\",\"paymentMethodId\":\"pm-ok\",\"idempotencyKey\":\"${redis_key_prefix}runner-witness-$attempt\"}" >/dev/null
  sleep "$WAIT_INTERVAL_SECONDS"
done
wait_at_least 1 'shoppingmall-order started order=' "$log_dir/workflow-a.log"
wait_at_least 1 'shoppingmall-order started order=' "$log_dir/workflow-b.log"

checkpoint_json="$(post "$api_a_http/self-check/workflow/inventory-reserved" "{\"cartId\":\"cart-success\",\"shippingAddressId\":\"addr-office\",\"paymentMethodId\":\"pm-ok\",\"idempotencyKey\":\"${redis_key_prefix}runner-relocation\"}")"
checkpoint_order="$(printf %s "$checkpoint_json" | field orderId)"; checkpoint_generation="$(printf %s "$checkpoint_json" | number objectGeneration)"
[[ -n "$checkpoint_order" && -n "$checkpoint_generation" ]]
rebuild_json="$(post "$api_a_http/orders/start" "{\"cartId\":\"cart-success\",\"shippingAddressId\":\"addr-home\",\"paymentMethodId\":\"pm-ok\",\"idempotencyKey\":\"${redis_key_prefix}runner-rebuild\"}")"
rebuild_order="$(printf %s "$rebuild_json" | field orderId)"; [[ -n "$rebuild_order" ]]
wait_order_status "$api_a_http" "$rebuild_order" Confirmed
post "$api_a_http/self-check/projection/$rebuild_order/delete" '{}' >/dev/null
relocation_http=""
for _ in $(seq 1 "$WAIT_ATTEMPTS"); do
  if (( $(count "shoppingmall-order started order=$checkpoint_order " "$log_dir/workflow-a.log") >= 1 )); then relocation_http="$workflow_a_http"; break; fi
  if (( $(count "shoppingmall-order started order=$checkpoint_order " "$log_dir/workflow-b.log") >= 1 )); then relocation_http="$workflow_b_http"; break; fi
  sleep "$WAIT_INTERVAL_SECONDS"
done
[[ -n "$relocation_http" ]] || { echo 'Runner could not locate the relocation source workflow.' >&2; exit 1; }
post "$relocation_http/self-check/relocate" '{}' | grep -Fq '"outcome":"RELOCATED"'
cat >"$client_config" <<EOF
sample.apiAHttpUrl=$api_a_http
sample.apiBHttpUrl=$api_b_http
sample.pendingIdempotencyKey=$pending_key
sample.pendingOrderId=$pending_order
sample.resumeOrderId=$checkpoint_order
sample.rebuildOrderId=$rebuild_order
EOF

"$(app_bin Client Client)" --config "$client_config" >"$log_dir/client.log" 2>&1
grep -Fxq 'shoppingmall=completed' "$log_dir/client.log"
success_order="$(client_order success)"; concurrent_order="$(client_order concurrent)"
inventory_failure_order="$(client_order inventory-failure)"; payment_failure_order="$(client_order payment-failure)"; scale_out_order="$(client_order scale-out)"
for order in "$success_order" "$concurrent_order" "$inventory_failure_order" "$payment_failure_order" "$scale_out_order"; do [[ -n "$order" ]] || { echo 'Client did not report its produced order ID.' >&2; exit 1; }; done
assertion="$(post "$api_a_http/self-check/assert" "{\"successfulOrderId\":\"$success_order\",\"pendingRecoveredOrderId\":\"$pending_order\",\"concurrentOrderId\":\"$concurrent_order\",\"resumedOrderId\":\"$checkpoint_order\",\"inventoryFailureOrderId\":\"$inventory_failure_order\",\"paymentFailureOrderId\":\"$payment_failure_order\",\"scaleOutOrderId\":\"$scale_out_order\"}")"
grep -Fq '"passed":true' <<<"$assertion"
wait_total_at_least 1 "shoppingmall-evidence order=$checkpoint_order events=" "$log_dir/api-a.log" "$log_dir/api-b.log"
wait_replay_once "shoppingmall-order replayed order=$checkpoint_order generation=$checkpoint_generation"
wait_count 0 "shoppingmall-order external-effect-repeated order=$checkpoint_order" "$log_dir/workflow-a.log"
wait_count 0 "shoppingmall-order external-effect-repeated order=$checkpoint_order" "$log_dir/workflow-b.log"

trap - EXIT
cleanup
echo 'shoppingmall-placement=completed'
