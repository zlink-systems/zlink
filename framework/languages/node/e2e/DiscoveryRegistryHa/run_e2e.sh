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
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
ROUTE_SETTLE_TIMEOUT_SECONDS=5
SCENARIO_SETTLE_TIMEOUT_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
LONG_OUTAGE_SECONDS=4
SCENARIOS=(
  SF-A1
  SF-A2
  SF-B1
  SF-B2
  SF-B3
  SF-C1
  SF-C2
  SF-C3
  SF-C4
  SF-C5
  SF-D1
  SF-D2
  SF-D3
  SF-E1
  SF-F1
  SF-F2
  SF-F3
  SF-F4
  SF-F5
  SF-F6
  SF-F7
  SF-F8
  SF-F9
  SF-F10
  SF-F11
  SF-G1
  SF-G2
  SF-G3
)
mkdir -p "$LOG_DIR"

pids=()
REDIS_CONTAINER_ID=""
LAST_STARTED_PID=""
API_A_PID=""
PROVIDER_A_CHANNEL_ENDPOINT=""
PROVIDER_B_CHANNEL_PORT=""
cleanup() {
  local code=$?
  # SIGTERM cannot stop a provider that is intentionally paused by SF-C3.
  [[ -z "$API_A_PID" ]] || kill -CONT "$API_A_PID" 2>/dev/null || true
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

kill_pid() {
  local pid="$1"
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
}

run_all_scenarios() {
  local scenario
  for scenario in "${SCENARIOS[@]}"; do
    "$0" "$scenario"
  done
  echo "store-failure-recovery e2e result=passed"
}

wait_location_unhealthy() {
  local url="$1"
  local name="$2"
  for _ in $(seq 1 150); do
    if node -e "fetch(process.argv[1] + '/location/status').then(async (r) => { if (!r.ok) process.exit(1); const s = await r.json(); process.exit(!s.storeHealthy && !s.ownerLeaseHealthy ? 0 : 1); }).catch(() => process.exit(1));" "$url"; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for unhealthy location status on $name at $url" >&2
  return 1
}

stop_redis() {
  docker rm -fv "$REDIS_CONTAINER_ID" >/dev/null
  REDIS_CONTAINER_ID=""
}

start_empty_redis() {
  start_redis_container "zlink-redis-node-e2e-${RANDOM}-$$" \
    -p "127.0.0.1:$REDIS_PORT:6379" "redis:7.2-alpine"
  wait_tcp redis "tcp://$REDIS_ENDPOINT"
}

echo "log_dir=$LOG_DIR"

if [[ "$SCENARIO" == "all" ]]; then
  run_all_scenarios
  exit 0
fi

(cd "$NODE_ROOT" && npm run build >/dev/null)
build_package "$ROOT_DIR/Server/LocationProbe"
build_package "$ROOT_DIR/Server/Provider"
build_package "$ROOT_DIR/Server/Consumer"
build_package "$ROOT_DIR/Client"

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run DiscoveryRegistryHa because it provisions a dedicated Redis location store." >&2
  exit 1
fi

start_redis_container "zlink-redis-node-e2e-${RANDOM}-$$" -p "127.0.0.1::6379" "redis:7.2-alpine"
REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
REDIS_PORT="${REDIS_ENDPOINT##*:}"
REDIS_KEY_PREFIX="store-failure:node:$RUN_ID"
wait_tcp redis "tcp://$REDIS_ENDPOINT"

LOCATION_PROBE_MAIN="$ROOT_DIR/Server/LocationProbe/dist/Server/LocationProbe/main.js"
PROVIDER_MAIN="$ROOT_DIR/Server/Provider/dist/Server/Provider/main.js"
CONSUMER_MAIN="$ROOT_DIR/Server/Consumer/dist/Server/Consumer/main.js"
CLIENT_MAIN="$ROOT_DIR/Client/dist/DiscoveryRegistryHa/Client/main.js"

start_configured_server() {
  local name="$1"; local main="$2"; shift 2
  local config="$CONFIG_DIR/$name.config.json"
  node "$ROOT_DIR/write-config.mjs" "$config" "$@"
  start_server "$name" "$main" --config "$config"
}

start_topology() {
  local with_provider_b="${1:-yes}"
  local store_response_gate="${2:-disabled}"
  local reg_http_port consumer_http_port provider_a_http_port provider_b_http_port
  local provider_a_channel_port provider_b_channel_port
  reg_http_port="$(pick_port)"
  consumer_http_port="$(pick_port)"
  provider_a_http_port="$(pick_port)"
  provider_b_http_port="$(pick_port)"
  provider_a_channel_port="$(pick_port)"
  provider_b_channel_port="$(pick_port)"

  LOCATION_PROBE_URL="http://127.0.0.1:$reg_http_port"
  CONSUMER_URL="http://127.0.0.1:$consumer_http_port"
  PROVIDER_A_URL="http://127.0.0.1:$provider_a_http_port"
  PROVIDER_B_URL="http://127.0.0.1:$provider_b_http_port"
  PROVIDER_B_CHANNEL_PORT="$provider_b_channel_port"
  PROVIDER_A_CHANNEL_ENDPOINT="tcp://127.0.0.1:$provider_a_channel_port"

  start_configured_server reg-1 "$LOCATION_PROBE_MAIN" \
    --rid reg-1 \
    --probe-id 1 \
    --http-url "$LOCATION_PROBE_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --log-dir "$LOG_DIR"
  wait_health "$LOCATION_PROBE_URL" reg-1 "$LAST_STARTED_PID"

  start_configured_server api-a "$PROVIDER_MAIN" \
    --rid api-a \
    --http-url "$PROVIDER_A_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --channel-endpoint "tcp://127.0.0.1:$provider_a_channel_port" \
    --evidence-file "$LOG_DIR/api-a.evidence.log" \
    --log-dir "$LOG_DIR"
  API_A_PID="$LAST_STARTED_PID"
  wait_health "$PROVIDER_A_URL" api-a "$LAST_STARTED_PID"

  if [[ "$with_provider_b" == "yes" ]]; then
    start_provider_b
  else
    PROVIDER_B_URL=""
  fi

  start_configured_server consumer "$CONSUMER_MAIN" \
    --http-url "$CONSUMER_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --trace-label consumer \
    --store-response-gate "$store_response_gate" \
    --log-dir "$LOG_DIR"
  wait_health "$CONSUMER_URL" consumer "$LAST_STARTED_PID"
}

start_provider_b() {
  start_configured_server api-b "$PROVIDER_MAIN" \
    --rid api-b \
    --http-url "$PROVIDER_B_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --channel-endpoint "tcp://127.0.0.1:$PROVIDER_B_CHANNEL_PORT" \
    --evidence-file "$LOG_DIR/api-b.evidence.log" \
    --log-dir "$LOG_DIR"
  API_B_PID="$LAST_STARTED_PID"
  wait_health "$PROVIDER_B_URL" api-b "$LAST_STARTED_PID"
}

start_provider_a_replacement() {
  local provider_a_http_port provider_a_channel_port
  provider_a_http_port="$(pick_port)"
  provider_a_channel_port="$(pick_port)"
  PROVIDER_A_URL="http://127.0.0.1:$provider_a_http_port"
  PROVIDER_A_CHANNEL_ENDPOINT="tcp://127.0.0.1:$provider_a_channel_port"
  start_configured_server api-a-replacement "$PROVIDER_MAIN" \
    --rid api-a \
    --http-url "$PROVIDER_A_URL" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --channel-endpoint "tcp://127.0.0.1:$provider_a_channel_port" \
    --evidence-file "$LOG_DIR/api-a-replacement.evidence.log" \
    --log-dir "$LOG_DIR"
  API_A_REPLACEMENT_PID="$LAST_STARTED_PID"
  wait_health "$PROVIDER_A_URL" api-a-replacement "$LAST_STARTED_PID"
}

wait_for_peer_endpoint() {
  local endpoint="$1"
  local deadline=$((SECONDS + ROUTE_SETTLE_TIMEOUT_SECONDS))
  while (( SECONDS < deadline )); do
    if node -e "fetch(process.argv[1] + '/location/peers', { signal: AbortSignal.timeout(500) }).then(async (r) => { const rows = await r.json(); process.exit(rows.some((row) => row.endpoint === process.argv[2]) ? 0 : 1); }).catch(() => process.exit(1));" "$CONSUMER_URL" "$endpoint"; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for current peer endpoint $endpoint" >&2
  echo "Current peer rows:" >&2
  curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS "$CONSUMER_URL/location/peers" >&2 || true
  echo >&2
  echo "Current location status:" >&2
  curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS "$CONSUMER_URL/location/status" >&2 || true
  echo >&2
  return 1
}

wait_for_peer_absent() {
  local endpoint="$1"
  local deadline=$((SECONDS + ROUTE_SETTLE_TIMEOUT_SECONDS))
  while (( SECONDS < deadline )); do
    if node -e "fetch(process.argv[1] + '/location/peers', { signal: AbortSignal.timeout(500) }).then(async (r) => { const rows = await r.json(); process.exit(rows.some((row) => row.endpoint === process.argv[2]) ? 1 : 0); }).catch(() => process.exit(1));" "$CONSUMER_URL" "$endpoint"; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for stale peer endpoint removal $endpoint" >&2
  echo "Current peer rows:" >&2
  curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS "$CONSUMER_URL/location/peers" >&2 || true
  echo >&2
  return 1
}

wait_for_profile_ready() {
  local deadline=$((SECONDS + ROUTE_SETTLE_TIMEOUT_SECONDS))
  while (( SECONDS < deadline )); do
    if node -e "fetch(process.argv[1] + '/route/status', { signal: AbortSignal.timeout(500) }).then(async (r) => { const status = await r.json(); const apiA = status.peers?.find((peer) => peer.nodeRid === 'api-a'); const channel = status.channels?.find((entry) => entry.channelName === 'profile'); process.exit(status.isReady === true && apiA?.state === 1 && channel?.isReady === true && channel.readyTargetCount >= 1 ? 0 : 1); }).catch(() => process.exit(1));" "$CONSUMER_URL"; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for profile route readiness" >&2
  echo "Current route status:" >&2
  curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS "$CONSUMER_URL/route/status" >&2 || true
  echo >&2
  return 1
}

run_client() {
  local scenario="$1"
  local stdout="$2"
  local stderr="$3"
  local client_config="$CONFIG_DIR/client-${scenario}.config.json"
  local -a config_args=(
    --topology-url "$LOCATION_PROBE_URL" --consumer-url "$CONSUMER_URL"
    --provider-a-url "$PROVIDER_A_URL" --scenario "$scenario"
  )
  [[ -z "$PROVIDER_B_URL" ]] || config_args+=(--provider-b-url "$PROVIDER_B_URL")
  node "$ROOT_DIR/write-config.mjs" "$client_config" "${config_args[@]}"
  if ! node "$CLIENT_MAIN" \
    --config "$client_config" \
    >"$stdout" 2>"$stderr"; then
    echo "Client scenario $scenario failed; consumer location and route snapshots:" >&2
    curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS "$CONSUMER_URL/location/mesh" >&2 || true
    echo >&2
    curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS "$CONSUMER_URL/route/status" >&2 || true
    echo >&2
    return 1
  fi
}

run_warmup() {
  run_client SF-A1 "$LOG_DIR/client-warmup.stdout.log" "$LOG_DIR/client-warmup.stderr.log"
}

run_sf_a1() {
  start_topology
  run_client SF-A1 "$LOG_DIR/client.stdout.log" "$LOG_DIR/client.stderr.log"
}

run_sf_a2() {
  local client_pid
  start_topology no
  run_client SF-A2 "$LOG_DIR/client.stdout.log" "$LOG_DIR/client.stderr.log" &
  client_pid="$!"
  wait_file_contains "$LOG_DIR/client.stdout.log" "scenario-control SF-A2 start-provider-b" \
    "SF-A2 client did not request api-b startup" "$client_pid" 120
  start_provider_b
  wait_file_contains "$LOG_DIR/client.stdout.log" "scenario-control SF-A2 shutdown-provider-b" \
    "SF-A2 client did not request api-b shutdown" "$client_pid" 120
  curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS -X POST "$PROVIDER_B_URL/shutdown" >/dev/null
  wait "$client_pid"
}

run_sf_b1() {
  start_topology
  run_warmup
  docker rm -fv "$REDIS_CONTAINER_ID" >/dev/null
  REDIS_CONTAINER_ID=""
  run_client SF-B1 "$LOG_DIR/client.stdout.log" "$LOG_DIR/client.stderr.log"
}

run_sf_b2() {
  start_topology
  run_warmup
  stop_redis
  kill_pid "$API_B_PID"
  start_provider_b
  run_client SF-B2 "$LOG_DIR/client.stdout.log" "$LOG_DIR/client.stderr.log"
}

run_sf_c1() {
  start_topology
  run_warmup
  kill_pid "$API_B_PID"
  run_client SF-C1 "$LOG_DIR/client.stdout.log" "$LOG_DIR/client.stderr.log"
}

run_sf_c2() {
  local started elapsed
  start_topology
  run_warmup
  started="$SECONDS"
  run_client SF-C2 "$LOG_DIR/client.stdout.log" "$LOG_DIR/client.stderr.log"
  if ! wait "$API_B_PID"; then
    echo "SF-C2 api-b did not exit cleanly after framework drain" >&2
    return 1
  fi
  elapsed=$((SECONDS - started))
  if (( elapsed > 30 )); then
    echo "SF-C2 api-b exceeded the 30 second drain deadline (elapsed=${elapsed}s)" >&2
    return 1
  fi
}

run_sf_d1() {
  local client_pid
  start_topology
  run_warmup
  run_client SF-D1 "$LOG_DIR/client.stdout.log" "$LOG_DIR/client.stderr.log" &
  client_pid="$!"
  wait_file_contains "$LOG_DIR/client.stdout.log" "scenario-control SF-D1 stop-redis" \
    "SF-D1 client did not request Redis stop" "$client_pid"
  stop_redis
  wait_file_contains "$LOG_DIR/client.stdout.log" "scenario-control SF-D1 restart-redis" \
    "SF-D1 client did not request Redis restart" "$client_pid"
  start_empty_redis
  wait "$client_pid"
}

run_sf_d2() {
  local client_pid
  start_topology
  run_warmup
  run_client SF-D2 "$LOG_DIR/client.stdout.log" "$LOG_DIR/client.stderr.log" &
  client_pid="$!"
  wait_file_contains "$LOG_DIR/client.stdout.log" "scenario-control SF-D2 stop-redis-and-kill-api-b" \
    "SF-D2 client did not request Redis stop and api-b kill" "$client_pid"
  stop_redis
  kill_pid "$API_B_PID"
  wait_file_contains "$LOG_DIR/client.stdout.log" "scenario-control SF-D2 restart-redis" \
    "SF-D2 client did not request Redis restart" "$client_pid"
  sleep "$LONG_OUTAGE_SECONDS"
  start_empty_redis
  wait "$client_pid"
}

run_sf_d3() {
  local client_pid
  start_topology
  run_client SF-D3 "$LOG_DIR/client.stdout.log" "$LOG_DIR/client.stderr.log" &
  client_pid="$!"
  wait_file_contains "$LOG_DIR/client.stdout.log" "scenario-control SF-D3 stop-redis" \
    "SF-D3 client did not request Redis stop" "$client_pid"
  stop_redis
  wait_location_unhealthy "$CONSUMER_URL" consumer
  start_empty_redis
  wait "$client_pid"
}

run_sf_e1() {
  start_topology yes enabled
  run_warmup
  run_client SF-E1 "$LOG_DIR/client.stdout.log" "$LOG_DIR/client.stderr.log"
}

run_sf_c3() {
  local old_provider_a_channel_endpoint old_evidence_lines current_old_evidence_lines
  # Isolate the same-role replacement from unrelated load-balancing choices.
  start_topology no
  run_client SF-C3 "$LOG_DIR/client-baseline.stdout.log" "$LOG_DIR/client-baseline.stderr.log"
  old_evidence_lines="$(wc -l < "$LOG_DIR/api-a.evidence.log")"
  old_provider_a_channel_endpoint="$PROVIDER_A_CHANNEL_ENDPOINT"
  kill -STOP "$API_A_PID"
  wait_for_peer_absent "$old_provider_a_channel_endpoint"
  start_provider_a_replacement
  wait_for_peer_endpoint "$PROVIDER_A_CHANNEL_ENDPOINT"
  wait_for_profile_ready
  run_client SF-C3 "$LOG_DIR/client-replacement.stdout.log" "$LOG_DIR/client-replacement.stderr.log"
  kill -CONT "$API_A_PID"
  wait_for_peer_endpoint "$PROVIDER_A_CHANNEL_ENDPOINT"
  wait_for_profile_ready
  run_client SF-C3 "$LOG_DIR/client-resumed.stdout.log" "$LOG_DIR/client-resumed.stderr.log"
  current_old_evidence_lines="$(wc -l < "$LOG_DIR/api-a.evidence.log")"
  if [[ "$current_old_evidence_lines" != "$old_evidence_lines" ]]; then
    echo "SF-C3 old provider handled requests after replacement: before=$old_evidence_lines after=$current_old_evidence_lines" >&2
    return 1
  fi
  cat "$LOG_DIR/client-replacement.stdout.log"
  cat "$LOG_DIR/client-resumed.stdout.log"
}

run_unimplemented_scenario() {
  echo "$SCENARIO is not implemented by the Config 6 Node fixture; refusing profile-only success." >&2
  exit 3
}

case "$SCENARIO" in
  SF-A1)
    run_sf_a1
    cat "$LOG_DIR/client.stdout.log"
    ;;
  SF-A2)
    run_sf_a2
    cat "$LOG_DIR/client.stdout.log"
    ;;
  SF-B1)
    run_sf_b1
    cat "$LOG_DIR/client-warmup.stdout.log"
    cat "$LOG_DIR/client.stdout.log"
    ;;
  SF-B2)
    run_sf_b2
    cat "$LOG_DIR/client-warmup.stdout.log"
    cat "$LOG_DIR/client.stdout.log"
    ;;
  SF-C1)
    run_sf_c1
    cat "$LOG_DIR/client-warmup.stdout.log"
    cat "$LOG_DIR/client.stdout.log"
    ;;
  SF-C2)
    run_sf_c2
    cat "$LOG_DIR/client-warmup.stdout.log"
    cat "$LOG_DIR/client.stdout.log"
    ;;
  SF-D1)
    run_sf_d1
    cat "$LOG_DIR/client-warmup.stdout.log"
    cat "$LOG_DIR/client.stdout.log"
    ;;
  SF-D2)
    run_sf_d2
    cat "$LOG_DIR/client-warmup.stdout.log"
    cat "$LOG_DIR/client.stdout.log"
    ;;
  SF-D3)
    run_sf_d3
    cat "$LOG_DIR/client.stdout.log"
    ;;
  SF-E1)
    run_sf_e1
    cat "$LOG_DIR/client-warmup.stdout.log"
    cat "$LOG_DIR/client.stdout.log"
    ;;
  SF-C3)
    run_sf_c3
    ;;
  SF-C5)
    start_topology no
    PROVIDER_B_URL=""
    wait_for_peer_endpoint "$PROVIDER_A_CHANNEL_ENDPOINT"
    run_client SF-C5 "$LOG_DIR/client.stdout.log" "$LOG_DIR/client.stderr.log"
    cat "$LOG_DIR/client.stdout.log"
    ;;
  SF-B3|SF-C4|SF-F1|SF-F2|SF-F3|SF-F4|SF-F5|SF-F6|SF-F7|SF-F8|SF-F9|SF-F10|SF-F11|SF-G1|SF-G2|SF-G3)
    run_unimplemented_scenario
    ;;
  *)
    echo "Unsupported scenario '$SCENARIO'. Supported: all, ${SCENARIOS[*]}" >&2
    exit 2
    ;;
esac
