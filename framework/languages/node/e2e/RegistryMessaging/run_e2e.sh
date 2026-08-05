#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NODE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
source "$NODE_ROOT/e2e/redis-container.sh"
source "$NODE_ROOT/e2e/runner-common.sh"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/log/$RUN_ID"
CONFIG_DIR=""
SCENARIO="${1:-all}"
E2E_START_ORDER="${E2E_START_ORDER:-forward}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
ROUTE_SETTLE_TIMEOUT_SECONDS=5
SCENARIO_SETTLE_TIMEOUT_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
mkdir -p "$LOG_DIR"

pids=()
REDIS_CONTAINER_ID=""
cleanup() {
  local code=$?
  stop_live_pids
  wait_all_pids_ignoring_status
  remove_redis_container
  [[ -z "$CONFIG_DIR" ]] || rm -rf "$CONFIG_DIR"
  if [[ "$code" -ne 0 ]]; then
    tail_failure_logs
  fi
}
trap cleanup EXIT
CONFIG_DIR="$(mktemp -d)"
chmod 700 "$CONFIG_DIR"

echo "log_dir=$LOG_DIR"
echo "start_order=$E2E_START_ORDER"

(cd "$NODE_ROOT" && npm run build >/dev/null)
if [[ "$SCENARIO" == "RM-A3" || "$SCENARIO" == "rm-a3" ]]; then
  build_package "$ROOT_DIR/Server/ObjectClient"
  build_package "$ROOT_DIR/Client"
else
  build_package "$ROOT_DIR/Server/Provider"
  build_package "$ROOT_DIR/Server/Workflow"
  build_package "$ROOT_DIR/Server/Consumer"
  build_package "$ROOT_DIR/Client"
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run RegistryMessaging because it provisions a dedicated Redis location store." >&2
  exit 1
fi

start_redis_container "zlink-redis-node-e2e-${RANDOM}-$$" -p "127.0.0.1::6379" "redis:7.2-alpine"
REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
REDIS_KEY_PREFIX="location-messaging:node:$RUN_ID"
wait_tcp redis "tcp://$REDIS_ENDPOINT"

PROVIDER_A_HTTP_PORT="$(pick_port)"
PROVIDER_B_HTTP_PORT="$(pick_port)"
WORKFLOW_HTTP_PORT="$(pick_port)"
CONSUMER_HTTP_PORT="$(pick_port)"
SINGLE_CONSUMER_HTTP_PORT="$(pick_port)"
BACKPRESSURE_CONSUMER_HTTP_PORT="$(pick_port)"
LOCATION_CONSUMER_HTTP_PORT="$(pick_port)"
MANUAL_CONSUMER_HTTP_PORT="$(pick_port)"
API_A_PORT="$(pick_port)"
API_B_PORT="$(pick_port)"
WORKFLOW_PORT="$(pick_port)"
ROUTE_A_PORT="$(pick_port)"
ROUTE_B_PORT="$(pick_port)"
RM_A3_ROUTE_A_PORT="$(pick_port)"
RM_A3_ROUTE_B_PORT="$(pick_port)"
RM_A3_HTTP_A_PORT="$(pick_port)"
RM_A3_HTTP_B_PORT="$(pick_port)"
RM_A3_PROXY_A_PORT="$(pick_port)"
RM_A3_PROXY_B_PORT="$(pick_port)"

API_A="tcp://127.0.0.1:$API_A_PORT"
API_B="tcp://127.0.0.1:$API_B_PORT"
WORKFLOW="tcp://127.0.0.1:$WORKFLOW_PORT"
ROUTE_A="tcp://127.0.0.1:$ROUTE_A_PORT"
ROUTE_B="tcp://127.0.0.1:$ROUTE_B_PORT"
RM_A3_ROUTE_A="tcp://127.0.0.1:$RM_A3_ROUTE_A_PORT"
RM_A3_ROUTE_B="tcp://127.0.0.1:$RM_A3_ROUTE_B_PORT"
RM_A3_HTTP_A="http://127.0.0.1:$RM_A3_HTTP_A_PORT"
RM_A3_HTTP_B="http://127.0.0.1:$RM_A3_HTTP_B_PORT"
RM_A3_PROXY_A="tcp://127.0.0.1:$RM_A3_PROXY_A_PORT"
RM_A3_PROXY_B="tcp://127.0.0.1:$RM_A3_PROXY_B_PORT"
MANUAL_CONSUMER_HTTP="http://127.0.0.1:$MANUAL_CONSUMER_HTTP_PORT"

PROVIDER_MAIN="$ROOT_DIR/Server/Provider/dist/Server/Provider/main.js"
WORKFLOW_MAIN="$ROOT_DIR/Server/Workflow/dist/Server/Workflow/main.js"
CONSUMER_MAIN="$ROOT_DIR/Server/Consumer/dist/Server/Consumer/main.js"
OBJECT_CLIENT_MAIN="$ROOT_DIR/Server/ObjectClient/dist/Server/ObjectClient/main.js"
CLIENT_MAIN="$ROOT_DIR/Client/dist/RegistryMessaging/Client/main.js"

start_configured_server() {
  local name="$1"; local main="$2"; shift 2
  local config="$CONFIG_DIR/$name.config.json"
  node "$ROOT_DIR/write-config.mjs" "$config" "$@"
  start_server "$name" "$main" --config "$config"
}

start_rm_a3_object_client() {
  local name="$1"
  local route_endpoint="$2"
  local http_url="$3"
  local peer_endpoint="${4:-}"
  local peer_rid="${5:-}"
  local server_weight="${6:-}"
  local config="$CONFIG_DIR/$name.config.json"
  local args=(
    --rid "$name"
    --http-url "$http_url"
    --log-dir "$LOG_DIR"
    --redis-endpoint "$REDIS_ENDPOINT"
    --redis-key-prefix "$REDIS_KEY_PREFIX"
    --route-endpoint "$route_endpoint"
  )
  if [[ -n "$peer_endpoint" ]]; then
    args+=(--route-peer "$peer_endpoint" --route-peer-rid "$peer_rid")
  fi
  if [[ -n "$server_weight" ]]; then
    args+=(--server-weight "$server_weight")
  fi
  node "$ROOT_DIR/write-config.mjs" "$config" "${args[@]}"
  start_server "$name" "$OBJECT_CLIENT_MAIN" --config "$config"
  RM_A3_LAST_PID="$LAST_STARTED_PID"
}

stop_rm_a3_process() {
  local pid="$1"
  local http_url="${2:-}"
  if ! kill -0 "$pid" >/dev/null 2>&1; then
    wait "$pid" >/dev/null 2>&1 || true
    return
  fi
  if [[ -n "$http_url" ]]; then
    curl --max-time "$HTTP_PROBE_TIMEOUT_SECONDS" -fsS \
      -X POST "$http_url/shutdown" >/dev/null 2>&1 || true
  else
    kill "$pid" >/dev/null 2>&1 || true
  fi
  for _ in $(seq 1 50); do
    if ! kill -0 "$pid" >/dev/null 2>&1; then
      wait "$pid" >/dev/null 2>&1 || true
      return
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  kill -9 "$pid" >/dev/null 2>&1 || true
  wait "$pid" >/dev/null 2>&1 || true
}

start_rm_a3_proxy() {
  local name="$1"
  local listen_endpoint="$2"
  local upstream_endpoint="$3"
  local ready="$LOG_DIR/$name.ready"
  local evidence="$LOG_DIR/$name.connections.log"
  node "$ROOT_DIR/Support/tcp-connection-proxy.mjs" \
    --listen-port "${listen_endpoint##*:}" \
    --upstream-port "${upstream_endpoint##*:}" \
    --evidence "$evidence" \
    --ready "$ready" \
    >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  RM_A3_LAST_PID="$!"
  pids+=("$RM_A3_LAST_PID")
  wait_file_contains "$ready" ready "RM-A3 proxy '$name' did not start" "$RM_A3_LAST_PID"
}

run_rm_a3_client() {
  local suffix="$1"
  local expected_state="$2"
  local expected_ready="$3"
  local stable_milliseconds="$4"
  local check_node_direct="$5"
  local expected_server_weight="${6:-}"
  local config="$CONFIG_DIR/rm-a3-$suffix.config.json"
  local args=(
    --provider-a-url "$RM_A3_HTTP_A"
    --provider-b-url "$RM_A3_HTTP_B"
    --workflow-url "$RM_A3_HTTP_A"
    --direct-consumer-url "$RM_A3_HTTP_A"
    --single-consumer-url "$RM_A3_HTTP_A"
    --backpressure-consumer-url "$RM_A3_HTTP_A"
    --location-consumer-url "$RM_A3_HTTP_A"
    --provider-main "$OBJECT_CLIENT_MAIN"
    --consumer-main "$OBJECT_CLIENT_MAIN"
    --redis-endpoint "$REDIS_ENDPOINT"
    --redis-key-prefix "$REDIS_KEY_PREFIX"
    --log-dir "$LOG_DIR"
    --scenario RM-A3
    --rm-a3-client-a-url "$RM_A3_HTTP_A"
    --rm-a3-client-b-url "$RM_A3_HTTP_B"
    --rm-a3-expected-state "$expected_state"
    --rm-a3-expected-ready "$expected_ready"
    --rm-a3-stable-milliseconds "$stable_milliseconds"
    --rm-a3-check-node-direct "$check_node_direct"
  )
  if [[ -n "$expected_server_weight" ]]; then
    args+=(--rm-a3-expected-server-weight "$expected_server_weight")
  fi
  node "$ROOT_DIR/write-config.mjs" "$config" "${args[@]}"
  node "$CLIENT_MAIN" --config "$config" \
    >"$LOG_DIR/client-rm-a3-$suffix.stdout.log" \
    2>"$LOG_DIR/client-rm-a3-$suffix.stderr.log"
  cat "$LOG_DIR/client-rm-a3-$suffix.stdout.log"
}

wait_rm_a3_peer_unavailable() {
  local base_url="$1"
  local peer_rid="$2"
  node - "$base_url" "$peer_rid" <<'NODE'
const [baseUrl, peerRid] = process.argv.slice(2);
(async () => {
  let last;
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const response = await fetch(`${baseUrl}/rm-a3/status`);
    if (!response.ok) throw new Error(`status request failed: ${response.status}`);
    last = await response.json();
    const peer = last.peers.find((candidate) => candidate.rid === peerRid);
    if (peer?.state === 'not_connected'
        && peer.ready === false
        && last.readyPeerCount === 0) {
      console.log(`rm-a3 peer=${peerRid} state=not_connected ready=false`);
      return;
    }
    if (peer === undefined && last.readyPeerCount === 0
        && last.channels.every((channel) => channel.readyTargetCount === 0)) {
      // Automatic discovery may remove the peer row after its owner lease
      // expires. In that terminal state the stale peer is no longer a target.
      console.log(`rm-a3 peer=${peerRid} state=removed ready=false`);
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`peer ${peerRid} did not become unavailable: ${JSON.stringify(last)}`);
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
NODE
}

verify_rm_a3_single_manual_attempt() {
  local evidence="$1"
  local count
  count="$(wc -l <"$evidence")"
  # A RouteMesh request/reply peer uses one Application lane and one
  # Completion lane. Two TCP accepts therefore represent one logical manual
  # connection; a second logical connection would produce four accepts.
  if [[ "$count" != "2" ]]; then
    echo "RM-A3 manual endpoint opened $count physical lanes; expected 2 for one logical connection: $evidence" >&2
    return 1
  fi
  echo "rm-a3 manual-connect-attempts=1 physical-lanes=2 evidence=$evidence"
}

start_role() {
  case "$1" in
    api-a)
      start_configured_server api-a "$PROVIDER_MAIN" \
        --rid api-a --http-url "http://127.0.0.1:$PROVIDER_A_HTTP_PORT" \
        --redis-endpoint "$REDIS_ENDPOINT" --redis-key-prefix "$REDIS_KEY_PREFIX" \
        --channel-endpoint "$API_A" \
        --route-endpoint "$ROUTE_A" --route-peer "$ROUTE_B" \
        --evidence-file "$LOG_DIR/api-a.evidence.log" --log-dir "$LOG_DIR"
      ;;
    api-b)
      start_configured_server api-b "$PROVIDER_MAIN" \
        --rid api-b --http-url "http://127.0.0.1:$PROVIDER_B_HTTP_PORT" \
        --redis-endpoint "$REDIS_ENDPOINT" --redis-key-prefix "$REDIS_KEY_PREFIX" \
        --channel-endpoint "$API_B" \
        --route-endpoint "$ROUTE_B" --route-peer "$ROUTE_A" \
        --evidence-file "$LOG_DIR/api-b.evidence.log" --log-dir "$LOG_DIR"
      ;;
    workflow-a)
      start_configured_server workflow-a "$WORKFLOW_MAIN" \
        --rid workflow-a --http-url "http://127.0.0.1:$WORKFLOW_HTTP_PORT" \
        --redis-endpoint "$REDIS_ENDPOINT" --redis-key-prefix "$REDIS_KEY_PREFIX" \
        --workflow-endpoint "$WORKFLOW" \
        --evidence-file "$LOG_DIR/workflow-a.evidence.log" --log-dir "$LOG_DIR"
      ;;
    direct-consumer)
      start_configured_server direct-consumer "$CONSUMER_MAIN" \
        --http-url "http://127.0.0.1:$CONSUMER_HTTP_PORT" \
        --provider-endpoint "$API_A" --provider-endpoint "$API_B" \
        --trace-label direct-consumer --log-dir "$LOG_DIR"
      ;;
    single-consumer)
      start_configured_server single-consumer "$CONSUMER_MAIN" \
        --http-url "http://127.0.0.1:$SINGLE_CONSUMER_HTTP_PORT" \
        --provider-endpoint "$API_A" --trace-label single-consumer --log-dir "$LOG_DIR"
      ;;
    backpressure-consumer)
      start_configured_server backpressure-consumer "$CONSUMER_MAIN" \
        --http-url "http://127.0.0.1:$BACKPRESSURE_CONSUMER_HTTP_PORT" \
        --provider-endpoint "$API_A" --trace-label backpressure-consumer --log-dir "$LOG_DIR"
      ;;
    location-consumer)
      start_configured_server location-consumer "$CONSUMER_MAIN" \
        --http-url "http://127.0.0.1:$LOCATION_CONSUMER_HTTP_PORT" \
        --redis-endpoint "$REDIS_ENDPOINT" --redis-key-prefix "$REDIS_KEY_PREFIX" \
        --trace-label location-consumer --log-dir "$LOG_DIR"
      ;;
    *) echo "Unknown RegistryMessaging role: $1" >&2; return 1 ;;
  esac
}

wait_role() {
  case "$1" in
    api-a) wait_health "http://127.0.0.1:$PROVIDER_A_HTTP_PORT" api-a ;;
    api-b) wait_health "http://127.0.0.1:$PROVIDER_B_HTTP_PORT" api-b ;;
    workflow-a) wait_health "http://127.0.0.1:$WORKFLOW_HTTP_PORT" workflow-a ;;
    direct-consumer) wait_health "http://127.0.0.1:$CONSUMER_HTTP_PORT" direct-consumer ;;
    single-consumer) wait_health "http://127.0.0.1:$SINGLE_CONSUMER_HTTP_PORT" single-consumer ;;
    backpressure-consumer) wait_health "http://127.0.0.1:$BACKPRESSURE_CONSUMER_HTTP_PORT" backpressure-consumer ;;
    location-consumer) wait_health "http://127.0.0.1:$LOCATION_CONSUMER_HTTP_PORT" location-consumer ;;
    *) echo "Unknown RegistryMessaging role: $1" >&2; return 1 ;;
  esac
}

if [[ "$SCENARIO" == "RM-A3" || "$SCENARIO" == "rm-a3" ]]; then
  echo "rm-a3 phase=automatic-not-required"
  start_rm_a3_object_client client-a "$RM_A3_ROUTE_A" "$RM_A3_HTTP_A"
  RM_A3_A_PID="$RM_A3_LAST_PID"
  start_rm_a3_object_client client-b "$RM_A3_ROUTE_B" "$RM_A3_HTTP_B"
  RM_A3_B_PID="$RM_A3_LAST_PID"
  wait_health "$RM_A3_HTTP_A" client-a "$RM_A3_A_PID"
  wait_health "$RM_A3_HTTP_B" client-b "$RM_A3_B_PID"
  run_rm_a3_client automatic not_required false 20000 true
  stop_rm_a3_process "$RM_A3_A_PID" "$RM_A3_HTTP_A"
  stop_rm_a3_process "$RM_A3_B_PID" "$RM_A3_HTTP_B"

  echo "rm-a3 phase=manual-not-required"
  start_rm_a3_proxy proxy-a "$RM_A3_PROXY_A" "$RM_A3_ROUTE_A"
  RM_A3_PROXY_A_PID="$RM_A3_LAST_PID"
  start_rm_a3_proxy proxy-b "$RM_A3_PROXY_B" "$RM_A3_ROUTE_B"
  RM_A3_PROXY_B_PID="$RM_A3_LAST_PID"
  start_rm_a3_object_client \
    client-a "$RM_A3_ROUTE_A" "$RM_A3_HTTP_A" "$RM_A3_PROXY_B" client-b
  RM_A3_A_PID="$RM_A3_LAST_PID"
  start_rm_a3_object_client \
    client-b "$RM_A3_ROUTE_B" "$RM_A3_HTTP_B" "$RM_A3_PROXY_A" client-a
  RM_A3_B_PID="$RM_A3_LAST_PID"
  wait_health "$RM_A3_HTTP_A" client-a "$RM_A3_A_PID"
  wait_health "$RM_A3_HTTP_B" client-b "$RM_A3_B_PID"
  run_rm_a3_client manual not_required false 20000 false
  verify_rm_a3_single_manual_attempt "$LOG_DIR/proxy-a.connections.log"
  verify_rm_a3_single_manual_attempt "$LOG_DIR/proxy-b.connections.log"
  stop_rm_a3_process "$RM_A3_A_PID" "$RM_A3_HTTP_A"
  stop_rm_a3_process "$RM_A3_B_PID" "$RM_A3_HTTP_B"
  stop_rm_a3_process "$RM_A3_PROXY_A_PID"
  stop_rm_a3_process "$RM_A3_PROXY_B_PID"

  echo "rm-a3 phase=weight-zero-server-required"
  start_rm_a3_object_client client-b "$RM_A3_ROUTE_B" "$RM_A3_HTTP_B" "" "" 0
  RM_A3_B_PID="$RM_A3_LAST_PID"
  start_rm_a3_object_client client-a "$RM_A3_ROUTE_A" "$RM_A3_HTTP_A"
  RM_A3_A_PID="$RM_A3_LAST_PID"
  wait_health "$RM_A3_HTTP_A" client-a "$RM_A3_A_PID"
  wait_health "$RM_A3_HTTP_B" client-b "$RM_A3_B_PID"
  run_rm_a3_client weight-zero ready true 0 true 0

  kill -9 "$RM_A3_B_PID"
  wait "$RM_A3_B_PID" >/dev/null 2>&1 || true
  wait_rm_a3_peer_unavailable "$RM_A3_HTTP_A" client-b
  stop_rm_a3_process "$RM_A3_A_PID" "$RM_A3_HTTP_A"

  printf '%s\n' "result=passed" >"$LOG_DIR/RM-A3.result.tmp"
  mv -f "$LOG_DIR/RM-A3.result.tmp" "$LOG_DIR/RM-A3.result"
  echo "scenario RM-A3 passed"
  exit 0
fi

SERVER_ROLES=(api-a api-b workflow-a direct-consumer single-consumer backpressure-consumer location-consumer)
mapfile -t ORDERED_SERVER_ROLES < <(ordered_e2e_roles "$E2E_START_ORDER" "${SERVER_ROLES[@]}")
for role in "${ORDERED_SERVER_ROLES[@]}"; do
  start_role "$role"
done
for role in "${SERVER_ROLES[@]}"; do
  wait_role "$role"
done

if [[ "$SCENARIO" == "all" || "$SCENARIO" == "RM-A2" || "$SCENARIO" == "rm-a2" ]]; then
  start_configured_server manual-consumer "$CONSUMER_MAIN" \
    --http-url "$MANUAL_CONSUMER_HTTP" \
    --provider-endpoint "$API_A" \
    --trace-label manual-consumer --log-dir "$LOG_DIR"
  MANUAL_CONSUMER_PID="$LAST_STARTED_PID"
  wait_health "$MANUAL_CONSUMER_HTTP" manual-consumer "$MANUAL_CONSUMER_PID"
fi

CLIENT_CONFIG="$CONFIG_DIR/client.config.json"
node "$ROOT_DIR/write-config.mjs" "$CLIENT_CONFIG" \
  --provider-a-url "http://127.0.0.1:$PROVIDER_A_HTTP_PORT" \
  --provider-b-url "http://127.0.0.1:$PROVIDER_B_HTTP_PORT" \
  --workflow-url "http://127.0.0.1:$WORKFLOW_HTTP_PORT" \
  --direct-consumer-url "http://127.0.0.1:$CONSUMER_HTTP_PORT" \
  --single-consumer-url "http://127.0.0.1:$SINGLE_CONSUMER_HTTP_PORT" \
  --backpressure-consumer-url "http://127.0.0.1:$BACKPRESSURE_CONSUMER_HTTP_PORT" \
  --location-consumer-url "http://127.0.0.1:$LOCATION_CONSUMER_HTTP_PORT" \
  --manual-consumer-url "$MANUAL_CONSUMER_HTTP" \
  --provider-main "$PROVIDER_MAIN" \
  --consumer-main "$CONSUMER_MAIN" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --log-dir "$LOG_DIR" \
  --scenario "$SCENARIO"
node "$CLIENT_MAIN" --config "$CLIENT_CONFIG" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"

cat "$LOG_DIR/client.stdout.log"
