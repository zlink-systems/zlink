#!/usr/bin/env bash
set -euo pipefail

# Config 11 Kotlin topology runner. OBS-A1/A2 are produced exclusively from
# connector/framework logs emitted by the deployed processes.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAVA_E2E_DIR="$(cd "${SCRIPT_DIR}/../../e2e" && pwd)"
ATD_DIR="${JAVA_E2E_DIR}/AutomaticTurnDispatch"
source "${JAVA_E2E_DIR}/../e2e-redis-common.sh"

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
    "${BASH_SOURCE[0]}" "${selector}"
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
atd_build="${ZLINK_KOTLIN_OBSERVABILITY_ATD_BUILD_DIR:-${HOME}/.cache/zlink/kotlin-e2e/ObservabilityOps-atd}"
obs_build="${ZLINK_KOTLIN_OBSERVABILITY_BUILD_DIR:-${HOME}/.cache/zlink/kotlin-e2e/ObservabilityOps}"
gradle_cache="${ZLINK_KOTLIN_OBSERVABILITY_GRADLE_CACHE:-${HOME}/.cache/zlink/kotlin-e2e/ObservabilityOps-gradle-cache}"
pids=()
REDIS_CONTAINER=""
mkdir -p "${log_dir}" "${evidence_dir}"
echo "log_dir=${log_dir}"

if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-kotlin-e2e-observability" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
export ZLINK_JAVA_E2E_REDIS_LOCATION_ENDPOINT="127.0.0.1:${redis_port}"
export ZLINK_JAVA_E2E_LOCATION_KEY_PREFIX="${ZLINK_KOTLIN_OBSERVABILITY_LOCATION_KEY_PREFIX:-zlink:e2e:kotlin-observability:${run_id}}"

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
  if [[ "${status}" != 0 ]]; then
    for log in "${log_dir}"/*.log; do [[ -f "${log}" ]] && { echo "===== ${log} =====" >&2; tail -n 120 "${log}" >&2; }; done
  fi
  exit "${status}"
}
trap cleanup EXIT

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
  for _ in $(seq 1 300); do
    (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}
wait_http() {
  local name="$1" endpoint="$2"
  for _ in $(seq 1 600); do
    python3 - "${endpoint}/evidence" >/dev/null 2>&1 <<'PY' && return 0
import sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response: response.read()
PY
    sleep 0.1
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}
wait_metrics_state() {
  local endpoint="$1" expected="$2"
  for _ in $(seq 1 300); do
    if [[ "$(python3 - "${endpoint}/metrics-ready" 2>/dev/null <<'PY'
import sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    print(response.read().decode().strip())
PY
)" == "${expected}" ]]; then
      return 0
    fi
    sleep 0.1
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

(cd "${ATD_DIR}" && ZLINK_JAVA_E2E_BUILD_DIR="${atd_build}" ../../gradlew \
  --project-cache-dir "${gradle_cache}" --no-daemon --no-parallel --max-workers=1 --quiet installDist)
ZLINK_KOTLIN_E2E_BUILD_DIR="${obs_build}" "${SCRIPT_DIR}/gradlew" \
  --project-cache-dir "${gradle_cache}" --no-daemon --no-parallel --max-workers=1 --quiet installDist

delay_bin="${atd_build}/Server-Delay/install/automatic-turn-dispatch-delay/bin/automatic-turn-dispatch-delay"
play_bin="${atd_build}/Server-Play/install/automatic-turn-dispatch-play/bin/automatic-turn-dispatch-play"
session_bin="${atd_build}/Server-Session/install/automatic-turn-dispatch-session/bin/automatic-turn-dispatch-session"
client_bin="${atd_build}/Client/install/automatic-turn-dispatch-client/bin/automatic-turn-dispatch-client"
trigger_bin="${obs_build}/Trigger/install/observability-ops-trigger/bin/observability-ops-trigger"
verifier_bin="${obs_build}/Verifier/install/observability-ops-verifier/bin/observability-ops-verifier"

common_env=(ZLINK_JAVA_E2E_REDIS_LOCATION_ENDPOINT="${ZLINK_JAVA_E2E_REDIS_LOCATION_ENDPOINT}" ZLINK_JAVA_E2E_LOCATION_KEY_PREFIX="${ZLINK_JAVA_E2E_LOCATION_KEY_PREFIX}" ZLINK_JAVA_E2E_LOG_DIR="${log_dir}")
play_a_drain_policy=natural
[[ "${SELECTOR}" == OBS-C5 ]] && play_a_drain_policy=release
env "${common_env[@]}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" "${delay_bin}" >"${log_dir}/delay.stdout.log" 2>"${log_dir}/delay.stderr.log" & pids+=("$!")
wait_port delay "${DELAY_ENDPOINT}"
env "${common_env[@]}" ZLINK_JAVA_E2E_NODE_RID=play-a ZLINK_JAVA_E2E_SPOT_DRAIN_POLICY="${play_a_drain_policy}" ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_PEER_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_SPOT_ENDPOINT="${SPOT_A_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_OBS_FANOUT_ENDPOINT="${FANOUT_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${PLAY_A_HTTP}" "${play_bin}" >"${log_dir}/play-a.stdout.log" 2>"${log_dir}/play-a.stderr.log" & pids+=("$!")
play_a_pid="$!"
wait_port play-a-route "${ROUTE_A_ENDPOINT}"; wait_port play-a-spot "${SPOT_A_ENDPOINT}"; wait_http play-a-http "${PLAY_A_HTTP}"
env "${common_env[@]}" ZLINK_JAVA_E2E_NODE_RID=play-b ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_PEER_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_SPOT_ENDPOINT="${SPOT_B_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_OBS_FANOUT_ENDPOINT="${FANOUT_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${PLAY_B_HTTP}" "${play_bin}" >"${log_dir}/play-b.stdout.log" 2>"${log_dir}/play-b.stderr.log" & pids+=("$!")
play_b_pid="$!"
wait_port play-b-route "${ROUTE_B_ENDPOINT}"; wait_port play-b-spot "${SPOT_B_ENDPOINT}"; wait_http play-b-http "${PLAY_B_HTTP}"
env "${common_env[@]}" ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_B_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_SESSION_ROUTE_ENDPOINT="${SESSION_ROUTE_ENDPOINT}" ZLINK_JAVA_E2E_SESSION_SPOT_ENDPOINT="${SESSION_SPOT_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_STREAM_ENDPOINT="${STREAM_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${SESSION_HTTP}" "${session_bin}" >"${log_dir}/session.stdout.log" 2>"${log_dir}/session.stderr.log" & pids+=("$!")
session_pid="$!"
wait_port session-route "${SESSION_ROUTE_ENDPOINT}"; wait_port session-spot "${SESSION_SPOT_ENDPOINT}"; wait_port session-stream "${STREAM_ENDPOINT}"; wait_http session-http "${SESSION_HTTP}"
wait_metrics_state "${SESSION_HTTP}" true

client_env=(ZLINK_JAVA_STREAM_TRACE=1 JAVA_TOOL_OPTIONS="-Djava.util.logging.config.file=${SCRIPT_DIR}/logging.properties" ZLINK_JAVA_E2E_STREAM_ENDPOINT="${STREAM_ENDPOINT}" ZLINK_JAVA_E2E_PLAY_HTTP="${PLAY_A_HTTP}" ZLINK_JAVA_E2E_PLAY_B_HTTP="${PLAY_B_HTTP}" ZLINK_JAVA_E2E_SESSION_HTTP="${SESSION_HTTP}" ZLINK_JAVA_E2E_LOG_DIR="${log_dir}")
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-A1 ]]; then
  env "${client_env[@]}" timeout -k 5s 90s "${client_bin}" ATD-D4 >"${log_dir}/a1-client.stdout.log" 2>"${log_dir}/a1-client.stderr.log"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-A2 ]]; then
  env "${client_env[@]}" timeout -k 5s 30s "${trigger_bin}" "${STREAM_ENDPOINT}" >"${log_dir}/a2-trigger.stdout.log" 2>"${log_dir}/a2-trigger.stderr.log"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-B2 ]]; then
  env "${client_env[@]}" timeout -k 5s 90s "${client_bin}" OBS-B2 >"${log_dir}/b2-client.stdout.log" 2>"${log_dir}/b2-client.stderr.log"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-A3 ]]; then
  kill -TERM "${session_pid}"
  wait "${session_pid}" || true
  env "${common_env[@]}" ZLINK_JAVA_E2E_MESSAGE_FLOW=off ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_B_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_SESSION_ROUTE_ENDPOINT="${SESSION_ROUTE_ENDPOINT}" ZLINK_JAVA_E2E_SESSION_SPOT_ENDPOINT="${SESSION_SPOT_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_STREAM_ENDPOINT="${STREAM_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${SESSION_HTTP}" "${session_bin}" >"${log_dir}/off-node.stdout.log" 2>"${log_dir}/off-node.stderr.log" & pids+=("$!")
  session_pid="$!"
  wait_port off-node-stream "${STREAM_ENDPOINT}"; wait_http off-node-http "${SESSION_HTTP}"
  wait_metrics_state "${SESSION_HTTP}" true
  env "${client_env[@]}" timeout -k 5s 90s "${client_bin}" ATD-D4 >"${log_dir}/a3-client.stdout.log" 2>"${log_dir}/a3-client.stderr.log"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-A4 || "${SELECTOR}" == OBS-B3 ]]; then
  env "${client_env[@]}" timeout -k 5s 90s "${client_bin}" ATD-C1 >"${log_dir}/a4-client.stdout.log" 2>"${log_dir}/a4-client.stderr.log"
  sleep 1
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-B1 ]]; then
  env "${client_env[@]}" timeout -k 5s 60s "${trigger_bin}" --metrics-b1 "${STREAM_ENDPOINT}" "${log_dir}/connector-metrics.json" >"${log_dir}/b1-trigger.stdout.log" 2>"${log_dir}/b1-trigger.stderr.log"
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
  env "${common_env[@]}" SPRING_AUTOCONFIGURE_EXCLUDE=systems.zlink.framework.spring.ZLinkMetricsAutoConfiguration ZLINK_JAVA_E2E_MESSAGE_FLOW=off ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_B_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_SESSION_ROUTE_ENDPOINT="${SESSION_ROUTE_ENDPOINT}" ZLINK_JAVA_E2E_SESSION_SPOT_ENDPOINT="${SESSION_SPOT_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_STREAM_ENDPOINT="${STREAM_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${SESSION_HTTP}" "${session_bin}" >"${log_dir}/reader-free.stdout.log" 2>"${log_dir}/reader-free.stderr.log" & pids+=("$!")
  session_pid="$!"
  wait_port reader-free-stream "${STREAM_ENDPOINT}"; wait_http reader-free-http "${SESSION_HTTP}"
  wait_metrics_state "${SESSION_HTTP}" false
  env "${client_env[@]}" timeout -k 5s 120s "${trigger_bin}" --reader-free-b4 "${STREAM_ENDPOINT}" "${log_dir}/reader-free-result.json" >"${log_dir}/b4-trigger.stdout.log" 2>"${log_dir}/b4-trigger.stderr.log"
  fetch_url "${SESSION_HTTP}/metrics" "${log_dir}/reader-free-metrics.json"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-C1 ]]; then
  fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c1-before.json"
  env "${client_env[@]}" timeout -k 5s 90s "${client_bin}" ATD-A1 >"${log_dir}/c1-existing.stdout.log" 2>"${log_dir}/c1-existing.stderr.log"
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
  cp "${log_dir}/play-a-flow.log" "${log_dir}/play-a-before-c1-flow.log"
  truncate -s 0 "${log_dir}/play-a-flow.log"
  env "${common_env[@]}" ZLINK_JAVA_E2E_NODE_RID=play-a ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_PEER_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_SPOT_ENDPOINT="${SPOT_A_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_OBS_FANOUT_ENDPOINT="${FANOUT_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${PLAY_A_HTTP}" "${play_bin}" >"${log_dir}/play-a-c1-restart.stdout.log" 2>"${log_dir}/play-a-c1-restart.stderr.log" & pids+=("$!")
  play_a_pid="$!"
  wait_port play-a-restart-route "${ROUTE_A_ENDPOINT}"; wait_port play-a-restart-spot "${SPOT_A_ENDPOINT}"; wait_http play-a-restart-http "${PLAY_A_HTTP}"
  wait_drain_ready "${PLAY_A_HTTP}" play-a
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-C2 ]]; then
  env "${client_env[@]}" timeout -k 5s 60s "${client_bin}" OBS-C2 >"${log_dir}/c2-client.stdout.log" 2>"${log_dir}/c2-client.stderr.log" &
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
  env "${common_env[@]}" ZLINK_JAVA_E2E_NODE_RID=play-a ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_PEER_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_SPOT_ENDPOINT="${SPOT_A_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_OBS_FANOUT_ENDPOINT="${FANOUT_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${PLAY_A_HTTP}" "${play_bin}" >"${log_dir}/play-a-c2-restart.stdout.log" 2>"${log_dir}/play-a-c2-restart.stderr.log" & pids+=("$!")
  play_a_pid="$!"
  wait_port play-a-c2-restart-route "${ROUTE_A_ENDPOINT}"; wait_port play-a-c2-restart-spot "${SPOT_A_ENDPOINT}"; wait_http play-a-c2-restart-http "${PLAY_A_HTTP}"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-C3 ]]; then
  env "${client_env[@]}" timeout -k 5s 60s "${client_bin}" OBS-C3-WRITE >"${log_dir}/c3-write.stdout.log" 2>"${log_dir}/c3-write.stderr.log"
  fetch_url "${PLAY_A_HTTP}/drain/start?deadlineMs=15000" "${log_dir}/c3-natural-start.json"
  fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c3-natural-during.json"
  fetch_url "${PLAY_A_HTTP}/spot/close?spotRid=obs-c3-persistent-room" "${log_dir}/c3-natural-close-room.json"
  fetch_url "${PLAY_A_HTTP}/spot/close?spotRid=await-probe" "${log_dir}/c3-natural-close-probe.json"
  for _ in $(seq 1 150); do
    fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c3-natural-terminal.json"
    python3 - "${log_dir}/c3-natural-terminal.json" <<'PY' && break
import json,sys
raise SystemExit(0 if json.load(open(sys.argv[1])).get('result') else 1)
PY
    sleep 0.1
  done
  fetch_url "${PLAY_A_HTTP}/metrics" "${log_dir}/c3-natural-metrics.json"
  kill -TERM "${play_a_pid}" >/dev/null 2>&1 || true
  wait "${play_a_pid}" || true
  env "${common_env[@]}" ZLINK_JAVA_E2E_NODE_RID=play-a ZLINK_JAVA_E2E_SPOT_DRAIN_POLICY=release ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_PEER_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_SPOT_ENDPOINT="${SPOT_A_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_OBS_FANOUT_ENDPOINT="${FANOUT_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${PLAY_A_HTTP}" "${play_bin}" >"${log_dir}/play-a-c3-release.stdout.log" 2>"${log_dir}/play-a-c3-release.stderr.log" & pids+=("$!")
  play_a_pid="$!"
  wait_port c3-release-route "${ROUTE_A_ENDPOINT}"; wait_http c3-release-http "${PLAY_A_HTTP}"
  env "${client_env[@]}" timeout -k 5s 60s "${client_bin}" OBS-C3-WRITE >"${log_dir}/c3-release-write.stdout.log" 2>"${log_dir}/c3-release-write.stderr.log"
  fetch_url "${PLAY_A_HTTP}/drain/start?deadlineMs=15000" "${log_dir}/c3-release-start.json"
  for _ in $(seq 1 150); do
    fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c3-release-terminal.json"
    python3 - "${log_dir}/c3-release-terminal.json" <<'PY' && break
import json,sys
raise SystemExit(0 if json.load(open(sys.argv[1])).get('result') else 1)
PY
    sleep 0.1
  done
  fetch_url "${PLAY_A_HTTP}/metrics" "${log_dir}/c3-release-metrics.json"
  env "${client_env[@]}" timeout -k 5s 60s "${client_bin}" OBS-C3-READ >"${log_dir}/c3-read.stdout.log" 2>"${log_dir}/c3-read.stderr.log"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-C4 ]]; then
  kill -TERM "${session_pid}" >/dev/null 2>&1 || true
  wait "${session_pid}" || true
  env "${common_env[@]}" ZLINK_JAVA_E2E_SESSION_DRAIN_SPOT="obs-c4-held-spot" ZLINK_JAVA_E2E_MESSAGE_FLOW=off ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_B_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_SESSION_ROUTE_ENDPOINT="${SESSION_ROUTE_ENDPOINT}" ZLINK_JAVA_E2E_SESSION_SPOT_ENDPOINT="${SESSION_SPOT_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_STREAM_ENDPOINT="${STREAM_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${SESSION_HTTP}" "${session_bin}" >"${log_dir}/c4-session.stdout.log" 2>"${log_dir}/c4-session.stderr.log" & pids+=("$!")
  session_pid="$!"
  wait_port c4-session-stream "${STREAM_ENDPOINT}"; wait_http c4-session-http "${SESSION_HTTP}"; wait_metrics_state "${SESSION_HTTP}" true
  env "${client_env[@]}" timeout -k 5s 30s "${trigger_bin}" --drain-watch "${STREAM_ENDPOINT}" "${SESSION_HTTP}/drain/start?deadlineMs=500" "${log_dir}/c4-connector-result.json" >"${log_dir}/c4-trigger.stdout.log" 2>"${log_dir}/c4-trigger.stderr.log"
  fetch_url "${SESSION_HTTP}/drain/status" "${log_dir}/c4-drain-status.json"
  fetch_url "${SESSION_HTTP}/metrics" "${log_dir}/c4-metrics.json"
fi
if [[ "${SELECTOR}" == all || "${SELECTOR}" == OBS-C5 ]]; then
  if [[ "${SELECTOR}" == all ]]; then
    kill -TERM "${play_a_pid}" "${play_b_pid}" "${session_pid}" >/dev/null 2>&1 || true
    wait "${play_a_pid}" || true
    wait "${play_b_pid}" || true
    wait "${session_pid}" || true
    env "${common_env[@]}" ZLINK_JAVA_E2E_NODE_RID=play-a ZLINK_JAVA_E2E_SPOT_DRAIN_POLICY=release ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_PEER_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_SPOT_ENDPOINT="${SPOT_A_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_OBS_FANOUT_ENDPOINT="${FANOUT_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${PLAY_A_HTTP}" "${play_bin}" >"${log_dir}/c5-serving-play-a.stdout.log" 2>"${log_dir}/c5-serving-play-a.stderr.log" & pids+=("$!")
    play_a_pid="$!"
    wait_port c5-serving-play-a-route "${ROUTE_A_ENDPOINT}"; wait_http c5-serving-play-a-http "${PLAY_A_HTTP}"
    env "${common_env[@]}" ZLINK_JAVA_E2E_NODE_RID=play-b ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_PEER_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_SPOT_ENDPOINT="${SPOT_B_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_OBS_FANOUT_ENDPOINT="${FANOUT_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${PLAY_B_HTTP}" "${play_bin}" >"${log_dir}/c5-serving-play-b.stdout.log" 2>"${log_dir}/c5-serving-play-b.stderr.log" & pids+=("$!")
    play_b_pid="$!"
    wait_port c5-serving-play-b-route "${ROUTE_B_ENDPOINT}"; wait_http c5-serving-play-b-http "${PLAY_B_HTTP}"
    env "${common_env[@]}" ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_B_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_SESSION_ROUTE_ENDPOINT="${SESSION_ROUTE_ENDPOINT}" ZLINK_JAVA_E2E_SESSION_SPOT_ENDPOINT="${SESSION_SPOT_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_STREAM_ENDPOINT="${STREAM_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${SESSION_HTTP}" "${session_bin}" >"${log_dir}/c5-serving-session.stdout.log" 2>"${log_dir}/c5-serving-session.stderr.log" & pids+=("$!")
    session_pid="$!"
    wait_port c5-serving-session-route "${SESSION_ROUTE_ENDPOINT}"; wait_port c5-serving-session-spot "${SESSION_SPOT_ENDPOINT}"; wait_port c5-serving-session-stream "${STREAM_ENDPOINT}"; wait_http c5-serving-session-http "${SESSION_HTTP}"; wait_metrics_state "${SESSION_HTTP}" true
  fi
  truncate -s 0 "${log_dir}/play-a-flow.log"
  env "${client_env[@]}" timeout -k 5s 60s "${client_bin}" OBS-C5-PROBE >"${log_dir}/c5-serving-probe.stdout.log" 2>"${log_dir}/c5-serving-probe.stderr.log"
  fetch_url "${PLAY_A_HTTP}/drain/status" "${log_dir}/c5-serving-source-before-probe.json"
  fetch_url "${PLAY_A_HTTP}/route-probe" "${log_dir}/c5-serving-source-probe.json"
  env "${client_env[@]}" timeout -k 5s 60s "${client_bin}" OBS-C5-BIND >"${log_dir}/c5-serving-client.stdout.log" 2>"${log_dir}/c5-serving-client.stderr.log"
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
  env "${common_env[@]}" ZLINK_JAVA_E2E_NODE_RID=play-a ZLINK_JAVA_E2E_SPOT_DRAIN_POLICY=release ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_PEER_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_SPOT_ENDPOINT="${SPOT_A_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_OBS_FANOUT_ENDPOINT="${FANOUT_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${PLAY_A_HTTP}" "${play_bin}" >"${log_dir}/c5-zero-play-a.stdout.log" 2>"${log_dir}/c5-zero-play-a.stderr.log" & pids+=("$!")
  play_a_pid="$!"
  wait_port c5-zero-route "${ROUTE_A_ENDPOINT}"; wait_http c5-zero-http "${PLAY_A_HTTP}"
  env "${common_env[@]}" ZLINK_JAVA_E2E_NODE_RID=play-b ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_PEER_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_SPOT_ENDPOINT="${SPOT_B_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_OBS_FANOUT_ENDPOINT="${FANOUT_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${PLAY_B_HTTP}" "${play_bin}" >"${log_dir}/c5-zero-play-b.stdout.log" 2>"${log_dir}/c5-zero-play-b.stderr.log" & pids+=("$!")
  play_b_pid="$!"
  wait_port c5-zero-play-b-route "${ROUTE_B_ENDPOINT}"; wait_http c5-zero-play-b-http "${PLAY_B_HTTP}"
  env "${common_env[@]}" ZLINK_JAVA_E2E_ROUTE_ENDPOINT="${ROUTE_A_ENDPOINT}" ZLINK_JAVA_E2E_ROUTE_B_ENDPOINT="${ROUTE_B_ENDPOINT}" ZLINK_JAVA_E2E_SESSION_ROUTE_ENDPOINT="${SESSION_ROUTE_ENDPOINT}" ZLINK_JAVA_E2E_SESSION_SPOT_ENDPOINT="${SESSION_SPOT_ENDPOINT}" ZLINK_JAVA_E2E_DELAY_ENDPOINT="${DELAY_ENDPOINT}" ZLINK_JAVA_E2E_STREAM_ENDPOINT="${STREAM_ENDPOINT}" ZLINK_JAVA_E2E_HTTP_ENDPOINT="${SESSION_HTTP}" "${session_bin}" >"${log_dir}/c5-zero-session.stdout.log" 2>"${log_dir}/c5-zero-session.stderr.log" & pids+=("$!")
  session_pid="$!"
  wait_port c5-zero-session-route "${SESSION_ROUTE_ENDPOINT}"; wait_port c5-zero-session-spot "${SESSION_SPOT_ENDPOINT}"; wait_port c5-zero-session-stream "${STREAM_ENDPOINT}"; wait_http c5-zero-session-http "${SESSION_HTTP}"; wait_metrics_state "${SESSION_HTTP}" true
  env "${client_env[@]}" timeout -k 5s 60s "${client_bin}" OBS-C5-BIND >"${log_dir}/c5-zero-bind.stdout.log" 2>"${log_dir}/c5-zero-bind.stderr.log"
  env "${client_env[@]}" timeout -k 5s 60s "${client_bin}" OBS-C5-PROBE >"${log_dir}/c5-zero-held-spot.stdout.log" 2>"${log_dir}/c5-zero-held-spot.stderr.log"
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
