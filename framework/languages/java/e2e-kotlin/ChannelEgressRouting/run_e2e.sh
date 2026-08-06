#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAVA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "${JAVA_ROOT}/e2e-redis-common.sh"
source "${JAVA_ROOT}/e2e/start-order-common.sh"

cd "${SCRIPT_DIR}"

SELECTOR="${1:-all}"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="${SCRIPT_DIR}/logs/${RUN_ID}"
CONFIG_DIR="$(mktemp -d)"
GRADLE_CACHE_DIR="${HOME}/.cache/zlink/kotlin-e2e/ChannelEgressRouting-gradle-cache"
REDIS_CONTAINER=""
REDIS_ENDPOINT=""
LOCATION_KEY_PREFIX="zlink:e2e:kotlin-channel-egress:${RUN_ID}"

declare -a PIDS=()
declare -A ROLE_PID ROLE_HTTP ROLE_GAME ROLE_AUDIT ROLE_WORKFLOW ROLE_RID ROLE_CONFIG
declare -A ROLE_GAME_BIND_HOST ROLE_GAME_ADVERTISE_HOST
declare -A ROLE_WORKFLOW_BIND_HOST ROLE_WORKFLOW_ADVERTISE_HOST
declare -A ROLE_FANOUT_PUBLISHER ROLE_FANOUT_SUBSCRIBER ROLE_STREAM_SERVER
GAME_PROXY_PID=""

mkdir -p "${LOG_DIR}"
chmod 0700 "${CONFIG_DIR}"
echo "log_dir=${LOG_DIR}"
echo "scenario=${SELECTOR}"
echo "start_order=$(zlink_e2e_start_order_mode "$@")"

cleanup_processes() {
  local pid
  if [[ -n "${GAME_PROXY_PID}" ]]; then
    kill -KILL -- "-${GAME_PROXY_PID}" >/dev/null 2>&1 \
      || kill -KILL "${GAME_PROXY_PID}" >/dev/null 2>&1 || true
    wait "${GAME_PROXY_PID}" >/dev/null 2>&1 || true
    GAME_PROXY_PID=""
  fi
  for pid in "${PIDS[@]}"; do kill -TERM "${pid}" >/dev/null 2>&1 || true; done
  sleep 0.2
  for pid in "${PIDS[@]}"; do kill -KILL "${pid}" >/dev/null 2>&1 || true; done
  wait >/dev/null 2>&1 || true
  PIDS=()
  ROLE_PID=()
}

print_failure_logs() {
  local status="$1"
  [[ "${status}" == "0" ]] && return
  local role log
  for role in "${!ROLE_HTTP[@]}"; do
    echo "===== ${role} evidence =====" >&2
    curl --max-time 1 -fsS "${ROLE_HTTP[${role}]}/evidence" >&2 || true
    echo >&2
  done
  for log in "${LOG_DIR}"/*.log "${LOG_DIR}"/*/*.log "${LOG_DIR}"/*/*/*.log; do
    [[ -f "${log}" ]] || continue
    echo "===== ${log} =====" >&2
    tail -n 160 "${log}" >&2 || true
  done
}

cleanup() {
  local status="$?"
  set +e
  print_failure_logs "${status}"
  cleanup_processes
  if [[ -n "${REDIS_CONTAINER}" ]]; then docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true; fi
  rm -rf "${CONFIG_DIR}"
  exit "${status}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ -z "${ZLINK_LIBRARY_PATH:-}" ]]; then
  repo_root="$(cd "${SCRIPT_DIR}/../../../../.." && pwd)"
  default_core_lib="${repo_root}/core/build/lib/libzlink.so"
  [[ -f "${default_core_lib}" ]] && export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi

command -v docker >/dev/null 2>&1 || {
  echo "Docker is required for ChannelEgressRouting because it provisions a Redis location store." >&2
  exit 1
}

zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-kotlin-e2e-channel-egress" "redis:7.2-alpine" "127.0.0.1::6379"
REDIS_ENDPOINT="127.0.0.1:${redis_port}"

gradle_run() {
  "${JAVA_ROOT}/gradlew" -p "${SCRIPT_DIR}" \
    --project-cache-dir "${GRADLE_CACHE_DIR}" \
    --no-daemon --no-parallel --max-workers=1 "$@" --quiet
}

if [[ "${ZLINK_E2E_SKIP_BUILD:-0}" != "1" ]]; then
  gradle_run :Role:installDist :Client:installDist
fi
ROLE_BIN="${SCRIPT_DIR}/Role/build/install/channel-egress-kotlin-role/bin/channel-egress-kotlin-role"
CLIENT_BIN="${SCRIPT_DIR}/Client/build/install/channel-egress-kotlin-client/bin/channel-egress-kotlin-client"

free_port() {
  python3 - <<'PY'
import socket
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
}

allocate_role() {
  local role="$1"
  ROLE_HTTP["${role}"]="http://127.0.0.1:$(free_port)"
  ROLE_GAME["${role}"]="tcp://127.0.0.1:$(free_port)"
  ROLE_AUDIT["${role}"]="tcp://127.0.0.1:$(free_port)"
  ROLE_WORKFLOW["${role}"]="$(free_port)"
  mkdir -p "${LOG_DIR}/${CURRENT_SCENARIO}/${role}"
}

write_role_config() {
  local role="$1" rid="$2" game_peer_rids="$3" game_peer_endpoints="$4"
  local game_servers="$5" game_clients="$6" audit_peer_rids="$7" audit_peer_endpoints="$8"
  local audit_servers="$9" audit_clients="${10}" workflow_client="${11}"
  local workflow_server="${12}" workflow_weight="${13}"
  local instance_spot="${14:-false}" object_client="${15:-false}"
  local path="${CONFIG_DIR}/${CURRENT_SCENARIO}-${role}.properties"
  {
    printf 'e2e.role=%s\n' "${role}"
    printf 'e2e.rid=%s\n' "${rid}"
    printf 'e2e.instance-marker=%s\n' "${RUN_ID}-${CURRENT_SCENARIO}-${role}"
    printf 'e2e.http-endpoint=%s\n' "${ROLE_HTTP[${role}]}"
    printf 'e2e.redis-location-endpoint=%s\n' "${REDIS_ENDPOINT}"
    printf 'e2e.location-key-prefix=%s\n' "${LOCATION_KEY_PREFIX}-${CURRENT_SCENARIO}"
    printf 'e2e.log-directory=%s\n' "${LOG_DIR}/${CURRENT_SCENARIO}/${role}"
    printf 'e2e.game-endpoint=%s\n' "${ROLE_GAME[${role}]}"
    printf 'e2e.game-bind-host=%s\n' "${ROLE_GAME_BIND_HOST[${role}]:-127.0.0.1}"
    printf 'e2e.game-advertise-host=%s\n' "${ROLE_GAME_ADVERTISE_HOST[${role}]:-127.0.0.1}"
    printf 'e2e.game-peer-rids=%s\n' "${game_peer_rids}"
    printf 'e2e.game-peer-endpoints=%s\n' "${game_peer_endpoints}"
    printf 'e2e.game-servers=%s\n' "${game_servers}"
    printf 'e2e.game-clients=%s\n' "${game_clients}"
    printf 'e2e.audit-endpoint=%s\n' "${ROLE_AUDIT[${role}]}"
    printf 'e2e.audit-peer-rids=%s\n' "${audit_peer_rids}"
    printf 'e2e.audit-peer-endpoints=%s\n' "${audit_peer_endpoints}"
    printf 'e2e.audit-servers=%s\n' "${audit_servers}"
    printf 'e2e.audit-clients=%s\n' "${audit_clients}"
    printf 'e2e.workflow-bind-host=%s\n' "${ROLE_WORKFLOW_BIND_HOST[${role}]:-127.0.0.1}"
    printf 'e2e.workflow-advertise-host=%s\n' "${ROLE_WORKFLOW_ADVERTISE_HOST[${role}]:-127.0.0.1}"
    printf 'e2e.workflow-port=%s\n' "${ROLE_WORKFLOW[${role}]}"
    printf 'e2e.workflow-weight=%s\n' "${workflow_weight}"
    printf 'e2e.workflow-client=%s\n' "${workflow_client}"
    printf 'e2e.workflow-server=%s\n' "${workflow_server}"
    printf 'e2e.instance-spot=%s\n' "${instance_spot}"
    printf 'e2e.object-client=%s\n' "${object_client}"
    printf 'e2e.fanout-publisher=%s\n' "${ROLE_FANOUT_PUBLISHER[${role}]:-false}"
    printf 'e2e.fanout-subscriber=%s\n' "${ROLE_FANOUT_SUBSCRIBER[${role}]:-false}"
    printf 'e2e.fanout-bind-host=%s\n' "${ROLE_GAME_BIND_HOST[${role}]:-127.0.0.1}"
    printf 'e2e.fanout-advertise-host=%s\n' "${ROLE_GAME_ADVERTISE_HOST[${role}]:-127.0.0.1}"
    printf 'e2e.fanout-port=0\n'
    printf 'e2e.stream-server=%s\n' "${ROLE_STREAM_SERVER[${role}]:-false}"
    printf 'e2e.stream-bind-host=%s\n' "${ROLE_GAME_BIND_HOST[${role}]:-127.0.0.1}"
    printf 'e2e.stream-advertise-host=%s\n' "${ROLE_GAME_ADVERTISE_HOST[${role}]:-127.0.0.1}"
    printf 'e2e.stream-port=0\n'
  } >"${path}"
  chmod 0600 "${path}"
  ROLE_RID["${role}"]="${rid}"
  ROLE_CONFIG["${role}"]="${path}"
}

start_role() {
  local role="$1"
  local path="${ROLE_CONFIG[${role}]}"
  "${ROLE_BIN}" --config "${path}" \
    >"${LOG_DIR}/${CURRENT_SCENARIO}/${role}/stdout.log" \
    2>"${LOG_DIR}/${CURRENT_SCENARIO}/${role}/stderr.log" &
  local pid="$!"
  ROLE_PID["${role}"]="${pid}"
  PIDS+=("${pid}")
}

wait_health() {
  local role="$1"
  local url="${ROLE_HTTP[${role}]}"
  local pid="${ROLE_PID[${role}]}"
  local attempt
  for attempt in $(seq 1 120); do
    curl --max-time 1 -fsS "${url}/health" >/dev/null 2>&1 && return 0
    kill -0 "${pid}" >/dev/null 2>&1 || { echo "${role} exited before health became ready" >&2; return 1; }
    sleep 0.1
  done
  echo "timed out waiting for ${role} health at ${url}" >&2
  return 1
}

wait_route() {
  local role="$1" channel="$2" status_path="${3:-/status/route}" minimum="${4:-1}" minimum_peers="${5:-1}"
  python3 - "${ROLE_HTTP[${role}]}" "${channel}" "${status_path}" "${minimum}" "${minimum_peers}" <<'PY'
import json, sys, time, urllib.request
base, channel, path, minimum, peers = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
deadline, last = time.monotonic() + 25, None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(base + path, timeout=1) as response: last = json.load(response)
        target = next((x for x in last.get("channels", []) if x.get("channelName") == channel), None)
        if int(last.get("readyPeerCount", 0)) >= peers and target and target.get("isReady") is True and int(target.get("readyTargetCount", 0)) >= minimum: raise SystemExit(0)
    except OSError: pass
    time.sleep(.1)
print("route channel did not become ready: " + json.dumps(last), file=sys.stderr)
raise SystemExit(1)
PY
}

wait_workflow() {
  local role="$1" expected="${2:-1}"
  python3 - "${ROLE_HTTP[${role}]}" "${expected}" <<'PY'
import json, sys, time, urllib.request
base, expected = sys.argv[1], int(sys.argv[2]); deadline, last = time.monotonic() + 25, None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(base + "/status/workflow", timeout=1) as response: last = json.load(response)
        if last.get("isReady") is True and int(last.get("readyTargetCount", 0)) == expected: raise SystemExit(0)
    except OSError: pass
    time.sleep(.1)
print("workflow channel did not become ready: " + json.dumps(last), file=sys.stderr)
raise SystemExit(1)
PY
}

wait_fanout() {
  local role="$1" expected="${2:-1}"
  python3 - "${ROLE_HTTP[${role}]}" "${expected}" <<'PY'
import json, sys, time, urllib.request
base, expected = sys.argv[1], int(sys.argv[2]); deadline, last = time.monotonic() + 25, None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(base + "/status/fanout", timeout=1) as response: last = json.load(response)
        if last.get("isReady") is True and int(last.get("readyPublisherCount", 0)) >= expected: raise SystemExit(0)
    except OSError: pass
    time.sleep(.1)
print("fanout did not become ready: " + json.dumps(last), file=sys.stderr)
raise SystemExit(1)
PY
}

wait_route_unavailable() {
  local role="$1" channel="$2"
  python3 - "${ROLE_HTTP[${role}]}" "${channel}" <<'PY'
import json, sys, time, urllib.request
base, channel = sys.argv[1:]; deadline, last = time.monotonic() + 25, None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(base + "/status/route", timeout=1) as response: last = json.load(response)
        target = next((x for x in last.get("channels", []) if x.get("channelName") == channel), None)
        if target and target.get("isReady") is False and int(target.get("readyTargetCount", -1)) == 0: raise SystemExit(0)
    except OSError: pass
    time.sleep(.1)
print("route channel did not become unavailable: " + json.dumps(last), file=sys.stderr)
raise SystemExit(1)
PY
}

stop_role() {
  local role="$1"
  local pid="${ROLE_PID[${role}]}"
  kill -TERM "${pid}" >/dev/null 2>&1 || true
  for _ in $(seq 1 60); do
    if ! kill -0 "${pid}" >/dev/null 2>&1; then
      break
    fi
    sleep 0.1
  done
  kill -0 "${pid}" >/dev/null 2>&1 && kill -KILL "${pid}" >/dev/null 2>&1 || true
  wait "${pid}" >/dev/null 2>&1 || true
  local -a remaining=()
  local tracked
  for tracked in "${PIDS[@]}"; do
    [[ "${tracked}" == "${pid}" ]] || remaining+=("${tracked}")
  done
  PIDS=("${remaining[@]}")
  unset 'ROLE_PID['"${role}"']'
}

start_game_proxy() {
  local role="$1"
  local endpoint="${ROLE_GAME[${role}]}"
  local port="${endpoint##*:}"
  setsid python3 "${JAVA_ROOT}/e2e/ChannelEgressRouting/tcp_proxy.py" \
    --listen-host 127.0.0.2 \
    --listen-port "${port}" \
    --upstream-host 127.0.0.1 \
    --upstream-port "${port}" \
    >"${LOG_DIR}/${CURRENT_SCENARIO}/game-route-proxy.log" 2>&1 &
  GAME_PROXY_PID="$!"
  for _ in $(seq 1 50); do
    if rg -q '^proxy ready ' "${LOG_DIR}/${CURRENT_SCENARIO}/game-route-proxy.log" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "${GAME_PROXY_PID}" >/dev/null 2>&1; then
      cat "${LOG_DIR}/${CURRENT_SCENARIO}/game-route-proxy.log" >&2 || true
      return 1
    fi
    sleep 0.1
  done
  echo "timed out waiting for game route proxy" >&2
  return 1
}

stop_game_proxy() {
  if [[ -z "${GAME_PROXY_PID}" ]]; then
    return 0
  fi
  kill -KILL -- "-${GAME_PROXY_PID}" >/dev/null 2>&1 \
    || kill -KILL "${GAME_PROXY_PID}" >/dev/null 2>&1 || true
  wait "${GAME_PROXY_PID}" >/dev/null 2>&1 || true
  GAME_PROXY_PID=""
}

assert_invalid_startup() {
  local role="invalid-duplicate"
  allocate_role "${role}"
  write_role_config "${role}" "${role}" "" "" workflow.command "" "" "" "" "" true false 0
  start_role "${role}"
  local pid="${ROLE_PID[${role}]}"
  for _ in $(seq 1 80); do
    if ! kill -0 "${pid}" >/dev/null 2>&1; then
      wait "${pid}" >/dev/null 2>&1 && { echo "duplicate ChannelName host exited successfully" >&2; return 1; }
      rg -qi 'channelname|already registered|duplicate' \
        "${LOG_DIR}/${CURRENT_SCENARIO}/${role}/stderr.log" \
        "${LOG_DIR}/${CURRENT_SCENARIO}/${role}/stdout.log" || {
          echo "duplicate ChannelName host did not report a configuration error" >&2
          return 1
        }
      return 0
    fi
    curl --max-time 1 -fsS "${ROLE_HTTP[${role}]}/health" >/dev/null 2>&1 && {
      echo "duplicate ChannelName host became healthy" >&2
      return 1
    }
    sleep 0.1
  done
  echo "duplicate ChannelName host did not terminate" >&2
  return 1
}

run_client() {
  local scenario="$1"
  local fallback="${ROLE_HTTP[session]:-${ROLE_HTTP[caller]:-${ROLE_HTTP[workflow-a]:-${ROLE_HTTP[api-a]:-${ROLE_HTTP[play]:-}}}}}"
  local path="${CONFIG_DIR}/${CURRENT_SCENARIO}-client.properties"
  {
    printf 'sessionEndpoint=%s\n' "${ROLE_HTTP[session]:-${fallback}}"
    printf 'playEndpoint=%s\n' "${ROLE_HTTP[play]:-${fallback}}"
    printf 'auditEndpoint=%s\n' "${ROLE_HTTP[audit]:-${fallback}}"
    printf 'workflowAEndpoint=%s\n' "${ROLE_HTTP[workflow-a]:-${fallback}}"
    printf 'workflowBEndpoint=%s\n' "${ROLE_HTTP[workflow-b]:-${fallback}}"
    printf 'apiAEndpoint=%s\n' "${ROLE_HTTP[api-a]:-${fallback}}"
    printf 'apiBEndpoint=%s\n' "${ROLE_HTTP[api-b]:-${fallback}}"
    printf 'callerEndpoint=%s\n' "${ROLE_HTTP[caller]:-${fallback}}"
    printf 'fanoutSubscriberEndpoint=%s\n' "${ROLE_HTTP[fanout-sub]:-${fallback}}"
  } >"${path}"
  chmod 0600 "${path}"
  "${CLIENT_BIN}" --config "${path}" --scenario "${scenario}" \
    >"${LOG_DIR}/${CURRENT_SCENARIO}/client.stdout.log" \
    2>"${LOG_DIR}/${CURRENT_SCENARIO}/client.stderr.log"
  cat "${LOG_DIR}/${CURRENT_SCENARIO}/client.stdout.log"
}

run_one() {
  CURRENT_SCENARIO="$1"
  cleanup_processes
  ROLE_PID=(); ROLE_HTTP=(); ROLE_GAME=(); ROLE_AUDIT=(); ROLE_WORKFLOW=(); ROLE_RID=(); ROLE_CONFIG=()
  ROLE_GAME_BIND_HOST=(); ROLE_GAME_ADVERTISE_HOST=(); ROLE_WORKFLOW_BIND_HOST=(); ROLE_WORKFLOW_ADVERTISE_HOST=()
  ROLE_FANOUT_PUBLISHER=(); ROLE_FANOUT_SUBSCRIBER=(); ROLE_STREAM_SERVER=()
  mkdir -p "${LOG_DIR}/${CURRENT_SCENARIO}"

  case "${CURRENT_SCENARIO}" in
    CH-E2E-01)
      allocate_role session; allocate_role play
      write_role_config session session play "${ROLE_GAME[play]}" game.session game.play "" "" "" "" false false 0
      write_role_config play play session "${ROLE_GAME[session]}" game.play game.session "" "" "" "" false false 0
      ;;
    CH-E2E-02)
      allocate_role session; allocate_role play; allocate_role audit; allocate_role workflow-a; allocate_role workflow-b
      write_role_config session session play "${ROLE_GAME[play]}" game.session game.play "" "" "" "" false false 0
      write_role_config play play session "${ROLE_GAME[session]}" game.play game.session audit-audit "${ROLE_AUDIT[audit]}" "" audit.record true false 0
      write_role_config audit audit "" "" "" "" play-audit "${ROLE_AUDIT[play]}" audit.record "" false false 0
      write_role_config workflow-a workflow-a "" "" "" "" "" "" "" "" false true 100
      write_role_config workflow-b workflow-b "" "" "" "" "" "" "" "" false true 300
      ;;
    CH-E2E-03)
      allocate_role play; allocate_role workflow-a
      write_role_config play play "" "" game.play "" "" "" "" "" true false 0 true true
      write_role_config workflow-a workflow-a "" "" "" "" "" "" "" "" false true 100
      ;;
    CH-E2E-04A|CH-E2E-04B|CH-E2E-10)
      allocate_role caller; allocate_role workflow-a; allocate_role workflow-b
      write_role_config caller caller "" "" "" "" "" "" "" "" true false 0
      write_role_config workflow-a workflow-a "" "" "" "" "" "" "" "" false true 100
      write_role_config workflow-b workflow-b "" "" "" "" "" "" "" "" false true 300
      ;;
    CH-E2E-04C)
      allocate_role caller; allocate_role workflow-a
      write_role_config caller caller "" "" "" "" "" "" "" "" true false 0
      write_role_config workflow-a workflow-old "" "" "" "" "" "" "" "" false true 100
      ;;
    CH-E2E-05)
      allocate_role caller; allocate_role workflow-a; allocate_role workflow-b
      write_role_config caller caller "" "" "" "" "" "" "" "" true false 0
      write_role_config workflow-a workflow-a "" "" "" "" "" "" "" "" false true 100
      write_role_config workflow-b workflow-server-only "" "" "" "" "" "" "" "" false true 100
      ;;
    CH-E2E-06)
      assert_invalid_startup
      unset 'ROLE_PID[invalid-duplicate]' 'ROLE_CONFIG[invalid-duplicate]'
      allocate_role session; allocate_role play; allocate_role caller; allocate_role workflow-a
      write_role_config session session play "${ROLE_GAME[play]}" game.session game.play "" "" "" "" false false 0
      write_role_config play play session "${ROLE_GAME[session]}" game.play "" "" "" "" "" false false 0
      write_role_config caller caller "" "" "" "" "" "" "" "" true false 0
      write_role_config workflow-a workflow-a "" "" "" "" "" "" "" "" false true 100
      ;;
    CH-E2E-07A)
      allocate_role session
      write_role_config session session "" "" game.session "" "" "" "" "" false false 0
      ;;
    CH-E2E-07B)
      allocate_role api-a; allocate_role api-b
      write_role_config api-a api-a api-b "${ROLE_GAME[api-b]}" game.api "" "" "" "" "" false false 0
      write_role_config api-b api-b api-a "${ROLE_GAME[api-a]}" game.api "" "" "" "" "" false false 0
      ;;
    CH-E2E-07C)
      allocate_role session; allocate_role api-a
      ROLE_GAME_ADVERTISE_HOST[api-a]="127.0.0.2"
      local api_rid="z-api-a"
      local api_port="${ROLE_GAME[api-a]##*:}"
      write_role_config session session "" "" game.session game.api "" "" "" "" false false 0
      write_role_config api-a "${api_rid}" "" "" game.api "" "" "" "" "" false false 0
      ;;
    CH-E2E-08)
      allocate_role play; allocate_role workflow-a; allocate_role caller
      write_role_config play play workflow-a "${ROLE_GAME[workflow-a]}" game.play "" "" "" "" "" false false 0 true true
      write_role_config workflow-a workflow-a play "${ROLE_GAME[play]}" "" game.play "" "" "" "" false true 100 false true
      write_role_config caller caller "" "" "" "" "" "" "" "" true false 0
      ;;
    CH-E2E-09)
      allocate_role session; allocate_role api-a; allocate_role caller; allocate_role workflow-a; allocate_role fanout-sub
      ROLE_GAME[api-a]="tcp://0.0.0.0:0"; ROLE_GAME_BIND_HOST[api-a]="0.0.0.0"
      ROLE_WORKFLOW[workflow-a]=0; ROLE_WORKFLOW_BIND_HOST[workflow-a]="0.0.0.0"
      ROLE_FANOUT_PUBLISHER[api-a]=true; ROLE_STREAM_SERVER[api-a]=true
      ROLE_FANOUT_SUBSCRIBER[fanout-sub]=true
      write_role_config session session "" "" game.session game.api "" "" "" "" false false 0
      write_role_config api-a api-a session "${ROLE_GAME[session]}" game.api "" "" "" "" "" false false 0
      write_role_config caller caller "" "" "" "" "" "" "" "" true false 0
      write_role_config workflow-a workflow-a "" "" "" "" "" "" "" "" false true 100
      write_role_config fanout-sub fanout-sub "" "" "" "" "" "" "" "" false false 0
      ;;
    CH-E2E-11)
      allocate_role session; allocate_role api-a; allocate_role api-b
      write_role_config session session "api-a,api-b" "${ROLE_GAME[api-a]},${ROLE_GAME[api-b]}" "" game.api "" "" "" "" false false 0
      write_role_config api-a api-a session "${ROLE_GAME[session]}" game.api "" "" "" "" "" false false 0
      write_role_config api-b api-b session "${ROLE_GAME[session]}" game.api "" "" "" "" "" false false 0
      ;;
    CH-E2E-12)
      allocate_role workflow-a; allocate_role workflow-b
      write_role_config workflow-a workflow-a "" "" "" "" "" "" "" "" true true 100
      write_role_config workflow-b workflow-b "" "" "" "" "" "" "" "" false true 100
      ;;
    *) echo "unsupported ChannelEgressRouting selector ${CURRENT_SCENARIO}" >&2; return 2 ;;
  esac

  local role
  if [[ "${CURRENT_SCENARIO}" == "CH-E2E-07C" ]]; then
    start_role api-a
    wait_health api-a
    start_game_proxy api-a
    start_role session
    wait_health session
  else
    for role in "${!ROLE_CONFIG[@]}"; do [[ -n "${ROLE_PID[${role}]:-}" ]] || start_role "${role}"; done
    for role in "${!ROLE_PID[@]}"; do wait_health "${role}"; done
  fi

  case "${CURRENT_SCENARIO}" in
    CH-E2E-01) wait_route session game.play; wait_route play game.session ;;
    CH-E2E-02) wait_route session game.play; wait_route play game.session; wait_route play audit.record /status/audit; wait_workflow play 2 ;;
    CH-E2E-03) wait_workflow play 1 ;;
    CH-E2E-04A|CH-E2E-04B|CH-E2E-10) wait_workflow caller 2 ;;
    CH-E2E-04C)
      wait_workflow caller 1
      stop_role workflow-a
      workflow_port="${ROLE_WORKFLOW[workflow-a]}"
      allocate_role workflow-a; ROLE_WORKFLOW[workflow-a]="${workflow_port}"
      write_role_config workflow-a workflow-new "" "" "" "" "" "" "" "" false true 100
      start_role workflow-a; wait_health workflow-a; wait_workflow caller 1
      ;;
    CH-E2E-05) wait_workflow caller 2 ;;
    CH-E2E-06) wait_route session game.play; wait_workflow caller 1 ;;
    CH-E2E-07B) wait_route api-a game.api ;;
    CH-E2E-07C) wait_route session game.api; stop_game_proxy; wait_route_unavailable session game.api ;;
    CH-E2E-08) wait_route workflow-a game.play; wait_workflow caller 1 ;;
    CH-E2E-09) wait_route session game.api; wait_workflow caller 1; wait_fanout fanout-sub 1 ;;
    CH-E2E-11) wait_route session game.api /status/route 2 2 ;;
    CH-E2E-12) wait_workflow workflow-a 2 ;;
  esac
  run_client "${CURRENT_SCENARIO}"
  echo "scenario ${CURRENT_SCENARIO} passed"
}

SCENARIOS=(
  CH-E2E-01 CH-E2E-02 CH-E2E-03 CH-E2E-04A CH-E2E-04B CH-E2E-04C
  CH-E2E-05 CH-E2E-06 CH-E2E-07A CH-E2E-07B CH-E2E-07C
  CH-E2E-08 CH-E2E-09 CH-E2E-10 CH-E2E-11 CH-E2E-12
)

case "${SELECTOR^^}" in
  ALL) for scenario in "${SCENARIOS[@]}"; do run_one "${scenario}"; done ;;
  CH01) run_one CH-E2E-01 ;; CH02) run_one CH-E2E-02 ;; CH03) run_one CH-E2E-03 ;;
  CH04A) run_one CH-E2E-04A ;; CH04B) run_one CH-E2E-04B ;; CH04C) run_one CH-E2E-04C ;;
  CH05) run_one CH-E2E-05 ;; CH06) run_one CH-E2E-06 ;;
  CH07A) run_one CH-E2E-07A ;; CH07B) run_one CH-E2E-07B ;; CH07C) run_one CH-E2E-07C ;;
  CH08) run_one CH-E2E-08 ;; CH09) run_one CH-E2E-09 ;; CH10) run_one CH-E2E-10 ;;
  CH11) run_one CH-E2E-11 ;; CH12) run_one CH-E2E-12 ;;
  CH-E2E-*) run_one "${SELECTOR^^}" ;;
  *) echo "unsupported ChannelEgressRouting selector ${SELECTOR}" >&2; exit 2 ;;
esac

echo "channel-egress-routing result=passed scenarios=${SELECTOR}"
