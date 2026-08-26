#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

if rg -n 'customer-1' \
    Server/Tracking/src/main/kotlin --glob 'DeliveryStatusChangedHandler.kt'; then
  echo "Tracking must route status by DeliveryStatusChangedReq.customerId" >&2
  exit 1
fi
if ! rg -q 'customerId = request.customerId' \
    Server/Tracking/src/main/kotlin --glob 'DeliveryStatusChangedHandler.kt'; then
  echo "Tracking must preserve the delivery customer id" >&2
  exit 1
fi
if ! rg -q 'waitForSequence<DeliveryStatusNotify>' \
    Client/src/main/kotlin --glob 'Program.kt'; then
  echo "Client must assert notification arrival order with the connector helper" >&2
  exit 1
fi
if ! rg -q 'expectNone<OfferDeliveryNotify>' \
    Client/src/main/kotlin --glob 'Program.kt'; then
  echo "Client must verify that the other courier receives no offer" >&2
  exit 1
fi
if ! rg -q 'ZLinkKotlinStreamAssert\.ensure\(' \
    Client/src/main/kotlin --glob 'Program.kt'; then
  echo "Client must use the connector assertion utility" >&2
  exit 1
fi
if rg -n 'StatusWaits|arrivals|waitStatus\(|waitStatuses\(' \
    Client/src/main/kotlin --glob 'Program.kt'; then
  echo "Client must not rebuild the connector sequence helper locally" >&2
  exit 1
fi
if rg -n 'runScaffold|waitNotifications|readNotifications|--stream-runtime' \
    Client/src/main/kotlin --glob '*.kt'; then
  echo "DeliveryDispatch client must use the stream connector path only" >&2
  exit 1
fi
coroutine_hosts=(
  Server/CourierSession/src/main/kotlin
  Server/CourierSpotNode/src/main/kotlin
  Server/CustomerGateway/src/main/kotlin
  Server/Dispatch/src/main/kotlin
  Server/Tracking/src/main/kotlin
)
for host in "${coroutine_hosts[@]}"; do
  if ! rg -q 'useCoroutineHandlers\(Dispatchers\.Default\)' "${host}" --glob '*.kt'; then
    echo "DeliveryDispatch framework host must configure coroutine handlers: ${host}" >&2
    exit 1
  fi
done

source "../../runner-common.sh"
zlink_sample_configure_port_pool kotlin
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)

pids=()
redis_container_id=""
log_dir="build/sample-logs"
# Graceful-shutdown verification in runner-common.sh needs the role log list; without it the
# cleanup hook fails with "Framework role logs are not configured for this sample".
ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="tracking.log customer-gateway.log courier-session.log courier-node1.log courier-node2.log dispatch.log"
state_dir="$(pwd)/build/sample-state"
flow_log_dir="$(pwd)/logs"
config_dir="build/sample-config"
readonly log_wait_attempts=300
readonly log_wait_interval=0.1
export ZLINK_JAVA_STREAM_TRACE="${ZLINK_JAVA_STREAM_TRACE:-1}"
mkdir -p "${log_dir}" "${flow_log_dir}" "${config_dir}"
rm -f "${log_dir}"/*.log
rm -f "${flow_log_dir}"/*.log "${config_dir}"/*.properties
rm -rf "${state_dir}"
mkdir -p "${state_dir}"

print_logs() {
  local status="$1"
  if [[ "${status}" == "0" ]]; then
    return
  fi
  for log in "${log_dir}"/*.log; do
    [[ -f "${log}" ]] || continue
    echo "===== ${log} =====" >&2
    tail -n 200 "${log}" >&2 || true
  done
}

log_count() {
  local evidence="$1"
  shift
  { grep -Fh -- "${evidence}" "$@" 2>/dev/null || true; } | wc -l | tr -d '[:space:]'
}

wait_log_count() {
  local expected="$1" evidence="$2"
  shift 2
  local actual
  for _ in $(seq 1 "${log_wait_attempts}"); do
    actual="$(log_count "${evidence}" "$@")"
    if [[ "${actual}" == "${expected}" ]]; then
      return 0
    fi
    if (( actual > expected )); then
      echo "Expected ${expected} matches for '${evidence}' in $*, found ${actual}." >&2
      return 1
    fi
    sleep "${log_wait_interval}"
  done
  echo "Timed out waiting for ${expected} matches for '${evidence}' in $*." >&2
  return 1
}

trap cleanup EXIT

build_framework_jars() {
  (
    cd ../../..
    zlink_sample_gradle_locked ./gradlew --no-daemon --no-parallel --max-workers=1 \
      :zlink-framework-core:jar \
      :zlink-framework-spring-boot-starter:jar \
      :zlink-framework-locations-redis:jar \
      :zlink-stream-connector:jar \
      --quiet
  )
}

read -r tracking tracking_spot customer_stream courier_stream dispatch_http dispatch_spot dispatch_channel customer_spot customer_router courier_node1_spot courier_node2_spot courier_session_spot \
  <<<"$(zlink_sample_reserve_endpoints 12)"

endpoint_host() { echo "${1%:*}"; }
endpoint_port() { echo "${1##*:}"; }

deliverydispatch_redis_key_prefix="${DELIVERYDISPATCH_REDIS_KEY_PREFIX:-deliverydispatch:kotlin:${RANDOM}:$$:}"
zlink_redis_start_scoped_assign redis_container_id redis_port \
  "zlink-redis-kotlin-sample-deliverydispatch" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
DELIVERYDISPATCH_REDIS_ENDPOINT="127.0.0.1:${redis_port}"
wait_port "${DELIVERYDISPATCH_REDIS_ENDPOINT%:*}" "${DELIVERYDISPATCH_REDIS_ENDPOINT##*:}"
write_config() {
  local path="$1" courier_node="$2"
  cat >"$path" <<EOF
trackingChannelEndpoint=tcp://$(endpoint_host "${tracking}"):$(endpoint_port "${tracking}")
trackingSpotEndpoint=tcp://$(endpoint_host "${tracking_spot}"):$(endpoint_port "${tracking_spot}")
customerStreamEndpoint=tcp://$(endpoint_host "${customer_stream}"):$(endpoint_port "${customer_stream}")
courierStreamEndpoint=tcp://$(endpoint_host "${courier_stream}"):$(endpoint_port "${courier_stream}")
dispatchHttpEndpoint=http://$(endpoint_host "${dispatch_http}"):$(endpoint_port "${dispatch_http}")
dispatchSpotEndpoint=tcp://$(endpoint_host "${dispatch_spot}"):$(endpoint_port "${dispatch_spot}")
dispatchChannelEndpoint=tcp://$(endpoint_host "${dispatch_channel}"):$(endpoint_port "${dispatch_channel}")
customerSpotEndpoint=tcp://$(endpoint_host "${customer_spot}"):$(endpoint_port "${customer_spot}")
customerSpotRouterEndpoint=tcp://$(endpoint_host "${customer_router}"):$(endpoint_port "${customer_router}")
courierActorNode1SpotEndpoint=tcp://$(endpoint_host "${courier_node1_spot}"):$(endpoint_port "${courier_node1_spot}")
courierActorNode2SpotEndpoint=tcp://$(endpoint_host "${courier_node2_spot}"):$(endpoint_port "${courier_node2_spot}")
courierSessionSpotEndpoint=tcp://$(endpoint_host "${courier_session_spot}"):$(endpoint_port "${courier_session_spot}")
redisEndpoint=${DELIVERYDISPATCH_REDIS_ENDPOINT}
redisKeyPrefix=${deliverydispatch_redis_key_prefix}
courierNode=${courier_node}
logDirectory=${flow_log_dir}
stateDirectory=${state_dir}
EOF
  chmod 0600 "$path"
}
tracking_config="${config_dir}/tracking.properties"
customer_gateway_config="${config_dir}/customer-gateway.properties"
courier_session_config="${config_dir}/courier-session.properties"
courier_node1_config="${config_dir}/courier-node1.properties"
courier_node2_config="${config_dir}/courier-node2.properties"
dispatch_config="${config_dir}/dispatch.properties"
client_config="${config_dir}/client.properties"
write_config "$tracking_config" node1
write_config "$customer_gateway_config" node1
write_config "$courier_session_config" node1
write_config "$courier_node1_config" node1
write_config "$courier_node2_config" node2
write_config "$dispatch_config" node1
write_config "$client_config" node1

build_framework_jars
gradle_run \
  :Server:Tracking:installDist \
  :Server:CustomerGateway:installDist \
  :Server:CourierSession:installDist \
  :Server:CourierSpotNode:installDist \
  :Server:Dispatch:installDist \
  :Client:installDist

"$(app_bin Server/Tracking Tracking)" --config "$tracking_config" >"${log_dir}/tracking.log" 2>&1 &
pids+=("$!")
wait_port "$(endpoint_host "${tracking}")" "$(endpoint_port "${tracking}")"
wait_port "$(endpoint_host "${tracking_spot}")" "$(endpoint_port "${tracking_spot}")"

"$(app_bin Server/CustomerGateway CustomerGateway)" --config "$customer_gateway_config" >"${log_dir}/customer-gateway.log" 2>&1 &
pids+=("$!")
wait_port "$(endpoint_host "${customer_stream}")" "$(endpoint_port "${customer_stream}")"
wait_port "$(endpoint_host "${customer_router}")" "$(endpoint_port "${customer_router}")"

"$(app_bin Server/CourierSession CourierSession)" --config "$courier_session_config" >"${log_dir}/courier-session.log" 2>&1 &
pids+=("$!")
wait_port "$(endpoint_host "${courier_stream}")" "$(endpoint_port "${courier_stream}")"
wait_port "$(endpoint_host "${courier_session_spot}")" "$(endpoint_port "${courier_session_spot}")"

"$(app_bin Server/CourierSpotNode CourierSpotNode)" --config "$courier_node1_config" >"${log_dir}/courier-node1.log" 2>&1 &
pids+=("$!")
"$(app_bin Server/CourierSpotNode CourierSpotNode)" --config "$courier_node2_config" >"${log_dir}/courier-node2.log" 2>&1 &
pids+=("$!")
wait_port "$(endpoint_host "${courier_node1_spot}")" "$(endpoint_port "${courier_node1_spot}")"
wait_port "$(endpoint_host "${courier_node2_spot}")" "$(endpoint_port "${courier_node2_spot}")"

"$(app_bin Server/Dispatch Dispatch)" --config "$dispatch_config" >"${log_dir}/dispatch.log" 2>&1 &
pids+=("$!")
wait_port "$(endpoint_host "${dispatch_http}")" "$(endpoint_port "${dispatch_http}")"
wait_port "$(endpoint_host "${dispatch_spot}")" "$(endpoint_port "${dispatch_spot}")"
wait_log_count 1 "deliverydispatch-ready kind=route node=tracking" "${log_dir}/tracking.log"
wait_log_count 1 "deliverydispatch-ready kind=route node=customer-gateway" "${log_dir}/customer-gateway.log"
wait_log_count 1 "deliverydispatch-ready kind=route node=courier-session" "${log_dir}/courier-session.log"
wait_log_count 1 "deliverydispatch-ready kind=route node=courier-node-1" "${log_dir}/courier-node1.log"
wait_log_count 1 "deliverydispatch-ready kind=route node=courier-node-2" "${log_dir}/courier-node2.log"
wait_log_count 1 "deliverydispatch-ready kind=route node=dispatch" "${log_dir}/dispatch.log"
wait_log_count 1 "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-1" "${log_dir}/dispatch.log"
wait_log_count 1 "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-2" "${log_dir}/dispatch.log"

"$(app_bin Client Client)" --config "$client_config" >"${log_dir}/client.log" 2>&1
cat "${log_dir}/client.log"

wait_log_count 1 "deliverydispatch-reassignment=completed" "${log_dir}/client.log"
wait_log_count 1 "deliverydispatch-server-evidence=completed" "${log_dir}/client.log"
wait_log_count 1 "deliverydispatch=completed" "${log_dir}/client.log"
wait_log_count 1 "deliverydispatch-courier bound courier=courier-a" "${log_dir}/courier-session.log"
wait_log_count 1 "deliverydispatch-courier bound courier=courier-b" "${log_dir}/courier-session.log"
wait_log_count 1 "deliverydispatch-courier bind-relayed courier=courier-a" "${log_dir}"/courier-node*.log
wait_log_count 1 "deliverydispatch-courier bind-relayed courier=courier-b" "${log_dir}"/courier-node*.log
wait_log_count 1 "deliverydispatch-customer bound customer=customer-1" "${log_dir}/customer-gateway.log"
wait_log_count 2 "deliverydispatch-customer pushed status=Delivered" "${log_dir}/customer-gateway.log"
wait_log_count 2 "deliverydispatch-tracking status=Delivered" "${log_dir}/tracking.log"
wait_log_count 1 "deliverydispatch-dispatch stale-decision-ignored delivery=delivery-reassign courier=courier-a attempt=1" "${log_dir}/dispatch.log"
wait_log_count 1 "deliverydispatch-dispatch failed delivery=delivery-exhausted reason=candidates-exhausted" "${log_dir}/dispatch.log"

echo "deliverydispatch-placement=completed"
