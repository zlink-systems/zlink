#!/usr/bin/env bash
set -euo pipefail

# Config 11 Java topology runner. OBS-A1/A2 are produced exclusively from
# connector/framework logs emitted by the deployed processes.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAVA_E2E_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${JAVA_E2E_DIR}/../e2e-redis-common.sh"
source "${JAVA_E2E_DIR}/start-order-common.sh"
e2e_start_order="$(zlink_e2e_start_order_mode "$@")"
echo "start_order=${e2e_start_order}"

forbidden_config_ref="Automatic""TurnDispatch|ATD""_DIR|ATD""-[A-Z][0-9]"
if rg -n "${forbidden_config_ref}" \
    "${SCRIPT_DIR}/run_e2e.sh" \
    "${SCRIPT_DIR}/Client/src/main/java/systems/zlink/e2e/observabilityops/client/Program.java" \
    "${SCRIPT_DIR}/Server" --glob '*.java'; then
  echo "ObservabilityOps must own its role apps and client scenarios" >&2
  exit 1
fi
legacy_config_dir="${JAVA_E2E_DIR}/Automatic""TurnDispatch"
if rg -n 'OBS-[A-Z][0-9]' \
    "${legacy_config_dir}/Client/src/main/java" --glob '*.java'; then
  echo "The adjacent config client must not own ObservabilityOps scenarios" >&2
  exit 1
fi

SELECTOR="${1:-all}"
case "${SELECTOR}" in
  all|OBS-A1|OBS-A2|OBS-A3|OBS-A4|OBS-B1|OBS-B2|OBS-B3|OBS-B4|OBS-C1|OBS-C2|OBS-C3|OBS-C4|OBS-C5) ;;
  *) echo "Unknown ObservabilityOps selector." >&2; exit 2 ;;
esac

if [[ "${SELECTOR}" == all ]]; then
  for selector in \
    OBS-A1 OBS-A2 OBS-A3 OBS-A4 \
    OBS-B1 OBS-B2 OBS-B3 OBS-B4 \
    OBS-C1 OBS-C2 OBS-C3 OBS-C4 OBS-C5; do
    echo "===== OBSERVABILITY OPS START ${selector} ====="
    "${BASH_SOURCE[0]}" "${selector}" --start-order "${e2e_start_order}"
    echo "===== OBSERVABILITY OPS PASS ${selector} ====="
  done
  echo "observability-ops all result=passed"
  exit 0
fi

run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="${SCRIPT_DIR}/logs/${run_id}"
evidence_dir="${log_dir}/evidence"
repo_root="$(cd "${SCRIPT_DIR}/../../../../.." && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
obs_build="${HOME}/.cache/zlink/java-e2e/ObservabilityOps"
gradle_cache="${HOME}/.cache/zlink/java-e2e/ObservabilityOps-gradle-cache"
pids=()
REDIS_CONTAINER=""
config_dir="${log_dir}/config"
mkdir -p -m 0700 "${log_dir}" "${evidence_dir}" "${config_dir}"
echo "log_dir=${log_dir}"

if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-java-e2e-observability" "redis:7.2-alpine"
redis_location_endpoint="127.0.0.1:${redis_port}"
location_key_prefix="zlink:e2e:observability:${run_id}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30

descendants() {
  local pid="$1" child
  (pgrep -P "${pid}" 2>/dev/null || true) | while read -r child; do
    descendants "${child}"
    echo "${child}"
  done
}

cleanup() {
  local status="$?"
  set +e
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    for child in $(descendants "${pids[$i]}"); do kill "${child}" >/dev/null 2>&1 || true; done
    kill "${pids[$i]}" >/dev/null 2>&1 || true
  done
  for _ in $(seq 1 50); do
    local alive=0
    for pid in "${pids[@]}"; do kill -0 "${pid}" >/dev/null 2>&1 && alive=1; done
    [[ "${alive}" == 0 ]] && break
    sleep 0.1
  done
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    for child in $(descendants "${pids[$i]}"); do kill -9 "${child}" >/dev/null 2>&1 || true; done
    kill -9 "${pids[$i]}" >/dev/null 2>&1 || true
  done
  [[ -z "${REDIS_CONTAINER}" ]] || docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
  wait >/dev/null 2>&1 || true
  rm -rf "${config_dir}"
  if [[ "${status}" != 0 ]]; then
    for log in "${log_dir}"/*.log; do [[ -f "${log}" ]] && { echo "===== ${log} =====" >&2; tail -n 120 "${log}" >&2; }; done
  fi
  exit "${status}"
}
trap cleanup EXIT

if [[ "${LOCAL_READINESS_TIMEOUT_SECONDS:-}" != 3 \
   || "${LOCAL_READINESS_ATTEMPTS:-}" != 30 ]]; then
  echo "ObservabilityOps must use a 3s readiness limit" >&2
  exit 1
fi
if rg -n 'java\.net\.http\.HttpClient|HttpClient\.new' Trigger/src/main/java --glob '*.java'; then
  echo "ObservabilityOps trigger must use ZLinkHttpClient" >&2
  exit 1
fi

reserve_ports() {
  python3 - <<'PY'
import socket
socks=[]
try:
    for _ in range(11):
        sock=socket.socket(); sock.bind(("127.0.0.1", 0)); socks.append(sock)
    print(" ".join(str(sock.getsockname()[1]) for sock in socks))
finally:
    for sock in socks: sock.close()
PY
}
tcp() { echo "tcp://127.0.0.1:$1"; }
http() { echo "http://127.0.0.1:$1"; }
port_of() { echo "${1##*:}"; }
wait_port() {
  local name="$1" endpoint="$2" port
  port="$(port_of "${endpoint}")"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1 && return 0
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}
wait_http() {
  local name="$1" endpoint="$2"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    python3 - "${endpoint}/evidence" >/dev/null 2>&1 <<'PY' && return 0
import sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response: response.read()
PY
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}
wait_metrics_state() {
  local endpoint="$1" expected="$2"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if [[ "$(python3 - "${endpoint}/metrics-ready" 2>/dev/null <<'PY'
import sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    print(response.read().decode().strip())
PY
)" == "${expected}" ]]; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for metrics-ready=${expected} at ${endpoint}" >&2
  return 1
}
wait_drain_ready() {
  local endpoint="$1" node_rid="$2"
  for _ in $(seq 1 300); do
    if python3 - "${endpoint}/drain/status" "${node_rid}" 2>/dev/null <<'PY'
import json, sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    status=json.loads(response.read())
rows=status.get('peerRows', [])
ready=(status.get('ready') is True
       and status.get('locationStatus', {}).get('ownerLeaseRenewedAt')
       and any(row.get('nodeRid') == sys.argv[2] and not row.get('draining', False) for row in rows))
raise SystemExit(0 if ready else 1)
PY
    then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ready non-draining node ${node_rid} at ${endpoint}" >&2
  return 1
}
fetch_url() {
  python3 - "$1" "$2" <<'PY'
import pathlib, sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=5) as response:
    pathlib.Path(sys.argv[2]).write_bytes(response.read())
PY
}
wait_metric_value_on_either_node() {
  local endpoint_a="$1" endpoint_b="$2" name="$3" tag_key="$4" tag_value="$5" minimum="$6" output="$7"
  local output_a="${output}.a" output_b="${output}.b"
  for _ in $(seq 1 100); do
    fetch_url "${endpoint_a}/metrics" "${output_a}"
    fetch_url "${endpoint_b}/metrics" "${output_b}"
    if python3 - "${output_a}" "${output_b}" "${output}" "${name}" "${tag_key}" "${tag_value}" "${minimum}" <<'PY'
import json, pathlib, sys
for source in sys.argv[1:3]:
    rows=json.load(open(source, encoding='utf-8'))
    for row in rows:
        if (row.get('name') == sys.argv[4]
            and row.get('tags', {}).get(sys.argv[5]) == sys.argv[6]
            and float(row.get('value', 0)) >= float(sys.argv[7])):
            pathlib.Path(sys.argv[3]).write_text(json.dumps(rows), encoding='utf-8')
            raise SystemExit(0)
raise SystemExit(1)
PY
    then
      rm -f "${output_a}" "${output_b}"
      return 0
    fi
    sleep 0.02
  done
  rm -f "${output_a}" "${output_b}"
  echo "Timed out waiting for ${name}{${tag_key}=${tag_value}} >= ${minimum} on either Play node" >&2
  return 1
}

read -r DELAY_PORT ROUTE_A_PORT SPOT_A_PORT ROUTE_B_PORT SPOT_B_PORT STREAM_PORT PLAY_A_HTTP_PORT PLAY_B_HTTP_PORT SESSION_HTTP_PORT SESSION_ROUTE_PORT FANOUT_PORT <<<"$(reserve_ports)"
DELAY_ENDPOINT="$(tcp "${DELAY_PORT}")"
ROUTE_A_ENDPOINT="$(tcp "${ROUTE_A_PORT}")"
SPOT_A_ENDPOINT="$(tcp "${SPOT_A_PORT}")"
ROUTE_B_ENDPOINT="$(tcp "${ROUTE_B_PORT}")"
SPOT_B_ENDPOINT="$(tcp "${SPOT_B_PORT}")"
STREAM_ENDPOINT="$(tcp "${STREAM_PORT}")"
PLAY_A_HTTP="$(http "${PLAY_A_HTTP_PORT}")"
PLAY_B_HTTP="$(http "${PLAY_B_HTTP_PORT}")"
SESSION_HTTP="$(http "${SESSION_HTTP_PORT}")"
SESSION_ROUTE_ENDPOINT="$(tcp "${SESSION_ROUTE_PORT}")"
FANOUT_ENDPOINT="$(tcp "${FANOUT_PORT}")"
SESSION_SPOT_ENDPOINT="$(tcp "$(python3 - <<'PY'
import socket
s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()
PY
)")"

"${SCRIPT_DIR}/gradlew" -PzlinkE2eBuildDir="${obs_build}" \
  --project-cache-dir "${gradle_cache}" --no-daemon --no-parallel --max-workers=1 --quiet \
  clean installDist

delay_bin="${obs_build}/Server-Delay/install/observability-ops-delay/bin/observability-ops-delay"
play_bin="${obs_build}/Server-Play/install/observability-ops-play/bin/observability-ops-play"
session_bin="${obs_build}/Server-Session/install/observability-ops-session/bin/observability-ops-session"
client_bin="${obs_build}/Client/install/observability-ops-client/bin/observability-ops-client"
verifier_bin="${obs_build}/Verifier/install/observability-ops-verifier/bin/observability-ops-verifier"

write_config() {
  local path="$1"
  shift
  : >"${path}"
  chmod 0600 "${path}"
  printf '%s\n' "$@" >"${path}"
}
start_delay() {
  local stdout="$1" stderr="$2" config="${config_dir}/delay.properties"
  write_config "${config}" \
    "e2e.delay-endpoint=${DELAY_ENDPOINT}" \
    "e2e.redis-location-endpoint=${redis_location_endpoint}" \
    "e2e.location-key-prefix=${location_key_prefix}" \
    "e2e.log-dir=${log_dir}"
  "${delay_bin}" --config "${config}" >"${stdout}" 2>"${stderr}" &
}
start_play() {
  local node="$1" stdout="$2" stderr="$3"
  local route_endpoint route_peer_endpoint spot_endpoint http_endpoint
  if [[ "${node}" == play-a ]]; then
    route_endpoint="${ROUTE_A_ENDPOINT}"; route_peer_endpoint="${ROUTE_B_ENDPOINT}"
    spot_endpoint="${SPOT_A_ENDPOINT}"; http_endpoint="${PLAY_A_HTTP}"
  else
    route_endpoint="${ROUTE_B_ENDPOINT}"; route_peer_endpoint="${ROUTE_A_ENDPOINT}"
    spot_endpoint="${SPOT_B_ENDPOINT}"; http_endpoint="${PLAY_B_HTTP}"
  fi
  local config="${config_dir}/${node}.properties"
  write_config "${config}" \
    "e2e.node-rid=${node}" \
    "e2e.route-endpoint=${route_endpoint}" \
    "e2e.route-peer-endpoint=${route_peer_endpoint}" \
    "e2e.spot-endpoint=${spot_endpoint}" \
    "e2e.delay-endpoint=${DELAY_ENDPOINT}" \
    "e2e.fanout-endpoint=${FANOUT_ENDPOINT}" \
    "e2e.http-endpoint=${http_endpoint}" \
    "e2e.redis-location-endpoint=${redis_location_endpoint}" \
    "e2e.location-key-prefix=${location_key_prefix}" \
    "e2e.log-dir=${log_dir}"
  "${play_bin}" --config "${config}" >"${stdout}" 2>"${stderr}" &
}
start_session() {
  local flow="$1" drain_spot="$2" metrics="$3" stdout="$4" stderr="$5"
  local config="${config_dir}/session.properties"
  local properties=(
    "e2e.message-flow=${flow}"
    "e2e.route-endpoint=${ROUTE_A_ENDPOINT}"
    "e2e.route-b-endpoint=${ROUTE_B_ENDPOINT}"
    "e2e.session-route-endpoint=${SESSION_ROUTE_ENDPOINT}"
    "e2e.session-spot-endpoint=${SESSION_SPOT_ENDPOINT}"
    "e2e.delay-endpoint=${DELAY_ENDPOINT}"
    "e2e.stream-endpoint=${STREAM_ENDPOINT}"
    "e2e.http-endpoint=${SESSION_HTTP}"
    "e2e.session-drain-spot=${drain_spot}"
    "e2e.redis-location-endpoint=${redis_location_endpoint}"
    "e2e.location-key-prefix=${location_key_prefix}"
    "e2e.log-dir=${log_dir}"
  )
  if [[ "${metrics}" == off ]]; then
    properties+=("spring.autoconfigure.exclude=systems.zlink.framework.spring.ZLinkMetricsAutoConfiguration")
  fi
  write_config "${config}" "${properties[@]}"
  "${session_bin}" --config "${config}" >"${stdout}" 2>"${stderr}" &
}
run_client() {
  local scenario="$1" scenario_output="$2" drain_url="$3" limit="$4" stdout="$5" stderr="$6"
  local config="${config_dir}/client-${scenario}.properties"
  write_config "${config}" \
    "streamEndpoint=${STREAM_ENDPOINT}" \
    "playHttpEndpoint=${PLAY_A_HTTP}" \
    "playBHttpEndpoint=${PLAY_B_HTTP}" \
    "sessionHttpEndpoint=${SESSION_HTTP}" \
    "scenarioOutput=${scenario_output}" \
    "drainUrl=${drain_url}"
  timeout -k 5s "${limit}" "${client_bin}" --config "${config}" --scenario "${scenario}" >"${stdout}" 2>"${stderr}"
}

start_initial_role() {
  case "$1" in
    delay)
      start_delay "${log_dir}/delay.stdout.log" "${log_dir}/delay.stderr.log"
      ;;
    play-a)
      start_play play-a "${log_dir}/play-a.stdout.log" "${log_dir}/play-a.stderr.log"
      play_a_pid="$!"
      ;;
    play-b)
      start_play play-b "${log_dir}/play-b.stdout.log" "${log_dir}/play-b.stderr.log"
      play_b_pid="$!"
      ;;
    session)
      start_session on "" on "${log_dir}/session.stdout.log" "${log_dir}/session.stderr.log"
      session_pid="$!"
      ;;
  esac
  pids+=("$!")
}

mapfile -t ORDERED_SERVER_ROLES < <(zlink_e2e_order_roles delay play-a play-b session)
for role in "${ORDERED_SERVER_ROLES[@]}"; do
  start_initial_role "${role}"
done

wait_port delay "${DELAY_ENDPOINT}"
wait_port play-a-route "${ROUTE_A_ENDPOINT}"
wait_port play-a-spot "${SPOT_A_ENDPOINT}"
wait_http play-a-http "${PLAY_A_HTTP}"
wait_port play-b-route "${ROUTE_B_ENDPOINT}"
wait_port play-b-spot "${SPOT_B_ENDPOINT}"
wait_http play-b-http "${PLAY_B_HTTP}"
wait_port session-route "${SESSION_ROUTE_ENDPOINT}"
wait_port session-spot "${SESSION_SPOT_ENDPOINT}"
wait_port session-stream "${STREAM_ENDPOINT}"
wait_http session-http "${SESSION_HTTP}"
wait_metrics_state "${SESSION_HTTP}" true

if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-A1 ]]; then
  run_client OBS-A1 "" "" 90s "${log_dir}/a1-client.stdout.log" "${log_dir}/a1-client.stderr.log"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-A2 ]]; then
  run_client OBS-A2 "" "" 30s "${log_dir}/a2-client.stdout.log" "${log_dir}/a2-client.stderr.log"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-B2 ]]; then
  run_client OBS-B2 "" "" 90s "${log_dir}/b2-client.stdout.log" "${log_dir}/b2-client.stderr.log"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-A3 ]]; then
  kill -TERM "${session_pid}"
  wait "${session_pid}" || true
  start_session off "" on "${log_dir}/off-node.stdout.log" "${log_dir}/off-node.stderr.log"; pids+=("$!")
  session_pid="$!"
  wait_port off-node-stream "${STREAM_ENDPOINT}"; wait_http off-node-http "${SESSION_HTTP}"
  wait_metrics_state "${SESSION_HTTP}" true
  run_client OBS-A3 "" "" 90s "${log_dir}/a3-client.stdout.log" "${log_dir}/a3-client.stderr.log"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-A4 || "${SELECTOR}" == OBS-B3 ]]; then
  run_client OBS-A4 "" "" 90s "${log_dir}/a4-client.stdout.log" "${log_dir}/a4-client.stderr.log"
  sleep 1
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-B1 ]]; then
  run_client OBS-B1 "${log_dir}/connector-metrics.json" "" 60s "${log_dir}/b1-client.stdout.log" "${log_dir}/b1-client.stderr.log"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-B3 ]]; then
  kill -STOP "${pids[1]}"
  sleep 7
  kill -CONT "${pids[1]}"
  sleep 6
fi
fetch_url "${PLAY_A_HTTP}/metrics" "${log_dir}/play-a-metrics.json"
fetch_url "${PLAY_B_HTTP}/metrics" "${log_dir}/play-b-metrics.json"
fetch_url "${SESSION_HTTP}/metrics" "${log_dir}/session-metrics.json"
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-B4 ]]; then
  kill -TERM "${session_pid}"
  wait "${session_pid}" || true
  start_session off "" off "${log_dir}/reader-free.stdout.log" "${log_dir}/reader-free.stderr.log"; pids+=("$!")
  session_pid="$!"
  wait_port reader-free-stream "${STREAM_ENDPOINT}"; wait_http reader-free-http "${SESSION_HTTP}"
  wait_metrics_state "${SESSION_HTTP}" false
  run_client OBS-B4 "${log_dir}/reader-free-result.json" "" 120s "${log_dir}/b4-client.stdout.log" "${log_dir}/b4-client.stderr.log"
  fetch_url "${SESSION_HTTP}/metrics" "${log_dir}/reader-free-metrics.json"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-C1 ]]; then
  fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c1-before.json"
  run_client OBS-C1 "" "" 90s "${log_dir}/c1-existing.stdout.log" "${log_dir}/c1-existing.stderr.log"
  fetch_url "${PLAY_A_HTTP}/drain/start?deadlineMs=9000" "${log_dir}/c1-start.json"
  python3 - "${PLAY_A_HTTP}/drain/status" "${log_dir}/c1-before.json" "${log_dir}/c1-during.json" <<'PY'
import json, pathlib, sys, time, urllib.request
before=json.load(open(sys.argv[2], encoding='utf-8'))
renewed=before.get('locationStatus',{}).get('ownerLeaseRenewedAt','')
deadline=time.time()+8
while time.time()<deadline:
    with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
        current=json.loads(response.read())
    peers=current.get('peerRows',[])
    now=current.get('locationStatus',{}).get('ownerLeaseRenewedAt','')
    if (not current.get('ready',True)
        and any(row.get('nodeRid')=='play-a' and row.get('draining') for row in peers)
        and now and now != renewed):
        pathlib.Path(sys.argv[3]).write_text(json.dumps(current), encoding='utf-8')
        raise SystemExit(0)
    time.sleep(.1)
raise SystemExit('OBS-C1 draining/lease readiness evidence timed out')
PY
  for _ in $(seq 1 100); do
    fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c1-terminal.json"
    python3 - "${log_dir}/c1-terminal.json" <<'PY' && break
import json,sys
raise SystemExit(0 if json.load(open(sys.argv[1])).get('result') else 1)
PY
    sleep 0.1
  done
  kill -TERM "${play_a_pid}" >/dev/null 2>&1 || true
  wait "${play_a_pid}" || true
  truncate -s 0 "${log_dir}/play-a-flow.log"
  start_play play-a "${log_dir}/play-a-c1-restart.stdout.log" "${log_dir}/play-a-c1-restart.stderr.log"; pids+=("$!")
  play_a_pid="$!"
  wait_port play-a-restart-route "${ROUTE_A_ENDPOINT}"; wait_port play-a-restart-spot "${SPOT_A_ENDPOINT}"; wait_http play-a-restart-http "${PLAY_A_HTTP}"
  wait_drain_ready "${PLAY_A_HTTP}" play-a
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-C2 ]]; then
  run_client OBS-C2 "" "" 60s "${log_dir}/c2-client.stdout.log" "${log_dir}/c2-client.stderr.log" &
  c2_client_pid="$!"
  for _ in $(seq 1 300); do
    grep -q "OBS-C2 pending-started" "${log_dir}/c2-client.stdout.log" && break
    kill -0 "${c2_client_pid}" >/dev/null 2>&1 || { echo "OBS-C2 client exited before pending request" >&2; exit 1; }
    sleep 0.1
  done
  grep -q "OBS-C2 pending-started" "${log_dir}/c2-client.stdout.log"
  for _ in $(seq 1 300); do
    grep -q "outcome=RECEIVED.*packet=ActorPushAwaitReq" "${log_dir}/play-a-flow.log" && break
    kill -0 "${c2_client_pid}" >/dev/null 2>&1 || { echo "OBS-C2 client exited before Play-A received pending request" >&2; exit 1; }
    sleep 0.1
  done
  grep -q "outcome=RECEIVED.*packet=ActorPushAwaitReq" "${log_dir}/play-a-flow.log"
  fetch_url "${PLAY_A_HTTP}/drain/start?deadlineMs=9000" "${log_dir}/c2-start.json"
  wait "${c2_client_pid}"
  fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c2-drain-status.json"
  fetch_url "${PLAY_A_HTTP}/metrics" "${log_dir}/c2-metrics.json"
  for _ in $(seq 1 100); do
    fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c2-terminal.json"
    python3 - "${log_dir}/c2-terminal.json" <<'PY' && break
import json,sys
raise SystemExit(0 if json.load(open(sys.argv[1])).get('result') else 1)
PY
    sleep 0.1
  done
  kill -TERM "${play_a_pid}" >/dev/null 2>&1 || true
  wait "${play_a_pid}" || true
  start_play play-a "${log_dir}/play-a-c2-restart.stdout.log" "${log_dir}/play-a-c2-restart.stderr.log"; pids+=("$!")
  play_a_pid="$!"
  wait_port play-a-c2-restart-route "${ROUTE_A_ENDPOINT}"; wait_port play-a-c2-restart-spot "${SPOT_A_ENDPOINT}"; wait_http play-a-c2-restart-http "${PLAY_A_HTTP}"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-C3 ]]; then
  run_client OBS-C3-WRITE "" "" 60s "${log_dir}/c3-write.stdout.log" "${log_dir}/c3-write.stderr.log"
  fetch_url "${PLAY_A_HTTP}/drain/start?deadlineMs=15000" "${log_dir}/c3-fixed-start.json"
  for _ in $(seq 1 150); do
    fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c3-fixed-terminal.json"
    python3 - "${log_dir}/c3-fixed-terminal.json" <<'PY' && break
import json,sys
raise SystemExit(0 if json.load(open(sys.argv[1])).get('result') else 1)
PY
    sleep 0.1
  done
  fetch_url "${PLAY_A_HTTP}/metrics" "${log_dir}/c3-fixed-metrics.json"
  run_client OBS-C3-READ "" "" 60s "${log_dir}/c3-read.stdout.log" "${log_dir}/c3-read.stderr.log"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-C4 ]]; then
  kill -TERM "${session_pid}" >/dev/null 2>&1 || true
  wait "${session_pid}" || true
  start_session off obs-c4-held-spot on "${log_dir}/c4-session.stdout.log" "${log_dir}/c4-session.stderr.log"; pids+=("$!")
  session_pid="$!"
  wait_port c4-session-stream "${STREAM_ENDPOINT}"; wait_http c4-session-http "${SESSION_HTTP}"; wait_metrics_state "${SESSION_HTTP}" true
  run_client OBS-C4 "${log_dir}/c4-connector-result.json" \
    "${SESSION_HTTP}/drain/start?deadlineMs=500" 30s "${log_dir}/c4-client.stdout.log" "${log_dir}/c4-client.stderr.log"
  fetch_url "${SESSION_HTTP}/drain/status" "${log_dir}/c4-drain-status.json"
  fetch_url "${SESSION_HTTP}/metrics" "${log_dir}/c4-metrics.json"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-C5 ]]; then
  if [[ "${SELECTOR}" == all ]]; then
    kill -TERM "${play_a_pid}" "${play_b_pid}" "${session_pid}" >/dev/null 2>&1 || true
    wait "${play_a_pid}" || true
    wait "${play_b_pid}" || true
    wait "${session_pid}" || true
    start_play play-a "${log_dir}/c5-serving-play-a.stdout.log" "${log_dir}/c5-serving-play-a.stderr.log"; pids+=("$!")
    play_a_pid="$!"
    wait_port c5-serving-play-a-route "${ROUTE_A_ENDPOINT}"; wait_http c5-serving-play-a-http "${PLAY_A_HTTP}"
    start_play play-b "${log_dir}/c5-serving-play-b.stdout.log" "${log_dir}/c5-serving-play-b.stderr.log"; pids+=("$!")
    play_b_pid="$!"
    wait_port c5-serving-play-b-route "${ROUTE_B_ENDPOINT}"; wait_http c5-serving-play-b-http "${PLAY_B_HTTP}"
    start_session on "" on "${log_dir}/c5-serving-session.stdout.log" "${log_dir}/c5-serving-session.stderr.log"; pids+=("$!")
    session_pid="$!"
    wait_port c5-serving-session-route "${SESSION_ROUTE_ENDPOINT}"; wait_port c5-serving-session-spot "${SESSION_SPOT_ENDPOINT}"; wait_port c5-serving-session-stream "${STREAM_ENDPOINT}"; wait_http c5-serving-session-http "${SESSION_HTTP}"; wait_metrics_state "${SESSION_HTTP}" true
  fi
  truncate -s 0 "${log_dir}/play-a-flow.log"
  run_client OBS-C5-PROBE "" "" 60s "${log_dir}/c5-serving-probe.stdout.log" "${log_dir}/c5-serving-probe.stderr.log"
  fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c5-serving-source-before-probe.json"
  fetch_url "${PLAY_A_HTTP}/route-probe" "${log_dir}/c5-serving-source-probe.json"
  run_client OBS-C5-BIND "" "" 60s "${log_dir}/c5-serving-client.stdout.log" "${log_dir}/c5-serving-client.stderr.log"
  fetch_url "${PLAY_A_HTTP}/drain/start?deadlineMs=20000" "${log_dir}/c5-serving-start.json"
  for _ in $(seq 1 250); do
    fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c5-serving-terminal.json"
    python3 - "${log_dir}/c5-serving-terminal.json" <<'PY' && break
import json,sys
raise SystemExit(0 if json.load(open(sys.argv[1])).get('result') else 1)
PY
    sleep 0.1
  done
  kill -TERM "${play_a_pid}" "${play_b_pid}" "${session_pid}" >/dev/null 2>&1 || true
  wait "${play_a_pid}" || true
  wait "${play_b_pid}" || true
  wait "${session_pid}" || true
  start_play play-a "${log_dir}/c5-zero-play-a.stdout.log" "${log_dir}/c5-zero-play-a.stderr.log"; pids+=("$!")
  play_a_pid="$!"
  wait_port c5-zero-route "${ROUTE_A_ENDPOINT}"; wait_http c5-zero-http "${PLAY_A_HTTP}"
  start_play play-b "${log_dir}/c5-zero-play-b.stdout.log" "${log_dir}/c5-zero-play-b.stderr.log"; pids+=("$!")
  play_b_pid="$!"
  wait_port c5-zero-play-b-route "${ROUTE_B_ENDPOINT}"; wait_http c5-zero-play-b-http "${PLAY_B_HTTP}"
  start_session on "" on "${log_dir}/c5-zero-session.stdout.log" "${log_dir}/c5-zero-session.stderr.log"; pids+=("$!")
  session_pid="$!"
  wait_port c5-zero-session-route "${SESSION_ROUTE_ENDPOINT}"; wait_port c5-zero-session-spot "${SESSION_SPOT_ENDPOINT}"; wait_port c5-zero-session-stream "${STREAM_ENDPOINT}"; wait_http c5-zero-session-http "${SESSION_HTTP}"; wait_metrics_state "${SESSION_HTTP}" true
  run_client OBS-C5-BIND "" "" 60s "${log_dir}/c5-zero-bind.stdout.log" "${log_dir}/c5-zero-bind.stderr.log"
  run_client OBS-C5-PROBE "" "" 60s "${log_dir}/c5-zero-held-spot.stdout.log" "${log_dir}/c5-zero-held-spot.stderr.log"
  fetch_url "${PLAY_B_HTTP}/drain/start?deadlineMs=5000" "${log_dir}/c5-zero-play-b-start.json"
  for _ in $(seq 1 100); do
    fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c5-zero-play-b-row.json"
    python3 - "${log_dir}/c5-zero-play-b-row.json" <<'PY' && break
import json,sys
rows=json.load(open(sys.argv[1])).get('peerRows', [])
raise SystemExit(0 if any(row.get('nodeRid') == 'play-b' and row.get('draining') for row in rows) else 1)
PY
    sleep 0.05
  done
  fetch_url "${PLAY_B_HTTP}/drain/status" "${log_dir}/c5-zero-play-b-during.json"
  fetch_url "${PLAY_A_HTTP}/drain/start?deadlineMs=500" "${log_dir}/c5-zero-start.json"
  fetch_url "${PLAY_A_HTTP}/metrics" "${log_dir}/c5-zero-metrics.json"
  sleep 0.7
  fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c5-zero-terminal.json"
fi

python3 "${SCRIPT_DIR}/extract_flow_evidence.py" "${log_dir}" "${evidence_dir}" "${SELECTOR}"
if [[ "${SELECTOR}" == all ]]; then
  "${verifier_bin}" "${evidence_dir}" OBS-A1
  "${verifier_bin}" "${evidence_dir}" OBS-A2
  "${verifier_bin}" "${evidence_dir}" OBS-A3
  "${verifier_bin}" "${evidence_dir}" OBS-A4
  "${verifier_bin}" "${evidence_dir}" OBS-B1
  "${verifier_bin}" "${evidence_dir}" OBS-B2
  "${verifier_bin}" "${evidence_dir}" OBS-B3
  "${verifier_bin}" "${evidence_dir}" OBS-B4
  "${verifier_bin}" "${evidence_dir}" OBS-C1
  "${verifier_bin}" "${evidence_dir}" OBS-C2
  "${verifier_bin}" "${evidence_dir}" OBS-C3
  "${verifier_bin}" "${evidence_dir}" OBS-C4
  "${verifier_bin}" "${evidence_dir}" OBS-C5
else
  "${verifier_bin}" "${evidence_dir}" "${SELECTOR}"
fi
