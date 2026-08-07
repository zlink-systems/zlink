#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NODE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
source "$NODE_ROOT/e2e/redis-container.sh"
source "$NODE_ROOT/e2e/runner-common.sh"

SELECTOR="${1:-all}"
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

pids=()
REDIS_CONTAINER_ID=""
ROLE_MAIN="$ROOT_DIR/Role/dist/Role/main.js"
CLIENT_MAIN="$ROOT_DIR/Client/dist/Client/main.js"
declare -A ROLE_URL ROLE_PID ROLE_MARKER ROLE_RID ROLE_WORKFLOW_PORT

cleanup() {
  local code=$?
  if [[ "$code" -ne 0 ]]; then
    for role in "${!ROLE_URL[@]}"; do
      echo "----- $role evidence -----" >&2
      curl --max-time 2 -fsS "${ROLE_URL[$role]}/evidence" >&2 || true
      echo >&2
    done
    tail_failure_logs
  fi
  for role in "${!ROLE_PID[@]}"; do
    kill -CONT "${ROLE_PID[$role]}" >/dev/null 2>&1 || true
  done
  stop_live_pids
  wait_all_pids_ignoring_status
  remove_redis_container
  [[ -z "$CONFIG_DIR" ]] || rm -rf "$CONFIG_DIR"
}
trap cleanup EXIT

echo "log_dir=$LOG_DIR"
echo "scenario=$SELECTOR"

(cd "$NODE_ROOT" && npm run build >/dev/null)
build_package "$ROOT_DIR/Role"
build_package "$ROOT_DIR/Client"

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run ChannelEgressRouting because it provisions a Redis location store." >&2
  exit 1
fi

start_redis_container "zlink-redis-node-channel-egress-${RANDOM}-$$" -p "127.0.0.1::6379" "redis:7.2-alpine"
REDIS_ENDPOINT="$(redis_container_endpoint "$REDIS_CONTAINER_ID")"

stop_scenario_processes() {
  stop_live_pids
  wait_all_pids_ignoring_status
  pids=()
  ROLE_PID=()
}

role_url() {
  local role="$1"
  printf '%s' "${ROLE_URL[$role]:-http://127.0.0.1:1}"
}

start_role() {
  local scenario="$1"
  local role="$2"
  local rid="${3:-$role}"
  local config="$CONFIG_DIR/$scenario-$role.config.json"
  local http_port game_port audit_port workflow_port
  http_port="$(pick_port)"
  game_port="$(pick_port)"
  audit_port="$(pick_port)"
  workflow_port="$(pick_port)"
  ROLE_URL[$role]="http://127.0.0.1:$http_port"
  ROLE_RID[$role]="$rid"
  ROLE_WORKFLOW_PORT[$role]="$workflow_port"
  ROLE_MARKER[$role]="$RUN_ID-$scenario-$role-$(date +%s%N)"

  local args=(
    --role "$role"
    --rid "$rid"
    --http-url "${ROLE_URL[$role]}"
    --log-dir "$LOG_DIR/$scenario"
    --evidence-file "$LOG_DIR/$scenario/$role.evidence.log"
    --instance-marker "${ROLE_MARKER[$role]}"
    --redis-endpoint "$REDIS_ENDPOINT"
    --redis-key-prefix "channel-egress:$RUN_ID:$scenario"
    --game-endpoint "tcp://127.0.0.1:$game_port"
    --audit-endpoint "tcp://127.0.0.1:$audit_port"
    --workflow-port "$workflow_port"
    --workflow-weight 100
  )

  case "$role" in
    session)
      args+=(--game-server game.session --game-client game.play --game-client game.api)
      ;;
    play)
      args+=(--game-server game.play --game-client game.session --game-client game.api)
      args+=(--audit-client audit.record)
      if [[ "$scenario" == CH02 || "$scenario" == CH03 || "$scenario" == CH08 ]]; then
        args+=(--workflow-client true)
      fi
      ;;
    api-a|api-b)
      args+=(--game-server game.api)
      ;;
    audit)
      args+=(--audit-server audit.record)
      ;;
    workflow-a)
      args+=(--workflow-server true --workflow-weight 100)
      if [[ "$scenario" == CH12 ]]; then args+=(--workflow-client true); fi
      ;;
    workflow-b)
      args+=(--workflow-server true --workflow-weight 300)
      ;;
    caller)
      # Instance Spot request/reply uses the object client together with the
      # RouteMesh channel that owns the target Spot.
      args+=(--workflow-client true --game-client game.play)
      ;;
    negative)
      args+=(--workflow-client true --invalid-mode duplicate-workflow-client)
      ;;
    *)
      echo "Unknown ChannelEgressRouting role '$role'." >&2
      return 1
      ;;
  esac

  node "$ROOT_DIR/write-config.mjs" "$config" "${args[@]}"
  start_server "$role" "$ROLE_MAIN" --config "$config"
  ROLE_PID[$role]="$LAST_STARTED_PID"
}

wait_role() {
  local role="$1"
  wait_health "$(role_url "$role")" "$role" "${ROLE_PID[$role]}"
}

wait_route_channel() {
  local base_url="$1"
  local mesh="$2"
  local channel="$3"
  local minimum="$4"
  node - "$base_url" "$mesh" "$channel" "$minimum" <<'NODE'
const [baseUrl, mesh, channel, minimumText] = process.argv.slice(2);
const minimum = Number(minimumText);
const deadline = Date.now() + 20000;
let last;
(async () => {
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`${baseUrl}/status/route`);
      if (response.ok) {
        last = await response.json();
        const candidate = last.channels?.find((entry) => entry.channelName === channel);
        if (last.isReady === true && candidate?.isReady === true
            && Number(candidate.readyTargetCount) >= minimum) {
          return;
        }
      }
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`route ${mesh}/${channel} did not become ready: ${JSON.stringify(last)}`);
})().catch((error) => { console.error(error); process.exitCode = 1; });
NODE
}

wait_client_server() {
  local base_url="$1"
  local minimum="$2"
  node - "$base_url" "$minimum" <<'NODE'
const [baseUrl, minimumText] = process.argv.slice(2);
const minimum = Number(minimumText);
const deadline = Date.now() + 20000;
let last;
(async () => {
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`${baseUrl}/status/workflow`);
      if (response.ok) {
        last = await response.json();
        if (last.isReady === true && Number(last.readyTargetCount) >= minimum) return;
      }
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`workflow ClientServer did not become ready: ${JSON.stringify(last)}`);
})().catch((error) => { console.error(error); process.exitCode = 1; });
NODE
}

wait_disconnected_target() {
  local base_url="$1"
  node - "$base_url" <<'NODE'
const [baseUrl] = process.argv.slice(2);
const deadline = Date.now() + 25000;
let last;
(async () => {
  while (Date.now() < deadline) {
    const response = await fetch(`${baseUrl}/status/workflow`);
    last = await response.json();
    const target = last.targets?.find((entry) => entry.state === '3' || entry.state === 3);
    if (target?.state === '3' || target?.state === 3) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`workflow target was not retained as disconnected: ${JSON.stringify(last)}`);
})().catch((error) => { console.error(error); process.exitCode = 1; });
NODE
}

run_client() {
  local scenario="$1"
  local client_scenario
  case "$scenario" in
    CH01) client_scenario=CH-E2E-01 ;;
    CH02) client_scenario=CH-E2E-02 ;;
    CH03) client_scenario=CH-E2E-03 ;;
    CH04A) client_scenario=CH-E2E-04A ;;
    CH04B) client_scenario=CH-E2E-04B ;;
    CH04C) client_scenario=CH-E2E-04C ;;
    CH05) client_scenario=CH-E2E-05 ;;
    CH06) client_scenario=CH-E2E-06 ;;
    CH07A) client_scenario=CH-E2E-07A ;;
    CH07B) client_scenario=CH-E2E-07B ;;
    CH07C) client_scenario=CH-E2E-07C ;;
    CH08) client_scenario=CH-E2E-08 ;;
    CH09) client_scenario=CH-E2E-09 ;;
    CH10) client_scenario=CH-E2E-10 ;;
    CH11) client_scenario=CH-E2E-11 ;;
    CH12) client_scenario=CH-E2E-12 ;;
    *) echo "Unknown client scenario '$scenario'." >&2; return 1 ;;
  esac
  local config="$CONFIG_DIR/$scenario-client.config.json"
  node "$ROOT_DIR/write-config.mjs" "$config" \
    --scenario "$client_scenario" \
    --session-url "$(role_url session)" \
    --play-url "$(role_url play)" \
    --spot-caller-url "$(role_url caller)" \
    --api-a-url "$(role_url api-a)" \
    --api-b-url "$(role_url api-b)" \
    --workflow-caller-url "$(role_url caller)" \
    --workflow-a-url "$(role_url workflow-a)" \
    --workflow-b-url "$(role_url workflow-b)" \
    --audit-url "$(role_url audit)" \
    --invalid-url "$(role_url negative)" \
    --expected-workflow-lifecycle "${ROLE_MARKER[workflow-a]:-not-provided}" \
    --expected-workflow-rid "${ROLE_RID[workflow-a]:-not-provided}"
  node "$CLIENT_MAIN" --config "$config" \
    >"$LOG_DIR/$scenario-client.stdout.log" \
    2>"$LOG_DIR/$scenario-client.stderr.log"
  cat "$LOG_DIR/$scenario-client.stdout.log"
}

run_one() {
  local scenario="$1"
  CONFIG_DIR="$(mktemp -d)"
  chmod 700 "$CONFIG_DIR"
  ROLE_URL=()
  ROLE_PID=()
  ROLE_RID=()
  ROLE_WORKFLOW_PORT=()
  pids=()

  case "$scenario" in
    CH01) start_role "$scenario" session; start_role "$scenario" play ;;
    CH02) start_role "$scenario" session; start_role "$scenario" play; start_role "$scenario" audit; start_role "$scenario" workflow-a; start_role "$scenario" workflow-b ;;
    CH03) start_role "$scenario" play; start_role "$scenario" workflow-a; start_role "$scenario" caller ;;
    CH04A|CH04B) start_role "$scenario" workflow-a; start_role "$scenario" workflow-b; start_role "$scenario" caller ;;
    CH04C) start_role "$scenario" workflow-a; start_role "$scenario" caller ;;
    CH05) start_role "$scenario" workflow-a ;;
    CH06) start_role "$scenario" negative ;;
    CH07A) start_role "$scenario" session ;;
    CH07B) start_role "$scenario" api-a; start_role "$scenario" api-b ;;
    CH07C) start_role "$scenario" workflow-a; start_role "$scenario" caller ;;
    CH08) start_role "$scenario" session; start_role "$scenario" play; start_role "$scenario" audit; start_role "$scenario" workflow-a; start_role "$scenario" workflow-b ;;
    CH09) start_role "$scenario" session; start_role "$scenario" play; start_role "$scenario" workflow-a; start_role "$scenario" caller ;;
    CH10) start_role "$scenario" workflow-a; start_role "$scenario" workflow-b; start_role "$scenario" caller ;;
    CH11) start_role "$scenario" session; start_role "$scenario" api-a; start_role "$scenario" api-b ;;
    CH12) start_role "$scenario" workflow-a; start_role "$scenario" workflow-b ;;
    *) echo "Unknown ChannelEgressRouting scenario '$scenario'." >&2; return 1 ;;
  esac

  if [[ "$scenario" == CH06 ]]; then
    set +e
    wait "${ROLE_PID[negative]}"
    local negative_code=$?
    set -e
    if [[ "$negative_code" == 0 ]]; then
      echo "CH06 negative host unexpectedly exited successfully." >&2
      return 1
    fi
    run_client "$scenario"
    stop_scenario_processes
    return 0
  fi

  for role in "${!ROLE_PID[@]}"; do wait_role "$role"; done

  case "$scenario" in
    CH01) wait_route_channel "$(role_url play)" game game.play 1 ;;
    CH02) wait_route_channel "$(role_url play)" game game.play 1 ;;
    CH03)
      wait_client_server "$(role_url play)" 1
      curl --max-time "$HTTP_PROBE_TIMEOUT_SECONDS" -fsS "$(role_url play)/status/route"
      ;;
    CH04A|CH04B|CH04C|CH07C|CH10) wait_client_server "$(role_url caller)" 1 ;;
    CH12) wait_client_server "$(role_url workflow-a)" 1 ;;
    CH07B|CH11) : ;;
    CH09) wait_client_server "$(role_url caller)" 1 ;;
    CH08) wait_route_channel "$(role_url play)" game game.play 1 ;;
  esac

  if [[ "$scenario" == CH04C ]]; then
    pre_id="ch-04c-pre-$(date +%s%N)"
    node - "$(role_url caller)" "$pre_id" <<'NODE'
const [baseUrl, id] = process.argv.slice(2);
const response = await fetch(`${baseUrl}/request`, {
  method: 'POST',
  headers: { 'content-type': 'application/json' },
  body: JSON.stringify({ channel: 'workflow.command', id })
});
const result = await response.json();
if (result.succeeded !== true || result.reply?.role !== 'workflow-a') {
  throw new Error(`pre-restart request failed: ${JSON.stringify(result)}`);
}
NODE
    wait_file_contains "$LOG_DIR/$scenario/workflow-a.evidence.log" "id=$pre_id" "pre-restart workflow request did not reach workflow-a" "${ROLE_PID[workflow-a]}"
    kill -9 "${ROLE_PID[workflow-a]}" >/dev/null 2>&1 || true
    wait "${ROLE_PID[workflow-a]}" >/dev/null 2>&1 || true
    start_role "$scenario" workflow-a "workflow-a-restart-$RUN_ID"
    wait_role workflow-a
    wait_client_server "$(role_url caller)" 1
  elif [[ "$scenario" == CH07C ]]; then
    # Pause the server so its descriptor remains known, then close only the
    # established ClientServer TCP connection. The target must remain in the
    # caller snapshot as disconnected; removing its descriptor would test
    # NotFound instead of the required Unavailable result.
    kill -STOP "${ROLE_PID[workflow-a]}"
    ss -K dst 127.0.0.1 dport = "${ROLE_WORKFLOW_PORT[workflow-a]}" >/dev/null 2>&1 || true
    wait_disconnected_target "$(role_url caller)"
  fi

  run_client "$scenario"
  if [[ "$scenario" == CH07C ]]; then
    kill -CONT "${ROLE_PID[workflow-a]}" >/dev/null 2>&1 || true
  fi
  stop_scenario_processes
  rm -rf "$CONFIG_DIR"
  CONFIG_DIR=""
}

case "${SELECTOR^^}" in
  ALL)
    for scenario in CH01 CH02 CH03 CH04A CH04B CH04C CH05 CH06 CH07A CH07B CH07C CH08 CH09 CH10 CH11 CH12; do
      run_one "$scenario"
    done
    ;;
  *)
    IFS=',' read -r -a selected <<< "${SELECTOR^^}"
    for scenario in "${selected[@]}"; do run_one "$scenario"; done
    ;;
esac

echo "channel-egress-routing result=passed scenarios=$SELECTOR"
