#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../redis-common.sh"
RUN_DIR="$(mktemp -d)"
RUN_ID="$(basename "${RUN_DIR}")-$$-${RANDOM}"
LOG_DIR="${RUN_DIR}/logs"
BINGO_LOG_DIR="${RUN_DIR}/sample-logs"
mkdir -p "${LOG_DIR}" "${BINGO_LOG_DIR}"

PIDS=()
REDIS_CONTAINER=""
RUN_SUCCEEDED=0
BINGO_REDIS_KEY_PREFIX="bingo:dotnet:${RUN_ID}:"

cleanup() {
  set +e
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
  for ((i=${#PIDS[@]}-1; i>=0; i--)); do
    local pid="${PIDS[$i]}"
    if kill -0 "${pid}" 2>/dev/null; then
      kill -9 "${pid}" 2>/dev/null || true
    fi
  done
  for pid in "${PIDS[@]}"; do
    wait "${pid}" 2>/dev/null || true
  done
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
  fi
  zlink_sample_copy_evidence "${RUN_DIR}" "Bingo"
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
try:
    chosen = set()
    while len(sockets) < 11:
        port = random.randint(48000, 60999)
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

BINGO_API_A_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[0]}"
BINGO_API_B_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[1]}"
BINGO_PLAY_A_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[2]}"
BINGO_PLAY_B_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[3]}"
BINGO_SESSION_A_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[4]}"
BINGO_SESSION_B_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[5]}"
BINGO_SESSION_A_STREAM_ENDPOINT="tcp://127.0.0.1:${PORTS[6]}"
BINGO_SESSION_B_STREAM_ENDPOINT="tcp://127.0.0.1:${PORTS[7]}"
BINGO_API_A_MATCHMAKING_ENDPOINT="tcp://127.0.0.1:${PORTS[8]}"
BINGO_API_B_MATCHMAKING_ENDPOINT="tcp://127.0.0.1:${PORTS[9]}"
BINGO_MATCHMAKING_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[10]}"
API_A_CONFIG_FILE="${RUN_DIR}/appsettings.api-a.json"
API_B_CONFIG_FILE="${RUN_DIR}/appsettings.api-b.json"
PLAY_A_CONFIG_FILE="${RUN_DIR}/appsettings.play-a.json"
PLAY_B_CONFIG_FILE="${RUN_DIR}/appsettings.play-b.json"
SESSION_A_CONFIG_FILE="${RUN_DIR}/appsettings.session-a.json"
SESSION_B_CONFIG_FILE="${RUN_DIR}/appsettings.session-b.json"
CLIENT_CONFIG_FILE="${RUN_DIR}/appsettings.client.json"
MATCHMAKING_CONFIG_FILE="${RUN_DIR}/appsettings.matchmaking.json"

endpoint_host() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  echo "${endpoint%:*}"
}

endpoint_port() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  echo "${endpoint##*:}"
}

wait_port() {
  local name="$1"
  local endpoint="$2"
  local host
  local port
  host="$(endpoint_host "${endpoint}")"
  port="$(endpoint_port "${endpoint}")"
  for _ in $(seq 1 100); do
    if (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}

require_log_count() {
  local expected="$1"
  local pattern="$2"
  shift 2
  local actual
  actual="$({ grep -Eh "${pattern}" "$@" 2>/dev/null || true; } | wc -l)"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "Expected ${expected} matches for '${pattern}' in $*, found ${actual}." >&2
    return 1
  fi
}

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run the Bingo sample." >&2
  exit 1
fi
REDIS_CONTAINER="zlink-bingo-dotnet-redis-${RUN_ID}"
zlink_redis_start_scoped_assign REDIS_CONTAINER BINGO_REDIS_ENDPOINT "zlink-bingo-dotnet-redis" redis:7.2-alpine
wait_port redis "tcp://${BINGO_REDIS_ENDPOINT}"

python3 - "${API_A_CONFIG_FILE}" "${API_B_CONFIG_FILE}" "${PLAY_A_CONFIG_FILE}" "${PLAY_B_CONFIG_FILE}" "${SESSION_A_CONFIG_FILE}" "${SESSION_B_CONFIG_FILE}" "${MATCHMAKING_CONFIG_FILE}" "${CLIENT_CONFIG_FILE}" <<PY
import json
import sys

common = {
    "LogDirectory": "${BINGO_LOG_DIR}",
    "RedisEndpoint": "${BINGO_REDIS_ENDPOINT}",
    "RedisKeyPrefix": "${BINGO_REDIS_KEY_PREFIX}",
}
roles = [
    {**common, "NodeName": "a", "MeshEndpoint": "${BINGO_API_A_MESH_ENDPOINT}",
     "MatchmakingMeshEndpoint": "${BINGO_API_A_MATCHMAKING_ENDPOINT}"},
    {**common, "NodeName": "b", "MeshEndpoint": "${BINGO_API_B_MESH_ENDPOINT}",
     "MatchmakingMeshEndpoint": "${BINGO_API_B_MATCHMAKING_ENDPOINT}"},
    {**common, "NodeName": "a", "MeshEndpoint": "${BINGO_PLAY_A_MESH_ENDPOINT}"},
    {**common, "NodeName": "b", "MeshEndpoint": "${BINGO_PLAY_B_MESH_ENDPOINT}"},
    {**common, "NodeName": "a", "MeshEndpoint": "${BINGO_SESSION_A_MESH_ENDPOINT}",
     "StreamEndpoint": "${BINGO_SESSION_A_STREAM_ENDPOINT}"},
    {**common, "NodeName": "b", "MeshEndpoint": "${BINGO_SESSION_B_MESH_ENDPOINT}",
     "StreamEndpoint": "${BINGO_SESSION_B_STREAM_ENDPOINT}"},
    {**common, "NodeName": "matchmaking", "MeshEndpoint": "${BINGO_MATCHMAKING_MESH_ENDPOINT}"},
]
for path, role in zip(sys.argv[1:-1], roles):
    with open(path, "w", encoding="utf-8") as output:
        json.dump({"Sample": role}, output, indent=2)
with open(sys.argv[-1], "w", encoding="utf-8") as output:
    json.dump({"Client": {
        "LogDirectory": "${BINGO_LOG_DIR}",
        "SessionAStreamEndpoint": "${BINGO_SESSION_A_STREAM_ENDPOINT}",
        "SessionBStreamEndpoint": "${BINGO_SESSION_B_STREAM_ENDPOINT}",
    }}, output, indent=2)
PY

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

dotnet build "${SCRIPT_DIR}/Bingo.csproj" --maxcpucount:1

# Start B before A. The sample must not depend on the process named "a" becoming
# Ready first; Framework placement selects from the Ready owners it discovers.
start_server play-b "${SCRIPT_DIR}/Server/Play/Bingo.Server.Play.csproj" --config "${PLAY_B_CONFIG_FILE}"
wait_port play-b-mesh "${BINGO_PLAY_B_MESH_ENDPOINT}"
start_server play-a "${SCRIPT_DIR}/Server/Play/Bingo.Server.Play.csproj" --config "${PLAY_A_CONFIG_FILE}"
wait_port play-a-mesh "${BINGO_PLAY_A_MESH_ENDPOINT}"

start_server matchmaking "${SCRIPT_DIR}/Server/Matchmaking/Bingo.Server.Matchmaking.csproj" --config "${MATCHMAKING_CONFIG_FILE}"
wait_port matchmaking-mesh "${BINGO_MATCHMAKING_MESH_ENDPOINT}"

start_server api-a "${SCRIPT_DIR}/Server/Api/Bingo.Server.Api.csproj" --config "${API_A_CONFIG_FILE}"
wait_port api-a-mesh "${BINGO_API_A_MESH_ENDPOINT}"
wait_port api-a-matchmaking "${BINGO_API_A_MATCHMAKING_ENDPOINT}"
start_server api-b "${SCRIPT_DIR}/Server/Api/Bingo.Server.Api.csproj" --config "${API_B_CONFIG_FILE}"
wait_port api-b-mesh "${BINGO_API_B_MESH_ENDPOINT}"
wait_port api-b-matchmaking "${BINGO_API_B_MATCHMAKING_ENDPOINT}"

start_server session-a "${SCRIPT_DIR}/Server/Session/Bingo.Server.Session.csproj" --config "${SESSION_A_CONFIG_FILE}"
wait_port session-a-mesh "${BINGO_SESSION_A_MESH_ENDPOINT}"
wait_port session-a-stream "${BINGO_SESSION_A_STREAM_ENDPOINT}"
start_server session-b "${SCRIPT_DIR}/Server/Session/Bingo.Server.Session.csproj" --config "${SESSION_B_CONFIG_FILE}"
wait_port session-b-mesh "${BINGO_SESSION_B_MESH_ENDPOINT}"
wait_port session-b-stream "${BINGO_SESSION_B_STREAM_ENDPOINT}"

dotnet run --no-build --project "${SCRIPT_DIR}/Client/Bingo.Client.csproj" -- \
  --config "${CLIENT_CONFIG_FILE}" >"${LOG_DIR}/client.log" 2>&1


# Server-side evidence is written asynchronously after the client exits;
# poll briefly instead of failing on the first read.
wait_log() {
  local pattern="$1"
  shift
  for _ in $(seq 1 50); do
    if grep -Eq "${pattern}" "$@"; then
      return 0
    fi
    sleep 0.2
  done
  echo "Timed out waiting for '${pattern}' in $*" >&2
  return 1
}

grep -q "bingo=completed" "${LOG_DIR}/client.log"
grep -q "stream-inbound sample=Bingo" "${LOG_DIR}/client.log"
grep -Eq "stream-inbound sample=Bingo .* seq=[0-9]" "${LOG_DIR}/client.log"
grep -Eq "stream-inbound sample=Bingo .* name=.*Notify" "${LOG_DIR}/client.log"
PLAY_LOGS=("${LOG_DIR}/play-a.log" "${LOG_DIR}/play-b.log")
wait_log "bingo room: player record loaded. room=.*actor=player-1, wins=0, losses=0" "${PLAY_LOGS[@]}"
wait_log "bingo room: player record loaded. room=.*actor=player-2, wins=0, losses=0" "${PLAY_LOGS[@]}"
wait_log "bingo room: result reported. room=.*actor=player-1, won=True, wins=1, losses=0" "${PLAY_LOGS[@]}"
wait_log "bingo room: result reported. room=.*actor=player-2, won=False, wins=0, losses=1" "${PLAY_LOGS[@]}"
wait_log "bingo observer room: actor left. observedRoom=.*observer=observer" "${PLAY_LOGS[@]}"
wait_log "bingo room: actor left. room=.*actor=player-1" "${PLAY_LOGS[@]}"
wait_log "bingo room: actor left. room=.*actor=player-2" "${PLAY_LOGS[@]}"
wait_log "entry spot: actor destroy completed. actor=player-1" "${PLAY_LOGS[@]}"
wait_log "entry spot: actor destroy completed. actor=player-2" "${PLAY_LOGS[@]}"
require_log_count 1 "entry spot: actor destroy completed\\. actor=player-1" "${PLAY_LOGS[@]}"
require_log_count 1 "entry spot: actor destroy completed\\. actor=player-2" "${PLAY_LOGS[@]}"
require_log_count 0 "entry spot: actor destroy completed\\. actor=observer" "${PLAY_LOGS[@]}"
require_log_count 2 "bingo room: player record loaded\\." "${PLAY_LOGS[@]}"
require_log_count 2 "bingo room: result reported\\." "${PLAY_LOGS[@]}"
grep -Eq "zlink metric name=zlink\.stream\.connections\.(active|opened)" "${LOG_DIR}/session-a.log"
grep -Eq "zlink metric name=zlink\.spot\.(count|queue\.depth)" "${LOG_DIR}/play-a.log"
# Reaching this marker proves the six placement checks in the common sample
# contract through one owner-neutral run: no fixed NodeRid exists in the generated
# configuration, B started before A, Actor and room creation completed through the
# managers, global ActorId/SpotId routing remained usable, and the level-bucket
# Instance Spot returned the same reservation to both players.
echo "bingo-placement=completed"
RUN_SUCCEEDED=1
