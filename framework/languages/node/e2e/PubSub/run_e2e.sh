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

start_configured_server() {
  local name="$1"; local main="$2"; shift 2
  local config="$CONFIG_DIR/$name.config.json"
  node "$ROOT_DIR/write-config.mjs" "$config" "$@"
  start_server "$name" "$main" --config "$config"
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
build_package "$ROOT_DIR/Server/Publisher"
build_package "$ROOT_DIR/Server/Subscriber"
build_package "$ROOT_DIR/Client"

PUB_HTTP_PORT="$(pick_port)"
SUB_1_HTTP_PORT="$(pick_port)"
SUB_2_HTTP_PORT="$(pick_port)"
SUB_3_HTTP_PORT="$(pick_port)"
SUB_LATE_HTTP_PORT="$(pick_port)"
PUB_PORT="$(pick_port)"

PUB_ENDPOINT="tcp://127.0.0.1:$PUB_PORT"
PUB_URL="http://127.0.0.1:$PUB_HTTP_PORT"
SUB_1_URL="http://127.0.0.1:$SUB_1_HTTP_PORT"
SUB_2_URL="http://127.0.0.1:$SUB_2_HTTP_PORT"
SUB_3_URL="http://127.0.0.1:$SUB_3_HTTP_PORT"
SUB_LATE_URL="http://127.0.0.1:$SUB_LATE_HTTP_PORT"

PUBLISHER_MAIN="$ROOT_DIR/Server/Publisher/dist/Server/Publisher/main.js"
SUBSCRIBER_MAIN="$ROOT_DIR/Server/Subscriber/dist/Server/Subscriber/main.js"
CLIENT_MAIN="$ROOT_DIR/Client/dist/PubSub/Client/main.js"

if [[ "${SCENARIO^^}" == "PS-E2C" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required to run PS-E2C because it verifies a Store-backed publisher." >&2
    exit 1
  fi
  start_redis_container "zlink-redis-node-pubsub-${RANDOM}-$$" \
    -p "127.0.0.1::6379" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
  REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
  REDIS_KEY_PREFIX="pubsub:node:$RUN_ID"
  wait_tcp redis "tcp://$REDIS_ENDPOINT"

  E2C_MISSING_HTTP_PORT="$(pick_port)"
  E2C_BOTH_HTTP_PORT="$(pick_port)"
  E2C_MISSING_PORT="$(pick_port)"
  E2C_BOTH_PORT="$(pick_port)"
  E2C_MISSING_URL="http://127.0.0.1:$E2C_MISSING_HTTP_PORT"
  E2C_BOTH_URL="http://127.0.0.1:$E2C_BOTH_HTTP_PORT"
  E2C_MISSING_ENDPOINT="tcp://127.0.0.1:$E2C_MISSING_PORT"
  E2C_BOTH_ENDPOINT="tcp://127.0.0.1:$E2C_BOTH_PORT"
  CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
  node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
    --publisher-url "$E2C_MISSING_URL" \
    --subscriber-url "$E2C_BOTH_URL" \
    --late-subscriber-url "$E2C_BOTH_URL" \
    --publisher-endpoint "$E2C_MISSING_ENDPOINT" \
    --publisher-main "$PUBLISHER_MAIN" \
    --subscriber-main "$SUBSCRIBER_MAIN" \
    --log-dir "$LOG_DIR" \
    --scenario "$SCENARIO" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --publisher-identity-missing-endpoint "$E2C_MISSING_ENDPOINT" \
    --publisher-identity-both-endpoint "$E2C_BOTH_ENDPOINT"
  node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
    >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
  cat "$LOG_DIR/client.stdout.log"
  exit 0
fi

if [[ "${SCENARIO^^}" == "PS-D1" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required to run PS-D1 because it verifies automatic discovery through a Store." >&2
    exit 1
  fi
  start_redis_container "zlink-redis-node-pubsub-${RANDOM}-$$" \
    -p "127.0.0.1::6379" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
  REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
  REDIS_KEY_PREFIX="pubsub:node:$RUN_ID"
  wait_tcp redis "tcp://$REDIS_ENDPOINT"

  D1_PUB_HTTP_PORT="$(pick_port)"
  D1_SUB_HTTP_PORT="$(pick_port)"
  D1_PUB_ENDPOINT="tcp://127.0.0.1:0"
  D1_PUB_URL="http://127.0.0.1:$D1_PUB_HTTP_PORT"
  D1_SUB_URL="http://127.0.0.1:$D1_SUB_HTTP_PORT"

  start_configured_server pub-d1 "$PUBLISHER_MAIN" \
    --rid pub-d1 \
    --http-url "$D1_PUB_URL" \
    --publisher-endpoint "$D1_PUB_ENDPOINT" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --evidence-file "$LOG_DIR/pub-d1.evidence.log" \
    --log-dir "$LOG_DIR"
  wait_health "$D1_PUB_URL" pub-d1

  start_configured_server sub-d1 "$SUBSCRIBER_MAIN" \
    --rid sub-d1 \
    --http-url "$D1_SUB_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --evidence-file "$LOG_DIR/sub-d1.evidence.log" \
    --log-dir "$LOG_DIR"
  wait_health "$D1_SUB_URL" sub-d1

  CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
  node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
    --publisher-url "$D1_PUB_URL" \
    --subscriber-url "$D1_SUB_URL" \
    --late-subscriber-url "$D1_SUB_URL" \
    --publisher-endpoint "$D1_PUB_ENDPOINT" \
    --publisher-main "$PUBLISHER_MAIN" \
    --subscriber-main "$SUBSCRIBER_MAIN" \
    --log-dir "$LOG_DIR" \
    --scenario "$SCENARIO" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX"
  node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
    >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
  cat "$LOG_DIR/client.stdout.log"
  exit 0
fi

if [[ "${SCENARIO^^}" == "PS-D2" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required to run PS-D2 because it verifies ChannelName-filtered discovery through a Store." >&2
    exit 1
  fi
  start_redis_container "zlink-redis-node-pubsub-${RANDOM}-$$" \
    -p "127.0.0.1::6379" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
  REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
  REDIS_KEY_PREFIX="pubsub:node:$RUN_ID"
  wait_tcp redis "tcp://$REDIS_ENDPOINT"

  D2_EVENTS_HTTP_PORT="$(pick_port)"
  D2_AUDIT_HTTP_PORT="$(pick_port)"
  D2_SUB_HTTP_PORT="$(pick_port)"
  D2_EVENTS_URL="http://127.0.0.1:$D2_EVENTS_HTTP_PORT"
  D2_AUDIT_URL="http://127.0.0.1:$D2_AUDIT_HTTP_PORT"
  D2_SUB_URL="http://127.0.0.1:$D2_SUB_HTTP_PORT"

  start_configured_server pub-events "$PUBLISHER_MAIN" \
    --rid pub-events \
    --http-url "$D2_EVENTS_URL" \
    --publisher-endpoint "tcp://127.0.0.1:0" \
    --channel-name events \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --evidence-file "$LOG_DIR/pub-events.evidence.log" \
    --log-dir "$LOG_DIR"
  wait_health "$D2_EVENTS_URL" pub-events

  start_configured_server pub-audit "$PUBLISHER_MAIN" \
    --rid pub-audit \
    --http-url "$D2_AUDIT_URL" \
    --publisher-endpoint "tcp://127.0.0.1:0" \
    --channel-name audit \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --evidence-file "$LOG_DIR/pub-audit.evidence.log" \
    --log-dir "$LOG_DIR"
  wait_health "$D2_AUDIT_URL" pub-audit

  start_configured_server sub-d2 "$SUBSCRIBER_MAIN" \
    --rid sub-d2 \
    --http-url "$D2_SUB_URL" \
    --channel-name events \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --evidence-file "$LOG_DIR/sub-d2.evidence.log" \
    --log-dir "$LOG_DIR"
  wait_health "$D2_SUB_URL" sub-d2

  CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
  node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
    --publisher-url "$D2_EVENTS_URL" \
    --secondary-publisher-url "$D2_AUDIT_URL" \
    --subscriber-url "$D2_SUB_URL" \
    --late-subscriber-url "$D2_SUB_URL" \
    --publisher-endpoint "tcp://127.0.0.1:0" \
    --publisher-main "$PUBLISHER_MAIN" \
    --subscriber-main "$SUBSCRIBER_MAIN" \
    --log-dir "$LOG_DIR" \
    --scenario "$SCENARIO" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX"
  node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
    >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
  cat "$LOG_DIR/client.stdout.log"
  exit 0
fi

if [[ "${SCENARIO^^}" == "PS-D4" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required to run PS-D4 because it verifies Store-backed crash replacement." >&2
    exit 1
  fi
  start_redis_container "zlink-redis-node-pubsub-${RANDOM}-$$" \
    -p "127.0.0.1::6379" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
  REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
  REDIS_KEY_PREFIX="pubsub:node:$RUN_ID"
  wait_tcp redis "tcp://$REDIS_ENDPOINT"

  D4_PUB_HTTP_PORT="$(pick_port)"
  D4_REPLACEMENT_HTTP_PORT="$(pick_port)"
  D4_SUB_HTTP_PORT="$(pick_port)"
  D4_PUB_URL="http://127.0.0.1:$D4_PUB_HTTP_PORT"
  D4_REPLACEMENT_URL="http://127.0.0.1:$D4_REPLACEMENT_HTTP_PORT"
  D4_SUB_URL="http://127.0.0.1:$D4_SUB_HTTP_PORT"

  CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
  node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
    --publisher-url "$D4_PUB_URL" \
    --secondary-publisher-url "$D4_REPLACEMENT_URL" \
    --subscriber-url "$D4_SUB_URL" \
    --late-subscriber-url "$D4_SUB_URL" \
    --publisher-endpoint "tcp://127.0.0.1:0" \
    --publisher-main "$PUBLISHER_MAIN" \
    --subscriber-main "$SUBSCRIBER_MAIN" \
    --log-dir "$LOG_DIR" \
    --scenario "$SCENARIO" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX"
  node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
    >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
  cat "$LOG_DIR/client.stdout.log"
  exit 0
fi

if [[ "${SCENARIO^^}" == "PS-D3" || "${SCENARIO^^}" == "PS-F4" || "${SCENARIO^^}" == "PS-D7A" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required to run PS-D3 because it verifies Store-backed publisher set convergence." >&2
    exit 1
  fi
  start_redis_container "zlink-redis-node-pubsub-${RANDOM}-$$" \
    -p "127.0.0.1::6379" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
  REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
  REDIS_KEY_PREFIX="pubsub:node:$RUN_ID"
  wait_tcp redis "tcp://$REDIS_ENDPOINT"

  D3_PUB_A_HTTP_PORT="$(pick_port)"
  D3_PUB_B_HTTP_PORT="$(pick_port)"
  D3_SUB_HTTP_PORT="$(pick_port)"
  D3_PUB_A_URL="http://127.0.0.1:$D3_PUB_A_HTTP_PORT"
  D3_PUB_B_URL="http://127.0.0.1:$D3_PUB_B_HTTP_PORT"
  D3_SUB_URL="http://127.0.0.1:$D3_SUB_HTTP_PORT"

  start_configured_server pub-a "$PUBLISHER_MAIN" \
    --rid pub-a \
    --http-url "$D3_PUB_A_URL" \
    --publisher-endpoint "tcp://127.0.0.1:0" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --evidence-file "$LOG_DIR/pub-a.evidence.log" \
    --log-dir "$LOG_DIR"
  wait_health "$D3_PUB_A_URL" pub-a

  start_configured_server sub-d3 "$SUBSCRIBER_MAIN" \
    --rid sub-d3 \
    --http-url "$D3_SUB_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --evidence-file "$LOG_DIR/sub-d3.evidence.log" \
    --log-dir "$LOG_DIR"
  wait_health "$D3_SUB_URL" sub-d3

  CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
  node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
    --publisher-url "$D3_PUB_A_URL" \
    --secondary-publisher-url "$D3_PUB_B_URL" \
    --subscriber-url "$D3_SUB_URL" \
    --late-subscriber-url "$D3_SUB_URL" \
    --publisher-endpoint "tcp://127.0.0.1:0" \
    --publisher-main "$PUBLISHER_MAIN" \
    --subscriber-main "$SUBSCRIBER_MAIN" \
    --log-dir "$LOG_DIR" \
    --scenario "$SCENARIO" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX"
  node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
    >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
  cat "$LOG_DIR/client.stdout.log"
  exit 0
fi

if [[ "${SCENARIO^^}" == "PS-D5" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required to run PS-D5 because it verifies Store failure recovery." >&2
    exit 1
  fi
  start_redis_container "zlink-redis-node-pubsub-${RANDOM}-$$" \
    -p "127.0.0.1::6379" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
  REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
  REDIS_KEY_PREFIX="pubsub:node:$RUN_ID"
  wait_tcp redis "tcp://$REDIS_ENDPOINT"

  D5_PUB_HTTP_PORT="$(pick_port)"
  D5_SUB_HTTP_PORT="$(pick_port)"
  D5_PUB_URL="http://127.0.0.1:$D5_PUB_HTTP_PORT"
  D5_SUB_URL="http://127.0.0.1:$D5_SUB_HTTP_PORT"

  CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
  node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
    --publisher-url "$D5_PUB_URL" \
    --subscriber-url "$D5_SUB_URL" \
    --late-subscriber-url "$D5_SUB_URL" \
    --publisher-endpoint "tcp://127.0.0.1:0" \
    --publisher-main "$PUBLISHER_MAIN" \
    --subscriber-main "$SUBSCRIBER_MAIN" \
    --log-dir "$LOG_DIR" \
    --scenario "$SCENARIO" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX"
  node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
    >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
  cat "$LOG_DIR/client.stdout.log"
  exit 0
fi

if [[ "${SCENARIO^^}" == "PS-D6" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required to run PS-D6 because it verifies automatic discovery through a Store." >&2
    exit 1
  fi
  start_redis_container "zlink-redis-node-pubsub-${RANDOM}-$$" \
    -p "127.0.0.1::6379" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
  REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
  REDIS_KEY_PREFIX="pubsub:node:$RUN_ID"
  wait_tcp redis "tcp://$REDIS_ENDPOINT"

  D6_PUB_HTTP_PORT="$(pick_port)"
  D6_SUB_HTTP_PORT="$(pick_port)"
  D6_PUB_URL="http://127.0.0.1:$D6_PUB_HTTP_PORT"
  D6_SUB_URL="http://127.0.0.1:$D6_SUB_HTTP_PORT"

  CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
  node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
    --publisher-url "$D6_PUB_URL" \
    --subscriber-url "$D6_SUB_URL" \
    --late-subscriber-url "$D6_SUB_URL" \
    --publisher-endpoint "tcp://127.0.0.1:0" \
    --publisher-main "$PUBLISHER_MAIN" \
    --subscriber-main "$SUBSCRIBER_MAIN" \
    --log-dir "$LOG_DIR" \
    --scenario "$SCENARIO" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX"
  node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
    >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
  cat "$LOG_DIR/client.stdout.log"
  exit 0
fi

if [[ "${SCENARIO^^}" == "PS-F5" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required to run PS-F5 because it verifies liveness during unhandled traffic." >&2
    exit 1
  fi
  start_redis_container "zlink-redis-node-pubsub-${RANDOM}-$$" \
    -p "127.0.0.1::6379" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
  REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
  REDIS_KEY_PREFIX="pubsub:node:$RUN_ID"
  wait_tcp redis "tcp://$REDIS_ENDPOINT"

  F5_PUB_HTTP_PORT="$(pick_port)"
  F5_SUB_HTTP_PORT="$(pick_port)"
  F5_PUB_URL="http://127.0.0.1:$F5_PUB_HTTP_PORT"
  F5_SUB_URL="http://127.0.0.1:$F5_SUB_HTTP_PORT"

  start_configured_server pub-f5 "$PUBLISHER_MAIN" \
    --rid pub-f5 \
    --http-url "$F5_PUB_URL" \
    --publisher-endpoint "tcp://127.0.0.1:0" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --evidence-file "$LOG_DIR/pub-f5.evidence.log" \
    --log-dir "$LOG_DIR"
  wait_health "$F5_PUB_URL" pub-f5

  start_configured_server sub-f5 "$SUBSCRIBER_MAIN" \
    --rid sub-f5 \
    --http-url "$F5_SUB_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --evidence-file "$LOG_DIR/sub-f5.evidence.log" \
    --log-dir "$LOG_DIR"
  wait_health "$F5_SUB_URL" sub-f5

  CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
  node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
    --publisher-url "$F5_PUB_URL" \
    --subscriber-url "$F5_SUB_URL" \
    --late-subscriber-url "$F5_SUB_URL" \
    --publisher-endpoint "tcp://127.0.0.1:0" \
    --publisher-main "$PUBLISHER_MAIN" \
    --subscriber-main "$SUBSCRIBER_MAIN" \
    --log-dir "$LOG_DIR" \
    --scenario "$SCENARIO" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX"
  node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
    >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
  cat "$LOG_DIR/client.stdout.log"
  exit 0
fi

if [[ "${SCENARIO^^}" == "PS-F2" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required to run PS-F2 because it verifies publisher-specific liveness isolation." >&2
    exit 1
  fi
  start_redis_container "zlink-redis-node-pubsub-${RANDOM}-$$" \
    -p "127.0.0.1::6379" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
  REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
  REDIS_KEY_PREFIX="pubsub:node:$RUN_ID"
  wait_tcp redis "tcp://$REDIS_ENDPOINT"

  F2_PUB_A_HTTP_PORT="$(pick_port)"
  F2_PUB_B_HTTP_PORT="$(pick_port)"
  F2_SUB_HTTP_PORT="$(pick_port)"
  F2_PUBLISHER_PROXY_PORT="$(pick_port)"
  F2_PUB_A_URL="http://127.0.0.1:$F2_PUB_A_HTTP_PORT"
  F2_PUB_B_URL="http://127.0.0.1:$F2_PUB_B_HTTP_PORT"
  F2_SUB_URL="http://127.0.0.1:$F2_SUB_HTTP_PORT"

  CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
  node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
    --publisher-url "$F2_PUB_A_URL" \
    --secondary-publisher-url "$F2_PUB_B_URL" \
    --subscriber-url "$F2_SUB_URL" \
    --late-subscriber-url "$F2_SUB_URL" \
    --publisher-endpoint "tcp://127.0.0.1:0" \
    --publisher-proxy-port "$F2_PUBLISHER_PROXY_PORT" \
    --publisher-main "$PUBLISHER_MAIN" \
    --subscriber-main "$SUBSCRIBER_MAIN" \
    --log-dir "$LOG_DIR" \
    --scenario "$SCENARIO" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX"
  node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
    >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
  cat "$LOG_DIR/client.stdout.log"
  exit 0
fi

start_configured_server pub-a "$PUBLISHER_MAIN" \
  --rid pub-a \
  --http-url "$PUB_URL" \
  --publisher-endpoint "$PUB_ENDPOINT" \
  --evidence-file "$LOG_DIR/pub-a.evidence.log" \
  --log-dir "$LOG_DIR"
wait_health "$PUB_URL" pub-a

for sub in 1 2 3; do
  url_var="SUB_${sub}_URL"
  extra_args=()
  if [[ "$sub" == "3" ]]; then
    extra_args+=(--handler-delay-ms 3000)
  fi
  start_configured_server "sub-$sub" "$SUBSCRIBER_MAIN" \
    --rid "sub-$sub" \
    --http-url "${!url_var}" \
    --publisher-endpoint "$PUB_ENDPOINT" \
    --evidence-file "$LOG_DIR/sub-$sub.evidence.log" \
    --log-dir "$LOG_DIR" \
    "${extra_args[@]}"
  wait_health "${!url_var}" "sub-$sub"
done

CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
  --publisher-url "$PUB_URL" \
  --subscriber-url "$SUB_1_URL" \
  --subscriber-url "$SUB_2_URL" \
  --subscriber-url "$SUB_3_URL" \
  --late-subscriber-url "$SUB_LATE_URL" \
  --publisher-endpoint "$PUB_ENDPOINT" \
  --publisher-main "$PUBLISHER_MAIN" \
  --subscriber-main "$SUBSCRIBER_MAIN" \
  --log-dir "$LOG_DIR" \
  --scenario "$SCENARIO"
node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"

cat "$LOG_DIR/client.stdout.log"
