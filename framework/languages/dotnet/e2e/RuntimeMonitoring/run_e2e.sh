#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../redis-common.sh"
if [[ "$#" -eq 0 ]]; then
  SCENARIO="all"
else
  SCENARIO="$*"
  SCENARIO="${SCENARIO// /,}"
fi
if [[ "$SCENARIO" == "all" ]]; then
  for scenario in \
    MON-A1 MON-A2 MON-A3 MON-A4A MON-A4B MON-A5 MON-B1 MON-B2 MON-C1 MON-D1A MON-D1B; do
    "$0" "$scenario"
  done
  echo "runtime-monitoring all scenarios passed"
  exit 0
fi
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/logs/$RUN_ID"
mkdir -p "$LOG_DIR"
CONFIG_DIR="$(mktemp -d)"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
REDIS_READINESS_TIMEOUT_SECONDS=60
HTTP_PROBE_TIMEOUT_SECONDS=3
LOCAL_READINESS_ATTEMPTS="$(
  python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"

SERVICE_PROJECT="$ROOT_DIR/Server/Service/RuntimeMonitoring.Service.csproj"
FILTERED_SERVICE_PROJECT="$ROOT_DIR/Server/FilteredService/RuntimeMonitoring.FilteredService.csproj"
THROWING_SERVICE_PROJECT="$ROOT_DIR/Server/ThrowingService/RuntimeMonitoring.ThrowingService.csproj"
CLIENT_PROJECT="$ROOT_DIR/Client/RuntimeMonitoring.Client.csproj"
VALIDATION_HOST_PROJECT="$ROOT_DIR/Server/ValidationHost/RuntimeMonitoring.ValidationHost.csproj"

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

SVC_HTTP_PORT="$(pick_port)"
SVC_B_HTTP_PORT="$(pick_port)"
THROW_HTTP_PORT="$(pick_port)"
FILTERED_HTTP_PORT="$(pick_port)"
CHANNEL_PORT="$(pick_port)"
CHANNEL_B_PORT="$(pick_port)"
THROW_CHANNEL_PORT="$(pick_port)"
FILTERED_CHANNEL_PORT="$(pick_port)"
SPOT_ROUTER_PORT="$(pick_port)"
SPOT_PUB_PORT="$(pick_port)"
SPOT_B_ROUTER_PORT="$(pick_port)"
SPOT_B_PUB_PORT="$(pick_port)"

SVC_URL="http://127.0.0.1:$SVC_HTTP_PORT"
SVC_B_URL="http://127.0.0.1:$SVC_B_HTTP_PORT"
THROW_URL="http://127.0.0.1:$THROW_HTTP_PORT"
FILTERED_URL="http://127.0.0.1:$FILTERED_HTTP_PORT"
CHANNEL_ENDPOINT="tcp://127.0.0.1:$CHANNEL_PORT"
CHANNEL_B_ENDPOINT="tcp://127.0.0.1:$CHANNEL_B_PORT"
THROW_CHANNEL_ENDPOINT="tcp://127.0.0.1:$THROW_CHANNEL_PORT"
FILTERED_CHANNEL_ENDPOINT="tcp://127.0.0.1:$FILTERED_CHANNEL_PORT"
SPOT_ROUTER_ENDPOINT="tcp://127.0.0.1:$SPOT_ROUTER_PORT"
SPOT_PUB_ENDPOINT="tcp://127.0.0.1:$SPOT_PUB_PORT"
SPOT_B_ROUTER_ENDPOINT="tcp://127.0.0.1:$SPOT_B_ROUTER_PORT"
SPOT_B_PUB_ENDPOINT="tcp://127.0.0.1:$SPOT_B_PUB_PORT"

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

start_service() {
  local name="$1" project="$2"
  shift 2
  local config="$CONFIG_DIR/$name.json"
  python3 "$ROOT_DIR/../write_role_config.py" "$config" -- --role "$name" "$@"
  setsid dotnet run --no-build --project "$project" -- --config "$config" \
    >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  pids+=("$!")
}

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

echo "log_dir=$LOG_DIR"
dotnet build "$SERVICE_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$FILTERED_SERVICE_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$THROWING_SERVICE_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$CLIENT_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$VALIDATION_HOST_PROJECT" --maxcpucount:1 >/dev/null

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run the RuntimeMonitoring E2E (it provisions a dedicated Redis container)." >&2
  exit 1
fi
zlink_redis_start_scoped_assign \
  REDIS_CONTAINER \
  REDIS_ENDPOINT \
  "zlink-redis-dotnet-e2e-runtime-monitoring" \
  "redis:7.2-alpine" \
  "$LOG_DIR"
zlink_redis_wait_ready "$REDIS_CONTAINER" "$REDIS_READINESS_TIMEOUT_SECONDS"
REDIS_KEY_PREFIX="monitoring-e2e:$$:"

start_service svc-a "$SERVICE_PROJECT" \
  --rid svc-a \
  --http-url "$SVC_URL" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --channel-endpoint "$CHANNEL_ENDPOINT" \
  --spot-router-endpoint "$SPOT_ROUTER_ENDPOINT" \
  --spot-pub-endpoint "$SPOT_PUB_ENDPOINT" \
  --evidence-file "$LOG_DIR/svc-a.evidence.log" \
  --log-dir "$LOG_DIR"
wait_health "$SVC_URL" svc-a

SERVICE_B_PID=0
if [[ "$SCENARIO" != "MON-A1" && "$SCENARIO" != "MON-A2" && "$SCENARIO" != "MON-D1A" ]]; then
  start_service svc-b "$SERVICE_PROJECT" \
    --rid svc-b \
    --http-url "$SVC_B_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --channel-endpoint "$CHANNEL_B_ENDPOINT" \
    --spot-router-endpoint "$SPOT_B_ROUTER_ENDPOINT" \
    --spot-pub-endpoint "$SPOT_B_PUB_ENDPOINT" \
    --evidence-file "$LOG_DIR/svc-b.evidence.log" \
    --log-dir "$LOG_DIR"
  SERVICE_B_PID="${pids[-1]}"
  wait_health "$SVC_B_URL" svc-b
fi

start_service svc-filtered "$FILTERED_SERVICE_PROJECT" \
  --rid svc-filtered \
  --http-url "$FILTERED_URL" \
  --channel-endpoint "$FILTERED_CHANNEL_ENDPOINT" \
  --evidence-file "$LOG_DIR/svc-filtered.evidence.log" \
  --log-dir "$LOG_DIR"
wait_health "$FILTERED_URL" svc-filtered

start_service svc-throw "$THROWING_SERVICE_PROJECT" \
  --rid svc-throw \
  --http-url "$THROW_URL" \
  --channel-endpoint "$THROW_CHANNEL_ENDPOINT" \
  --evidence-file "$LOG_DIR/svc-throw.evidence.log" \
  --log-dir "$LOG_DIR"
wait_health "$THROW_URL" svc-throw

python3 "$ROOT_DIR/../write_role_config.py" "$CONFIG_DIR/client.json" -- \
    --config-dir "$CONFIG_DIR" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --redis-container "$REDIS_CONTAINER" \
  --service-url "$SVC_URL" \
  --service-channel-endpoint "$CHANNEL_ENDPOINT" \
  --service-b-url "$SVC_B_URL" \
  --service-b-process-id "$SERVICE_B_PID" \
  --service-b-channel-endpoint "$CHANNEL_B_ENDPOINT" \
  --service-b-spot-router-endpoint "$SPOT_B_ROUTER_ENDPOINT" \
  --service-b-spot-pub-endpoint "$SPOT_B_PUB_ENDPOINT" \
  --filtered-service-url "$FILTERED_URL" \
  --filtered-channel-endpoint "$FILTERED_CHANNEL_ENDPOINT" \
  --throw-service-url "$THROW_URL" \
  --throw-channel-endpoint "$THROW_CHANNEL_ENDPOINT" \
  --filtered-service-project "$FILTERED_SERVICE_PROJECT" \
  --service-project "$SERVICE_PROJECT" \
  --validation-host-project "$VALIDATION_HOST_PROJECT" \
  --scenario "$SCENARIO" \
  --log-dir "$LOG_DIR"
dotnet run --no-build --project "$CLIENT_PROJECT" -- --config "$CONFIG_DIR/client.json" \
  > >(tee "$LOG_DIR/client.stdout.log") \
  2> >(tee "$LOG_DIR/client.stderr.log" >&2)
