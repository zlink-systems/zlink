#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../redis-common.sh"
RUN_DIR="$(mktemp -d)"
RUN_ID="$(basename "${RUN_DIR}")-$$-${RANDOM}"
LOG_DIR="${RUN_DIR}/logs"
SAMPLE_LOG_DIR="${RUN_DIR}/sample-logs"
SUPPORTCHAT_LOG_DIR="${SAMPLE_LOG_DIR}"
mkdir -p "${LOG_DIR}" "${SUPPORTCHAT_LOG_DIR}"

PIDS=()
REDIS_CONTAINER=""
RUN_SUCCEEDED=0
SUPPORTCHAT_REDIS_KEY_PREFIX="supportchat:dotnet:${RUN_ID}:"

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
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
  fi
  zlink_sample_copy_evidence "${RUN_DIR}" "SupportChat"
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
    while len(sockets) < 4:
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

SUPPORTCHAT_SUPPORT_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[0]}"
SUPPORTCHAT_API_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[1]}"
SUPPORTCHAT_SESSION_MESH_ENDPOINT="tcp://127.0.0.1:${PORTS[2]}"
SUPPORTCHAT_STREAM_ENDPOINT="tcp://127.0.0.1:${PORTS[3]}"

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

wait_log() {
  local pattern="$1"
  local file="$2"
  for _ in $(seq 1 50); do
    if grep -Eq "${pattern}" "${file}"; then
      return 0
    fi
    sleep 0.2
  done
  echo "Timed out waiting for '${pattern}' in ${file}" >&2
  return 1
}

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run the SupportChat sample." >&2
  exit 1
fi
REDIS_CONTAINER="zlink-supportchat-dotnet-redis-${RUN_ID}"
zlink_redis_start_scoped_assign REDIS_CONTAINER SUPPORTCHAT_REDIS_ENDPOINT "zlink-supportchat-dotnet-redis" redis:7.2-alpine
wait_port redis "tcp://${SUPPORTCHAT_REDIS_ENDPOINT}"
SUPPORT_CONFIG_FILE="${RUN_DIR}/appsettings.support.json"
API_CONFIG_FILE="${RUN_DIR}/appsettings.api.json"
SESSION_CONFIG_FILE="${RUN_DIR}/appsettings.session.json"
CLIENT_CONFIG_FILE="${RUN_DIR}/appsettings.client.json"
python3 - "${SUPPORT_CONFIG_FILE}" "${API_CONFIG_FILE}" "${SESSION_CONFIG_FILE}" "${CLIENT_CONFIG_FILE}" <<PY
import json
import sys

settings = {
    "LogDirectory": "${SUPPORTCHAT_LOG_DIR}",
    "RedisEndpoint": "${SUPPORTCHAT_REDIS_ENDPOINT}",
    "RedisKeyPrefix": "${SUPPORTCHAT_REDIS_KEY_PREFIX}",
    "StreamEndpoint": "${SUPPORTCHAT_STREAM_ENDPOINT}",
}
server_settings = {
    "LogDirectory": settings["LogDirectory"],
    "RedisEndpoint": settings["RedisEndpoint"],
    "RedisKeyPrefix": settings["RedisKeyPrefix"],
}
with open(sys.argv[1], "w", encoding="utf-8") as output:
    json.dump({"Sample": {**server_settings,
        "MeshEndpoint": "${SUPPORTCHAT_SUPPORT_MESH_ENDPOINT}",
    }}, output, indent=2)
with open(sys.argv[2], "w", encoding="utf-8") as output:
    json.dump({"Sample": {**server_settings,
        "MeshEndpoint": "${SUPPORTCHAT_API_MESH_ENDPOINT}",
    }}, output, indent=2)
with open(sys.argv[3], "w", encoding="utf-8") as output:
    json.dump({"Sample": {**server_settings,
        "MeshEndpoint": "${SUPPORTCHAT_SESSION_MESH_ENDPOINT}",
        "StreamEndpoint": settings["StreamEndpoint"],
    }}, output, indent=2)
with open(sys.argv[4], "w", encoding="utf-8") as output:
    json.dump({"Client": {
        "LogDirectory": "${SUPPORTCHAT_LOG_DIR}",
        "StreamEndpoint": "${SUPPORTCHAT_STREAM_ENDPOINT}",
    }}, output, indent=2)
PY

dotnet build "${SCRIPT_DIR}/SupportChat.csproj" --maxcpucount:1

start_server support "${SCRIPT_DIR}/Server/Support/SupportChat.Server.Support.csproj" --config "${SUPPORT_CONFIG_FILE}"
wait_port support-mesh "${SUPPORTCHAT_SUPPORT_MESH_ENDPOINT}"

start_server api "${SCRIPT_DIR}/Server/Api/SupportChat.Server.Api.csproj" --config "${API_CONFIG_FILE}"
wait_port api-mesh "${SUPPORTCHAT_API_MESH_ENDPOINT}"

start_server session "${SCRIPT_DIR}/Server/Session/SupportChat.Server.Session.csproj" --config "${SESSION_CONFIG_FILE}"
wait_port session-mesh "${SUPPORTCHAT_SESSION_MESH_ENDPOINT}"
wait_port session-stream "${SUPPORTCHAT_STREAM_ENDPOINT}"

dotnet run --no-build --project "${SCRIPT_DIR}/Client/SupportChat.Client.csproj" -- \
  --config "${CLIENT_CONFIG_FILE}" >"${LOG_DIR}/client.log" 2>&1

grep -q "supportchat=completed" "${LOG_DIR}/client.log"
grep -q "supportchat-closed-typing-ignore=verified" "${LOG_DIR}/client.log"
wait_log "support conversation: created" "${LOG_DIR}/support.log"
wait_log "support conversation: actor joined" "${LOG_DIR}/support.log"
wait_log "status=WaitingForAgent" "${LOG_DIR}/support.log"
wait_log "status=Active" "${LOG_DIR}/support.log"
wait_log "status=WaitingForClose" "${LOG_DIR}/support.log"
wait_log "status=Closed" "${LOG_DIR}/support.log"
echo "supportchat-server-evidence=completed"
RUN_SUCCEEDED=1
