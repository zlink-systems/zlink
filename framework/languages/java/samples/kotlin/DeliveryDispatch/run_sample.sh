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
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)

pids=()
redis_container_id=""
log_dir="build/sample-logs"
state_dir="$(pwd)/build/sample-state"
flow_log_dir="$(pwd)/logs"
config_dir="build/sample-config"
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

trap cleanup EXIT

reserve_ports() {
  local base=$((20000 + ((RANDOM + $$) % 1000) * 12 % 9000))
  local endpoints=()
  for offset in $(seq 0 11); do
    endpoints+=("127.0.0.1:$((base + offset))")
  done
  echo "${endpoints[*]}"
}

build_framework_jars() {
  (
    cd ../../..
    ./gradlew --no-daemon \
      :zlink-framework-core:jar \
      :zlink-framework-spring-boot-starter:jar \
      :zlink-framework-locations-redis:jar \
      :zlink-stream-connector:jar \
      --quiet
  )
}

read -r tracking tracking_spot customer_stream courier_stream dispatch_http dispatch_spot dispatch_channel customer_spot customer_router courier_node1_spot courier_node2_spot courier_session_spot < <(reserve_ports)

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
wait_framework_ready_logs "${log_dir}" 1
wait_framework_peer_ready_counts "${log_dir}" \
  "tracking.log:1" \
  "customer-gateway.log:1" \
  "courier-session.log:2" \
  "courier-node1.log:3" \
  "courier-node2.log:3" \
  "dispatch.log:2"

echo "topology=ready"
"$(app_bin Client Client)" --config "$client_config" >"${log_dir}/client.log" 2>&1
cat "${log_dir}/client.log"

grep -q "deliverydispatch-reassignment=completed" "${log_dir}/client.log"
grep -q "deliverydispatch-server-evidence=completed" "${log_dir}/client.log"
grep -q "deliverydispatch=completed" "${log_dir}/client.log"

echo "deliverydispatch full client/server self-check completed"
