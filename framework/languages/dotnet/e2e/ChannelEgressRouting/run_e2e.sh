#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../redis-common.sh"

SCENARIO="${*:-all}"
SCENARIO="${SCENARIO// /,}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
FANOUT_READINESS_TIMEOUT_SECONDS=20
CONFIG12_SCENARIOS=(
  CH-E2E-01 CH-E2E-02 CH-E2E-03
  CH-E2E-04A CH-E2E-04B CH-E2E-04C
  CH-E2E-05 CH-E2E-06
  CH-E2E-07A CH-E2E-07B CH-E2E-07C
  CH-E2E-08 CH-E2E-09 CH-E2E-10 CH-E2E-11 CH-E2E-12
  CH-REG-01 CH-REG-02 CH-REG-03 CH-REG-04 CH-REG-05
  CH-REG-06 CH-REG-07 CH-REG-08 CH-REG-09 CH-REG-10
)
if [[ "$SCENARIO" == "all" ]]; then
  for scenario in "${CONFIG12_SCENARIOS[@]}"; do
    "$0" "$scenario"
  done
  echo "channel-egress-routing e2e result=passed scenarios=${#CONFIG12_SCENARIOS[@]}"
  exit 0
fi
if [[ "$SCENARIO" == *,* ]]; then
  IFS=',' read -r -a selected_scenarios <<<"$SCENARIO"
  selected_count=0
  for scenario in "${selected_scenarios[@]}"; do
    [[ -n "$scenario" ]] || continue
    "$0" "$scenario"
    selected_count=$((selected_count + 1))
  done
  echo "channel-egress-routing e2e result=passed scenarios=$selected_count"
  exit 0
fi
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/logs/$RUN_ID"
CONFIG_DIR="$(mktemp -d)"
mkdir -p "$LOG_DIR"

SERVER_PROJECT="$ROOT_DIR/Server/ChannelEgressRouting.Server.csproj"
CLIENT_PROJECT="$ROOT_DIR/Client/ChannelEgressRouting.Client.csproj"
FIXTURE="$ROOT_DIR/../../../../doc/framework/common/e2e/fixtures/config-12-channel-egress-routing.json"
REDIS_CONTAINER=""
REDIS_ENDPOINT=""
pids=()
declare -A ROLE_PIDS=()

cleanup() {
  local code=$?
  for pid in "${pids[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true
    fi
  done
  sleep 0.3
  for pid in "${pids[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -KILL -- "-$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null || true
    fi
  done
  wait "${pids[@]:-}" 2>/dev/null || true
  if [[ -n "$REDIS_CONTAINER" ]]; then
    timeout -k 2s 10s docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  fi
  rm -rf "$CONFIG_DIR"
  if [[ "$code" != "0" ]]; then
    echo "ChannelEgressRouting failed. Logs: $LOG_DIR" >&2
  fi
}
trap cleanup EXIT

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

declare -A URLS=()
declare -A EVIDENCE=()

write_role_config() {
  local role="$1" rid="$2" url="$3" route_servers="$4" route_clients="$5"
  local workflow_client="$6" workflow_server="$7" weight="$8"
  local workflow_endpoint="${9:-}"
  local stream_endpoint="${10:-}"
  local route_endpoint="${11:-}"
  local route_advertise_host="${12:-}"
  local config="$CONFIG_DIR/$role.json"
  local evidence="$LOG_DIR/$role.evidence.log"
  EVIDENCE["$role"]="$evidence"
  local args=(
    --role "$role"
    --rid "$rid"
    --http-url "$url"
    --redis-endpoint "$REDIS_ENDPOINT"
    --redis-key-prefix "channel-egress:$RUN_ID:"
    --evidence-file "$evidence"
    --workflow-client "$workflow_client"
    --workflow-server "$workflow_server"
    --workflow-weight "$weight"
  )
  local channel
  IFS=',' read -ra channels <<<"$route_servers"
  for channel in "${channels[@]}"; do
    [[ -z "$channel" ]] || args+=(--route-server "$channel")
  done
  IFS=',' read -ra channels <<<"$route_clients"
  for channel in "${channels[@]}"; do
    [[ -z "$channel" ]] || args+=(--route-client "$channel")
  done
  [[ -z "$workflow_endpoint" ]] || args+=(--workflow-endpoint "$workflow_endpoint")
  [[ -z "$stream_endpoint" ]] || args+=(--stream-endpoint "$stream_endpoint")
  [[ -z "$route_endpoint" ]] || args+=(--route-endpoint "$route_endpoint")
  [[ -z "$route_advertise_host" ]] || args+=(--route-advertise-host "$route_advertise_host")
  python3 "$ROOT_DIR/../write_role_config.py" "$config" -- "${args[@]}"
}

start_role() {
  local role="$1" rid="$2" servers="$3" clients="$4"
  local workflow_client="$5" workflow_server="$6" weight="$7"
  local workflow_endpoint="${8:-}"
  local stream_endpoint="${9:-}"
  local route_endpoint="${10:-}"
  local route_advertise_host="${11:-}"
  local port url
  port="$(pick_port)"
  url="http://127.0.0.1:$port"
  URLS["$role"]="$url"
  write_role_config "$role" "$rid" "$url" "$servers" "$clients" \
    "$workflow_client" "$workflow_server" "$weight" "$workflow_endpoint" \
    "$stream_endpoint" "$route_endpoint" "$route_advertise_host"
  setsid dotnet run --no-build --project "$SERVER_PROJECT" -- \
    --config "$CONFIG_DIR/$role.json" \
    >"$LOG_DIR/$role.stdout.log" 2>"$LOG_DIR/$role.stderr.log" &
  pids+=("$!")
  ROLE_PIDS["$role"]="$!"
}

wait_json() {
  local url="$1" expression="$2" name="$3"
  local budget="${4:-$LOCAL_READINESS_TIMEOUT_SECONDS}"
  local deadline=$((SECONDS + budget))
  while (( SECONDS < deadline )); do
    if curl --max-time 1 --connect-timeout 1 -fsS "$url" 2>/dev/null \
      | python3 -c "import json,sys; value=json.load(sys.stdin); assert ($expression)" \
        >/dev/null 2>&1; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${budget}s for $name at $url" >&2
  curl --max-time 1 --connect-timeout 1 -fsS "$url" >&2 || true
  echo >&2
  return 1
}

python3 - "$FIXTURE" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    fixture = json.load(stream)
assert fixture["config"] == "ChannelEgressRouting"
assert fixture["roles"]["play"]["channels"]["audit.record"] == "client"
assert fixture["roles"]["workflowServer"]["clientServer"]["workflow.command"] == "client_and_server"
PY

echo "log_dir=$LOG_DIR"
zlink_redis_start_scoped_assign REDIS_CONTAINER REDIS_ENDPOINT \
  "zlink-dotnet-channel-egress" "redis:7.2-alpine" "$LOG_DIR"
zlink_redis_wait_ready "$REDIS_CONTAINER" 30 0.2

dotnet build "$SERVER_PROJECT" --maxcpucount:1
dotnet build "$CLIENT_PROJECT" --maxcpucount:1

SESSION_STREAM_ENDPOINT="tcp://127.0.0.1:$(pick_port)"
SERVER_ONLY_URL=""
SERVER_ONLY_EVIDENCE=""
start_role session 00-session \
  "game.session" "game.play,game.api" false false 100 "" \
  "$SESSION_STREAM_ENDPOINT"
start_role play 10-play \
  "game.play" "game.session,game.api,audit.record" true false 100
API_ROUTE_ENDPOINT=""
API_ROUTE_ADVERTISE_HOST=""
API_ROUTE_PORT=""
API_PROXY_PID=""
if [[ "$SCENARIO" == *"CH-E2E-07C"* ]]; then
  API_ROUTE_PORT="$(pick_port)"
  API_ROUTE_ENDPOINT="tcp://127.0.0.1:$API_ROUTE_PORT"
  API_ROUTE_ADVERTISE_HOST="127.0.0.2"
fi
start_role api 20-api \
  "game.api" "" false false 100 "" "" \
  "$API_ROUTE_ENDPOINT" "$API_ROUTE_ADVERTISE_HOST"
if [[ -n "$API_ROUTE_PORT" ]]; then
  setsid python3 "$ROOT_DIR/tcp_proxy.py" \
    --listen-host 127.0.0.2 \
    --listen-port "$API_ROUTE_PORT" \
    --upstream-host 127.0.0.1 \
    --upstream-port "$API_ROUTE_PORT" \
    >"$LOG_DIR/api-route-proxy.log" 2>&1 &
  API_PROXY_PID="$!"
  pids+=("$API_PROXY_PID")
  proxy_deadline=$((SECONDS + 5))
  while ! nc -z -w 1 127.0.0.2 "$API_ROUTE_PORT"; do
    if ! kill -0 "$API_PROXY_PID" 2>/dev/null; then
      cat "$LOG_DIR/api-route-proxy.log" >&2 || true
      exit 1
    fi
    if (( SECONDS >= proxy_deadline )); then
      echo "Timed out waiting for the API route proxy." >&2
      exit 1
    fi
    sleep 0.1
  done
fi
start_role audit 30-audit \
  "audit.record" "" false false 100
start_role workflow100 workflow-100 \
  "" "" true true 100
WORKFLOW300_ENDPOINT="tcp://127.0.0.1:$(pick_port)"
start_role workflow300 workflow-300 \
  "" "" true true 300 "$WORKFLOW300_ENDPOINT"
start_role workflow-client workflow-client \
  "" "" true false 100
if [[ "$SCENARIO" == "CH-E2E-05" ]]; then
  start_role workflow-server-only workflow-server-only \
    "" "" false true 0
  SERVER_ONLY_URL="${URLS[workflow-server-only]}"
  SERVER_ONLY_EVIDENCE="${EVIDENCE[workflow-server-only]}"
fi

for role in session play api audit workflow100 workflow300 workflow-client; do
  wait_json "${URLS[$role]}/health" \
    "'status' in value and value['status'] == 'ready'" "$role health"
done
if [[ -n "$SERVER_ONLY_URL" ]]; then
  wait_json "$SERVER_ONLY_URL/health" \
    "'status' in value and value['status'] == 'ready'" \
    "workflow-server-only health"
fi

wait_json "${URLS[session]}/topology/game" \
  "value['readyPeerCount'] >= 2 and value['channels'] and any(channel['channelName'] == 'game.play' and channel['isReady'] for channel in value['channels'])" "session game topology"
wait_json "${URLS[play]}/topology/game" \
  "value['readyPeerCount'] >= 1 and value['channels'] and any(channel['channelName'] == 'game.session' and channel['isReady'] for channel in value['channels'])" "play game topology"
wait_json "${URLS[play]}/topology/audit" \
  "value['channels'] and any(channel['channelName'] == 'audit.record' for channel in value['channels'])" "play audit topology"
wait_json "${URLS[workflow-client]}/client-server/workflow.command" \
  "value['isReady'] and value['readyTargetCount'] == 2" "workflow targets"
if [[ "$SCENARIO" == *"CH-REG-03"* || "$SCENARIO" == *"CH-E2E-09"* ]]; then
  # An automatic fanout publisher counts as ready once the subscriber
  # receives an application record or a liveness beacon (spec 24 §2.2), and
  # nothing is published before this point. The beacon interval is five
  # seconds, so the shared three-second budget cannot cover a single one.
  wait_json "${URLS[play]}/fanout-status" \
    "value['isReady'] and value['readyPublisherCount'] == 1" "fanout publisher" \
    "$FANOUT_READINESS_TIMEOUT_SECONDS"
fi

curl --max-time 2 -fsS "${URLS[session]}/topology/game" \
  >"$LOG_DIR/session.game.topology.json"
curl --max-time 2 -fsS "${URLS[play]}/topology/game" \
  >"$LOG_DIR/play.game.topology.json"
curl --max-time 2 -fsS "${URLS[api]}/topology/game" \
  >"$LOG_DIR/api.game.topology.json"
curl --max-time 2 -fsS "${URLS[session]}/locations" \
  >"$LOG_DIR/location.topology.json"

if [[ "$SCENARIO" == *"CH-REG-05"* || "$SCENARIO" == *"CH-E2E-04C"* ]]; then
  curl --max-time 2 -fsS \
    "${URLS[workflow-client]}/client-server/workflow.command" \
    >"$LOG_DIR/workflow.before-replacement.json"
  curl --max-time 2 -fsS -X POST "${URLS[workflow300]}/shutdown" >/dev/null
  wait "${ROLE_PIDS[workflow300]}" || true
  start_role workflow300replacement workflow-300-new \
    "" "" true true 300 "$WORKFLOW300_ENDPOINT"
  wait_json "${URLS[workflow300replacement]}/health" \
    "'status' in value and value['status'] == 'ready'" \
    "workflow replacement health"
  wait_json "${URLS[workflow-client]}/client-server/workflow.command" \
    "value['isReady'] and value['readyTargetCount'] == 2" \
    "workflow replacement target"
  curl --max-time 2 -fsS \
    "${URLS[workflow-client]}/client-server/workflow.command" \
    >"$LOG_DIR/workflow.after-replacement.json"
fi

if [[ "$SCENARIO" == *"CH-E2E-07C"* ]]; then
  # The API descriptor remains published, but its advertised TCP endpoint is
  # isolated behind this runner-owned proxy. Stopping the proxy blocks the
  # network path without changing membership or channel weight.
  kill -KILL -- "-$API_PROXY_PID" 2>/dev/null \
    || kill -KILL "$API_PROXY_PID" 2>/dev/null || true
  wait "$API_PROXY_PID" 2>/dev/null || true
  API_PROXY_PID=""
  wait_json "${URLS[session]}/topology/game" \
    "any(channel['channelName'] == 'game.api' and not channel['isReady'] and channel['readyTargetCount'] == 0 for channel in value['channels'])" \
    "game.api unavailable after network block" 20
  curl --max-time 2 -fsS "${URLS[session]}/topology/game" \
    >"$LOG_DIR/session.game.topology.after-network-block.json"
fi

python3 - "$CONFIG_DIR/client.json" "$SCENARIO" "$SERVER_PROJECT" \
  "$CONFIG_DIR" "$REDIS_ENDPOINT" "channel-egress:$RUN_ID:" "$LOG_DIR" \
  "$SESSION_STREAM_ENDPOINT" \
  "${URLS[session]}" "${URLS[play]}" "${URLS[api]}" "${URLS[audit]}" \
  "${URLS[workflow100]}" "${URLS[workflow300]}" "${URLS[workflow-client]}" \
  "$SERVER_ONLY_URL" \
  "${EVIDENCE[session]}" "${EVIDENCE[play]}" "${EVIDENCE[api]}" \
  "${EVIDENCE[audit]}" "${EVIDENCE[workflow100]}" \
  "${EVIDENCE[workflow300]}" "${EVIDENCE[workflow-client]}" \
  "$SERVER_ONLY_EVIDENCE" <<'PY'
import json
import os
import stat
import sys

(path, scenario, server_project, config_dir, redis, prefix, log_dir,
 stream_endpoint,
 session, play, api, audit, w100, w300, wc, server_only,
 es, ep, ea, eau, ew100, ew300, ewc, eserver_only) = sys.argv[1:]
value = {
    "Options": {
        "Scenario": scenario,
        "Urls": {
            "session": session, "play": play, "api": api, "audit": audit,
            "workflow100": w100, "workflow300": w300, "workflow-client": wc,
        },
        "EvidenceFiles": {
            "session": es, "play": ep, "api": ea, "audit": eau,
            "workflow100": ew100, "workflow300": ew300, "workflow-client": ewc,
        },
        "InvalidServerProject": server_project,
        "ConfigDir": config_dir,
        "RedisEndpoint": redis,
        "RedisKeyPrefix": prefix,
        "LogDir": log_dir,
        "StreamEndpoint": stream_endpoint,
    }
}
if server_only:
    value["Options"]["Urls"]["workflow-server-only"] = server_only
    value["Options"]["EvidenceFiles"]["workflow-server-only"] = eserver_only
with open(path, "w", encoding="utf-8") as stream:
    json.dump(value, stream, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY

dotnet run --no-build --project "$CLIENT_PROJECT" -- \
  --config "$CONFIG_DIR/client.json" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
cat "$LOG_DIR/client.stdout.log"
