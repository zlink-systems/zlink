#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NODE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
source "$NODE_ROOT/e2e/redis-container.sh"
source "$NODE_ROOT/e2e/runner-common.sh"

SCENARIO="${1:-all}"
CHILD_RUN="${2:-}"
C5_PHASE="${3:-sequential}"
if [[ "$CHILD_RUN" != "--child-run" && "$SCENARIO" == "all" ]]; then
  (cd "$NODE_ROOT" && npm run build >/dev/null)
  build_package "$ROOT_DIR/Server/Play"
  build_package "$ROOT_DIR/Server/Session"
  build_package "$ROOT_DIR/Server/Workflow"
  build_package "$ROOT_DIR/Client"
  for scenario in OBS-A1 OBS-A2 OBS-A3 OBS-A4 OBS-B1 OBS-B2 OBS-B3 OBS-B4 OBS-C1 OBS-C2 OBS-C3 OBS-C4; do
    OBSERVABILITY_OPS_SKIP_BUILD=1 "$0" "$scenario" --child-run
  done
  OBSERVABILITY_OPS_SKIP_BUILD=1 "$0" OBS-C5 --child-run sequential
  OBSERVABILITY_OPS_SKIP_BUILD=1 "$0" OBS-C5 --child-run simultaneous
  echo "observability-ops e2e result=passed"
  exit 0
fi

case "$SCENARIO" in
  OBS-A1|OBS-A2|OBS-A3|OBS-A4|OBS-B1|OBS-B2|OBS-B3|OBS-B4|OBS-C1|OBS-C2|OBS-C3|OBS-C4|OBS-C5) ;;
  *) echo "Unknown ObservabilityOps scenario '$SCENARIO'." >&2; exit 2 ;;
esac

RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/log/$RUN_ID"
CONFIG_DIR=""
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
ROUTE_SETTLE_TIMEOUT_SECONDS=5
SCENARIO_SETTLE_TIMEOUT_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
mkdir -p "$LOG_DIR"
echo "log_dir=$LOG_DIR"

pids=()
REDIS_CONTAINER_ID=""
cleanup() {
  local code=$?
  stop_live_pids
  wait_all_pids_ignoring_status
  remove_redis_container
  [[ -z "$CONFIG_DIR" ]] || rm -rf "$CONFIG_DIR"
  if [[ "$code" -ne 0 ]]; then tail_failure_logs; fi
}
trap cleanup EXIT
CONFIG_DIR="$(mktemp -d)"
chmod 700 "$CONFIG_DIR"

if [[ "${OBSERVABILITY_OPS_SKIP_BUILD:-0}" != "1" ]]; then
  (cd "$NODE_ROOT" && npm run build >/dev/null)
  build_package "$ROOT_DIR/Server/Play"
  build_package "$ROOT_DIR/Server/Session"
  build_package "$ROOT_DIR/Server/Workflow"
  build_package "$ROOT_DIR/Client"
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run ObservabilityOps." >&2
  exit 1
fi
start_redis_container "zlink-redis-node-observability-${RANDOM}-$$" -p "127.0.0.1::6379" "redis:7.2-alpine"
REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
wait_tcp redis "tcp://$REDIS_ENDPOINT" 100
REDIS_KEY_PREFIX="observability-ops:node:$RUN_ID:"

PLAY_A_URL="http://127.0.0.1:$(allocate_port)"
PLAY_B_URL="http://127.0.0.1:$(allocate_port)"
SESSION_URL="http://127.0.0.1:$(allocate_port)"
WORKFLOW_A_URL="http://127.0.0.1:$(allocate_port)"
WORKFLOW_B_URL="http://127.0.0.1:$(allocate_port)"
PLAY_A_ROUTER="tcp://127.0.0.1:$(allocate_port)"
PLAY_B_ROUTER="tcp://127.0.0.1:$(allocate_port)"
SESSION_ROUTER="tcp://127.0.0.1:$(allocate_port)"
WORKFLOW_A_ROUTER="tcp://127.0.0.1:$(allocate_port)"
WORKFLOW_B_ROUTER="tcp://127.0.0.1:$(allocate_port)"
PLAY_A_PUBSUB="tcp://127.0.0.1:$(allocate_port)"
PLAY_B_PUBSUB="tcp://127.0.0.1:$(allocate_port)"
SESSION_PUBSUB="tcp://127.0.0.1:$(allocate_port)"
WORKFLOW_A_PUBSUB="tcp://127.0.0.1:$(allocate_port)"
WORKFLOW_B_PUBSUB="tcp://127.0.0.1:$(allocate_port)"
WORKFLOW_A_FANOUT="tcp://127.0.0.1:$(allocate_port)"
WORKFLOW_B_FANOUT="tcp://127.0.0.1:$(allocate_port)"
SESSION_STREAM="ws://127.0.0.1:$(allocate_port)"
STATE_FILE="$LOG_DIR/workflow-state.json"

PLAY_MAIN="$ROOT_DIR/Server/Play/dist/Server/Play/main.js"
SESSION_MAIN="$ROOT_DIR/Server/Session/dist/Server/Session/main.js"
WORKFLOW_MAIN="$ROOT_DIR/Server/Workflow/dist/Server/Workflow/main.js"

write_role_config() {
  local name="$1" url="$2" router="$3" pubsub="$4"; shift 4
  node "$ROOT_DIR/write-config.mjs" "$CONFIG_DIR/$name.config.json" \
    --rid "$name" --http-url "$url" \
    --redis-endpoint "$REDIS_ENDPOINT" --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --router-endpoint "$router" --pubsub-endpoint "$pubsub" \
    --evidence-file "$LOG_DIR/$name.evidence.log" --log-dir "$LOG_DIR" "$@"
}

start_play() {
  local name="$1" url="$2" router="$3" pubsub="$4" metrics_enabled=true
  [[ "$SCENARIO" == "OBS-B4" && "$name" == "play-a" ]] && metrics_enabled=false
  write_role_config "$name" "$url" "$router" "$pubsub" --metrics-enabled "$metrics_enabled"
  start_server "$name" "$PLAY_MAIN" --config "$CONFIG_DIR/$name.config.json"
  wait_health "$url" "$name" "$LAST_STARTED_PID"
}

start_workflow() {
  local name="$1" url="$2" router="$3" pubsub="$4" fanout="$5"
  write_role_config "$name" "$url" "$router" "$pubsub" \
    --fanout-endpoint "$fanout" --state-file "$STATE_FILE"
  start_server "$name" "$WORKFLOW_MAIN" --config "$CONFIG_DIR/$name.config.json"
  wait_health "$url" "$name" "$LAST_STARTED_PID"
}

start_session() {
  local flow_enabled=true
  [[ "$SCENARIO" == "OBS-A3" ]] && flow_enabled=false
  write_role_config session-a "$SESSION_URL" "$SESSION_ROUTER" "$SESSION_PUBSUB" \
    --stream-endpoint "$SESSION_STREAM" --message-flow-enabled "$flow_enabled"
  start_server session-a "$SESSION_MAIN" --config "$CONFIG_DIR/session-a.config.json"
  wait_health "$SESSION_URL" session-a "$LAST_STARTED_PID"
  wait_tcp session-stream "$SESSION_STREAM" 100
}

start_play play-b "$PLAY_B_URL" "$PLAY_B_ROUTER" "$PLAY_B_PUBSUB"
start_play play-a "$PLAY_A_URL" "$PLAY_A_ROUTER" "$PLAY_A_PUBSUB"
start_workflow workflow-b "$WORKFLOW_B_URL" "$WORKFLOW_B_ROUTER" "$WORKFLOW_B_PUBSUB" "$WORKFLOW_B_FANOUT"
start_workflow workflow-a "$WORKFLOW_A_URL" "$WORKFLOW_A_ROUTER" "$WORKFLOW_A_PUBSUB" "$WORKFLOW_A_FANOUT"
start_session

node "$NODE_ROOT/e2e/location-readiness.js" \
  --redis-endpoint "$REDIS_ENDPOINT" --key-prefix "$REDIS_KEY_PREFIX" \
  --timeout-ms 5000 --interval-ms 100 \
  --peer-http route-mesh observability.play router "$PLAY_A_URL" \
    "$PLAY_A_ROUTER" \
  --peer-http route-mesh observability.play router "$PLAY_B_URL" \
    "$PLAY_B_ROUTER" \
  --peer-http route-mesh observability.play router "$SESSION_URL" \
    "$SESSION_ROUTER"
node "$NODE_ROOT/e2e/location-readiness.js" \
  --redis-endpoint "$REDIS_ENDPOINT" --key-prefix "$REDIS_KEY_PREFIX" \
  --timeout-ms 5000 --interval-ms 100 \
  --peer-http route-mesh observability.workflow router "$WORKFLOW_A_URL" \
    "$WORKFLOW_A_ROUTER" \
  --peer-http route-mesh observability.workflow router "$WORKFLOW_B_URL" \
    "$WORKFLOW_B_ROUTER"

if [[ "$SCENARIO" == "OBS-B3" ]]; then
  docker exec "$REDIS_CONTAINER_ID" redis-cli CLIENT PAUSE 1800 ALL >/dev/null
  sleep 3
fi

CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
  --node-a-url "$PLAY_A_URL" --node-b-url "$PLAY_B_URL" \
  --session-a-stream-endpoint "$SESSION_STREAM" --session-b-stream-endpoint "$SESSION_STREAM" \
  --session-url "$SESSION_URL" --workflow-a-url "$WORKFLOW_A_URL" --workflow-b-url "$WORKFLOW_B_URL" \
  --log-dir "$LOG_DIR" --scenario "$SCENARIO" --c5-phase "$C5_PHASE"
node "$NODE_ROOT/scripts/browser-e2e/run-e2e-client.mjs" "$ROOT_DIR/Client/main.ts" -- \
  --config "$CLIENT_CONFIG" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"

cat "$LOG_DIR/client.stdout.log"
