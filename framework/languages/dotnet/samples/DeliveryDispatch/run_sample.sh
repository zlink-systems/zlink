#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../redis-common.sh"
RUN_DIR="$(mktemp -d)"
RUN_DIR="$(cd "${RUN_DIR}" && pwd)"
LOG_DIR="${RUN_DIR}/logs"
WORK_DIR="${RUN_DIR}/work"
SAMPLE_LOG_DIR="${RUN_DIR}/sample-logs"
CONFIG_DIR="${RUN_DIR}/config"
mkdir -p "${LOG_DIR}" "${WORK_DIR}" "${CONFIG_DIR}" "${SAMPLE_LOG_DIR}"

PIDS=()
REDIS_CONTAINER=""
RUN_SUCCEEDED=0

cleanup() {
  find "${RUN_DIR}" -type f -name "*.json" -delete 2>/dev/null || true
  zlink_sample_stop_processes "${PIDS[@]}"
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    zlink_redis_remove_by_id "${REDIS_CONTAINER}" || true
  fi
  zlink_sample_copy_evidence "${RUN_DIR}" "DeliveryDispatch"
  if [[ "${RUN_SUCCEEDED}" == "1" ]]; then
    rm -rf "${RUN_DIR}"
  else
    echo "runDir=${RUN_DIR}"
  fi
}
trap cleanup EXIT

read -r -a PORTS <<<"$(python3 - <<'PY'
import random
import socket

sockets = []
chosen = set()
try:
    while len(sockets) < 9:
        port = random.randint(22100, 23999)
        if port in chosen:
            continue
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.bind(("127.0.0.1", port))
        except OSError:
            sock.close()
            continue
        chosen.add(port)
        sockets.append(sock)
    print(" ".join(str(sock.getsockname()[1]) for sock in sockets))
finally:
    for sock in sockets:
        sock.close()
PY
)"

REDIS_KEY_PREFIX="deliverydispatch:dotnet:${RANDOM}:$$:"
DISPATCH_HTTP="http://127.0.0.1:${PORTS[0]}"
DISPATCH_MESH="tcp://127.0.0.1:${PORTS[1]}"
TRACKING_MESH="tcp://127.0.0.1:${PORTS[2]}"
CUSTOMER_STREAM="tcp://127.0.0.1:${PORTS[3]}"
CUSTOMER_MESH="tcp://127.0.0.1:${PORTS[4]}"
COURIER_STREAM="tcp://127.0.0.1:${PORTS[5]}"
COURIER_SESSION_MESH="tcp://127.0.0.1:${PORTS[6]}"
COURIER_NODE1_MESH="tcp://127.0.0.1:${PORTS[7]}"
COURIER_NODE2_MESH="tcp://127.0.0.1:${PORTS[8]}"

endpoint_host() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  endpoint="${endpoint#http://}"
  endpoint="${endpoint#ws://}"
  echo "${endpoint%:*}"
}

endpoint_port() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  endpoint="${endpoint#http://}"
  endpoint="${endpoint#ws://}"
  echo "${endpoint##*:}"
}

wait_port() {
  local name="$1"
  local endpoint="$2"
  local host
  local port
  host="$(endpoint_host "${endpoint}")"
  port="$(endpoint_port "${endpoint}")"
  for _ in $(seq 1 300); do
    if (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}

start_server() {
  local name="$1"
  local project="$2"
  shift 2
  local project_dir
  local project_name
  local assembly
  project_dir="$(cd "$(dirname "${project}")" && pwd)"
  project_name="$(basename "${project}" .csproj)"
  assembly="${project_dir}/bin/Debug/net8.0/${project_name}.dll"
  dotnet "${assembly}" "$@" >"${LOG_DIR}/${name}.log" 2>&1 &
  PIDS+=("$!")
}

wait_log_count() {
  local description="$1"
  local line="$2"
  local file="$3"
  local expected="$4"
  for _ in $(seq 1 300); do
    local actual=0
    if [[ -f "${file}" ]]; then
      actual="$(grep -F -c -- "${line}" "${file}" || true)"
    fi
    if [[ "${actual}" -eq "${expected}" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${description}: expected ${expected} occurrence(s) of '${line}' in ${file}" >&2
  return 1
}

zlink_redis_start_scoped_assign REDIS_CONTAINER DELIVERYDISPATCH_REDIS_ENDPOINT "zlink-deliverydispatch-dotnet-redis" redis:7.2-alpine
REDIS_ENDPOINT="${DELIVERYDISPATCH_REDIS_ENDPOINT}"
wait_port redis "tcp://${REDIS_ENDPOINT}"

# Each role gets one configuration file holding the values it needs. The runner picks this run's
# ports, but it hands them over in a file rather than through the environment
# (framework/doc/framework/common/sample-e2e-configuration-policy.ko.md 2.2, 7).
write_role_config() {
  local role="$1"
  local mesh_endpoint=""
  case "${role}" in
    dispatch) mesh_endpoint="${DISPATCH_MESH}" ;;
    tracking) mesh_endpoint="${TRACKING_MESH}" ;;
    customer-gateway) mesh_endpoint="${CUSTOMER_MESH}" ;;
    courier-session) mesh_endpoint="${COURIER_SESSION_MESH}" ;;
    courier-node-1) mesh_endpoint="${COURIER_NODE1_MESH}" ;;
    courier-node-2) mesh_endpoint="${COURIER_NODE2_MESH}" ;;
    client) mesh_endpoint="unused" ;;
  esac
  python3 "${SCRIPT_DIR}/write_role_config.py" \
    --output "${CONFIG_DIR}/${role}.json" \
    --role "${role}" \
    --log-dir "${SAMPLE_LOG_DIR}" \
    --work-dir "${WORK_DIR}" \
    --redis-endpoint "${REDIS_ENDPOINT}" \
    --redis-key-prefix "${REDIS_KEY_PREFIX}" \
    --dispatch-http "${DISPATCH_HTTP}" \
    --mesh-endpoint "${mesh_endpoint}" \
    --customer-stream "${CUSTOMER_STREAM}" \
    --courier-stream "${COURIER_STREAM}"
}

write_role_config tracking
write_role_config customer-gateway
write_role_config courier-session
write_role_config dispatch
write_role_config courier-node-1
write_role_config courier-node-2
write_role_config client

dotnet build "${SCRIPT_DIR}/DeliveryDispatch.sln" --maxcpucount:1

start_server tracking "${SCRIPT_DIR}/Server/Tracking/DeliveryDispatch.Server.Tracking.csproj" --config "${CONFIG_DIR}/tracking.json"
start_server customer-gateway "${SCRIPT_DIR}/Server/CustomerGateway/DeliveryDispatch.Server.CustomerGateway.csproj" --config "${CONFIG_DIR}/customer-gateway.json"
start_server courier-node-1 "${SCRIPT_DIR}/Server/CourierActorNode/DeliveryDispatch.Server.CourierActorNode.csproj" --config "${CONFIG_DIR}/courier-node-1.json"
start_server courier-node-2 "${SCRIPT_DIR}/Server/CourierActorNode/DeliveryDispatch.Server.CourierActorNode.csproj" --config "${CONFIG_DIR}/courier-node-2.json"
start_server courier-session "${SCRIPT_DIR}/Server/CourierSession/DeliveryDispatch.Server.CourierSession.csproj" --config "${CONFIG_DIR}/courier-session.json"
start_server dispatch "${SCRIPT_DIR}/Server/Dispatch/DeliveryDispatch.Server.Dispatch.csproj" --config "${CONFIG_DIR}/dispatch.json"

wait_log_count "tracking route readiness" "deliverydispatch-ready kind=route node=tracking" "${LOG_DIR}/tracking.log" 1
wait_log_count "customer gateway route readiness" "deliverydispatch-ready kind=route node=customer-gateway" "${LOG_DIR}/customer-gateway.log" 1
wait_log_count "courier session route readiness" "deliverydispatch-ready kind=route node=courier-session" "${LOG_DIR}/courier-session.log" 1
wait_log_count "courier node 1 route readiness" "deliverydispatch-ready kind=route node=courier-node-1" "${LOG_DIR}/courier-node-1.log" 1
wait_log_count "courier node 2 route readiness" "deliverydispatch-ready kind=route node=courier-node-2" "${LOG_DIR}/courier-node-2.log" 1
wait_log_count "dispatch route readiness" "deliverydispatch-ready kind=route node=dispatch" "${LOG_DIR}/dispatch.log" 1
wait_log_count "dispatch courier node 1 readiness" "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-1" "${LOG_DIR}/dispatch.log" 1
wait_log_count "dispatch courier node 2 readiness" "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-2" "${LOG_DIR}/dispatch.log" 1

dotnet run --no-build --project "${SCRIPT_DIR}/Client/DeliveryDispatch.Client.csproj" -- \
  --config "${CONFIG_DIR}/client.json" >"${LOG_DIR}/client.log" 2>&1

wait_log_count "client completion" "deliverydispatch=completed" "${LOG_DIR}/client.log" 1
wait_log_count "client reassignment completion" "deliverydispatch-reassignment=completed" "${LOG_DIR}/client.log" 1
wait_log_count "client server-evidence completion" "deliverydispatch-server-evidence=completed" "${LOG_DIR}/client.log" 1
wait_log_count "courier a bound" "deliverydispatch-courier bound courier=courier-a" "${LOG_DIR}/courier-session.log" 1
wait_log_count "courier b bound" "deliverydispatch-courier bound courier=courier-b" "${LOG_DIR}/courier-session.log" 1
wait_log_count "courier a bind relayed" "deliverydispatch-courier bind-relayed courier=courier-a" "${LOG_DIR}/courier-session.log" 1
wait_log_count "courier b bind relayed" "deliverydispatch-courier bind-relayed courier=courier-b" "${LOG_DIR}/courier-session.log" 1
wait_log_count "customer bound" "deliverydispatch-customer bound customer=customer-1" "${LOG_DIR}/customer-gateway.log" 1
wait_log_count "customer delivered pushes" "deliverydispatch-customer pushed status=Delivered" "${LOG_DIR}/customer-gateway.log" 2
wait_log_count "tracking delivered statuses" "deliverydispatch-tracking status=Delivered" "${LOG_DIR}/tracking.log" 2
wait_log_count "stale courier decision" "deliverydispatch-dispatch stale-decision-ignored delivery=delivery-reassign courier=courier-a attempt=1" "${LOG_DIR}/dispatch.log" 1
wait_log_count "candidates exhausted" "deliverydispatch-dispatch failed delivery=delivery-exhausted reason=candidates-exhausted" "${LOG_DIR}/dispatch.log" 1
echo "deliverydispatch-placement=completed"
RUN_SUCCEEDED=1
