#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../redis-common.sh"
RUN_DIR="$(mktemp -d)"
RUN_ID="$(basename "${RUN_DIR}")-$$-${RANDOM}"
LOG_DIR="${RUN_DIR}/logs"
SAMPLE_LOG_DIR="${RUN_DIR}/sample-logs"
SHOPPINGMALL_LOG_DIR="${SAMPLE_LOG_DIR}"
mkdir -p "${LOG_DIR}" "${SAMPLE_LOG_DIR}"

PIDS=()
REDIS_CONTAINER=""
RUN_SUCCEEDED=0
WAIT_ATTEMPTS=300

cleanup() {
  find "${RUN_DIR}" -type f -name "*.json" -delete 2>/dev/null || true
  zlink_sample_stop_processes "${PIDS[@]}"
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    zlink_redis_remove_by_id "${REDIS_CONTAINER}" || true
  fi
  zlink_sample_copy_evidence "${RUN_DIR}" "ShoppingMall"
  if [[ "${RUN_SUCCEEDED}" == "1" ]]; then
    rm -rf "${RUN_DIR}"
    echo "shoppingmall-placement=completed"
  else
    echo "runDir=${RUN_DIR}"
  fi
}
trap zlink_sample_exit_trap EXIT

read -r -a PORTS <<<"$(zlink_sample_pick_ports 8)"

SHOPPINGMALL_REDIS_KEY_PREFIX="shoppingmall:dotnet:${RUN_ID}:"
SHOPPINGMALL_API_A_HTTP_URL="http://127.0.0.1:${PORTS[0]}"
SHOPPINGMALL_API_B_HTTP_URL="http://127.0.0.1:${PORTS[1]}"
SHOPPINGMALL_WORKFLOW_A_HTTP_URL="http://127.0.0.1:${PORTS[2]}"
SHOPPINGMALL_WORKFLOW_B_HTTP_URL="http://127.0.0.1:${PORTS[3]}"
SHOPPINGMALL_API_A_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[4]}"
SHOPPINGMALL_API_B_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[5]}"
SHOPPINGMALL_WORKFLOW_A_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[6]}"
SHOPPINGMALL_WORKFLOW_B_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[7]}"

endpoint_host() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  endpoint="${endpoint#http://}"
  echo "${endpoint%:*}"
}

endpoint_port() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  endpoint="${endpoint#http://}"
  echo "${endpoint##*:}"
}

wait_port() {
  local name="$1"
  local endpoint="$2"
  local host
  local port
  host="$(endpoint_host "${endpoint}")"
  port="$(endpoint_port "${endpoint}")"
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    if (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}

wait_http() {
  local name="$1"
  local endpoint="$2"
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    if curl -fsS "${endpoint}/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}

wait_log_contains() {
  local name="$1"
  local log_file="$2"
  local pattern="$3"
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    if [[ -f "${log_file}" ]] && grep -Fq -- "${pattern}" "${log_file}"; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${name}: ${pattern}" >&2
  return 1
}

wait_log_exact_count() {
  local name="$1"
  local log_file_a="$2"
  local log_file_b="$3"
  local pattern="$4"
  local expected="$5"
  local actual
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    actual=0
    [[ -f "${log_file_a}" ]] && actual=$((actual + $(grep -Fc -- "${pattern}" "${log_file_a}" || true)))
    [[ -f "${log_file_b}" ]] && actual=$((actual + $(grep -Fc -- "${pattern}" "${log_file_b}" || true)))
    if [[ "${actual}" == "${expected}" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${name}: expected ${expected} lines matching ${pattern}, found ${actual}" >&2
  return 1
}

post_json() {
  local endpoint="$1"
  local body="$2"
  curl -fsS -X POST "${endpoint}" \
    -H 'Content-Type: application/json' \
    --data "${body}" >/dev/null
}

relocate_planned_order() {
  local order_id="$1"
  local endpoint
  local response
  local state
  local source_instance
  local last_result="no owner observed"
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    for endpoint in "${SHOPPINGMALL_WORKFLOW_A_HTTP_URL}" "${SHOPPINGMALL_WORKFLOW_B_HTTP_URL}"; do
      response="$(curl -fsS -X POST "${endpoint}/self-check/relocate/${order_id}" \
        -H 'Content-Type: application/json' --data '{}')" || continue
      state="owner=$(zlink_json_field "${response}" isOwner) outcome=$(zlink_json_field "${response}" outcome) reason=$(zlink_json_field "${response}" reason)"
      if [[ "${state}" == "owner=true outcome=Started reason=None" || "${state}" == "owner=true outcome=AlreadyStarted reason=None" ]]; then
        RELOCATION_ANCHOR_ID="$(zlink_json_field "${response}" anchorId)"
        source_instance="$(zlink_json_field "${response}" sourceInstanceId)"
        [[ "${source_instance}" == "null" ]] && source_instance=""
        case "${source_instance}" in
          workflow-a) RELOCATION_SOURCE_ENDPOINT="${SHOPPINGMALL_WORKFLOW_A_HTTP_URL}" ;;
          workflow-b) RELOCATION_SOURCE_ENDPOINT="${SHOPPINGMALL_WORKFLOW_B_HTTP_URL}" ;;
          *) echo "Planned relocation returned no workflow source for ${RELOCATION_ANCHOR_ID}" >&2; return 1 ;;
        esac
        return 0
      fi
      last_result="${state}"
    done
    sleep 0.1
  done
  echo "Planned relocation did not complete for ${order_id}: ${last_result}" >&2
  return 1
}

wait_relocated_anchor_owner() {
  local anchor_id="$1"
  local endpoint
  local response
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    for endpoint in "${SHOPPINGMALL_WORKFLOW_A_HTTP_URL}" "${SHOPPINGMALL_WORKFLOW_B_HTTP_URL}"; do
      [[ "${endpoint}" == "${RELOCATION_SOURCE_ENDPOINT}" ]] && continue
      response="$(curl -fsS "${endpoint}/self-check/owner/${anchor_id}")" || continue
      if [[ "$(zlink_json_field "${response}" isOwner)" == "true" ]]; then
        return 0
      fi
    done
    sleep 0.1
  done
  curl -fsS "${RELOCATION_SOURCE_ENDPOINT}/self-check/relocation-status" \
    || true
  echo "Relocation fixture did not acquire a new owner: ${anchor_id}" >&2
  return 1
}

signal_relocation_ready() {
  local response
  local state
  local outcome reason relocation_state
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    response="$(curl -fsS "${RELOCATION_SOURCE_ENDPOINT}/self-check/relocation-status")" || {
      sleep 0.1
      continue
    }
    outcome="$(zlink_json_field "${response}" outcome)"
    reason="$(zlink_json_field "${response}" reason)"
    relocation_state="$(zlink_json_field "${response}" state)"
    [[ "${relocation_state}" == "null" ]] && relocation_state=""
    state="${outcome}:${reason}:${relocation_state}"
    if [[ "${state}" == "InProgress:None:Relocating" ]]; then
      response="$(curl -fsS -X POST "${RELOCATION_SOURCE_ENDPOINT}/self-check/relocation-ready/${RELOCATION_ANCHOR_ID}" \
        -H 'Content-Type: application/json' --data '{}')" || return 1
      [[ "$(zlink_json_field "${response}" deferred)" == "true" ]] && return 0
      return 1
    fi
    if [[ "${state}" != "InProgress:None:Serving" && "${state}" != "InProgress:None:Preparing" ]]; then
      echo "Planned relocation did not reach its application-signaled boundary: ${state}" >&2
      return 1
    fi
    sleep 0.1
  done
  echo "Timed out waiting for the planned relocation application-signaled boundary" >&2
  return 1
}

wait_relocated_order_completed() {
  local order_id="$1"
  local response
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    response="$(curl -fsS "${SHOPPINGMALL_API_A_HTTP_URL}/orders/${order_id}")" || {
      sleep 0.1
      continue
    }
    # GetOrderStateRes has exactly one "status" key, nested under "state"
    # (Shared/Contracts/Messages.cs OrderState.Status); zlink_json_field's flat text
    # search is safe on this response shape for that reason.
    if [[ "$(zlink_json_field "${response}" status)" == "Confirmed" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "Relocated order did not finish on its target lifecycle: ${order_id}" >&2
  return 1
}

wait_workflow_mesh_ready() {
  local endpoint
  local response
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    local all_ready=true
    for endpoint in "${SHOPPINGMALL_WORKFLOW_A_HTTP_URL}" "${SHOPPINGMALL_WORKFLOW_B_HTTP_URL}"; do
      response="$(curl -fsS "${endpoint}/self-check/mesh-ready")" || {
        all_ready=false
        continue
      }
      if [[ "$(zlink_json_field "${response}" ready)" != "true" ]]; then
        all_ready=false
      fi
    done
    if [[ "${all_ready}" == true ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for workflow RouteMesh readiness" >&2
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

dotnet build "${SCRIPT_DIR}/ShoppingMall.csproj" --maxcpucount:1

# The sample owns its Redis: a dedicated, throwaway container is the shared
# location store every server registers into (no registry process exists).
if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run the ShoppingMall sample (it provisions a dedicated Redis container)." >&2
  exit 1
fi
REDIS_CONTAINER="zlink-shoppingmall-dotnet-redis-${RUN_ID}"
zlink_redis_start_scoped_assign REDIS_CONTAINER SHOPPINGMALL_REDIS_ENDPOINT "zlink-shoppingmall-dotnet-redis" redis:7.2-alpine
wait_port redis "tcp://${SHOPPINGMALL_REDIS_ENDPOINT}"
WORKFLOW_A_CONFIG_FILE="${RUN_DIR}/appsettings.workflow-a.json"
WORKFLOW_B_CONFIG_FILE="${RUN_DIR}/appsettings.workflow-b.json"
API_A_CONFIG_FILE="${RUN_DIR}/appsettings.api-a.json"
API_B_CONFIG_FILE="${RUN_DIR}/appsettings.api-b.json"
CLIENT_CONFIG_FILE="${RUN_DIR}/appsettings.client.json"
python3 - "${WORKFLOW_A_CONFIG_FILE}" "${WORKFLOW_B_CONFIG_FILE}" "${API_A_CONFIG_FILE}" "${API_B_CONFIG_FILE}" "${CLIENT_CONFIG_FILE}" <<PY
import json
import sys

settings = {
    "LogDirectory": "${SHOPPINGMALL_LOG_DIR}",
    "RedisEndpoint": "${SHOPPINGMALL_REDIS_ENDPOINT}",
    "RedisKeyPrefix": "${SHOPPINGMALL_REDIS_KEY_PREFIX}",
    "ApiAHttpUrl": "${SHOPPINGMALL_API_A_HTTP_URL}",
    "ApiBHttpUrl": "${SHOPPINGMALL_API_B_HTTP_URL}",
    "WorkflowAHttpUrl": "${SHOPPINGMALL_WORKFLOW_A_HTTP_URL}",
    "WorkflowBHttpUrl": "${SHOPPINGMALL_WORKFLOW_B_HTTP_URL}",
    "ApiAMeshEndpoint": "${SHOPPINGMALL_API_A_MESH_ENDPOINT}",
    "ApiBMeshEndpoint": "${SHOPPINGMALL_API_B_MESH_ENDPOINT}",
    "WorkflowAMeshEndpoint": "${SHOPPINGMALL_WORKFLOW_A_MESH_ENDPOINT}",
    "WorkflowBMeshEndpoint": "${SHOPPINGMALL_WORKFLOW_B_MESH_ENDPOINT}",
}
common = {
    "LogDirectory": settings["LogDirectory"],
    "RedisEndpoint": settings["RedisEndpoint"],
    "RedisKeyPrefix": settings["RedisKeyPrefix"],
}
roles = [
    # Each workflow needs BOTH mesh endpoints: its own to listen on and its peer's to connect to.
    {**common, "InstanceId": "workflow-a", "WorkflowAHttpUrl": settings["WorkflowAHttpUrl"],
     "WorkflowAMeshEndpoint": settings["WorkflowAMeshEndpoint"],
     "WorkflowBMeshEndpoint": settings["WorkflowBMeshEndpoint"]},
    {**common, "InstanceId": "workflow-b", "WorkflowBHttpUrl": settings["WorkflowBHttpUrl"],
     "WorkflowBMeshEndpoint": settings["WorkflowBMeshEndpoint"],
     "WorkflowAMeshEndpoint": settings["WorkflowAMeshEndpoint"]},
    {**common, "InstanceId": "api-a", "ApiAHttpUrl": settings["ApiAHttpUrl"],
     "ApiAMeshEndpoint": settings["ApiAMeshEndpoint"]},
    {**common, "InstanceId": "api-b", "ApiBHttpUrl": settings["ApiBHttpUrl"],
     "ApiBMeshEndpoint": settings["ApiBMeshEndpoint"]},
]
for path, role in zip(sys.argv[1:-1], roles):
    with open(path, "w", encoding="utf-8") as output:
        json.dump({"Sample": role}, output, indent=2)
with open(sys.argv[-1], "w", encoding="utf-8") as output:
    json.dump({"Client": {
        "LogDirectory": "${SHOPPINGMALL_LOG_DIR}",
        "ApiAHttpUrl": "${SHOPPINGMALL_API_A_HTTP_URL}",
        "ApiBHttpUrl": "${SHOPPINGMALL_API_B_HTTP_URL}",
    }}, output, indent=2)
PY

start_server workflow-a "${SCRIPT_DIR}/Server/OrderWorkflow/ShoppingMall.OrderWorkflow.csproj" --config "${WORKFLOW_A_CONFIG_FILE}"
wait_port workflow-a-mesh "${SHOPPINGMALL_WORKFLOW_A_MESH_ENDPOINT}"
wait_http workflow-a "${SHOPPINGMALL_WORKFLOW_A_HTTP_URL}"

start_server workflow-b "${SCRIPT_DIR}/Server/OrderWorkflow/ShoppingMall.OrderWorkflow.csproj" --config "${WORKFLOW_B_CONFIG_FILE}"
wait_port workflow-b-mesh "${SHOPPINGMALL_WORKFLOW_B_MESH_ENDPOINT}"
wait_http workflow-b "${SHOPPINGMALL_WORKFLOW_B_HTTP_URL}"

start_server api-a "${SCRIPT_DIR}/Server/CommerceApi/ShoppingMall.CommerceApi.csproj" --config "${API_A_CONFIG_FILE}"
wait_port api-a-mesh "${SHOPPINGMALL_API_A_MESH_ENDPOINT}"
wait_http api-a "${SHOPPINGMALL_API_A_HTTP_URL}"

start_server api-b "${SCRIPT_DIR}/Server/CommerceApi/ShoppingMall.CommerceApi.csproj" --config "${API_B_CONFIG_FILE}"
wait_port api-b-mesh "${SHOPPINGMALL_API_B_MESH_ENDPOINT}"
wait_http api-b "${SHOPPINGMALL_API_B_HTTP_URL}"

# The sample emits these only after its HTTP edge is listening and its RouteMesh
# has passively observed both workflow peers. No readiness request is sent here.
wait_log_contains api-a-http "${LOG_DIR}/api-a.log" "shoppingmall-ready kind=http node=api-a"
wait_log_contains api-b-http "${LOG_DIR}/api-b.log" "shoppingmall-ready kind=http node=api-b"
wait_log_contains api-a-workflow-a "${LOG_DIR}/api-a.log" "shoppingmall-ready kind=object-route node=api-a target=workflow-a"
wait_log_contains api-a-workflow-b "${LOG_DIR}/api-a.log" "shoppingmall-ready kind=object-route node=api-a target=workflow-b"
wait_log_contains api-b-workflow-a "${LOG_DIR}/api-b.log" "shoppingmall-ready kind=object-route node=api-b target=workflow-a"
wait_log_contains api-b-workflow-b "${LOG_DIR}/api-b.log" "shoppingmall-ready kind=object-route node=api-b target=workflow-b"

# These calls prepare deterministic failure/recovery fixtures outside the
# Client process. The Client exercises only the public order endpoints; the
# runner is the observation hook allowed to create a pending mapping and to
# remove a projection before the public rebuild assertion.
post_json "${SHOPPINGMALL_API_A_HTTP_URL}/self-check/idempotency/pending" \
  '{"idempotencyKey":"order-pending-001","orderId":"order-pending-0001"}'
post_json "${SHOPPINGMALL_API_A_HTTP_URL}/self-check/idempotency/pending" \
  '{"idempotencyKey":"order-resume-001","orderId":"order-resume-001"}'
post_json "${SHOPPINGMALL_API_A_HTTP_URL}/self-check/workflow/inventory-reserved" \
  '{"cartId":"cart-success","shippingAddressId":"addr-home","paymentMethodId":"pm-ok","idempotencyKey":"order-resume-001"}'
post_json "${SHOPPINGMALL_API_A_HTTP_URL}/self-check/idempotency/pending" \
  '{"idempotencyKey":"order-repair-001","orderId":"order-repair-001"}'
post_json "${SHOPPINGMALL_API_A_HTTP_URL}/self-check/workflow/inventory-reserved" \
  '{"cartId":"cart-success","shippingAddressId":"addr-home","paymentMethodId":"pm-ok","idempotencyKey":"order-repair-001"}'
post_json "${SHOPPINGMALL_API_A_HTTP_URL}/orders/order-repair-001/continue" '{}'
post_json "${SHOPPINGMALL_API_A_HTTP_URL}/self-check/projection/order-repair-001/delete" '{}'
if curl -fsS "${SHOPPINGMALL_API_A_HTTP_URL}/orders/order-repair-001" >/dev/null 2>&1; then
  echo "Projection deletion fixture was not visible through the public read API." >&2
  exit 1
fi

dotnet run --no-build --project "${SCRIPT_DIR}/Client/ShoppingMall.Client.csproj" -- \
  --config "${CLIENT_CONFIG_FILE}" >"${LOG_DIR}/client.log" 2>&1

wait_log_contains client-completed "${SHOPPINGMALL_LOG_DIR}/client.log" "shoppingmall=completed"
wait_log_contains workflow-a-order "${LOG_DIR}/workflow-a.log" "shoppingmall-order started order="
wait_log_contains workflow-b-order "${LOG_DIR}/workflow-b.log" "shoppingmall-order started order="
ORDERS_JSON="$(cat "${SHOPPINGMALL_LOG_DIR}/shoppingmall-client-orders.json")"
ASSERTION_BODY="$(
  required=(SuccessfulOrderId PendingRecoveredOrderId ConcurrentOrderId ResumedOrderId \
    InventoryFailureOrderId PaymentFailureOrderId ScaleOutOrderId RepairOrderId)
  pairs=()
  for name in "${required[@]}"; do
    value="$(zlink_json_field "${ORDERS_JSON}" "${name}")"
    if [[ -z "${value}" || "${value}" == "null" ]]; then
      echo "Client order result is incomplete." >&2
      exit 1
    fi
    camel_name="${name,}"
    pairs+=("\"${camel_name}\":\"${value}\"")
  done
  IFS=,
  echo "{${pairs[*]}}"
)"
curl -fsS -X POST "${SHOPPINGMALL_API_A_HTTP_URL}/self-check/assert" \
  -H 'Content-Type: application/json' \
  --data "${ASSERTION_BODY}" \
  | tee "${LOG_DIR}/server-assertion.json" \
  | grep -q '"passed":true'
wait_log_contains commerce-evidence "${LOG_DIR}/api-a.log" "shoppingmall-evidence order="
wait_workflow_mesh_ready
# The runner owns the planned-relocation fixture: it creates a checkpoint,
# relocates the dedicated workflow fixture, then verifies its target lifecycle
# resumed the order through the public read API.
RELOCATION_CHECKPOINT="$(curl -fsS -X POST "${SHOPPINGMALL_API_A_HTTP_URL}/self-check/workflow/inventory-reserved" \
  -H 'Content-Type: application/json' \
  --data "{\"cartId\":\"cart-success\",\"shippingAddressId\":\"addr-home\",\"paymentMethodId\":\"pm-ok\",\"idempotencyKey\":\"order-relocation-${RUN_ID}\"}")"
RELOCATION_ORDER_ID="$(zlink_json_field "${RELOCATION_CHECKPOINT}" orderId)"
relocate_planned_order "${RELOCATION_ORDER_ID}"
wait_relocated_anchor_owner "${RELOCATION_ANCHOR_ID}"
wait_relocated_order_completed "${RELOCATION_ORDER_ID}"
# Planned relocation is intentionally required. Do not print replay evidence from
# store wiring: this wait can pass only when the sample actually drives relocation.
wait_log_exact_count replayed "${LOG_DIR}/workflow-a.log" "${LOG_DIR}/workflow-b.log" "shoppingmall-order replayed order=" 1
wait_log_exact_count no-external-effect-repeat "${LOG_DIR}/workflow-a.log" "${LOG_DIR}/workflow-b.log" "shoppingmall-order external-effect-repeated order=" 0
RUN_SUCCEEDED=1
