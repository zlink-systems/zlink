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

used_ports=()

pids=()
pid_names=()
declare -A pid_by_name=()
REDIS_CONTAINER_ID=""
BIND_RETRY_PATTERN="ZlinkBindException|BindException|Address already in use|EADDRINUSE|errno=98"

role_shutdown_url() {
  case "$1" in
    play-a) echo "${PLAY_A_URL:-}" ;;
    play-b) echo "${PLAY_B_URL:-}" ;;
    session-a) echo "${SESSION_A_URL:-}" ;;
    session-b) echo "${SESSION_B_URL:-}" ;;
    gateway) echo "${GATEWAY_URL:-}" ;;
    multi-node-a) echo "${MULTI_A_URL:-}" ;;
    multi-node-b) echo "${MULTI_B_URL:-}" ;;
    *) echo "" ;;
  esac
}

request_graceful_shutdowns() {
  local index pid name url
  for index in "${!pids[@]}"; do
    pid="${pids[$index]}"
    name="${pid_names[$index]}"
    if ! kill -0 "$pid" 2>/dev/null; then
      continue
    fi
    url="$(role_shutdown_url "$name")"
    if [[ -n "$url" ]]; then
      curl --max-time 1 -fsS -X POST "$url/shutdown" >/dev/null 2>&1 || true
    fi
  done

  for _ in $(seq 1 30); do
    local any_alive=0
    for pid in "${pids[@]:-}"; do
      if kill -0 "$pid" 2>/dev/null; then
        any_alive=1
        break
      fi
    done
    if [[ "$any_alive" -eq 0 ]]; then
      return 0
    fi
    sleep 0.1
  done
}

cleanup() {
  local code=$?
  local background_failure=0
  local index pid name status
  request_graceful_shutdowns
  stop_live_pids
  for index in "${!pids[@]}"; do
    pid="${pids[$index]}"
    name="${pid_names[$index]}"
    set +e
    wait "$pid" 2>/dev/null
    status=$?
    set -e
    if [[ "$SCENARIO" == "SM-G1" && "$name" == "play-a" && "$status" -eq 1 ]]; then
      continue
    fi
    if [[ "$status" -ne 0 && "$status" -ne 143 ]]; then
      if [[ "$code" -eq 0 ]]; then
        background_failure=1
      fi
      echo "Background role $name exited unexpectedly with status $status." >&2
    fi
  done
  remove_redis_container
  [[ -z "$CONFIG_DIR" ]] || rm -rf "$CONFIG_DIR"
  if [[ "$code" -ne 0 || "$background_failure" -ne 0 ]]; then
    tail_failure_logs
  fi
  if [[ "$code" -eq 0 && "$background_failure" -ne 0 ]]; then
    exit 1
  fi
}
trap cleanup EXIT
CONFIG_DIR="$(mktemp -d)"
chmod 700 "$CONFIG_DIR"

echo "log_dir=$LOG_DIR"
echo "start_order=$E2E_START_ORDER"

if [[ "$SCENARIO" != "all" && "${ZLINK_SPOT_SERVICE_RETRY_CHILD:-0}" != "1" && "${ZLINK_SPOT_SERVICE_ALL_CHILD:-0}" != "1" ]]; then
  output="$(mktemp)"
  scenario_passed=0
  for attempt in 1 2 3; do
    : >"${output}"
    set +e
    timeout 420s env ZLINK_SPOT_SERVICE_RETRY_CHILD=1 "$0" "$SCENARIO" 2>&1 | tee "${output}"
    status="${PIPESTATUS[0]}"
    set -e
    if [[ "${status}" == "0" ]]; then
      scenario_passed=1
      break
    fi
    if ! grep -Eq "${BIND_RETRY_PATTERN}" "${output}"; then
      break
    fi
    if [[ "${attempt}" == "3" ]]; then
      break
    fi
    echo "spot-service transient bind failure; retrying scenario=${SCENARIO} (${attempt}/3)" >&2
    sleep 1
  done
  rm -f "${output}"
  if [[ "${scenario_passed}" == "1" ]]; then
    exit 0
  fi
  echo "scenario=${SCENARIO} failed" >&2
  exit 1
fi

if [[ "$SCENARIO" == "all" && "${ZLINK_SPOT_SERVICE_ALL_CHILD:-0}" != "1" ]]; then
  for child_group in default-batch sm-d8 SM-F6 SM-G2 SM-G3 SM-G4 SM-G1; do
    echo "child scenario=$child_group"
    output="$(mktemp)"
    child_passed=0
    for attempt in 1 2 3; do
      : >"${output}"
      set +e
      timeout 420s env ZLINK_SPOT_SERVICE_ALL_CHILD=1 "$0" "$child_group" 2>&1 | tee "${output}"
      status="${PIPESTATUS[0]}"
      set -e
      if [[ "${status}" == "0" ]]; then
        child_passed=1
        break
      fi
      if ! grep -Eq "${BIND_RETRY_PATTERN}" "${output}"; then
        break
      fi
      if [[ "${attempt}" == "3" ]]; then
        break
      fi
      echo "spot-service transient bind failure; retrying child scenario=${child_group} (${attempt}/3)" >&2
      sleep 1
    done
    rm -f "${output}"
    if [[ "${child_passed}" != "1" ]]; then
      echo "child scenario=$child_group failed" >&2
      exit 1
    fi
  done
  echo "spot-service e2e result=passed"
  exit 0
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required for SpotService location-store scenarios." >&2
  exit 1
fi
start_redis_container "zlink-redis-node-e2e-${RANDOM}-$$" -p "127.0.0.1::6379" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
REDIS_KEY_PREFIX="spot-service:node:${RUN_ID}:location"
wait_port redis "tcp://$REDIS_ENDPOINT"

(cd "$NODE_ROOT" && npm run build >/dev/null)
case "$SCENARIO" in
  SM-F3|SM-F4|SM-F5)
    build_package "$ROOT_DIR/Server/Play"
    ;;
  SM-C4)
    build_package "$ROOT_DIR/Server/Play"
    build_package "$ROOT_DIR/Server/Gateway"
    ;;
  SM-F6|SM-G2|SM-G2-PREPARE|SM-G2-VERIFY)
    build_package "$ROOT_DIR/Server/MultiNode"
    ;;
  *)
    build_package "$ROOT_DIR/Server/Play"
    build_package "$ROOT_DIR/Server/Session"
    build_package "$ROOT_DIR/Server/Gateway"
    build_package "$ROOT_DIR/Server/MultiNode"
    ;;
esac
build_package "$ROOT_DIR/Client"

PLAY_A_HTTP_PORT="$(allocate_port)"
PLAY_B_HTTP_PORT="$(allocate_port)"
SESSION_A_HTTP_PORT="$(allocate_port)"
SESSION_B_HTTP_PORT="$(allocate_port)"
GATEWAY_HTTP_PORT="$(allocate_port)"
MULTI_A_HTTP_PORT="$(allocate_port)"
MULTI_B_HTTP_PORT="$(allocate_port)"
PLAY_A_CONTROL_PORT="$(allocate_port)"
PLAY_B_CONTROL_PORT="$(allocate_port)"
PLAY_A_EXTERNAL_SPOT_PORT="$(allocate_port)"
PLAY_B_EXTERNAL_SPOT_PORT="$(allocate_port)"
SESSION_A_CONTROL_PORT="$(allocate_port)"
SESSION_B_CONTROL_PORT="$(allocate_port)"
PLAY_A_ROUTER_PORT="$(allocate_port)"
PLAY_B_ROUTER_PORT="$(allocate_port)"
SESSION_A_ROUTER_PORT="$(allocate_port)"
SESSION_B_ROUTER_PORT="$(allocate_port)"
SESSION_A_ALTERNATE_OBJECT_PORT="$(allocate_port)"
SESSION_B_ALTERNATE_OBJECT_PORT="$(allocate_port)"
GATEWAY_ROUTER_PORT="$(allocate_port)"
MULTI_A_ROUTE_PORT="$(allocate_port)"
MULTI_B_ROUTE_PORT="$(allocate_port)"
PLAY_A_SPOT_PUB_PORT="$(allocate_port)"
PLAY_B_SPOT_PUB_PORT="$(allocate_port)"
GATEWAY_SPOT_PUB_PORT="$(allocate_port)"
PLAY_A_EXTERNAL_CLIENT_PORT="$(allocate_port)"
PLAY_B_EXTERNAL_CLIENT_PORT="$(allocate_port)"
SESSION_A_STREAM_PORT="$(allocate_port)"
SESSION_A_TLS_STREAM_PORT="$(allocate_port)"
SESSION_B_STREAM_PORT="$(allocate_port)"
MULTI_A_SPOT_ROUTER_PORT="$(allocate_port)"
MULTI_B_SPOT_ROUTER_PORT="$(allocate_port)"
MULTI_A_SPOT_PUB_PORT="$(allocate_port)"
MULTI_B_SPOT_PUB_PORT="$(allocate_port)"

PLAY_A_URL="http://127.0.0.1:$PLAY_A_HTTP_PORT"
PLAY_B_URL="http://127.0.0.1:$PLAY_B_HTTP_PORT"
SESSION_A_URL="http://127.0.0.1:$SESSION_A_HTTP_PORT"
SESSION_B_URL="http://127.0.0.1:$SESSION_B_HTTP_PORT"
GATEWAY_URL="http://127.0.0.1:$GATEWAY_HTTP_PORT"
MULTI_A_URL="http://127.0.0.1:$MULTI_A_HTTP_PORT"
MULTI_B_URL="http://127.0.0.1:$MULTI_B_HTTP_PORT"
PLAY_A_CONTROL="tcp://127.0.0.1:$PLAY_A_CONTROL_PORT"
PLAY_B_CONTROL="tcp://127.0.0.1:$PLAY_B_CONTROL_PORT"
PLAY_A_EXTERNAL_SPOT="tcp://127.0.0.1:$PLAY_A_EXTERNAL_SPOT_PORT"
PLAY_B_EXTERNAL_SPOT="tcp://127.0.0.1:$PLAY_B_EXTERNAL_SPOT_PORT"
SESSION_A_CONTROL="tcp://127.0.0.1:$SESSION_A_CONTROL_PORT"
SESSION_B_CONTROL="tcp://127.0.0.1:$SESSION_B_CONTROL_PORT"
PLAY_A_ROUTER="tcp://127.0.0.1:$PLAY_A_ROUTER_PORT"
PLAY_B_ROUTER="tcp://127.0.0.1:$PLAY_B_ROUTER_PORT"
SESSION_A_ROUTER="tcp://127.0.0.1:$SESSION_A_ROUTER_PORT"
SESSION_B_ROUTER="tcp://127.0.0.1:$SESSION_B_ROUTER_PORT"
SESSION_A_ALTERNATE_OBJECT="tcp://127.0.0.1:$SESSION_A_ALTERNATE_OBJECT_PORT"
SESSION_B_ALTERNATE_OBJECT="tcp://127.0.0.1:$SESSION_B_ALTERNATE_OBJECT_PORT"
GATEWAY_ROUTER="tcp://127.0.0.1:$GATEWAY_ROUTER_PORT"
MULTI_A_ROUTE="tcp://127.0.0.1:$MULTI_A_ROUTE_PORT"
MULTI_B_ROUTE="tcp://127.0.0.1:$MULTI_B_ROUTE_PORT"
PLAY_A_SPOT_PUB="tcp://127.0.0.1:$PLAY_A_SPOT_PUB_PORT"
PLAY_B_SPOT_PUB="tcp://127.0.0.1:$PLAY_B_SPOT_PUB_PORT"
GATEWAY_SPOT_PUB="tcp://127.0.0.1:$GATEWAY_SPOT_PUB_PORT"
PLAY_A_EXTERNAL_CLIENT="tcp://127.0.0.1:$PLAY_A_EXTERNAL_CLIENT_PORT"
PLAY_B_EXTERNAL_CLIENT="tcp://127.0.0.1:$PLAY_B_EXTERNAL_CLIENT_PORT"
SESSION_A_STREAM="ws://127.0.0.1:$SESSION_A_STREAM_PORT"
SESSION_A_TLS_STREAM="wss://127.0.0.1:$SESSION_A_TLS_STREAM_PORT"
SESSION_B_STREAM="ws://127.0.0.1:$SESSION_B_STREAM_PORT"
MULTI_A_SPOT_ROUTER="tcp://127.0.0.1:$MULTI_A_SPOT_ROUTER_PORT"
MULTI_B_SPOT_ROUTER="tcp://127.0.0.1:$MULTI_B_SPOT_ROUTER_PORT"
MULTI_A_SPOT_PUB="tcp://127.0.0.1:$MULTI_A_SPOT_PUB_PORT"
MULTI_B_SPOT_PUB="tcp://127.0.0.1:$MULTI_B_SPOT_PUB_PORT"

PLAY_MAIN="$ROOT_DIR/Server/Play/dist/Server/Play/main.js"
SESSION_MAIN="$ROOT_DIR/Server/Session/dist/Server/Session/main.js"
GATEWAY_MAIN="$ROOT_DIR/Server/Gateway/dist/Server/Gateway/main.js"
MULTI_NODE_MAIN="$ROOT_DIR/Server/MultiNode/dist/Server/MultiNode/main.js"

TLS_CERT="$CONFIG_DIR/session-a-tls.crt"
TLS_KEY="$CONFIG_DIR/session-a-tls.key"
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "$TLS_KEY" \
  -out "$TLS_CERT" \
  -subj "/CN=localhost" \
  -days 1 >/dev/null 2>&1
chmod 600 "$TLS_CERT" "$TLS_KEY"

write_config() {
  local name="$1"
  shift
  node "$ROOT_DIR/write-config.mjs" "$CONFIG_DIR/$name.json" "$@"
}

write_config play-a \
  --string rid play-a --string httpUrl "$PLAY_A_URL" \
  --string controlRouterEndpoint "$PLAY_A_CONTROL" --string externalSpotEndpoint "$PLAY_A_EXTERNAL_SPOT" \
  --string spotRouterEndpoint "$PLAY_A_ROUTER" --string spotPubEndpoint "$PLAY_A_SPOT_PUB" \
  --array clientSpotPubEndpoints "$GATEWAY_SPOT_PUB,$PLAY_B_SPOT_PUB" \
  --string externalClientEndpoint "$PLAY_A_EXTERNAL_CLIENT" \
  --string redisEndpoint "$REDIS_ENDPOINT" --string redisKeyPrefix "$REDIS_KEY_PREFIX" \
  --string evidenceFile "$LOG_DIR/play-a.evidence.log" --string logDir "$LOG_DIR"
write_config play-b \
  --string rid play-b --string httpUrl "$PLAY_B_URL" \
  --string controlRouterEndpoint "$PLAY_B_CONTROL" --string externalSpotEndpoint "$PLAY_B_EXTERNAL_SPOT" \
  --string playAExternalSpotEndpoint "$PLAY_A_EXTERNAL_SPOT" \
  --string spotRouterEndpoint "$PLAY_B_ROUTER" --string spotPubEndpoint "$PLAY_B_SPOT_PUB" \
  --array clientSpotPubEndpoints "$GATEWAY_SPOT_PUB,$PLAY_A_SPOT_PUB" \
  --string externalClientEndpoint "$PLAY_B_EXTERNAL_CLIENT" \
  --string redisEndpoint "$REDIS_ENDPOINT" --string redisKeyPrefix "$REDIS_KEY_PREFIX" \
  --string evidenceFile "$LOG_DIR/play-b.evidence.log" --string logDir "$LOG_DIR"
write_config session-a \
  --string rid session-a --string httpUrl "$SESSION_A_URL" \
  --string controlRouterEndpoint "$SESSION_A_CONTROL" --array playControlEndpoints "play-a=$PLAY_A_CONTROL,play-b=$PLAY_B_CONTROL" \
  --string spotRouterEndpoint "$SESSION_A_ROUTER" \
  --array playSpotRouterEndpoints "play-a=$PLAY_A_ROUTER,play-b=$PLAY_B_ROUTER" \
  --string alternateObjectRouterEndpoint "$SESSION_A_ALTERNATE_OBJECT" \
  --array playAlternateObjectRouterEndpoints "multi-node-a=$MULTI_A_SPOT_ROUTER" \
  --string redisEndpoint "$REDIS_ENDPOINT" --string redisKeyPrefix "$REDIS_KEY_PREFIX" \
  --string streamEndpoint "$SESSION_A_STREAM" --string tlsStreamEndpoint "$SESSION_A_TLS_STREAM" \
  --string tlsCertPath "$TLS_CERT" --string tlsKeyPath "$TLS_KEY" \
  --string evidenceFile "$LOG_DIR/session-a.evidence.log" --string logDir "$LOG_DIR"
write_config session-b \
  --string rid session-b --string httpUrl "$SESSION_B_URL" \
  --string controlRouterEndpoint "$SESSION_B_CONTROL" --array playControlEndpoints "play-a=$PLAY_A_CONTROL,play-b=$PLAY_B_CONTROL" \
  --string spotRouterEndpoint "$SESSION_B_ROUTER" \
  --array playSpotRouterEndpoints "play-a=$PLAY_A_ROUTER,play-b=$PLAY_B_ROUTER" \
  --string alternateObjectRouterEndpoint "$SESSION_B_ALTERNATE_OBJECT" \
  --array playAlternateObjectRouterEndpoints "multi-node-a=$MULTI_A_SPOT_ROUTER" \
  --string redisEndpoint "$REDIS_ENDPOINT" --string redisKeyPrefix "$REDIS_KEY_PREFIX" \
  --string streamEndpoint "$SESSION_B_STREAM" \
  --string evidenceFile "$LOG_DIR/session-b.evidence.log" --string logDir "$LOG_DIR"
write_config gateway \
  --string rid gateway --string httpUrl "$GATEWAY_URL" \
  --string spotRouterEndpoint "$GATEWAY_ROUTER" \
  --string redisEndpoint "$REDIS_ENDPOINT" --string redisKeyPrefix "$REDIS_KEY_PREFIX" \
  --string evidenceFile "$LOG_DIR/gateway.evidence.log" --string logDir "$LOG_DIR"

multi_a_peer=()
multi_b_peer=()
if [[ "$SCENARIO" != "SM-F6" && "$SCENARIO" != "SM-G2" && "$SCENARIO" != "cross-mesh-actor-dispatch" ]]; then
  multi_a_peer=(--string peerSpotRouterEndpoint "$MULTI_B_SPOT_ROUTER")
  multi_b_peer=(--string peerSpotRouterEndpoint "$MULTI_A_SPOT_ROUTER")
fi
write_config multi-node-a \
  --string rid multi-node-a --string httpUrl "$MULTI_A_URL" --string routeEndpoint "$MULTI_A_ROUTE" \
  --string spotRouterEndpoint "$MULTI_A_SPOT_ROUTER" --string spotPubEndpoint "$MULTI_A_SPOT_PUB" \
  "${multi_a_peer[@]}" --string redisEndpoint "$REDIS_ENDPOINT" --string redisKeyPrefix "$REDIS_KEY_PREFIX" \
  --boolean spotOnly "$([[ "$SCENARIO" == "SM-F6" || "$SCENARIO" == "SM-G2" || "$SCENARIO" == "cross-mesh-actor-dispatch" ]] && echo true || echo false)" \
  --string evidenceFile "$LOG_DIR/multi-node-a.evidence.log" --string logDir "$LOG_DIR"
write_config multi-node-b \
  --string rid multi-node-b --string httpUrl "$MULTI_B_URL" --string routeEndpoint "$MULTI_B_ROUTE" \
  --string spotRouterEndpoint "$MULTI_B_SPOT_ROUTER" --string spotPubEndpoint "$MULTI_B_SPOT_PUB" \
  "${multi_b_peer[@]}" --string redisEndpoint "$REDIS_ENDPOINT" --string redisKeyPrefix "$REDIS_KEY_PREFIX" \
  --boolean spotOnly "$([[ "$SCENARIO" == "SM-F6" || "$SCENARIO" == "SM-G2" ]] && echo true || echo false)" \
  --string evidenceFile "$LOG_DIR/multi-node-b.evidence.log" --string logDir "$LOG_DIR"

start_named_server() {
  case "$1" in
    play-a)
      start_server play-a "$PLAY_MAIN" --config "$CONFIG_DIR/play-a.json"
      ;;
    play-b)
      start_server play-b "$PLAY_MAIN" --config "$CONFIG_DIR/play-b.json"
      ;;
    session-a)
      start_server session-a "$SESSION_MAIN" --config "$CONFIG_DIR/session-a.json"
      ;;
    session-b)
      start_server session-b "$SESSION_MAIN" --config "$CONFIG_DIR/session-b.json"
      ;;
    gateway)
      start_server gateway "$GATEWAY_MAIN" --config "$CONFIG_DIR/gateway.json"
      ;;
    multi-node-a)
      start_server multi-node-a "$MULTI_NODE_MAIN" --config "$CONFIG_DIR/multi-node-a.json"
      ;;
    multi-node-b)
      start_server multi-node-b "$MULTI_NODE_MAIN" --config "$CONFIG_DIR/multi-node-b.json"
      ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

wait_named_server() {
  case "$1" in
    play-a)
      wait_health "$PLAY_A_URL" play-a "${pid_by_name[play-a]:-}"
      wait_port play-a-control "$PLAY_A_CONTROL"
      wait_port play-a-external-spot "$PLAY_A_EXTERNAL_SPOT"
      wait_port play-a-spot-router "$PLAY_A_ROUTER"
      wait_port play-a-external-client "$PLAY_A_EXTERNAL_CLIENT"
      ;;
    play-b)
      wait_health "$PLAY_B_URL" play-b "${pid_by_name[play-b]:-}"
      wait_port play-b-control "$PLAY_B_CONTROL"
      wait_port play-b-external-spot "$PLAY_B_EXTERNAL_SPOT"
      wait_port play-b-spot-router "$PLAY_B_ROUTER"
      wait_port play-b-external-client "$PLAY_B_EXTERNAL_CLIENT"
      ;;
    session-a)
      wait_health "$SESSION_A_URL" session-a "${pid_by_name[session-a]:-}"
      wait_port session-a-control "$SESSION_A_CONTROL"
      wait_port session-a-spot-router "$SESSION_A_ROUTER"
      wait_port session-a-alternate-object "$SESSION_A_ALTERNATE_OBJECT"
      wait_port session-a-stream "$SESSION_A_STREAM"
      wait_port session-a-tls-stream "$SESSION_A_TLS_STREAM"
      ;;
    session-b)
      wait_health "$SESSION_B_URL" session-b "${pid_by_name[session-b]:-}"
      wait_port session-b-control "$SESSION_B_CONTROL"
      wait_port session-b-spot-router "$SESSION_B_ROUTER"
      wait_port session-b-alternate-object "$SESSION_B_ALTERNATE_OBJECT"
      wait_port session-b-stream "$SESSION_B_STREAM"
      ;;
    gateway)
      wait_health "$GATEWAY_URL" gateway "${pid_by_name[gateway]:-}"
      wait_port gateway-router "$GATEWAY_ROUTER"
      ;;
    multi-node-a)
      wait_health "$MULTI_A_URL" multi-node-a "${pid_by_name[multi-node-a]:-}"
      if [[ "$SCENARIO" != "SM-F6" && "$SCENARIO" != "SM-G2" && "$SCENARIO" != "cross-mesh-actor-dispatch" ]]; then
        wait_port multi-node-a-route "$MULTI_A_ROUTE"
        wait_port multi-node-a-spot-pub "$MULTI_A_SPOT_PUB"
      fi
      wait_port multi-node-a-spot-router "$MULTI_A_SPOT_ROUTER"
      ;;
    multi-node-b)
      wait_health "$MULTI_B_URL" multi-node-b "${pid_by_name[multi-node-b]:-}"
      if [[ "$SCENARIO" != "SM-F6" && "$SCENARIO" != "SM-G2" ]]; then
        wait_port multi-node-b-route "$MULTI_B_ROUTE"
        wait_port multi-node-b-spot-pub "$MULTI_B_SPOT_PUB"
      fi
      wait_port multi-node-b-spot-router "$MULTI_B_SPOT_ROUTER"
      ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

wait_control_route() {
  local session_url="$1"
  local target_rid="$2"
  local name="$3"
  local marker="runner-ready-${name}-${RUN_ID}"
  local body
  body="$(printf '{"value":"%s"}' "$marker")"
  for _ in $(seq 1 "$((ROUTE_SETTLE_TIMEOUT_SECONDS * 10))"); do
    if curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS \
      -H 'content-type: application/json' \
      -d "$body" \
      "$session_url/channel/control-pingMsg/$target_rid" 2>/dev/null \
      | grep -F "\"nodeRid\":\"$target_rid\"" >/dev/null 2>&1; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for route $name through $session_url to $target_rid" >&2
  return 1
}

wait_topology_routes() {
  if [[ "$SCENARIO" == "SM-F6" || "$SCENARIO" == "SM-G2" || "$SCENARIO" == "SM-Q9" ]]; then
    return 0
  fi
  if [[ "$SCENARIO" == "SM-F3" || "$SCENARIO" == "SM-F4" || "$SCENARIO" == "SM-C4" ]]; then
    return 0
  fi
  if [[ "$SCENARIO" == "cross-mesh-actor-dispatch" ]]; then
    wait_control_route "$SESSION_A_URL" play-a session-a-to-play-a
    return 0
  fi
  if [[ "$SCENARIO" == "SM-F5" ]]; then
    local body='{"value":"route-ready"}'
    for _ in $(seq 1 "$((ROUTE_SETTLE_TIMEOUT_SECONDS * 10))"); do
      if curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS \
        -H 'content-type: application/json' \
        -d "$body" \
        "$PLAY_B_URL/channel/route/request" 2>/dev/null \
        | grep -F '"value":"echo-route-ready"' >/dev/null 2>&1; then
        return 0
      fi
      sleep "${LOCAL_READINESS_POLL_SECONDS}"
    done
    echo "Timed out waiting for play-b to play-a ChannelName route" >&2
    return 1
  fi
  wait_control_route "$SESSION_A_URL" play-a session-a-to-play-a
  wait_control_route "$SESSION_A_URL" play-b session-a-to-play-b
  wait_control_route "$SESSION_B_URL" play-a session-b-to-play-a
  wait_control_route "$SESSION_B_URL" play-b session-b-to-play-b
}

case "$SCENARIO" in
  SM-G2) SERVER_ROLES=(multi-node-a) ;;
  SM-F6|SM-Q9) SERVER_ROLES=(multi-node-a multi-node-b) ;;
  SM-C4) SERVER_ROLES=(play-a gateway) ;;
  SM-F3|SM-F4) SERVER_ROLES=(play-a) ;;
  SM-F5) SERVER_ROLES=(play-a play-b) ;;
  sm-d5|session-binding-e2e|session-binding-regression)
    SERVER_ROLES=(play-a play-b session-a session-b)
    ;;
  cross-mesh-actor-dispatch)
    SERVER_ROLES=(play-a session-a multi-node-a)
    ;;
  *) SERVER_ROLES=(play-a play-b session-a session-b gateway multi-node-a multi-node-b) ;;
esac
mapfile -t ORDERED_SERVER_ROLES < <(ordered_e2e_roles "$E2E_START_ORDER" "${SERVER_ROLES[@]}")
for role in "${ORDERED_SERVER_ROLES[@]}"; do
  start_named_server "$role"
done
for role in "${SERVER_ROLES[@]}"; do
  wait_named_server "$role"
done

wait_topology_routes

run_client() {
  local scenario="$1"
  echo "client scenario=${scenario}" >>"$LOG_DIR/client.stdout.log"
  local client_config="$CONFIG_DIR/client-${scenario}.json"
  node "$ROOT_DIR/write-config.mjs" "$client_config" \
    --string playAUrl "$PLAY_A_URL" --string playBUrl "$PLAY_B_URL" --string gatewayUrl "$GATEWAY_URL" \
    --string sessionAUrl "$SESSION_A_URL" --string sessionBUrl "$SESSION_B_URL" \
    --string sessionAStreamEndpoint "$SESSION_A_STREAM" --string sessionATlsStreamEndpoint "$SESSION_A_TLS_STREAM" \
    --string sessionBStreamEndpoint "$SESSION_B_STREAM" --string multiAUrl "$MULTI_A_URL" \
    --string multiBUrl "$MULTI_B_URL" --string scenario "$scenario"
  local -a browser_env=()
  if [[ "$scenario" == "sm-d14" || "$scenario" == "SM-D14" ]]; then
    browser_env=(env ZLINK_BROWSER_IGNORE_HTTPS_ERRORS=1)
  fi
  "${browser_env[@]}" node "$NODE_ROOT/scripts/browser-e2e/run-e2e-client.mjs" "$ROOT_DIR/Client/main.ts" -- \
    --config "$client_config" \
    >>"$LOG_DIR/client.stdout.log" 2>>"$LOG_DIR/client.stderr.log"
}

if [[ "$SCENARIO" == "SM-G2" ]]; then
  run_client SM-G2-PREPARE
  start_named_server multi-node-b
  wait_named_server multi-node-b
  run_client SM-G2-VERIFY
elif [[ "$SCENARIO" == "default-batch" ]]; then
  run_client SM-B2
  run_client SM-B1
  run_client SM-B3
  run_client SM-B5
  run_client SM-B9
  run_client sm-b6
  run_client sm-b8
  run_client sm-d1-d6
  run_client sm-d3
  run_client sm-d4
  run_client sm-d5
  run_client session-binding-regression
  run_client sm-d7
  run_client sm-d9-d11-d13
  run_client sm-d10
  run_client sm-d12
  run_client sm-d14
  run_client sm-c1-c2
  run_client sm-c3
  run_client SM-C5
  run_client SM-D15
  run_client sm-e4
  run_client sm-e1-f4
  run_client sm-e2-e3
  run_client sm-a7-a8-c4
  run_client sm-a3-a6-b4-b7
  run_client sm-a5
  run_client sm-a1-a2-a4-f1-f2
else
  run_client "$SCENARIO"
fi

cat "$LOG_DIR/client.stdout.log"
