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
BUILD_DIR="${HOME}/.cache/zlink/java-e2e/ChannelEgressRouting"
GRADLE_CACHE_DIR="${HOME}/.cache/zlink/java-e2e/ChannelEgressRouting-gradle-cache"
REDIS_CONTAINER=""
REDIS_ENDPOINT=""
LOCATION_KEY_PREFIX="zlink:e2e:channel-egress:${RUN_ID}"
LOCAL_READINESS_ATTEMPTS=60
LOCAL_READINESS_POLL_SECONDS=0.1

declare -a PIDS=()
declare -A ROLE_PID ROLE_HTTP ROLE_GAME ROLE_AUDIT ROLE_WORKFLOW ROLE_RID ROLE_CONFIG

mkdir -p "${LOG_DIR}"
chmod 0700 "${CONFIG_DIR}"
echo "log_dir=${LOG_DIR}"
echo "scenario=${SELECTOR}"
echo "start_order=$(zlink_e2e_start_order_mode "$@")"

cleanup_processes() {
  local pid
  for pid in "${PIDS[@]}"; do
    kill -TERM "${pid}" >/dev/null 2>&1 || true
  done
  sleep 0.2
  for pid in "${PIDS[@]}"; do
    kill -KILL "${pid}" >/dev/null 2>&1 || true
  done
  wait >/dev/null 2>&1 || true
  PIDS=()
  ROLE_PID=()
}

print_failure_logs() {
  local status="$1"
  [[ "${status}" == "0" ]] && return
  local role
  for role in "${!ROLE_HTTP[@]}"; do
    echo "===== ${role} evidence =====" >&2
    curl --max-time 1 -fsS "${ROLE_HTTP[${role}]}/evidence" >&2 || true
    echo >&2
  done
  local log
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
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
  fi
  rm -rf "${CONFIG_DIR}"
  exit "${status}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ -z "${ZLINK_LIBRARY_PATH:-}" ]]; then
  repo_root="$(cd "${SCRIPT_DIR}/../../../../.." && pwd)"
  default_core_lib="${repo_root}/core/build/lib/libzlink.so"
  if [[ -f "${default_core_lib}" ]]; then
    export ZLINK_LIBRARY_PATH="${default_core_lib}"
  fi
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required for ChannelEgressRouting because it provisions a Redis location store." >&2
  exit 1
fi

zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-java-e2e-channel-egress" "redis:7.2-alpine" "127.0.0.1::6379"
REDIS_ENDPOINT="127.0.0.1:${redis_port}"

gradle_run() {
  "${JAVA_ROOT}/gradlew" -p "${SCRIPT_DIR}" \
    -PzlinkE2eBuildDir="${BUILD_DIR}" \
    --project-cache-dir "${GRADLE_CACHE_DIR}" \
    --no-daemon --no-parallel --max-workers=1 "$@" --quiet
}

gradle_run :Role:installDist :Client:installDist
ROLE_BIN="${BUILD_DIR}/Role/install/channel-egress-role/bin/channel-egress-role"
CLIENT_BIN="${BUILD_DIR}/Client/install/channel-egress-client/bin/channel-egress-client"

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
  local role="$1"
  local rid="$2"
  local game_peer_rids="$3"
  local game_peer_endpoints="$4"
  local game_servers="$5"
  local game_clients="$6"
  local audit_peer_rids="$7"
  local audit_peer_endpoints="$8"
  local audit_servers="$9"
  local audit_clients="${10}"
  local workflow_client="${11}"
  local workflow_server="${12}"
  local workflow_weight="${13}"
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
    printf 'e2e.game-peer-rids=%s\n' "${game_peer_rids}"
    printf 'e2e.game-peer-endpoints=%s\n' "${game_peer_endpoints}"
    printf 'e2e.game-servers=%s\n' "${game_servers}"
    printf 'e2e.game-clients=%s\n' "${game_clients}"
    printf 'e2e.audit-endpoint=%s\n' "${ROLE_AUDIT[${role}]}"
    printf 'e2e.audit-peer-rids=%s\n' "${audit_peer_rids}"
    printf 'e2e.audit-peer-endpoints=%s\n' "${audit_peer_endpoints}"
    printf 'e2e.audit-servers=%s\n' "${audit_servers}"
    printf 'e2e.audit-clients=%s\n' "${audit_clients}"
    printf 'e2e.workflow-endpoint=tcp://127.0.0.1:%s\n' "${ROLE_WORKFLOW[${role}]}"
    printf 'e2e.workflow-port=%s\n' "${ROLE_WORKFLOW[${role}]}"
    printf 'e2e.workflow-weight=%s\n' "${workflow_weight}"
    printf 'e2e.workflow-client=%s\n' "${workflow_client}"
    printf 'e2e.workflow-server=%s\n' "${workflow_server}"
    printf 'e2e.instance-spot=false\n'
    printf 'e2e.object-client=false\n'
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
  for attempt in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if curl --max-time 1 -fsS "${url}/health" >/dev/null 2>&1; then
      return 0
    fi
    if ! kill -0 "${pid}" >/dev/null 2>&1; then
      echo "${role} exited before health became ready" >&2
      return 1
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "timed out waiting for ${role} health at ${url}" >&2
  return 1
}

wait_route() {
  local role="$1"
  local channel="$2"
  local status_path="${3:-/status/route}"
  local minimum="${4:-1}"
  local minimum_peers="${5:-1}"
  python3 - "${ROLE_HTTP[${role}]}" "${channel}" "${status_path}" \
    "${minimum}" "${minimum_peers}" <<'PY'
import json
import sys
import time
import urllib.error
import urllib.request

base_url, channel, status_path, minimum_text, minimum_peers_text = sys.argv[1:]
minimum = int(minimum_text)
minimum_peers = int(minimum_peers_text)
deadline = time.monotonic() + 20
last = None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(base_url + status_path, timeout=1) as response:
            last = json.load(response)
        target = next(
            (value for value in last.get("channels", [])
             if value.get("channelName") == channel),
            None)
        if (int(last.get("readyPeerCount", 0)) >= minimum_peers
                and target is not None and target.get("isReady") is True
                and int(target.get("readyTargetCount", 0)) >= minimum):
            raise SystemExit(0)
    except (OSError, ValueError, urllib.error.URLError):
        pass
    time.sleep(0.1)
print(f"route channel did not become ready: {json.dumps(last)}", file=sys.stderr)
raise SystemExit(1)
PY
}

wait_workflow() {
  local role="$1"
  python3 - "${ROLE_HTTP[${role}]}" <<'PY'
import json
import sys
import time
import urllib.error
import urllib.request

base_url = sys.argv[1]
deadline = time.monotonic() + 20
last = None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(base_url + "/status/workflow", timeout=1) as response:
            last = json.load(response)
        if (last.get("isReady") is True
                and int(last.get("readyTargetCount", 0)) >= 1):
            raise SystemExit(0)
    except (OSError, ValueError, urllib.error.URLError):
        pass
    time.sleep(0.1)
print(f"workflow channel did not become ready: {json.dumps(last)}", file=sys.stderr)
raise SystemExit(1)
PY
}

run_client() {
  local scenario="$1"
  local fallback="${ROLE_HTTP[session]}"
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
  } >"${path}"
  chmod 0600 "${path}"
  "${CLIENT_BIN}" --config "${path}" --scenario "${scenario}" \
    >"${LOG_DIR}/${CURRENT_SCENARIO}/client.stdout.log" \
    2>"${LOG_DIR}/${CURRENT_SCENARIO}/client.stderr.log"
  cat "${LOG_DIR}/${CURRENT_SCENARIO}/client.stdout.log"
}

run_one() {
  CURRENT_SCENARIO="$1"
  ROLE_PID=()
  ROLE_HTTP=()
  ROLE_GAME=()
  ROLE_AUDIT=()
  ROLE_WORKFLOW=()
  ROLE_RID=()
  ROLE_CONFIG=()
  cleanup_processes
  mkdir -p "${LOG_DIR}/${CURRENT_SCENARIO}"

  case "${CURRENT_SCENARIO}" in
    CH-E2E-01)
      allocate_role session
      allocate_role play
      write_role_config session session play "${ROLE_GAME[play]}" \
        game.session game.play "" "" "" "" false false 0
      write_role_config play play session "${ROLE_GAME[session]}" \
        game.play game.session "" "" "" "" false false 0
      start_role session
      start_role play
      ;;
    CH-E2E-02)
      allocate_role session
      allocate_role play
      allocate_role audit
      allocate_role workflow-a
      allocate_role workflow-b
      write_role_config session session play "${ROLE_GAME[play]}" \
        game.session game.play "" "" "" "" false false 0
      write_role_config play play session "${ROLE_GAME[session]}" \
        game.play game.session audit-audit "${ROLE_AUDIT[audit]}" "" audit.record true false 0
      write_role_config audit audit "" "" "" "" play-audit "${ROLE_AUDIT[play]}" \
        audit.record "" false false 0
      write_role_config workflow-a workflow-a "" "" "" "" "" "" "" "" false true 100
      write_role_config workflow-b workflow-b "" "" "" "" "" "" "" "" false true 300
      start_role session
      start_role play
      start_role audit
      start_role workflow-a
      start_role workflow-b
      ;;
    CH-E2E-11)
      allocate_role session
      allocate_role api-a
      allocate_role api-b
      write_role_config session session \
        "api-a,api-b" "${ROLE_GAME[api-a]},${ROLE_GAME[api-b]}" \
        "" game.api "" "" "" "" false false 0
      write_role_config api-a api-a session "${ROLE_GAME[session]}" \
        game.api "" "" "" "" "" false false 0
      write_role_config api-b api-b session "${ROLE_GAME[session]}" \
        game.api "" "" "" "" "" false false 0
      start_role session
      start_role api-a
      start_role api-b
      ;;
    *)
      echo "unsupported ChannelEgressRouting selector ${CURRENT_SCENARIO}" >&2
      return 2
      ;;
  esac

  local role
  for role in "${!ROLE_PID[@]}"; do
    wait_health "${role}"
  done
  case "${CURRENT_SCENARIO}" in
    CH-E2E-01)
      wait_route session game.play
      wait_route play game.session
      ;;
    CH-E2E-02)
      wait_route session game.play
      wait_route play game.session
      wait_route play audit.record /status/audit
      wait_workflow play
      ;;
    CH-E2E-11)
      wait_route session game.api /status/route 2 2
      ;;
  esac
  run_client "${CURRENT_SCENARIO}"
  echo "scenario ${CURRENT_SCENARIO} passed"
}

case "${SELECTOR^^}" in
  ALL)
    run_one CH-E2E-01
    run_one CH-E2E-02
    run_one CH-E2E-11
    ;;
  CH01|CH-E2E-01)
    run_one CH-E2E-01
    ;;
  CH02|CH-E2E-02)
    run_one CH-E2E-02
    ;;
  CH11|CH-E2E-11)
    run_one CH-E2E-11
    ;;
  *)
    echo "unsupported ChannelEgressRouting selector ${SELECTOR}" >&2
    exit 2
    ;;
esac

echo "channel-egress-routing result=passed scenarios=${SELECTOR}"
