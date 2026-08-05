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
RESILIENCE_CONSUMER_COUNT="${RESILIENCE_CONSUMER_COUNT:-8}"
RL_D5_DURATION_SECONDS="${RL_D5_DURATION_SECONDS:-120}"
mkdir -p "$LOG_DIR"

pick_port() {
  node "$NODE_ROOT/e2e/port-picker.js"
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
REDIS_CONTAINER_ID=""
cleanup() {
  local code=$?
  for pid in "${pids[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
  done
  wait "${pids[@]:-}" 2>/dev/null || true
  if [[ -n "$REDIS_CONTAINER_ID" ]]; then
    docker rm -fv "$REDIS_CONTAINER_ID" >/dev/null 2>&1 || true
  fi
  [[ -z "$CONFIG_DIR" ]] || rm -rf "$CONFIG_DIR"
  if [[ "$code" -ne 0 ]]; then
    echo "E2E failed. log_dir=$LOG_DIR" >&2
    for file in "$LOG_DIR"/*.stderr.log "$LOG_DIR"/client.stderr.log; do
      if [[ -f "$file" ]]; then
        echo "----- $file -----" >&2
        tail -n 80 "$file" >&2 || true
      fi
    done
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
build_package "$ROOT_DIR/Server/Provider"
build_package "$ROOT_DIR/Server/Consumer"
build_package "$ROOT_DIR/Client"

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run ResilienceLifecycle because it provisions a dedicated Redis location store." >&2
  exit 1
fi

PROVIDER_A_HTTP_PORT="$(pick_port)"
PROVIDER_B_HTTP_PORT="$(pick_port)"
PROVIDER_B_REMAP_HTTP_PORT="$(pick_port)"
PROVIDER_B_GREEN_HTTP_PORT="$(pick_port)"
CONSUMER_HTTP_PORT="$(pick_port)"
API_A_PORT="$(pick_port)"
API_B_PORT="$(pick_port)"
API_B_REMAP_PORT="$(pick_port)"
API_B_GREEN_PORT="$(pick_port)"
FANOUT_PORT="$(pick_port)"

API_A="tcp://127.0.0.1:$API_A_PORT"
API_B="tcp://127.0.0.1:$API_B_PORT"
API_B_REMAP="tcp://127.0.0.1:$API_B_REMAP_PORT"
API_B_GREEN="tcp://127.0.0.1:$API_B_GREEN_PORT"
FANOUT_ENDPOINT="tcp://127.0.0.1:$FANOUT_PORT"

start_redis_container "zlink-redis-node-e2e-${RANDOM}-$$" -p "127.0.0.1::6379" "redis:7.2-alpine"
REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
REDIS_KEY_PREFIX="resilience-lifecycle:node:$RUN_ID"
wait_tcp redis "tcp://$REDIS_ENDPOINT"

PROVIDER_MAIN="$ROOT_DIR/Server/Provider/dist/Server/Provider/main.js"
CONSUMER_MAIN="$ROOT_DIR/Server/Consumer/dist/Server/Consumer/main.js"
CLIENT_MAIN="$ROOT_DIR/Client/dist/ResilienceLifecycle/Client/main.js"

start_configured_server() {
  local name="$1"; local main="$2"; shift 2
  local config="$CONFIG_DIR/$name.config.json"
  node "$ROOT_DIR/write-config.mjs" "$config" "$@"
  start_server "$name" "$main" --config "$config"
}

start_configured_server api-a "$PROVIDER_MAIN" \
  --rid api-a \
  --http-url "http://127.0.0.1:$PROVIDER_A_HTTP_PORT" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --channel-endpoint "$API_A" \
  --fanout-endpoint "$FANOUT_ENDPOINT" \
  --evidence-file "$LOG_DIR/api-a.evidence.log" \
  --log-dir "$LOG_DIR"
wait_health "http://127.0.0.1:$PROVIDER_A_HTTP_PORT" api-a

start_configured_server api-b "$PROVIDER_MAIN" \
  --rid api-b \
  --http-url "http://127.0.0.1:$PROVIDER_B_HTTP_PORT" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --channel-endpoint "$API_B" \
  --evidence-file "$LOG_DIR/api-b.evidence.log" \
  --log-dir "$LOG_DIR"
wait_health "http://127.0.0.1:$PROVIDER_B_HTTP_PORT" api-b

start_configured_server consumer "$CONSUMER_MAIN" \
  --rid consumer-1 \
  --http-url "http://127.0.0.1:$CONSUMER_HTTP_PORT" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --trace-label consumer \
  --evidence-file "$LOG_DIR/consumer-1.evidence.log" \
  --log-dir "$LOG_DIR"
wait_health "http://127.0.0.1:$CONSUMER_HTTP_PORT" consumer

CONSUMER_URLS=("http://127.0.0.1:$CONSUMER_HTTP_PORT")
for index in $(seq 2 "$RESILIENCE_CONSUMER_COUNT"); do
  subscriber_port="$(pick_port)"
  subscriber_url="http://127.0.0.1:$subscriber_port"
  start_configured_server "consumer-$index" "$CONSUMER_MAIN" \
    --rid "consumer-$index" \
    --http-url "$subscriber_url" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --trace-label "consumer-$index" \
    --evidence-file "$LOG_DIR/consumer-$index.evidence.log" \
    --log-dir "$LOG_DIR"
  wait_health "$subscriber_url" "consumer-$index"
  CONSUMER_URLS+=("$subscriber_url")
done
CONSUMER_URL_LIST="$(IFS=,; echo "${CONSUMER_URLS[*]}")"

CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
  --peer-location-url "http://127.0.0.1:$CONSUMER_HTTP_PORT" \
  --provider-a-url "http://127.0.0.1:$PROVIDER_A_HTTP_PORT" \
  --provider-b-url "http://127.0.0.1:$PROVIDER_B_HTTP_PORT" \
  --provider-b-remap-url "http://127.0.0.1:$PROVIDER_B_REMAP_HTTP_PORT" \
  --provider-b-green-url "http://127.0.0.1:$PROVIDER_B_GREEN_HTTP_PORT" \
  --consumer-url "http://127.0.0.1:$CONSUMER_HTTP_PORT" \
  --consumer-urls "$CONSUMER_URL_LIST" \
  --soak-duration-seconds "$RL_D5_DURATION_SECONDS" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --redis-container "$REDIS_CONTAINER_ID" \
  --provider-a-channel-endpoint "$API_A" \
  --provider-b-channel-endpoint "$API_B" \
  --provider-b-remap-channel-endpoint "$API_B_REMAP" \
  --provider-b-green-channel-endpoint "$API_B_GREEN" \
  --provider-main "$PROVIDER_MAIN" \
  --log-dir "$LOG_DIR" \
  --scenario "$SCENARIO"
node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"

cat "$LOG_DIR/client.stdout.log"
