#!/usr/bin/env bash
set -euo pipefail

# Config 11 relocation scenarios. Every assertion is made from a response
# returned by a public Java framework API exposed by the suite-local control
# endpoint; the control files below are only orchestration state.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAVA_E2E_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${JAVA_E2E_DIR}/../e2e-redis-common.sh"

SELECTOR="${1:-}"
case "${SELECTOR}" in
  OBS-C6|OBS-C7|OBS-C8|OBS-C9A|OBS-C9B|OBS-C10|OBS-C11|OBS-C12) ;;
  *) echo "Unknown Config 11 C selector: ${SELECTOR}" >&2; exit 2 ;;
esac

run_id="$(date +%Y%m%d-%H%M%S)-$$-${SELECTOR,,}"
log_dir="${SCRIPT_DIR}/logs/${run_id}"
evidence_dir="${log_dir}/evidence"
config_dir="${log_dir}/config"
repo_root="$(cd "${SCRIPT_DIR}/../../../../.." && pwd)"
obs_build="${HOME}/.cache/zlink/java-e2e/ObservabilityOps"
gradle_cache="${HOME}/.cache/zlink/java-e2e/ObservabilityOps-gradle-cache"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
pids=()
REDIS_CONTAINER=""
mkdir -p -m 0700 "${log_dir}" "${evidence_dir}" "${config_dir}"
echo "log_dir=${log_dir}"

if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-java-e2e-observability-${SELECTOR,,}" "redis:7.2-alpine"
redis_location_endpoint="127.0.0.1:${redis_port}"
location_key_prefix="zlink:e2e:observability:${run_id}"

cleanup() {
  local status="$?"
  set +e
  for ((index=${#pids[@]}-1; index>=0; index--)); do
    kill "${pids[${index}]}" >/dev/null 2>&1 || true
  done
  for pid in "${pids[@]}"; do
    wait "${pid}" >/dev/null 2>&1 || true
  done
  [[ -z "${REDIS_CONTAINER}" ]] || docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
  rm -rf "${config_dir}"
  if [[ "${status}" != 0 ]]; then
    for log in "${log_dir}"/*.log; do
      [[ -f "${log}" ]] && { echo "===== ${log} =====" >&2; tail -n 120 "${log}" >&2; }
    done
  fi
  exit "${status}"
}
trap cleanup EXIT

reserve_ports() {
  python3 - <<'PY'
import socket
sockets=[]
try:
    for _ in range(24):
        sock=socket.socket(); sock.bind(("127.0.0.1", 0)); sockets.append(sock)
    print(" ".join(str(sock.getsockname()[1]) for sock in sockets))
finally:
    for sock in sockets: sock.close()
PY
}
tcp() { echo "tcp://127.0.0.1:$1"; }
http() { echo "http://127.0.0.1:$1"; }
port_of() { echo "${1##*:}"; }

read -r \
  delay_port \
  route_a_port spot_a_port http_a_port maintenance_a_port \
  route_b_port spot_b_port http_b_port maintenance_b_port \
  route_c_port spot_c_port http_c_port maintenance_c_port \
  route_d_port spot_d_port http_d_port maintenance_d_port \
  stream_port session_route_port session_spot_port session_http_port \
  unused_port_a unused_port_b <<<"$(reserve_ports)"

DELAY_ENDPOINT="$(tcp "${delay_port}")"
STREAM_ENDPOINT="$(tcp "${stream_port}")"
SESSION_ROUTE_ENDPOINT="$(tcp "${session_route_port}")"
SESSION_SPOT_ENDPOINT="$(tcp "${session_spot_port}")"
SESSION_HTTP="$(http "${session_http_port}")"

declare -A ROUTE_ENDPOINT=(
  [play-a]="$(tcp "${route_a_port}")"
  [play-b]="$(tcp "${route_b_port}")"
  [play-c]="$(tcp "${route_c_port}")"
  [play-d]="$(tcp "${route_d_port}")")
declare -A SPOT_ENDPOINT=(
  [play-a]="$(tcp "${spot_a_port}")"
  [play-b]="$(tcp "${spot_b_port}")"
  [play-c]="$(tcp "${spot_c_port}")"
  [play-d]="$(tcp "${spot_d_port}")")
declare -A HTTP_ENDPOINT=(
  [play-a]="$(http "${http_a_port}")"
  [play-b]="$(http "${http_b_port}")"
  [play-c]="$(http "${http_c_port}")"
  [play-d]="$(http "${http_d_port}")")
declare -A MAINTENANCE_ENDPOINT=(
  [play-a]="$(http "${maintenance_a_port}")"
  [play-b]="$(http "${maintenance_b_port}")"
  [play-c]="$(http "${maintenance_c_port}")"
  [play-d]="$(http "${maintenance_d_port}")")

write_config() {
  local path="$1"
  shift
  printf '%s\n' "$@" >"${path}"
  chmod 0600 "${path}"
}

fetch_json() {
  local url="$1" output="$2"
  python3 - "${url}" "${output}" <<'PY'
import pathlib, sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=5) as response:
    pathlib.Path(sys.argv[2]).write_bytes(response.read())
PY
}

post_json() {
  fetch_json "$1" "$2"
}

post_background() {
  local url="$1" output="$2"
  python3 - "${url}" "${output}" <<'PY'
import pathlib, sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=45) as response:
    pathlib.Path(sys.argv[2]).write_bytes(response.read())
PY
}

wait_http() {
  local name="$1" url="$2"
  for _ in $(seq 1 30); do
    if python3 - "${url}" 2>/dev/null <<'PY'
import sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    response.read()
PY
    then return 0; fi
    sleep 0.1
  done
  echo "Timed out waiting for ${name} at ${url}" >&2
  return 1
}

wait_ready() {
  local node="$1"
  for _ in $(seq 1 30); do
    if python3 - "${MAINTENANCE_ENDPOINT[${node}]}/maintenance/status" "${node}" 2>/dev/null <<'PY'
import json, sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    status=json.loads(response.read())
rows=status.get("topology", [])
ready=(status.get("isReady") is True
       and status.get("state") == "SERVING"
       and any(row.get("nodeRid") == sys.argv[2]
               and row.get("state") == "READY"
               and not row.get("draining", False) for row in rows))
raise SystemExit(0 if ready else 1)
PY
    then return 0; fi
    sleep 0.1
  done
  echo "Timed out waiting for ${node} readiness" >&2
  return 1
}

wait_peer() {
  local node="$1" peer="$2"
  local peer_log="${log_dir}/${node}.stdout.log"
  for _ in $(seq 1 300); do
    if python3 - "${MAINTENANCE_ENDPOINT[${node}]}/maintenance/status" "${peer}" 2>/dev/null <<'PY'
import json, sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    status=json.loads(response.read())
ready=any(row.get("nodeRid") == sys.argv[2] and row.get("state") == "READY"
          for row in status.get("topology", []))
raise SystemExit(0 if ready else 1)
PY
    then
      if rg -q "ZLINK_FRAMEWORK_PEER_READY .*peer=${peer}" "${peer_log}" 2>/dev/null; then
        return 0
      fi
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${peer} admission at ${node}" >&2
  return 1
}

wait_pending() {
  local node="$1"
  for _ in $(seq 1 100); do
    if python3 - "${MAINTENANCE_ENDPOINT[${node}]}/maintenance/status" 2>/dev/null <<'PY'
import json, sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    value=json.loads(response.read())
pending=(value.get("state") == "SERVING"
         and value.get("isReady") is True
         and value.get("relocationResult") is None)
raise SystemExit(0 if pending else 1)
PY
    then return 0; fi
    sleep 0.1
  done
  echo "Relocation did not remain in public Serving preflight" >&2
  return 1
}

wait_gate() {
  local node="$1"
  for _ in $(seq 1 100); do
    fetch_json "${MAINTENANCE_ENDPOINT[${node}]}/maintenance/status" "${log_dir}/gate-poll.json"
    if python3 - "${log_dir}/gate-poll.json" <<'PY'
import json, sys
value=json.load(open(sys.argv[1], encoding="utf-8"))
raise SystemExit(0 if value.get("gate", {}).get("callbackEnteredAt") else 1)
PY
    then cp "${log_dir}/gate-poll.json" "${log_dir}/gate-entered.json"; return 0; fi
    sleep 0.05
  done
  echo "Closing callback did not enter the public gate" >&2
  return 1
}

start_delay() {
  local config="${config_dir}/delay.properties"
  write_config "${config}" \
    "e2e.delay-endpoint=${DELAY_ENDPOINT}" \
    "e2e.redis-location-endpoint=${redis_location_endpoint}" \
    "e2e.location-key-prefix=${location_key_prefix}" \
    "e2e.log-dir=${log_dir}"
  "${delay_bin}" --config "${config}" \
    >"${log_dir}/delay.stdout.log" 2>"${log_dir}/delay.stderr.log" &
  pids+=("$!")
}

start_play() {
  local node="$1" version="$2" weight="$3" automatic="$4"
  local peer=""
  if [[ "${automatic}" == false ]]; then
    if [[ "${node}" == play-a ]]; then peer="${ROUTE_ENDPOINT[play-b]}"; else peer="${ROUTE_ENDPOINT[play-a]}"; fi
  fi
  local config="${config_dir}/${node}.properties"
  write_config "${config}" \
    "e2e.node-rid=${node}" \
    "e2e.route-endpoint=${ROUTE_ENDPOINT[${node}]}" \
    "e2e.route-peer-endpoint=${peer}" \
    "e2e.spot-endpoint=${SPOT_ENDPOINT[${node}]}" \
    "e2e.delay-endpoint=${DELAY_ENDPOINT}" \
    "e2e.fanout-endpoint=" \
    "e2e.http-endpoint=${HTTP_ENDPOINT[${node}]}" \
    "e2e.maintenance-endpoint=${MAINTENANCE_ENDPOINT[${node}]}" \
    "e2e.redis-location-endpoint=${redis_location_endpoint}" \
    "e2e.location-key-prefix=${location_key_prefix}" \
    "e2e.log-dir=${log_dir}/${node}-flow" \
    "e2e.application-version=${version}" \
    "e2e.placement-weight=${weight}" \
    "e2e.automatic-topology=${automatic}"
  mkdir -p "${log_dir}/${node}-flow"
  "${play_bin}" --config "${config}" \
    >"${log_dir}/${node}.stdout.log" 2>"${log_dir}/${node}.stderr.log" &
  pids+=("$!")
}

start_session() {
  local route_b="$1"
  local config="${config_dir}/session.properties"
  write_config "${config}" \
    "e2e.message-flow=on" \
    "e2e.route-endpoint=${ROUTE_ENDPOINT[play-a]}" \
    "e2e.route-b-endpoint=${route_b}" \
    "e2e.session-route-endpoint=${SESSION_ROUTE_ENDPOINT}" \
    "e2e.session-spot-endpoint=${SESSION_SPOT_ENDPOINT}" \
    "e2e.delay-endpoint=${DELAY_ENDPOINT}" \
    "e2e.stream-endpoint=${STREAM_ENDPOINT}" \
    "e2e.http-endpoint=${SESSION_HTTP}" \
    "e2e.session-drain-spot=" \
    "e2e.redis-location-endpoint=${redis_location_endpoint}" \
    "e2e.location-key-prefix=${location_key_prefix}" \
    "e2e.log-dir=${log_dir}/session-flow"
  mkdir -p "${log_dir}/session-flow"
  "${session_bin}" --config "${config}" \
    >"${log_dir}/session.stdout.log" 2>"${log_dir}/session.stderr.log" &
  pids+=("$!")
}

run_client() {
  local selector="$1" stdout="$2" timeout_value="$3" release_file="${4:-}"
  local config="${config_dir}/client-${selector}.properties"
  write_config "${config}" \
    "streamEndpoint=${STREAM_ENDPOINT}" \
    "playHttpEndpoint=${HTTP_ENDPOINT[play-a]}" \
    "playBHttpEndpoint=${HTTP_ENDPOINT[play-b]}" \
    "sessionHttpEndpoint=${SESSION_HTTP}" \
    "scenarioOutput=" \
    "drainUrl=" \
    "relocationReleaseFile=${release_file}"
  timeout -k 5s "${timeout_value}" "${client_bin}" \
    --config "${config}" --scenario "${selector}" \
    >"${stdout}" 2>&1
}

start_client_background() {
  local selector="$1" release_file="${2:-}"
  local stdout="${log_dir}/${selector}.stdout.log"
  run_client "${selector}" "${stdout}" 70s "${release_file}" &
  client_pid="$!"
  pids+=("${client_pid}")
}

wait_prepared() {
  local selector="$1"
  local output_selector="${selector}-PREPARE"
  for _ in $(seq 1 300); do
    if [[ -f "${log_dir}/${output_selector}.stdout.log" ]] \
      && grep -q "${selector} prepared" "${log_dir}/${output_selector}.stdout.log"; then
      return 0
    fi
    if ! kill -0 "${client_pid}" >/dev/null 2>&1; then
      echo "${selector} prepare client exited before public workload was bound" >&2
      return 1
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${selector} workload" >&2
  return 1
}

start_common() {
  local automatic="$1" route_b="$2"
  start_delay
  start_play play-a 1 100 "${automatic}"
  wait_http play-a "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/health"
  wait_ready play-a
  if [[ "${route_b}" != absent ]]; then
    start_play play-b "$3" "$4" "${automatic}"
    wait_http play-b "${MAINTENANCE_ENDPOINT[play-b]}/maintenance/health"
    wait_ready play-b
  fi
  start_session "${route_b_value:-}"
  wait_http session "${SESSION_HTTP}/evidence"
  wait_peer play-a session-a
  if [[ "${route_b}" != absent ]]; then
    wait_peer play-b session-a
  fi
}

scenario_spot() {
  printf '%s' "obs-${SELECTOR,,}-room" | tr -c 'a-z0-9-' '-'
}

compose_evidence() {
  python3 - "${SELECTOR}" "${evidence_dir}" "${log_dir}" <<'PY'
import json, pathlib, re, sys
selector, evidence_dir, log_dir = sys.argv[1:]
root=pathlib.Path(log_dir)
def load(name):
    return json.loads((root/name).read_text(encoding="utf-8"))
def save(value):
    pathlib.Path(evidence_dir, selector + ".json").write_text(
        json.dumps(value, sort_keys=True), encoding="utf-8")
def result(name): return load(name)
def node_from_client(name):
    text=(root/name).read_text(encoding="utf-8")
    match=re.findall(r" after node=([^ ]+)", text)
    return match[-1] if match else ""
if selector in ("OBS-C6", "OBS-C7"):
    relocation=result("relocation.json")
    save({"scenario":selector, "relocation":relocation,
          "before":load("source-before.json"),
          "after":load("target-after.json"),
          "handler":{"nodeRid":node_from_client(selector+"-AFTER.stdout.log")},
          "sourceStatus":load("source-status.json"),
          "shutdown":result("shutdown.json")})
elif selector == "OBS-C8":
    entered=load("gate-entered.json")
    terminal=load("terminal-status.json")
    stable=load("terminal-status-again.json")
    before=load("metrics-before.json")
    after=load("metrics-after.json")
    def metric(rows):
        return sum(float(row.get("value", 0)) for row in rows
                   if row.get("name") == "zlink.drain.forced")
    save({"scenario":selector, "gate":entered.get("gate", {}),
          "hostDeadline":entered.get("deadline", ""),
          "shutdown":load("shutdown.json"),
          "forcedMetricDelta":metric(after.get("metrics", []))-metric(before.get("metrics", [])),
          "terminal":terminal, "terminalAgain":stable,
          "terminalStable":terminal.get("state") == stable.get("state")
              and terminal.get("terminationResult") == stable.get("terminationResult")})
elif selector == "OBS-C9A":
    save({"scenario":selector, "sourceDuring":load("source-during.json"),
          "relocation":load("relocation.json"), "targetStatus":load("target-status.json"),
          "after":load("target-after.json"),
          "handler":{"nodeRid":node_from_client("OBS-C9A-AFTER.stdout.log")}})
elif selector == "OBS-C9B":
    save({"scenario":selector, "relocation":load("relocation.json"),
          "sourceDuring":load("source-before.json"), "shutdown":load("shutdown.json")})
elif selector == "OBS-C10":
    save({"scenario":selector, "planned":load("relocation-one.json"),
          "rolling":load("relocation-two.json"), "firstTarget":load("target-one.json"),
          "secondTarget":load("target-two.json"),
          "handler":{"nodeRid":node_from_client("OBS-C10-AFTER.stdout.log")},
          "highVersionNodeObserved":False})
elif selector == "OBS-C11":
    save({"scenario":selector, "first":load("relocation.json"),
          "joined":load("joined.json"), "conflictingMode":load("conflicting-mode.json"),
          "conflictingVersion":load("conflicting-version.json"),
          "target":load("target-after.json"),
          "handler":{"nodeRid":node_from_client("OBS-C11-AFTER.stdout.log")},
          "sourceStatus":load("source-status.json")})
elif selector == "OBS-C12":
    save({"scenario":selector, "primary":load("relocation.json"),
          "shutdown":load("shutdown.json"), "status":load("terminal-status.json"),
          "statusAgain":load("terminal-status-again.json"), "cancelledWaiter":True,
          "terminalStable":load("terminal-status.json") == load("terminal-status-again.json")})
PY
}

delay_bin="${obs_build}/Server-Delay/install/observability-ops-delay/bin/observability-ops-delay"
play_bin="${obs_build}/Server-Play/install/observability-ops-play/bin/observability-ops-play"
session_bin="${obs_build}/Server-Session/install/observability-ops-session/bin/observability-ops-session"
client_bin="${obs_build}/Client/install/observability-ops-client/bin/observability-ops-client"
verifier_bin="${obs_build}/Verifier/install/observability-ops-verifier/bin/observability-ops-verifier"

"${SCRIPT_DIR}/gradlew" -PzlinkE2eBuildDir="${obs_build}" \
  --project-cache-dir "${gradle_cache}" --no-daemon --no-parallel --max-workers=1 --quiet \
  installDist

route_b_value=""
case "${SELECTOR}" in
  OBS-C6|OBS-C7)
    route_b_value="${ROUTE_ENDPOINT[play-b]}"
    # Keep the workload on the explicit source node; the lower target weight
    # prevents initial placement on play-b before relocation begins.
    start_common true "present" 2 1
    release_file="${log_dir}/${SELECTOR}.release"
    start_client_background "${SELECTOR}-PREPARE" "${release_file}"
    wait_prepared "${SELECTOR}"
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/objects?spotId=$(scenario_spot)&actorId=obs-${SELECTOR,,}-actor" "${log_dir}/source-before.json"
    if [[ "${SELECTOR}" == OBS-C6 ]]; then
      post_background "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/relocate?mode=rolling-update&targetApplicationVersion=2&deadlineMs=30000" "${log_dir}/relocation.json" & relocation_pid="$!"
    else
      post_background "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/relocate?mode=planned-maintenance&deadlineMs=30000" "${log_dir}/relocation.json" & relocation_pid="$!"
    fi
    wait "${relocation_pid}"
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/status" "${log_dir}/source-status.json"
    fetch_json "${MAINTENANCE_ENDPOINT[play-b]}/maintenance/objects?spotId=$(scenario_spot)&actorId=obs-${SELECTOR,,}-actor" "${log_dir}/target-after.json"
    touch "${release_file}"
    wait "${client_pid}"
    cp "${log_dir}/${SELECTOR}-PREPARE.stdout.log" "${log_dir}/${SELECTOR}-AFTER.stdout.log"
    post_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/shutdown?deadlineMs=10000" "${log_dir}/shutdown.json"
    compose_evidence
    ;;
  OBS-C8)
    start_common true absent 0 0
    post_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/gate/arm" "${log_dir}/gate-armed.json"
    post_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/spot/create?spotId=obs-c8-blocking&spotType=maintenance-probe" "${log_dir}/spot.json"
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/status" "${log_dir}/metrics-before.json"
    post_background "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/shutdown?deadlineMs=300" "${log_dir}/shutdown.json" & shutdown_pid="$!"
    wait_gate play-a
    wait "${shutdown_pid}"
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/status" "${log_dir}/terminal-status.json"
    post_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/gate/release" "${log_dir}/gate-release.json"
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/status" "${log_dir}/terminal-status-again.json"
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/status" "${log_dir}/metrics-after.json"
    compose_evidence
    ;;
  OBS-C9A)
    start_common true absent 0 0
    release_file="${log_dir}/${SELECTOR}.release"
    start_client_background "OBS-C9A-PREPARE" "${release_file}"
    wait_prepared OBS-C9A
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/objects?spotId=$(scenario_spot)&actorId=obs-obs-c9a-actor" "${log_dir}/source-during.json"
    post_background "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/relocate?mode=rolling-update&targetApplicationVersion=2&deadlineMs=30000" "${log_dir}/relocation.json" & relocation_pid="$!"
    wait_pending play-a
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/status" "${log_dir}/source-during.json"
    start_play play-b 2 100 true
    wait_http play-b "${MAINTENANCE_ENDPOINT[play-b]}/maintenance/health"
    wait_ready play-b
    wait "${relocation_pid}"
    fetch_json "${MAINTENANCE_ENDPOINT[play-b]}/maintenance/status" "${log_dir}/target-status.json"
    fetch_json "${MAINTENANCE_ENDPOINT[play-b]}/maintenance/objects?spotId=$(scenario_spot)&actorId=obs-obs-c9a-actor" "${log_dir}/target-after.json"
    touch "${release_file}"
    wait "${client_pid}"
    cp "${log_dir}/OBS-C9A-PREPARE.stdout.log" "${log_dir}/OBS-C9A-AFTER.stdout.log"
    compose_evidence
    ;;
  OBS-C9B)
    route_b_value="${ROUTE_ENDPOINT[play-b]}"
    start_common false present 1 100
    start_client_background OBS-C9B-PREPARE
    wait_prepared OBS-C9B
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/objects?spotId=$(scenario_spot)&actorId=obs-obs-c9b-actor" "${log_dir}/source-before.json"
    post_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/relocate?mode=planned-maintenance&deadlineMs=5000" "${log_dir}/relocation.json"
    post_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/shutdown?deadlineMs=10000" "${log_dir}/shutdown.json"
    compose_evidence
    ;;
  OBS-C10)
    start_delay
    start_play play-a 1 100 true
    start_play play-b 1 10 true
    start_play play-c 2 10 true
    start_play play-d 3 1000 true
    wait_http play-a "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/health"
    wait_http play-b "${MAINTENANCE_ENDPOINT[play-b]}/maintenance/health"
    wait_http play-c "${MAINTENANCE_ENDPOINT[play-c]}/maintenance/health"
    wait_http play-d "${MAINTENANCE_ENDPOINT[play-d]}/maintenance/health"
    wait_ready play-a; wait_ready play-b; wait_ready play-c; wait_ready play-d
    route_b_value="${ROUTE_ENDPOINT[play-b]}"
    start_session "${route_b_value}"
    wait_http session "${SESSION_HTTP}/evidence"
    wait_peer play-a session-a
    wait_peer play-b session-a
    release_file="${log_dir}/${SELECTOR}.release"
    start_client_background OBS-C10-PREPARE "${release_file}"
    wait_prepared OBS-C10
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/objects?spotId=$(scenario_spot)&actorId=obs-obs-c10-actor" "${log_dir}/source-before.json"
    post_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/relocate?mode=planned-maintenance&deadlineMs=30000" "${log_dir}/relocation-one.json"
    fetch_json "${MAINTENANCE_ENDPOINT[play-b]}/maintenance/objects?spotId=$(scenario_spot)&actorId=obs-obs-c10-actor" "${log_dir}/target-one.json"
    post_json "${MAINTENANCE_ENDPOINT[play-b]}/maintenance/relocate?mode=rolling-update&targetApplicationVersion=2&deadlineMs=30000" "${log_dir}/relocation-two.json"
    fetch_json "${MAINTENANCE_ENDPOINT[play-c]}/maintenance/objects?spotId=$(scenario_spot)&actorId=obs-obs-c10-actor" "${log_dir}/target-two.json"
    touch "${release_file}"
    wait "${client_pid}"
    cp "${log_dir}/OBS-C10-PREPARE.stdout.log" "${log_dir}/OBS-C10-AFTER.stdout.log"
    compose_evidence
    ;;
  OBS-C11)
    start_common true absent 0 0
    release_file="${log_dir}/${SELECTOR}.release"
    start_client_background OBS-C11-PREPARE "${release_file}"
    wait_prepared OBS-C11
    post_background "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/relocate?mode=rolling-update&targetApplicationVersion=2&deadlineMs=30000" "${log_dir}/relocation.json" & relocation_pid="$!"
    wait_pending play-a
    post_background "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/relocate?mode=rolling-update&targetApplicationVersion=2&deadlineMs=30000" "${log_dir}/joined.json" & joined_pid="$!"
    post_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/relocate?mode=planned-maintenance&deadlineMs=5000" "${log_dir}/conflicting-mode.json"
    post_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/relocate?mode=rolling-update&targetApplicationVersion=3&deadlineMs=5000" "${log_dir}/conflicting-version.json"
    start_play play-b 2 100 true
    wait_http play-b "${MAINTENANCE_ENDPOINT[play-b]}/maintenance/health"
    wait_ready play-b
    wait "${relocation_pid}"; wait "${joined_pid}"
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/status" "${log_dir}/source-status.json"
    fetch_json "${MAINTENANCE_ENDPOINT[play-b]}/maintenance/objects?spotId=$(scenario_spot)&actorId=obs-obs-c11-actor" "${log_dir}/target-after.json"
    touch "${release_file}"
    wait "${client_pid}"
    cp "${log_dir}/OBS-C11-PREPARE.stdout.log" "${log_dir}/OBS-C11-AFTER.stdout.log"
    compose_evidence
    ;;
  OBS-C12)
    start_common true absent 0 0
    start_client_background OBS-C12-PREPARE
    wait_prepared OBS-C12
    post_background "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/relocate?mode=planned-maintenance&deadlineMs=30000" "${log_dir}/relocation.json" & relocation_pid="$!"
    wait_pending play-a
    post_background "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/relocate?mode=planned-maintenance&deadlineMs=30000" "${log_dir}/cancelled-waiter.json" & cancelled_pid="$!"
    sleep 0.15
    kill "${cancelled_pid}" >/dev/null 2>&1 || true
    post_background "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/shutdown?deadlineMs=10000" "${log_dir}/shutdown.json" & shutdown_pid="$!"
    wait "${relocation_pid}"; wait "${shutdown_pid}"
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/status" "${log_dir}/terminal-status.json"
    fetch_json "${MAINTENANCE_ENDPOINT[play-a]}/maintenance/status" "${log_dir}/terminal-status-again.json"
    compose_evidence
    ;;
esac

"${verifier_bin}" "${evidence_dir}" "${SELECTOR}"
echo "observability-ops ${SELECTOR} result=passed log_dir=${log_dir}"
