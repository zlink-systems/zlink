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
declare -A SERVER_PIDS=()
REDIS_CONTAINER=""
RUN_SUCCEEDED=0
WAIT_ATTEMPTS=300

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
    zlink_redis_remove_by_id "${REDIS_CONTAINER}" || true
  fi
  zlink_sample_copy_evidence "${RUN_DIR}" "GameQuest" || true
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

GAMEQUEST_REDIS_KEY_PREFIX="gamequest:dotnet:${RUN_ID}:"
GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL="http://127.0.0.1:${PORTS[0]}"
GAMEQUEST_GAMEAPI_B_HTTP_BASE_URL="http://127.0.0.1:${PORTS[1]}"
GAMEQUEST_API_A_STREAM_BIND_ENDPOINT="tcp://127.0.0.1:${PORTS[2]}"
GAMEQUEST_API_B_STREAM_BIND_ENDPOINT="tcp://127.0.0.1:${PORTS[3]}"
GAMEQUEST_GAMEAPI_A_STREAM_ENDPOINT="${GAMEQUEST_API_A_STREAM_BIND_ENDPOINT}"
GAMEQUEST_GAMEAPI_B_STREAM_ENDPOINT="${GAMEQUEST_API_B_STREAM_BIND_ENDPOINT}"
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
  SERVER_PIDS["${name}"]="$!"
}

wait_log_contains() {
  local file="$1"
  local pattern="$2"
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    if grep -Fq -- "${pattern}" "${file}" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for '${pattern}' in ${file}" >&2
  return 1
}

count_log_matches() {
  local file="$1"
  local pattern="$2"
  grep -Fc -- "${pattern}" "${file}" || true
}

require_at_least() {
  local file="$1"
  local pattern="$2"
  wait_log_contains "${file}" "${pattern}"
}

wait_total_at_least() {
  local expected="$1"
  local pattern="$2"
  shift 2
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    local total=0
    local file
    for file in "$@"; do
      total=$(( total + $(count_log_matches "${file}" "${pattern}") ))
    done
    if (( total >= expected )); then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${expected} '${pattern}' row(s)" >&2
  return 1
}

find_owner_mission() {
  local pattern_a="gamequest-owner ready player=player-alice generation=2 node=mission-a"
  local pattern_b="gamequest-owner ready player=player-alice generation=2 node=mission-b"
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    if grep -Fq -- "${pattern_a}" "${LOG_DIR}/mission-a.log" 2>/dev/null; then
      echo "mission-a"
      return 0
    fi
    if grep -Fq -- "${pattern_b}" "${LOG_DIR}/mission-b.log" 2>/dev/null; then
      echo "mission-b"
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for the player-alice owner-ready marker" >&2
  return 1
}

find_closed_mission() {
  local pattern_a="gamequest-owner closed player=player-alice generation=1 node=mission-a"
  local pattern_b="gamequest-owner closed player=player-alice generation=1 node=mission-b"
  for _ in $(seq 1 "${WAIT_ATTEMPTS}"); do
    if grep -Fq -- "${pattern_a}" "${LOG_DIR}/mission-a.log" 2>/dev/null; then
      echo "mission-a"
      return 0
    fi
    if grep -Fq -- "${pattern_b}" "${LOG_DIR}/mission-b.log" 2>/dev/null; then
      echo "mission-b"
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for the player-alice owner-closed marker" >&2
  return 1
}

wait_port redis "tcp://${GAMEQUEST_REDIS_ENDPOINT}"
MISSION_A_CONFIG_FILE="${RUN_DIR}/appsettings.mission-a.json"
MISSION_B_CONFIG_FILE="${RUN_DIR}/appsettings.mission-b.json"
API_A_CONFIG_FILE="${RUN_DIR}/appsettings.api-a.json"
API_B_CONFIG_FILE="${RUN_DIR}/appsettings.api-b.json"
CLIENT_CONFIG_FILE="${RUN_DIR}/appsettings.client.json"
CLOSE_REPLAY_RELEASE_FILE="${RUN_DIR}/close-replay-release"
OWNER_LOSS_RELEASE_FILE="${RUN_DIR}/owner-loss-release"
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
        "CloseReplayReleaseFile": "${CLOSE_REPLAY_RELEASE_FILE}",
        "OwnerLossReleaseFile": "${OWNER_LOSS_RELEASE_FILE}",
    }}, output, indent=2)
PY

start_server mission-a "${SCRIPT_DIR}/Server/QuestMission/GameQuest.QuestMission.csproj" --config "${MISSION_A_CONFIG_FILE}"
wait_port mission-a-mesh "${GAMEQUEST_MISSION_A_MESH_ENDPOINT}"
wait_http mission-a "${GAMEQUEST_MISSION_A_HTTP_URL}"
wait_log_contains "${LOG_DIR}/mission-a.log" "gamequest-ready kind=instance-factory node=mission-a"

start_server mission-b "${SCRIPT_DIR}/Server/QuestMission/GameQuest.QuestMission.csproj" --config "${MISSION_B_CONFIG_FILE}"
wait_port mission-b-mesh "${GAMEQUEST_MISSION_B_MESH_ENDPOINT}"
wait_http mission-b "${GAMEQUEST_MISSION_B_HTTP_URL}"
wait_log_contains "${LOG_DIR}/mission-b.log" "gamequest-ready kind=instance-factory node=mission-b"

start_server api-a "${SCRIPT_DIR}/Server/GameApi/GameQuest.GameApi.csproj" --config "${API_A_CONFIG_FILE}"
wait_port api-a-stream "${GAMEQUEST_API_A_STREAM_BIND_ENDPOINT}"
wait_port api-a-mesh "${GAMEQUEST_GAMEAPI_A_MESH_ENDPOINT}"
wait_http api-a "${GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL}"
wait_log_contains "${LOG_DIR}/api-a.log" "gamequest-ready kind=stream node=api-a"
wait_log_contains "${LOG_DIR}/api-a.log" "gamequest-ready kind=spot-route node=api-a mesh=gamequest"

start_server api-b "${SCRIPT_DIR}/Server/GameApi/GameQuest.GameApi.csproj" --config "${API_B_CONFIG_FILE}"
wait_port api-b-stream "${GAMEQUEST_API_B_STREAM_BIND_ENDPOINT}"
wait_port api-b-mesh "${GAMEQUEST_GAMEAPI_B_MESH_ENDPOINT}"
wait_http api-b "${GAMEQUEST_GAMEAPI_B_HTTP_BASE_URL}"
wait_log_contains "${LOG_DIR}/api-b.log" "gamequest-ready kind=stream node=api-b"
wait_log_contains "${LOG_DIR}/api-b.log" "gamequest-ready kind=spot-route node=api-b mesh=gamequest"

dotnet run --no-build --project "${SCRIPT_DIR}/Client/GameQuest.Client.csproj" -- \
  --config "${CLIENT_CONFIG_FILE}" >"${LOG_DIR}/client.log" 2>&1 &
CLIENT_PID="$!"
PIDS+=("${CLIENT_PID}")

wait_log_contains "${LOG_DIR}/client.log" "gamequest-client close-replay-armed player=player-alice"
CLOSED_MISSION="$(find_closed_mission)"
touch "${CLOSE_REPLAY_RELEASE_FILE}"
wait_log_contains "${LOG_DIR}/client.log" "gamequest-client owner-loss-armed player=player-alice"
OWNER_MISSION="$(find_owner_mission)"
kill -9 "${SERVER_PIDS["${OWNER_MISSION}"]}"
touch "${OWNER_LOSS_RELEASE_FILE}"
wait "${CLIENT_PID}"

# Section 10.1: counted across both node logs against a lower bound. An actor send handler runs on
# the node where the actor lives, not where the stream arrived, so per-node counts are unsatisfiable.
wait_total_at_least 4 "gamequest-api event-routed player=" "${LOG_DIR}/api-a.log" "${LOG_DIR}/api-b.log"
wait_total_at_least 4 "gamequest-mission processed player=" "${LOG_DIR}/mission-a.log" "${LOG_DIR}/mission-b.log"

wait_total_at_least 1 "gamequest-mission reconciled player=player-alice quest=" "${LOG_DIR}/mission-a.log" "${LOG_DIR}/mission-b.log"
reconciled_count=$(( $(count_log_matches "${LOG_DIR}/mission-a.log" "gamequest-mission reconciled player=player-alice quest=") + $(count_log_matches "${LOG_DIR}/mission-b.log" "gamequest-mission reconciled player=player-alice quest=") ))
[[ "${reconciled_count}" == "1" ]]
wait_total_at_least 1 "gamequest-mission replayed player=player-alice generation=" "${LOG_DIR}/mission-a.log" "${LOG_DIR}/mission-b.log"
replayed_count=$(( $(count_log_matches "${LOG_DIR}/mission-a.log" "gamequest-mission replayed player=player-alice generation=") + $(count_log_matches "${LOG_DIR}/mission-b.log" "gamequest-mission replayed player=player-alice generation=") ))
[[ "${replayed_count}" == "1" ]]
wait_total_at_least 1 "gamequest-owner unavailable player=player-alice" "${LOG_DIR}/api-a.log" "${LOG_DIR}/api-b.log"
unavailable_count=$(( $(count_log_matches "${LOG_DIR}/api-a.log" "gamequest-owner unavailable player=player-alice") + $(count_log_matches "${LOG_DIR}/api-b.log" "gamequest-owner unavailable player=player-alice") ))
[[ "${unavailable_count}" == "1" ]]
replacement_count=$(( $(count_log_matches "${LOG_DIR}/mission-a.log" "gamequest-owner replacement-handler-invoked player=player-alice") + $(count_log_matches "${LOG_DIR}/mission-b.log" "gamequest-owner replacement-handler-invoked player=player-alice") ))
[[ "${replacement_count}" == "0" ]]
wait_log_contains "${LOG_DIR}/client.log" "gamequest=completed"
wait_log_contains "${LOG_DIR}/client.log" "gamequest-server-evidence=completed"
[[ "$(count_log_matches "${LOG_DIR}/client.log" "gamequest=completed")" == "1" ]]
[[ "$(count_log_matches "${LOG_DIR}/client.log" "gamequest-server-evidence=completed")" == "1" ]]

RUN_SUCCEEDED=1
cleanup >/dev/null 2>&1
trap - EXIT
echo "gamequest-placement=completed"
