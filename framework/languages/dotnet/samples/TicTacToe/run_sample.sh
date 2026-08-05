#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../redis-common.sh"
RUN_DIR="$(mktemp -d)"
RUN_ID="$(basename "${RUN_DIR}")-$$-${RANDOM}"
LOG_DIR="${RUN_DIR}/logs"
SAMPLE_LOG_DIR="${RUN_DIR}/sample-logs"
TICTACTOE_LOG_DIR="${SAMPLE_LOG_DIR}"
mkdir -p "${LOG_DIR}" "${TICTACTOE_LOG_DIR}"

PIDS=()
REDIS_CONTAINER_ID=""
RUN_SUCCEEDED=0
TICTACTOE_REDIS_KEY_PREFIX="tictactoe:dotnet:${RUN_ID}:"

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
  for ((i=${#PIDS[@]}-1; i>=0; i--)); do
    local pid="${PIDS[$i]}"
    if kill -0 "${pid}" 2>/dev/null; then
      kill -9 "${pid}" 2>/dev/null || true
    fi
  done
  for pid in "${PIDS[@]}"; do
    wait "${pid}" 2>/dev/null || true
  done
  if [[ -n "${REDIS_CONTAINER_ID}" ]]; then
    docker rm -fv "${REDIS_CONTAINER_ID}" >/dev/null 2>&1 || true
  fi
  zlink_sample_copy_evidence "${RUN_DIR}" "TicTacToe"
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
    while len(sockets) < 8:
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

API_A_BIND_URL="http://127.0.0.1:${PORTS[0]}"
API_B_BIND_URL="http://127.0.0.1:${PORTS[1]}"
API_A_PUBLIC_URL="${API_A_BIND_URL}"
API_B_PUBLIC_URL="${API_B_BIND_URL}"
API_A_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[2]}"
API_B_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[3]}"
PLAY_A_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[4]}"
PLAY_B_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[5]}"
PLAY_A_ENDPOINT="tcp://127.0.0.1:${PORTS[6]}"
PLAY_B_ENDPOINT="tcp://127.0.0.1:${PORTS[7]}"
API_A_CONFIG_FILE="${RUN_DIR}/appsettings.api-a.json"
API_B_CONFIG_FILE="${RUN_DIR}/appsettings.api-b.json"
PLAY_A_CONFIG_FILE="${RUN_DIR}/appsettings.play-a.json"
PLAY_B_CONFIG_FILE="${RUN_DIR}/appsettings.play-b.json"
CLIENT_CONFIG_FILE="${RUN_DIR}/appsettings.client.json"

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run the TicTacToe sample." >&2
  exit 1
fi
zlink_redis_start_scoped_assign REDIS_CONTAINER_ID TICTACTOE_REDIS_ENDPOINT "zlink-tictactoe-dotnet-redis" redis:7.2-alpine
REDIS_ENDPOINT="${TICTACTOE_REDIS_ENDPOINT}"

python3 - "${API_A_CONFIG_FILE}" "${API_B_CONFIG_FILE}" "${PLAY_A_CONFIG_FILE}" "${PLAY_B_CONFIG_FILE}" "${CLIENT_CONFIG_FILE}" <<PY
import json
import sys

api_a_path, api_b_path, play_a_path, play_b_path, client_path = sys.argv[1:]

def api(instance_name, bind_url, mesh_endpoint):
    return {
        "Sample": {
            "InstanceName": instance_name,
            "ApiBindUrl": bind_url,
            "MeshEndpoint": mesh_endpoint,
            "PeerMeshEndpoints": ["${PLAY_A_MESH_ENDPOINT}", "${PLAY_B_MESH_ENDPOINT}"],
            "PlayEndpoints": ["${PLAY_A_ENDPOINT}", "${PLAY_B_ENDPOINT}"],
            "RedisEndpoint": "${REDIS_ENDPOINT}",
            "RedisKeyPrefix": "${TICTACTOE_REDIS_KEY_PREFIX}",
            "LogDirectory": "${SAMPLE_LOG_DIR}"
        }
    }

def play(instance_name, mesh_endpoint, peer_mesh_endpoints, play_endpoint):
    return {
        "Sample": {
            "InstanceName": instance_name,
            "MeshEndpoint": mesh_endpoint,
            "PeerMeshEndpoints": peer_mesh_endpoints,
            "PlayEndpoint": play_endpoint,
            "PlayEndpoints": ["${PLAY_A_ENDPOINT}", "${PLAY_B_ENDPOINT}"],
            "RedisEndpoint": "${REDIS_ENDPOINT}",
            "RedisKeyPrefix": "${TICTACTOE_REDIS_KEY_PREFIX}",
            "LogDirectory": "${SAMPLE_LOG_DIR}"
        }
    }

for path, settings in [
    (api_a_path, api("api-a", "${API_A_BIND_URL}", "${API_A_MESH_ENDPOINT}")),
    (api_b_path, api("api-b", "${API_B_BIND_URL}", "${API_B_MESH_ENDPOINT}")),
    (play_a_path, play("play-a", "${PLAY_A_MESH_ENDPOINT}", [], "${PLAY_A_ENDPOINT}")),
    (play_b_path, play("play-b", "${PLAY_B_MESH_ENDPOINT}", ["${PLAY_A_MESH_ENDPOINT}"], "${PLAY_B_ENDPOINT}")),
    (client_path, {"Sample": {"ApiPublicUrls": ["${API_A_PUBLIC_URL}"], "LogDirectory": "${SAMPLE_LOG_DIR}"}}),
]:
    with open(path, "w", encoding="utf-8") as output:
        json.dump(settings, output, indent=2)
PY

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

wait_log_contains() {
  local description="$1"
  local pattern="$2"
  shift 2
  for _ in $(seq 1 30); do
    if grep -Eq "${pattern}" "$@" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for log marker: ${description}" >&2
  return 1
}

start_server() {
  local name="$1"
  local assembly="$2"
  local config_file="$3"
  dotnet "${assembly}" --config "${config_file}" >"${LOG_DIR}/${name}.log" 2>&1 &
  PIDS+=("$!")
}

dotnet build "${SCRIPT_DIR}/TicTacToe.sln" --maxcpucount:1

wait_port redis "tcp://${REDIS_ENDPOINT}"

start_server play-a "${SCRIPT_DIR}/Server/Play/bin/Debug/net8.0/TicTacToe.Server.Play.dll" "${PLAY_A_CONFIG_FILE}"
wait_port play-a-stream "${PLAY_A_ENDPOINT}"
wait_port play-a-mesh "${PLAY_A_MESH_ENDPOINT}"

start_server play-b "${SCRIPT_DIR}/Server/Play/bin/Debug/net8.0/TicTacToe.Server.Play.dll" "${PLAY_B_CONFIG_FILE}"
wait_port play-b-stream "${PLAY_B_ENDPOINT}"
wait_port play-b-mesh "${PLAY_B_MESH_ENDPOINT}"

start_server api-a "${SCRIPT_DIR}/Server/Api/bin/Debug/net8.0/TicTacToe.Server.Api.dll" "${API_A_CONFIG_FILE}"
wait_port api-a-http "${API_A_BIND_URL}"
wait_port api-a-mesh "${API_A_MESH_ENDPOINT}"

start_server api-b "${SCRIPT_DIR}/Server/Api/bin/Debug/net8.0/TicTacToe.Server.Api.dll" "${API_B_CONFIG_FILE}"
wait_port api-b-http "${API_B_BIND_URL}"
wait_port api-b-mesh "${API_B_MESH_ENDPOINT}"

dotnet run --no-build --project "${SCRIPT_DIR}/Client/TicTacToe.Client.csproj" -- \
  --config "${CLIENT_CONFIG_FILE}" >"${LOG_DIR}/client.log" 2>&1
wait_log_contains "stream inbound evidence" "stream-inbound sample=TicTacToe" "${LOG_DIR}/client.log"
wait_log_contains "stream inbound sequenced packet" "stream-inbound sample=TicTacToe .* seq=[0-9]" "${LOG_DIR}/client.log"
wait_log_contains "stream inbound notify packet" "stream-inbound sample=TicTacToe .* name=.*Notify" "${LOG_DIR}/client.log"
wait_log_contains "observer milestone verification" "observer-win-milestone=verified" "${LOG_DIR}/client.log"
wait_log_contains "player-x leave completion" "actor: LeaveGameMsg completed. actor=player-x" "${LOG_DIR}"/play-*.log
wait_log_contains "player-o leave completion" "actor: LeaveGameMsg completed. actor=player-o" "${LOG_DIR}"/play-*.log
wait_log_contains "player-x actor destroy completion" "entry spot: actor destroy completed. actor=player-x" "${LOG_DIR}"/play-*.log
wait_log_contains "player-o actor destroy completion" "entry spot: actor destroy completed. actor=player-o" "${LOG_DIR}"/play-*.log
if grep -R -q "dispatch-error" "${LOG_DIR}"; then
  echo "Unexpected dispatch-error in TicTacToe sample logs." >&2
  grep -R -n "dispatch-error" "${LOG_DIR}" >&2 || true
  exit 1
fi
RUN_SUCCEEDED=1
