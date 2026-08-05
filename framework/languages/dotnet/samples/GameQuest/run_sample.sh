#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../redis-common.sh"
RUN_DIR="$(mktemp -d)"
RUN_ID="$(basename "${RUN_DIR}")-$$-${RANDOM}"
LOG_DIR="${RUN_DIR}/logs"
SAMPLE_LOG_DIR="${RUN_DIR}/sample-logs"
GAMEQUEST_LOG_DIR="${SAMPLE_LOG_DIR}"
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
  zlink_sample_copy_evidence "${RUN_DIR}" "GameQuest"
  if [[ "${RUN_SUCCEEDED}" == "1" ]]; then
    rm -rf "${RUN_DIR}"
  else
    echo "runDir=${RUN_DIR}"
  fi
}
trap cleanup EXIT

dotnet build "${SCRIPT_DIR}/GameQuest.csproj" --maxcpucount:1

# Provision shared dependencies before selecting application ports. Keeping the
# bind window short prevents unrelated ephemeral connections from claiming a
# port after the runner has checked it.
if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run the GameQuest sample (it provisions a dedicated Redis container)." >&2
  exit 1
fi
REDIS_CONTAINER="zlink-gamequest-dotnet-redis-${RUN_ID}"
zlink_redis_start_scoped_assign REDIS_CONTAINER GAMEQUEST_REDIS_ENDPOINT "zlink-gamequest-dotnet-redis" redis:7.2-alpine

read -r -a PORTS <<<"$(python3 - <<'PY'
import random
import socket

sockets = []
chosen = set()
try:
    while len(sockets) < 10:
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

GAMEQUEST_REDIS_KEY_PREFIX="gamequest:dotnet:${RUN_ID}:"
GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL="http://127.0.0.1:${PORTS[0]}"
GAMEQUEST_GAMEAPI_B_HTTP_BASE_URL="http://127.0.0.1:${PORTS[1]}"
GAMEQUEST_GAMEAPI_A_STREAM_ENDPOINT="ws://127.0.0.1:${PORTS[0]}/quest/ws"
GAMEQUEST_GAMEAPI_B_STREAM_ENDPOINT="ws://127.0.0.1:${PORTS[1]}/quest/ws"
GAMEQUEST_API_A_STREAM_BIND_ENDPOINT="tcp://127.0.0.1:${PORTS[2]}"
GAMEQUEST_API_B_STREAM_BIND_ENDPOINT="tcp://127.0.0.1:${PORTS[3]}"
GAMEQUEST_MISSION_A_HTTP_URL="http://127.0.0.1:${PORTS[4]}"
GAMEQUEST_MISSION_B_HTTP_URL="http://127.0.0.1:${PORTS[5]}"
GAMEQUEST_GAMEAPI_A_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[6]}"
GAMEQUEST_GAMEAPI_B_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[7]}"
GAMEQUEST_MISSION_A_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[8]}"
GAMEQUEST_MISSION_B_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[9]}"

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

wait_port redis "tcp://${GAMEQUEST_REDIS_ENDPOINT}"
MISSION_A_CONFIG_FILE="${RUN_DIR}/appsettings.mission-a.json"
MISSION_B_CONFIG_FILE="${RUN_DIR}/appsettings.mission-b.json"
API_A_CONFIG_FILE="${RUN_DIR}/appsettings.api-a.json"
API_B_CONFIG_FILE="${RUN_DIR}/appsettings.api-b.json"
CLIENT_CONFIG_FILE="${RUN_DIR}/appsettings.client.json"
python3 - "${MISSION_A_CONFIG_FILE}" "${MISSION_B_CONFIG_FILE}" "${API_A_CONFIG_FILE}" "${API_B_CONFIG_FILE}" "${CLIENT_CONFIG_FILE}" <<PY
import json
import sys

settings = {
    "LogDirectory": "${GAMEQUEST_LOG_DIR}",
    "RedisEndpoint": "${GAMEQUEST_REDIS_ENDPOINT}",
    "RedisKeyPrefix": "${GAMEQUEST_REDIS_KEY_PREFIX}",
    "GameApiAHttpBaseUrl": "${GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL}",
    "GameApiBHttpBaseUrl": "${GAMEQUEST_GAMEAPI_B_HTTP_BASE_URL}",
    "MissionAHttpBaseUrl": "${GAMEQUEST_MISSION_A_HTTP_URL}",
    "MissionBHttpBaseUrl": "${GAMEQUEST_MISSION_B_HTTP_URL}",
    "GameApiAStreamBindEndpoint": "${GAMEQUEST_API_A_STREAM_BIND_ENDPOINT}",
    "GameApiBStreamBindEndpoint": "${GAMEQUEST_API_B_STREAM_BIND_ENDPOINT}",
    "GameApiAMeshEndpoint": "${GAMEQUEST_GAMEAPI_A_MESH_ENDPOINT}",
    "GameApiBMeshEndpoint": "${GAMEQUEST_GAMEAPI_B_MESH_ENDPOINT}",
    "MissionAMeshEndpoint": "${GAMEQUEST_MISSION_A_MESH_ENDPOINT}",
    "MissionBMeshEndpoint": "${GAMEQUEST_MISSION_B_MESH_ENDPOINT}",
}
common = {
    "LogDirectory": settings["LogDirectory"],
    "RedisEndpoint": settings["RedisEndpoint"],
    "RedisKeyPrefix": settings["RedisKeyPrefix"],
}
roles = [
    {**common, "InstanceName": "mission-a", "GameApiAHttpBaseUrl": settings["GameApiAHttpBaseUrl"],
     "MissionAHttpBaseUrl": settings["MissionAHttpBaseUrl"],
     "MissionAMeshEndpoint": settings["MissionAMeshEndpoint"]},
    {**common, "InstanceName": "mission-b", "GameApiAHttpBaseUrl": settings["GameApiAHttpBaseUrl"],
     "MissionBHttpBaseUrl": settings["MissionBHttpBaseUrl"],
     "MissionBMeshEndpoint": settings["MissionBMeshEndpoint"]},
    {**common, "InstanceName": "api-a", "GameApiAHttpBaseUrl": settings["GameApiAHttpBaseUrl"],
     "GameApiAStreamBindEndpoint": settings["GameApiAStreamBindEndpoint"],
     "GameApiAMeshEndpoint": settings["GameApiAMeshEndpoint"]},
    {**common, "InstanceName": "api-b", "GameApiBHttpBaseUrl": settings["GameApiBHttpBaseUrl"],
     "GameApiBStreamBindEndpoint": settings["GameApiBStreamBindEndpoint"],
     "GameApiBMeshEndpoint": settings["GameApiBMeshEndpoint"]},
]
for path, role in zip(sys.argv[1:-1], roles):
    with open(path, "w", encoding="utf-8") as output:
        json.dump({"Sample": role}, output, indent=2)
with open(sys.argv[-1], "w", encoding="utf-8") as output:
    json.dump({"Client": {
        "GameApiAHttpBaseUrl": "${GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL}",
        "GameApiBHttpBaseUrl": "${GAMEQUEST_GAMEAPI_B_HTTP_BASE_URL}",
        "MissionAHttpBaseUrl": "${GAMEQUEST_MISSION_A_HTTP_URL}",
        "MissionBHttpBaseUrl": "${GAMEQUEST_MISSION_B_HTTP_URL}",
        "GameApiAStreamEndpoint": "${GAMEQUEST_GAMEAPI_A_STREAM_ENDPOINT}",
        "GameApiBStreamEndpoint": "${GAMEQUEST_GAMEAPI_B_STREAM_ENDPOINT}",
    }}, output, indent=2)
PY

start_server mission-a "${SCRIPT_DIR}/Server/QuestMission/GameQuest.QuestMission.csproj" --config "${MISSION_A_CONFIG_FILE}"
wait_port mission-a-mesh "${GAMEQUEST_MISSION_A_MESH_ENDPOINT}"
wait_http mission-a "${GAMEQUEST_MISSION_A_HTTP_URL}"

start_server mission-b "${SCRIPT_DIR}/Server/QuestMission/GameQuest.QuestMission.csproj" --config "${MISSION_B_CONFIG_FILE}"
wait_port mission-b-mesh "${GAMEQUEST_MISSION_B_MESH_ENDPOINT}"
wait_http mission-b "${GAMEQUEST_MISSION_B_HTTP_URL}"

start_server api-a "${SCRIPT_DIR}/Server/GameApi/GameQuest.GameApi.csproj" --config "${API_A_CONFIG_FILE}"
wait_port api-a-stream "${GAMEQUEST_API_A_STREAM_BIND_ENDPOINT}"
wait_port api-a-mesh "${GAMEQUEST_GAMEAPI_A_MESH_ENDPOINT}"
wait_http api-a "${GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL}"

start_server api-b "${SCRIPT_DIR}/Server/GameApi/GameQuest.GameApi.csproj" --config "${API_B_CONFIG_FILE}"
wait_port api-b-stream "${GAMEQUEST_API_B_STREAM_BIND_ENDPOINT}"
wait_port api-b-mesh "${GAMEQUEST_GAMEAPI_B_MESH_ENDPOINT}"
wait_http api-b "${GAMEQUEST_GAMEAPI_B_HTTP_BASE_URL}"

dotnet run --no-build --project "${SCRIPT_DIR}/Client/GameQuest.Client.csproj" -- \
  --config "${CLIENT_CONFIG_FILE}" >"${LOG_DIR}/client.log" 2>&1

# Each client connection terminates on its configured Session Server. The
# Actor handler may execute on either server because Actor placement is owned
# by the Framework, so a specific API process must not be asserted as the
# gameplay owner.
grep -q "surface=StreamSession kind=Request packet=JoinSessionReq" "${LOG_DIR}/api-a.log"
grep -q "surface=StreamSession kind=Request packet=JoinSessionReq" "${LOG_DIR}/api-b.log"
grep -q "gamequest api event routed" "${LOG_DIR}/api-a.log" "${LOG_DIR}/api-b.log"
grep -q "gamequest mission processed" "${LOG_DIR}/mission-a.log"
grep -q "gamequest mission processed" "${LOG_DIR}/mission-b.log"
grep -q "gamequest player quest spot ready" "${LOG_DIR}/mission-a.log"
grep -q "gamequest player quest spot ready" "${LOG_DIR}/mission-b.log"
curl -fsS -X POST "${GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL}/self-check/assert" | grep -q '"passed":true'
curl -fsS "${GAMEQUEST_MISSION_A_HTTP_URL}/self-check/events" | grep -q "QuestReconciled"
echo "gamequest-server-evidence=completed"
RUN_SUCCEEDED=1
