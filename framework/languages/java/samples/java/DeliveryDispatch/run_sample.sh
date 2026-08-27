#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

source "../../runner-common.sh"
zlink_sample_configure_port_pool java
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)
readonly DELIVERYDISPATCH_WAIT_ATTEMPTS=300
readonly DELIVERYDISPATCH_WAIT_INTERVAL_SECONDS=0.1

if rg -n 'System\.(getProperty|getenv)' Server Client --glob '*.java'; then
  echo "DeliveryDispatch application code must use sample config files" >&2
  exit 1
fi
if rg -n 'java\.util\.Properties|SampleTopology\.[A-Z]|SampleTopology\.configure' \
    Server --glob '*.java'; then
  echo "DeliveryDispatch server config must use typed Spring binding" >&2
  exit 1
fi
if ! rg -q '@ConfigurationProperties\("sample"\)' \
    Server/Configuration/src/main/java --glob 'SampleTopology.java' || \
    ! rg -q 'SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME' \
    Server/Configuration/src/main/java --glob 'SampleApplication.java'; then
  echo "DeliveryDispatch must isolate Spring config from environment and JVM providers" >&2
  exit 1
fi
if rg -n -U '\.enableClient\(\s*[^)\s]|\.connect(?:Router|PeerPub)\(' Server --glob '*.java'; then
  echo "DeliveryDispatch server code must use location-store automatic connections" >&2
  exit 1
fi
if ! rg -q 'waitForSequence\(Messages\.DeliveryStatusNotify\.class\)' \
    Client/src/main/java --glob 'DeliveryDispatchClientScenario.java'; then
  echo "DeliveryDispatch client must use the connector sequence helper" >&2
  exit 1
fi
if ! rg -q 'expectNone\(Messages\.OfferDeliveryNotify\.class\)' \
    Client/src/main/java --glob 'DeliveryDispatchClientScenario.java'; then
  echo "DeliveryDispatch must verify that the other courier receives no offer" >&2
  exit 1
fi
if ! rg -q 'ZLinkStreamAssert\.ensure\(' \
    Client/src/main/java --glob 'DeliveryDispatchClientScenario.java'; then
  echo "DeliveryDispatch must use the connector assertion utility" >&2
  exit 1
fi
if rg -n 'assertStatusOrder|observedStatuses|waitStatuses\(' \
    Client/src/main/java --glob 'DeliveryDispatchClientScenario.java'; then
  echo "DeliveryDispatch client must not rebuild the connector sequence helper locally" >&2
  exit 1
fi

if grep -q 'Server:CourierGateway' standalone.settings.gradle.kts; then
  echo "DeliveryDispatch must not include the dead CourierGateway role" >&2
  exit 1
fi
if grep -R -q '@ZLinkHandlerGroup("customer-route")' Server/CustomerGateway/src/main/java; then
  echo "DeliveryDispatch must not retain the unregistered customer-route handlers" >&2
  exit 1
fi

pids=()
redis_container_id=""
log_dir="build/sample-logs"
ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="tracking.log customer-gateway.log courier-session.log courier-node-1.log courier-node-2.log dispatch.log"
flow_log_dir="$(pwd)/logs"
config_dir="$(mktemp -d)"
chmod 0700 "${config_dir}"
mkdir -p "${log_dir}" "${flow_log_dir}"
rm -f "${log_dir}"/*.log
rm -f "${flow_log_dir}"/*.log

count_evidence() {
  local evidence="$1"
  shift
  local count=0 log matches
  for log in "$@"; do
    [[ -f "${log}" ]] || continue
    matches="$(grep -Fc "${evidence}" "${log}" || true)"
    count=$((count + matches))
  done
  echo "${count}"
}

wait_for_evidence_count() {
  local description="$1" expected="$2" evidence="$3"
  shift 3
  local attempt actual
  for ((attempt=1; attempt<=DELIVERYDISPATCH_WAIT_ATTEMPTS; attempt++)); do
    actual="$(count_evidence "${evidence}" "$@")"
    if [[ "${actual}" == "${expected}" ]]; then
      return 0
    fi
    if (( actual > expected )); then
      echo "DeliveryDispatch ${description}: expected ${expected} '${evidence}', found ${actual}" >&2
      return 1
    fi
    sleep "${DELIVERYDISPATCH_WAIT_INTERVAL_SECONDS}"
  done
  echo "DeliveryDispatch ${description}: timed out waiting for ${expected} '${evidence}'" >&2
  return 1
}

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

deliverydispatch_cleanup() {
  local status="$?"
  local cleanup_failed=0
  set +e
  print_logs "${status}"

  for ((i=${#pids[@]}-1; i>=0; i--)); do
    kill "${pids[$i]}" >/dev/null 2>&1 || true
  done

  # Runtime drain uses the public 30-second deadline and may then finish its
  # bounded owner/resource cleanup. Observe it for 90 seconds before SIGKILL.
  for _ in $(seq 1 900); do
    local running=0
    for pid in "${pids[@]}"; do
      local state
      state="$(ps -o stat= -p "${pid}" 2>/dev/null | tr -d ' ')"
      if [[ -n "${state}" && "${state}" != Z* ]]; then
        running=1
        break
      fi
    done
    [[ "${running}" == "0" ]] && break
    sleep 0.1
  done

  for pid in "${pids[@]}"; do
    local state exit_code
    state="$(ps -o stat= -p "${pid}" 2>/dev/null | tr -d ' ')"
    if [[ -n "${state}" && "${state}" != Z* ]]; then
      kill -9 "${pid}" >/dev/null 2>&1 || true
      cleanup_failed=1
    fi
    wait "${pid}"
    exit_code="$?"
    if [[ "${exit_code}" != "0" && "${exit_code}" != "143" ]]; then
      echo "deliverydispatch cleanup process ${pid} exited with ${exit_code}" >&2
      cleanup_failed=1
    fi
  done

  if [[ -n "${redis_container_id}" ]]; then
    zlink_redis_remove_by_id "${redis_container_id}" || cleanup_failed=1
  fi
  if [[ "${status}" == "0" && "${cleanup_failed}" == "0" ]]; then
    zlink_sample_verify_framework_termination "${log_dir}" || cleanup_failed=1
  fi
  rm -rf "${config_dir}"
  if [[ "${status}" != "0" ]]; then
    exit "${status}"
  fi
  if [[ "${cleanup_failed}" != "0" ]]; then
    exit 1
  fi
}

trap deliverydispatch_cleanup EXIT

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

read -r tracking customer_stream courier_stream dispatch_http dispatch_spot dispatch_channel customer_spot customer_router tracking_spot_router tracking_spot_pub courier_node1_spot courier_node2_spot courier_session_spot \
  <<<"$(zlink_sample_reserve_endpoints 13)"

endpoint_host() { echo "${1%:*}"; }
endpoint_port() { echo "${1##*:}"; }

deliverydispatch_redis_key_prefix="deliverydispatch:java:${RANDOM}:$$:"
zlink_redis_start_scoped_assign redis_container_id redis_port \
  "zlink-redis-java-sample-deliverydispatch" "redis:7.2-alpine"
redis_endpoint="127.0.0.1:${redis_port}"
wait_port "${redis_endpoint%:*}" "${redis_endpoint##*:}"
write_config() {
  local path="$1" role="$2" courier_node="${3:-node1}"
  if [[ "${role}" == "client" ]]; then
    cat >"$path" <<EOF
customerStreamEndpoint=tcp://$(endpoint_host "${customer_stream}"):$(endpoint_port "${customer_stream}")
courierStreamEndpoint=tcp://$(endpoint_host "${courier_stream}"):$(endpoint_port "${courier_stream}")
dispatchHttpEndpoint=http://$(endpoint_host "${dispatch_http}"):$(endpoint_port "${dispatch_http}")
EOF
    chmod 0600 "$path"
    return
  fi
  cat >"$path" <<EOF
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${deliverydispatch_redis_key_prefix}
sample.logDirectory=${flow_log_dir}
EOF
  case "${role}" in
    tracking)
      cat >>"$path" <<EOF
sample.trackingChannelEndpoint=tcp://$(endpoint_host "${tracking}"):$(endpoint_port "${tracking}")
sample.trackingSpotEndpoint=tcp://$(endpoint_host "${tracking_spot_router}"):$(endpoint_port "${tracking_spot_router}")
sample.trackingSpotPubEndpoint=tcp://$(endpoint_host "${tracking_spot_pub}"):$(endpoint_port "${tracking_spot_pub}")
EOF
      ;;
    customer-gateway)
      cat >>"$path" <<EOF
sample.customerStreamEndpoint=tcp://$(endpoint_host "${customer_stream}"):$(endpoint_port "${customer_stream}")
sample.customerSpotEndpoint=tcp://$(endpoint_host "${customer_spot}"):$(endpoint_port "${customer_spot}")
sample.customerSpotRouterEndpoint=tcp://$(endpoint_host "${customer_router}"):$(endpoint_port "${customer_router}")
EOF
      ;;
    courier-session)
      cat >>"$path" <<EOF
sample.courierStreamEndpoint=tcp://$(endpoint_host "${courier_stream}"):$(endpoint_port "${courier_stream}")
sample.courierSessionSpotEndpoint=tcp://$(endpoint_host "${courier_session_spot}"):$(endpoint_port "${courier_session_spot}")
EOF
      ;;
    courier-node)
      cat >>"$path" <<EOF
sample.courierNode=${courier_node}
EOF
      if [[ "${courier_node}" == "node2" ]]; then
        cat >>"$path" <<EOF
sample.courierActorNode2SpotEndpoint=tcp://$(endpoint_host "${courier_node2_spot}"):$(endpoint_port "${courier_node2_spot}")
EOF
      else
        cat >>"$path" <<EOF
sample.courierActorNode1SpotEndpoint=tcp://$(endpoint_host "${courier_node1_spot}"):$(endpoint_port "${courier_node1_spot}")
EOF
      fi
      ;;
    dispatch)
      cat >>"$path" <<EOF
sample.dispatchHttpEndpoint=http://$(endpoint_host "${dispatch_http}"):$(endpoint_port "${dispatch_http}")
sample.dispatchSpotEndpoint=tcp://$(endpoint_host "${dispatch_spot}"):$(endpoint_port "${dispatch_spot}")
sample.dispatchChannelEndpoint=tcp://$(endpoint_host "${dispatch_channel}"):$(endpoint_port "${dispatch_channel}")
EOF
      ;;
  esac
  chmod 0600 "$path"
}
tracking_config="${config_dir}/tracking.properties"
customer_gateway_config="${config_dir}/customer-gateway.properties"
courier_session_config="${config_dir}/courier-session.properties"
courier_node1_config="${config_dir}/courier-node-1.properties"
courier_node2_config="${config_dir}/courier-node-2.properties"
dispatch_config="${config_dir}/dispatch.properties"
client_config="${config_dir}/client.properties"
write_config "$tracking_config" tracking
write_config "$customer_gateway_config" customer-gateway
write_config "$courier_session_config" courier-session
write_config "$courier_node1_config" courier-node node1
write_config "$courier_node2_config" courier-node node2
write_config "$dispatch_config" dispatch
write_config "$client_config" client

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

"$(app_bin Server/CustomerGateway CustomerGateway)" --config "$customer_gateway_config" >"${log_dir}/customer-gateway.log" 2>&1 &
pids+=("$!")

"$(app_bin Server/CourierSession CourierSession)" --config "$courier_session_config" >"${log_dir}/courier-session.log" 2>&1 &
pids+=("$!")

"$(app_bin Server/CourierSpotNode CourierSpotNode)" --config "$courier_node1_config" >"${log_dir}/courier-node-1.log" 2>&1 &
pids+=("$!")
"$(app_bin Server/CourierSpotNode CourierSpotNode)" --config "$courier_node2_config" >"${log_dir}/courier-node-2.log" 2>&1 &
pids+=("$!")

"$(app_bin Server/Dispatch Dispatch)" --config "$dispatch_config" >"${log_dir}/dispatch.log" 2>&1 &
pids+=("$!")
wait_for_evidence_count "tracking route readiness" 1 \
  "deliverydispatch-ready kind=route node=tracking" "${log_dir}/tracking.log"
wait_for_evidence_count "customer gateway route readiness" 1 \
  "deliverydispatch-ready kind=route node=customer-gateway" "${log_dir}/customer-gateway.log"
wait_for_evidence_count "courier session route readiness" 1 \
  "deliverydispatch-ready kind=route node=courier-session" "${log_dir}/courier-session.log"
wait_for_evidence_count "courier node 1 route readiness" 1 \
  "deliverydispatch-ready kind=route node=courier-node-1" "${log_dir}/courier-node-1.log"
wait_for_evidence_count "courier node 2 route readiness" 1 \
  "deliverydispatch-ready kind=route node=courier-node-2" "${log_dir}/courier-node-2.log"
wait_for_evidence_count "dispatch route readiness" 1 \
  "deliverydispatch-ready kind=route node=dispatch" "${log_dir}/dispatch.log"
wait_for_evidence_count "dispatch courier node 1 readiness" 1 \
  "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-1" "${log_dir}/dispatch.log"
wait_for_evidence_count "dispatch courier node 2 readiness" 1 \
  "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-2" "${log_dir}/dispatch.log"

"$(app_bin Client Client)" --config "$client_config" >"${log_dir}/client.log" 2>&1
cat "${log_dir}/client.log"

wait_for_evidence_count "client reassignment marker" 1 \
  "deliverydispatch-reassignment=completed" "${log_dir}/client.log"
wait_for_evidence_count "client server evidence marker" 1 \
  "deliverydispatch-server-evidence=completed" "${log_dir}/client.log"
wait_for_evidence_count "client completion marker" 1 \
  "deliverydispatch=completed" "${log_dir}/client.log"

wait_for_evidence_count "courier a binding" 1 \
  "deliverydispatch-courier bound courier=courier-a" "${log_dir}/courier-session.log"
wait_for_evidence_count "courier b binding" 1 \
  "deliverydispatch-courier bound courier=courier-b" "${log_dir}/courier-session.log"
wait_for_evidence_count "courier a bind relay" 1 \
  "deliverydispatch-courier bind-relayed courier=courier-a" \
  "${log_dir}/courier-node-1.log" "${log_dir}/courier-node-2.log"
wait_for_evidence_count "courier b bind relay" 1 \
  "deliverydispatch-courier bind-relayed courier=courier-b" \
  "${log_dir}/courier-node-1.log" "${log_dir}/courier-node-2.log"
wait_for_evidence_count "customer binding" 1 \
  "deliverydispatch-customer bound customer=customer-1" "${log_dir}/customer-gateway.log"
wait_for_evidence_count "customer delivered pushes" 2 \
  "deliverydispatch-customer pushed status=Delivered delivery=" "${log_dir}/customer-gateway.log"
wait_for_evidence_count "tracking delivered statuses" 2 \
  "deliverydispatch-tracking status=Delivered delivery=" "${log_dir}/tracking.log"
wait_for_evidence_count "stale decision" 1 \
  "deliverydispatch-dispatch stale-decision-ignored delivery=delivery-reassign courier=courier-a attempt=1" \
  "${log_dir}/dispatch.log"
wait_for_evidence_count "candidates exhausted" 1 \
  "deliverydispatch-dispatch failed delivery=delivery-exhausted reason=candidates-exhausted" \
  "${log_dir}/dispatch.log"

deliverydispatch_cleanup
trap - EXIT
echo "deliverydispatch-placement=completed"
