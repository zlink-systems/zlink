#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../redis-common.sh"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/logs/$RUN_ID"
mkdir -p "$LOG_DIR"
CONFIG_DIR="$(mktemp -d)"
if [[ "$#" -eq 0 ]]; then
  SCENARIO="all"
else
  SCENARIO="$*"
  SCENARIO="${SCENARIO// /,}"
fi
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
REDIS_READINESS_TIMEOUT_SECONDS=60
HTTP_PROBE_TIMEOUT_SECONDS=3

if [[ "$SCENARIO" == "all" ]]; then
  scenarios=(
    RL-A1 RL-A2 RL-A3 RL-A4 RL-A5
    RL-B1 RL-B2 RL-B3 RL-B4 RL-B5 RL-B6
    RL-C1 RL-C2 RL-C3 RL-C4
    RL-D1 RL-D2 RL-D3 RL-D4 RL-D5
  )
  for index in "${!scenarios[@]}"; do
    "$0" "${scenarios[$index]}"
  done
  echo "resilience-lifecycle e2e result=passed"
  exit 0
fi

LOCAL_READINESS_ATTEMPTS="$(
  python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"

PROVIDER_PROJECT="$ROOT_DIR/Server/Provider/ResilienceLifecycle.Provider.csproj"
CONSUMER_PROJECT="$ROOT_DIR/Server/Consumer/ResilienceLifecycle.Consumer.csproj"
CLIENT_PROJECT="$ROOT_DIR/Client/ResilienceLifecycle.Client.csproj"

pick_ports() {
  local count="$1"
  python3 - "$count" <<'PY'
import socket
import sys

sockets = []
try:
    for _ in range(int(sys.argv[1])):
        current = socket.socket()
        current.bind(("127.0.0.1", 0))
        sockets.append(current)
    for current in sockets:
        print(current.getsockname()[1])
finally:
    for current in sockets:
        current.close()
PY
}

pids=()
cleanup() {
  local code=$?
  rm -rf "$CONFIG_DIR"
  for pid in "${pids[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true
    fi
  done
  sleep 0.5
  for pid in "${pids[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -KILL -- "-$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null || true
    fi
  done
  wait "${pids[@]:-}" 2>/dev/null || true
  if [[ -n "${REDIS_CONTAINER:-}" ]]; then
    docker unpause "$REDIS_CONTAINER" >/dev/null 2>&1 || true
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  fi
  if [[ "$code" -ne 0 ]]; then
    echo "E2E failed. Logs: $LOG_DIR" >&2
  fi
}
trap cleanup EXIT

wait_health() {
  local url="$1"
  local name="$2"
  local deadline_ns
  deadline_ns="$(
    python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" <<PY
import sys
import time

timeout = float(sys.argv[1])
print(time.monotonic_ns() + int(timeout * 1_000_000_000))
PY
  )"
  while true; do
    local probe_timeout
    probe_timeout="$(
      python3 - "$deadline_ns" "$HTTP_PROBE_TIMEOUT_SECONDS" <<PY
import sys
import time

deadline_ns = int(sys.argv[1])
probe_timeout = float(sys.argv[2])
remaining = (deadline_ns - time.monotonic_ns()) / 1_000_000_000
if remaining <= 0:
    print("0")
else:
    print(f"{min(probe_timeout, remaining):.3f}")
PY
    )"
    if [[ "$probe_timeout" == "0" ]]; then
      break
    fi
    if curl --max-time "$probe_timeout" \
      --connect-timeout "$probe_timeout" \
      -fsS "$url/health" >/dev/null 2>&1; then
      return 0
    fi
    python3 - "$deadline_ns" "$LOCAL_READINESS_POLL_SECONDS" <<PY
import sys
import time

deadline_ns = int(sys.argv[1])
poll = float(sys.argv[2])
remaining = (deadline_ns - time.monotonic_ns()) / 1_000_000_000
if remaining > 0:
    time.sleep(min(poll, remaining))
PY
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name at $url" >&2
  return 1
}

start_server() {
  local name="$1"
  local project="$2"
  local project_dir application
  shift
  shift
  local config="$CONFIG_DIR/$name.json"
  python3 "$ROOT_DIR/../write_role_config.py" "$config" -- --role "$name" "$@"
  project_dir="$(dirname "$project")"
  application="$project_dir/bin/Debug/net8.0/$(basename "${project%.csproj}").dll"
  setsid dotnet "$application" --config "$config" \
    >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  pids+=("$!")
}

echo "log_dir=$LOG_DIR"
dotnet build "$PROVIDER_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$CONSUMER_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$CLIENT_PROJECT" --maxcpucount:1 >/dev/null

# The run owns its Redis: a dedicated, throwaway container is the shared
# location store every server registers into (no registry process exists).
if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run the ResilienceLifecycle E2E (it provisions a dedicated Redis container)." >&2
  exit 1
fi
zlink_redis_start_scoped_assign \
  REDIS_CONTAINER \
  REDIS_ENDPOINT \
  "zlink-redis-dotnet-e2e-resilience-lifecycle" \
  "redis:7.2-alpine" \
  "$LOG_DIR"
zlink_redis_wait_ready "$REDIS_CONTAINER" "$REDIS_READINESS_TIMEOUT_SECONDS"
REDIS_KEY_PREFIX="resilience-e2e:$$:"

# Select all role ports in one operation so this process cannot receive a
# duplicate ephemeral port. Do this after build and Redis startup to keep the
# unavoidable close-to-bind interval as short as possible under parallel E2E.
mapfile -t ROLE_PORTS < <(pick_ports 9)
if [[ "${#ROLE_PORTS[@]}" -ne 9 ]]; then
  echo "Failed to allocate the ResilienceLifecycle role ports." >&2
  exit 1
fi
API_A_HTTP_PORT="${ROLE_PORTS[0]}"
API_B_HTTP_PORT="${ROLE_PORTS[1]}"
CONSUMER_HTTP_PORT="${ROLE_PORTS[2]}"
API_A_PORT="${ROLE_PORTS[3]}"
API_B_PORT="${ROLE_PORTS[4]}"
API_B_REMAP_HTTP_PORT="${ROLE_PORTS[5]}"
API_B_REMAP_PORT="${ROLE_PORTS[6]}"
API_B_GREEN_HTTP_PORT="${ROLE_PORTS[7]}"
API_B_GREEN_PORT="${ROLE_PORTS[8]}"

API_A="tcp://127.0.0.1:$API_A_PORT"
API_B="tcp://127.0.0.1:$API_B_PORT"
API_A_URL="http://127.0.0.1:$API_A_HTTP_PORT"
API_B_URL="http://127.0.0.1:$API_B_HTTP_PORT"
API_B_REMAP_URL="http://127.0.0.1:$API_B_REMAP_HTTP_PORT"
API_B_REMAP="tcp://127.0.0.1:$API_B_REMAP_PORT"
API_B_GREEN_URL="http://127.0.0.1:$API_B_GREEN_HTTP_PORT"
API_B_GREEN="tcp://127.0.0.1:$API_B_GREEN_PORT"

start_server api-a "$PROVIDER_PROJECT" \
  --rid api-a \
  --http-url "$API_A_URL" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --channel-endpoint "$API_A" \
  --weight 100 \
  --evidence-file "$LOG_DIR/api-a.evidence.log" \
  --log-dir "$LOG_DIR"
API_A_PID="${pids[$((${#pids[@]} - 1))]}"
wait_health "$API_A_URL" api-a

start_server api-b "$PROVIDER_PROJECT" \
  --rid api-b \
  --http-url "$API_B_URL" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --channel-endpoint "$API_B" \
  --weight 100 \
  --evidence-file "$LOG_DIR/api-b.evidence.log" \
  --log-dir "$LOG_DIR"
API_B_PID="${pids[$((${#pids[@]} - 1))]}"
wait_health "$API_B_URL" api-b

start_server consumer "$CONSUMER_PROJECT" \
  --http-url "http://127.0.0.1:$CONSUMER_HTTP_PORT" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --log-dir "$LOG_DIR"
wait_health "http://127.0.0.1:$CONSUMER_HTTP_PORT" consumer

python3 "$ROOT_DIR/../write_role_config.py" "$CONFIG_DIR/client.json" -- \
    --config-dir "$CONFIG_DIR" \
  --consumer-url "http://127.0.0.1:$CONSUMER_HTTP_PORT" \
  --topology-url "http://127.0.0.1:$CONSUMER_HTTP_PORT" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --redis-container "$REDIS_CONTAINER" \
  --provider-a-url "$API_A_URL" \
  --provider-a-process-id "$API_A_PID" \
  --provider-a-endpoint "$API_A" \
  --provider-a-evidence-file "$LOG_DIR/api-a.evidence.log" \
  --provider-b-url "$API_B_URL" \
  --provider-b-process-id "$API_B_PID" \
  --provider-b-endpoint "$API_B" \
  --provider-b-evidence-file "$LOG_DIR/api-b.evidence.log" \
  --provider-b-remap-url "$API_B_REMAP_URL" \
  --provider-b-remap-endpoint "$API_B_REMAP" \
  --provider-b-green-url "$API_B_GREEN_URL" \
  --provider-b-green-endpoint "$API_B_GREEN" \
  --provider-project "$PROVIDER_PROJECT" \
  --log-dir "$LOG_DIR" \
  --scenario "$SCENARIO"
CLIENT_APPLICATION="$ROOT_DIR/Client/bin/Debug/net8.0/ResilienceLifecycle.Client.dll"
dotnet "$CLIENT_APPLICATION" --config "$CONFIG_DIR/client.json" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"

cat "$LOG_DIR/client.stdout.log"
