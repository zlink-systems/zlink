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

cleanup() {
  find "${RUN_DIR}" -type f -name "*.json" -delete 2>/dev/null || true
  for ((i=${#PIDS[@]}-1; i>=0; i--)); do
    local pid="${PIDS[$i]}"
    if kill -0 "${pid}" 2>/dev/null; then
      kill -INT "${pid}" 2>/dev/null || true
    fi
  done
  for _ in $(seq 1 20); do
    local any_alive=0
    for pid in "${PIDS[@]}"; do
      if kill -0 "${pid}" 2>/dev/null; then
        any_alive=1
        break
      fi
    done
    if [[ "${any_alive}" == "0" ]]; then
      break
    fi
    sleep 0.1
  done
  for pid in "${PIDS[@]}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      kill -9 "${pid}" 2>/dev/null || true
    fi
    wait "${pid}" 2>/dev/null || true
  done
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
  fi
  zlink_sample_copy_evidence "${RUN_DIR}" "ShoppingMall"
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
    while len(sockets) < 8:
        port = random.randint(41000, 60999)
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
  for _ in $(seq 1 30); do
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
  for _ in $(seq 1 30); do
    if curl -fsS "${endpoint}/health" >/dev/null 2>&1; then
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
    {**common, "InstanceId": "workflow-a", "WorkflowAHttpUrl": settings["WorkflowAHttpUrl"],
     "WorkflowAMeshEndpoint": settings["WorkflowAMeshEndpoint"]},
    {**common, "InstanceId": "workflow-b", "WorkflowBHttpUrl": settings["WorkflowBHttpUrl"],
     "WorkflowBMeshEndpoint": settings["WorkflowBMeshEndpoint"]},
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

dotnet run --no-build --project "${SCRIPT_DIR}/Client/ShoppingMall.Client.csproj" -- \
  --config "${CLIENT_CONFIG_FILE}" >"${LOG_DIR}/client.log" 2>&1

grep -q "shoppingmall=completed" "${SHOPPINGMALL_LOG_DIR}/client.log"
grep -q "shoppingmall order: started" "${LOG_DIR}/workflow-a.log" \
  || grep -q "shoppingmall order: started" "${LOG_DIR}/workflow-b.log"
grep -q "shoppingmall evidence:" "${LOG_DIR}/api-a.log"
echo "shoppingmall-server-evidence=completed"
RUN_SUCCEEDED=1
