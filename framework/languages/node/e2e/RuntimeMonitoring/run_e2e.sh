#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NODE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
source "$NODE_ROOT/e2e/redis-container.sh"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/log/$RUN_ID"
CONFIG_DIR=""
SCENARIO="${1:-all}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
ROUTE_SETTLE_TIMEOUT_SECONDS=5
SCENARIO_SETTLE_TIMEOUT_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
mkdir -p "$LOG_DIR"

RESERVED_PORTS=()
reserve_port() {
  local variable_name="$1"
  local port
  while true; do
    port="$(node "$NODE_ROOT/e2e/port-picker.js")"
    if [[ ! " ${RESERVED_PORTS[*]} " =~ " $port " ]]; then
      RESERVED_PORTS+=("$port")
      printf -v "$variable_name" '%s' "$port"
      return
    fi
  done
}

build_package() {
  local dir="$1"
  (cd "$dir" && npm run build >/dev/null)
}

wait_health() {
  local url="$1"
  local name="$2"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS "$url/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for $name at $url" >&2
  return 1
}

pids=()
pid_names=()
REDIS_CONTAINER_ID=""
cleanup() {
  local code=$?
  local background_failure=0
  local index pid name status
  for index in "${!pids[@]}"; do
    pid="${pids[$index]}"
    name="${pid_names[$index]}"
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
  done
  for index in "${!pids[@]}"; do
    pid="${pids[$index]}"
    name="${pid_names[$index]}"
    set +e
    wait "$pid" 2>/dev/null
    status=$?
    set -e
    if [[ ("$SCENARIO" == "MON-A4" || "$SCENARIO" == "all") && "$name" == "svc-a" && "$status" -eq 137 ]]; then
      continue
    fi
    if [[ ("$SCENARIO" == "MON-A4B" || "$SCENARIO" == "all") && "$name" == "svc-b" && "$status" -eq 137 ]]; then
      continue
    fi
    if [[ "$status" -ne 0 && "$status" -ne 143 ]]; then
      background_failure=1
      echo "Background role $name exited unexpectedly with status $status." >&2
    fi
  done
  if [[ -n "$REDIS_CONTAINER_ID" ]]; then
    docker rm -fv "$REDIS_CONTAINER_ID" >/dev/null 2>&1 || true
  fi
  [[ -z "$CONFIG_DIR" ]] || rm -rf "$CONFIG_DIR"
  if [[ "$code" -ne 0 || "$background_failure" -ne 0 ]]; then
    echo "E2E failed. log_dir=$LOG_DIR" >&2
    for file in "$LOG_DIR"/*.stderr.log "$LOG_DIR"/client.stderr.log; do
      if [[ -f "$file" ]]; then
        echo "----- $file -----" >&2
        tail -n 80 "$file" >&2 || true
      fi
    done
  fi
  if [[ "$code" -eq 0 && "$background_failure" -ne 0 ]]; then
    exit 1
  fi
}
trap cleanup EXIT
CONFIG_DIR="$(mktemp -d)"
chmod 700 "$CONFIG_DIR"

start_server() {
  local name="$1"
  local main="$2"
  shift
  shift
  node "$main" "$@" >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  pids+=("$!")
  pid_names+=("$name")
}

wait_tcp() {
  local name="$1"
  local endpoint="$2"
  local host_port="${endpoint#tcp://}"
  local host="${host_port%:*}"
  local port="${host_port##*:}"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if node -e "const net=require('node:net'); const s=net.createConnection({host: process.argv[1], port: Number(process.argv[2])}); s.once('connect', () => { s.end(); process.exit(0); }); s.once('error', () => process.exit(1)); setTimeout(() => process.exit(1), 500);" "$host" "$port"; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for $name at $endpoint" >&2
  return 1
}

echo "log_dir=$LOG_DIR"

(cd "$NODE_ROOT" && npm run build >/dev/null)
build_package "$ROOT_DIR/Server/Service"
build_package "$ROOT_DIR/Server/FilteredService"
build_package "$ROOT_DIR/Server/ThrowingService"
build_package "$ROOT_DIR/Server/Trigger"
build_package "$ROOT_DIR/Client"

reserve_port SVC_HTTP_PORT
reserve_port SVC_B_HTTP_PORT
reserve_port SVC_B_REPLACEMENT_HTTP_PORT
reserve_port THROW_HTTP_PORT
reserve_port TRIGGER_HTTP_PORT
reserve_port CHANNEL_PORT
reserve_port CHANNEL_B_PORT
reserve_port CHANNEL_B_REPLACEMENT_PORT
reserve_port THROW_CHANNEL_PORT
reserve_port SPOT_ROUTER_PORT
reserve_port SPOT_PUB_PORT
reserve_port SPOT_B_ROUTER_PORT
reserve_port SPOT_B_PUB_PORT
reserve_port SPOT_B_REPLACEMENT_ROUTER_PORT
reserve_port SPOT_B_REPLACEMENT_PUB_PORT
reserve_port THROW_SPOT_ROUTER_PORT
reserve_port THROW_SPOT_PUB_PORT

SVC_URL="http://127.0.0.1:$SVC_HTTP_PORT"
SVC_B_URL="http://127.0.0.1:$SVC_B_HTTP_PORT"
SVC_B_REPLACEMENT_URL="http://127.0.0.1:$SVC_B_REPLACEMENT_HTTP_PORT"
THROW_URL="http://127.0.0.1:$THROW_HTTP_PORT"
TRIGGER_URL="http://127.0.0.1:$TRIGGER_HTTP_PORT"
CHANNEL_ENDPOINT="tcp://127.0.0.1:$CHANNEL_PORT"
CHANNEL_B_ENDPOINT="tcp://127.0.0.1:$CHANNEL_B_PORT"
CHANNEL_B_REPLACEMENT_ENDPOINT="tcp://127.0.0.1:$CHANNEL_B_REPLACEMENT_PORT"
THROW_CHANNEL_ENDPOINT="tcp://127.0.0.1:$THROW_CHANNEL_PORT"
SPOT_ROUTER_ENDPOINT="tcp://127.0.0.1:$SPOT_ROUTER_PORT"
SPOT_PUB_ENDPOINT="tcp://127.0.0.1:$SPOT_PUB_PORT"
SPOT_B_ROUTER_ENDPOINT="tcp://127.0.0.1:$SPOT_B_ROUTER_PORT"
SPOT_B_PUB_ENDPOINT="tcp://127.0.0.1:$SPOT_B_PUB_PORT"
SPOT_B_REPLACEMENT_ROUTER_ENDPOINT="tcp://127.0.0.1:$SPOT_B_REPLACEMENT_ROUTER_PORT"
SPOT_B_REPLACEMENT_PUB_ENDPOINT="tcp://127.0.0.1:$SPOT_B_REPLACEMENT_PUB_PORT"
THROW_SPOT_ROUTER_ENDPOINT="tcp://127.0.0.1:$THROW_SPOT_ROUTER_PORT"
THROW_SPOT_PUB_ENDPOINT="tcp://127.0.0.1:$THROW_SPOT_PUB_PORT"

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run RuntimeMonitoring because it provisions a dedicated Redis location store." >&2
  exit 1
fi

start_redis_container "zlink-redis-node-e2e-${RANDOM}-$$" -p "127.0.0.1::6379" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
REDIS_KEY_PREFIX="runtime-monitoring:node:$RUN_ID"
wait_tcp redis "tcp://$REDIS_ENDPOINT"

SERVICE_MAIN="$ROOT_DIR/Server/Service/dist/Server/Service/main.js"
FILTERED_SERVICE_MAIN="$ROOT_DIR/Server/FilteredService/dist/Server/FilteredService/main.js"
THROWING_SERVICE_MAIN="$ROOT_DIR/Server/ThrowingService/dist/Server/ThrowingService/main.js"
TRIGGER_MAIN="$ROOT_DIR/Server/Trigger/dist/Server/Trigger/main.js"
CLIENT_MAIN="$ROOT_DIR/Client/dist/RuntimeMonitoring/Client/main.js"

start_configured_server() {
  local name="$1"
  local main="$2"
  shift 2
  local config="$CONFIG_DIR/$name.config.json"
  node "$ROOT_DIR/write-config.mjs" "$config" "$@"
  start_server "$name" "$main" --config "$config"
}

start_configured_server svc-a "$SERVICE_MAIN" \
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

if [[ "$SCENARIO" != "MON-B2" ]]; then
  start_configured_server svc-b "$FILTERED_SERVICE_MAIN" \
    --rid svc-b \
    --http-url "$SVC_B_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --channel-endpoint "$CHANNEL_B_ENDPOINT" \
    --spot-router-endpoint "$SPOT_B_ROUTER_ENDPOINT" \
    --spot-pub-endpoint "$SPOT_B_PUB_ENDPOINT" \
    --evidence-file "$LOG_DIR/svc-b.evidence.log" \
    --log-dir "$LOG_DIR"
  wait_health "$SVC_B_URL" svc-b

  node "$ROOT_DIR/write-config.mjs" "$CONFIG_DIR/svc-b-replacement.config.json" \
    --rid svc-b \
    --http-url "$SVC_B_REPLACEMENT_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --channel-endpoint "$CHANNEL_B_REPLACEMENT_ENDPOINT" \
    --spot-router-endpoint "$SPOT_B_REPLACEMENT_ROUTER_ENDPOINT" \
    --spot-pub-endpoint "$SPOT_B_REPLACEMENT_PUB_ENDPOINT" \
    --evidence-file "$LOG_DIR/svc-b-replacement.evidence.log" \
    --log-dir "$LOG_DIR"

  start_configured_server svc-throw "$THROWING_SERVICE_MAIN" \
    --rid svc-throw \
    --http-url "$THROW_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --channel-endpoint "$THROW_CHANNEL_ENDPOINT" \
    --spot-router-endpoint "$THROW_SPOT_ROUTER_ENDPOINT" \
    --spot-pub-endpoint "$THROW_SPOT_PUB_ENDPOINT" \
    --evidence-file "$LOG_DIR/svc-throw.evidence.log" \
    --log-dir "$LOG_DIR"
  wait_health "$THROW_URL" svc-throw
else
  node "$ROOT_DIR/write-config.mjs" "$CONFIG_DIR/svc-b-replacement.config.json" \
    --rid svc-b \
    --http-url "$SVC_B_REPLACEMENT_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --channel-endpoint "$CHANNEL_B_REPLACEMENT_ENDPOINT" \
    --spot-router-endpoint "$SPOT_B_REPLACEMENT_ROUTER_ENDPOINT" \
    --spot-pub-endpoint "$SPOT_B_REPLACEMENT_PUB_ENDPOINT" \
    --evidence-file "$LOG_DIR/svc-b-replacement.evidence.log" \
    --log-dir "$LOG_DIR"
fi

start_configured_server trigger "$TRIGGER_MAIN" \
  --http-url "$TRIGGER_URL" \
  --service-channel-endpoint "$CHANNEL_ENDPOINT" \
  --service-b-channel-endpoint "$CHANNEL_B_ENDPOINT" \
  --replacement-service-channel-endpoint "$CHANNEL_B_REPLACEMENT_ENDPOINT" \
  --throw-channel-endpoint "$THROW_CHANNEL_ENDPOINT" \
  --log-dir "$LOG_DIR"
wait_health "$TRIGGER_URL" trigger

CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
  --trigger-url "$TRIGGER_URL" \
  --service-url "$SVC_URL" \
  --service-b-url "$SVC_B_URL" \
  --throw-service-url "$THROW_URL" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --redis-container "$REDIS_CONTAINER_ID" \
  --service-b-channel-endpoint "$CHANNEL_B_ENDPOINT" \
  --service-channel-endpoint "$CHANNEL_ENDPOINT" \
  --service-b-spot-router-endpoint "$SPOT_B_ROUTER_ENDPOINT" \
  --service-b-spot-pub-endpoint "$SPOT_B_PUB_ENDPOINT" \
  --service-main "$SERVICE_MAIN" \
  --filtered-service-main "$FILTERED_SERVICE_MAIN" \
  --service-b-config "$CONFIG_DIR/svc-b.config.json" \
  --replacement-service-url "$SVC_B_REPLACEMENT_URL" \
  --replacement-service-channel-endpoint "$CHANNEL_B_REPLACEMENT_ENDPOINT" \
  --replacement-service-config "$CONFIG_DIR/svc-b-replacement.config.json" \
  --log-dir "$LOG_DIR" \
  --scenario "$SCENARIO"
node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"

cat "$LOG_DIR/client.stdout.log"
