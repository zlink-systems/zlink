#!/usr/bin/env bash
set -euo pipefail
# Config 8 AutomaticTurnDispatch deployment runner.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/start-order-common.sh"

cd "$(dirname "${BASH_SOURCE[0]}")"

pids=()
REDIS_CONTAINER=""
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
SCENARIO="${1:-all}"
e2e_start_order="$(zlink_e2e_start_order_mode "$@")"
echo "start_order=${e2e_start_order}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
readonly e2e_build_dir="${HOME}/.cache/zlink/java-e2e/AutomaticTurnDispatch"
readonly gradle_cache_dir="${HOME}/.cache/zlink/java-e2e/AutomaticTurnDispatch-gradle-cache"
zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-java-e2e" "redis:7.2-alpine"
redis_location_endpoint="127.0.0.1:${redis_port}"
location_key_prefix="zlink:e2e:automaticturn:${run_id}"
config_dir="$(mktemp -d)"
chmod 0700 "${config_dir}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
if rg -n 'java\.net\.http\.HttpClient|HttpClient\.new' Client/src/main/java --glob '*.java'; then
  echo "AutomaticTurnDispatch client must use ZLinkHttpClient" >&2
  exit 1
fi

print_logs() {
  local status="$1"
  if [[ "${status}" == "0" ]]; then
    return
  fi
  for log in "${log_dir}"/*.log; do
    [[ -f "${log}" ]] || continue
    echo "===== ${log} =====" >&2
    tail -n 200 "${log}" >&2 || true
  done
}

descendants() {
  local pid="$1"
  local child
  (pgrep -P "${pid}" 2>/dev/null || true) | while read -r child; do
    descendants "${child}"
    echo "${child}"
  done
}

cleanup() {
  local status="$?"
  set +e
  print_logs "${status}"
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    local pid="${pids[$i]}"
    for child in $(descendants "${pid}"); do
      kill "${child}" >/dev/null 2>&1 || true
    done
    kill "${pid}" >/dev/null 2>&1 || true
  done
  for _ in $(seq 1 50); do
    local alive=0
    for pid in "${pids[@]}"; do
      if kill -0 "${pid}" >/dev/null 2>&1; then
        alive=1
        break
      fi
    done
    [[ "${alive}" == "0" ]] && break
    sleep 0.1
  done
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    local pid="${pids[$i]}"
    for child in $(descendants "${pid}"); do
      kill -9 "${child}" >/dev/null 2>&1 || true
    done
    kill -9 "${pid}" >/dev/null 2>&1 || true
  done
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    docker rm -fv "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
  fi
  rm -rf "${config_dir}"
  wait >/dev/null 2>&1 || true
  exit "${status}"
}
trap cleanup EXIT

reserve_ports() {
  python3 - <<'PY'
import socket
sockets = []
ports = []
try:
    for _ in range(13):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
    print(" ".join(str(port) for port in ports))
finally:
    for sock in sockets:
        sock.close()
PY
}

tcp() {
  echo "tcp://127.0.0.1:$1"
}

http() {
  echo "http://127.0.0.1:$1"
}

port_of() {
  echo "${1##*:}"
}

wait_port() {
  local name="$1"
  local endpoint="$2"
  local port
  port="$(port_of "${endpoint}")"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}

wait_http() {
  local name="$1"
  local endpoint="$2"
  for _ in $(seq 1 600); do
    if python3 - "${endpoint}/evidence" >/dev/null 2>&1 <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    response.read()
PY
    then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}

wait_evidence_contains() {
  local endpoint="$1"
  local marker="$2"
  local subject="$3"
  local message="$4"
  for _ in $(seq 1 600); do
    if python3 - "${endpoint}/evidence" "${marker}" "${subject}" >/dev/null 2>&1 <<'PY'
import json
import sys
import urllib.request

with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    snapshot = json.loads(response.read().decode("utf-8"))
for entry in snapshot.get("entries", []):
    if entry.get("marker") == sys.argv[2] and entry.get("subject") == sys.argv[3]:
        sys.exit(0)
sys.exit(1)
PY
    then
      return 0
    fi
    sleep 0.1
  done
  echo "${message}" >&2
  return 1
}

fetch_evidence() {
  local endpoint="$1"
  local output="$2"
  python3 - "${endpoint}/evidence" >"${output}" <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=5) as response:
    sys.stdout.write(response.read().decode("utf-8"))
PY
}

write_marker_report() {
  python3 - "${log_dir}/automatic-turn-dispatch-marker-report.json" \
    "${log_dir}/play-a-evidence.json" \
    "${log_dir}/play-b-evidence.json" \
    "${log_dir}/session-evidence.json" \
    "${log_dir}/runner-gates.json" <<'PY'
import json
import sys

output = sys.argv[1]
sources = {
    "play-a": sys.argv[2],
    "play-b": sys.argv[3],
    "session": sys.argv[4],
    "runner": sys.argv[5],
}
scenario_prefixes = {
    "ATD-A1": ["atda1-"],
    "ATD-A2": ["atda2-"],
    "ATD-A3": ["atda3-"],
    "ATD-A4": ["atda4-"],
    "ATD-B1": ["atdb1-"],
    "ATD-B2": ["atdb2-"],
    "ATD-B3": ["atdb3-"],
    "ATD-C1": ["atdc1-"],
    "ATD-C2": ["atdc2-"],
    "ATD-C3": ["atdc3-"],
    "ATD-D1": ["atda2-"],
    "ATD-D2": ["atdd2-"],
    "ATD-D3": ["atdd3-"],
    "ATD-D4": ["atdd4-"],
    "ATD-E1": ["atde1-"],
    "ATD-E2": ["atde2-"],
    "ATD-E3": ["atde3-"],
    "ATD-E4": ["atde4-"],
    "ATD-E5": ["atde5-"],
}
required_markers = {
    "ATD-A1": ["hold-started", "probe-started", "probe-completed", "hold-resumed", "hold-completed"],
    "ATD-A2": ["await-started", "await-released", "probe-started", "probe-completed", "await-resumed", "await-completed"],
    "ATD-A3": ["await-started", "await-released", "await-resumed", "await-completed"],
    "ATD-A4": ["worker-await-started", "worker-await-released", "probe-started", "probe-completed", "worker-await-resumed", "worker-await-completed"],
    "ATD-B1": ["actor-await-started", "actor-await-released", "actor-fast-started", "actor-fast-completed", "actor-await-resumed", "actor-await-completed"],
    "ATD-B2": ["actor-await-started", "actor-await-released", "actor-await-resumed", "actor-await-completed", "actor-fast-started", "actor-fast-completed"],
    "ATD-B3": ["actor-join-await-started", "actor-join-await-released", "actor-fast-started", "actor-fast-completed", "actor-join-await-resumed", "actor-join-await-completed"],
    "ATD-C1": ["timer-await-started", "timer-await-released", "timer-fast-started", "timer-fast-completed", "timer-await-resumed", "timer-await-completed"],
    "ATD-C2": ["timer-await-started", "timer-await-released", "timer-await-resumed", "timer-await-completed", "timer-next-started", "timer-next-completed"],
    "ATD-C3": ["actor-await-started", "actor-await-released", "timer-fast-started", "timer-fast-completed", "actor-await-resumed", "actor-await-completed", "timer-await-started", "timer-await-released", "actor-fast-started", "actor-fast-completed", "timer-await-resumed", "timer-await-completed"],
    "ATD-D1": ["await-started", "await-released", "probe-started", "probe-completed", "await-resumed", "await-completed"],
    "ATD-D2": ["remote-await-started", "remote-await-released", "remote-await-resumed", "remote-await-completed", "await-started", "await-released", "await-resumed", "await-completed"],
    "ATD-D3": ["await-started", "await-released", "probe-started", "probe-completed", "await-resumed", "await-completed"],
    "ATD-D4": ["actor-push-await-started", "actor-push-await-released", "actor-push-await-resumed", "actor-push-await-completed"],
    "ATD-E1": ["timeout-await-started", "timeout-await-released", "timeout-await-completed", "probe-started", "probe-completed"],
    "ATD-E2": ["cancel-await-started", "cancel-await-released", "cancel-await-completed", "probe-started", "probe-completed"],
    "ATD-E3": ["shutdown-await-cleaned", "shutdown-recovery-completed"],
    "ATD-E4": ["static-contract-verified"],
    "ATD-E5": ["marker-schema-verified"],
}

entries = []
for role, path in sources.items():
    with open(path, encoding="utf-8") as handle:
        snapshot = json.load(handle)
    for entry in snapshot.get("entries", []):
        entries.append({
            "role": role,
            "marker": entry.get("marker", ""),
            "subject": entry.get("subject", ""),
            "value": entry.get("value", ""),
        })

scenarios = {}
for scenario_id, prefixes in scenario_prefixes.items():
    scenario_entries = [
        entry for entry in entries
        if any(entry["subject"].startswith(prefix) for prefix in prefixes)
    ]
    observed = sorted({entry["marker"] for entry in scenario_entries})
    missing = [
        marker for marker in required_markers[scenario_id]
        if marker not in observed
    ]
    if missing:
        raise SystemExit(f"{scenario_id} marker report missing markers: {', '.join(missing)}")
    scenarios[scenario_id] = {
        "markers": observed,
        "roles": sorted({entry["role"] for entry in scenario_entries}),
    }

with open(output, "w", encoding="utf-8") as handle:
    json.dump({
        "language": "java",
        "config": "AutomaticTurnDispatch",
        "scenarios": scenarios,
    }, handle, ensure_ascii=False, indent=2)
    handle.write("\n")
PY
}

terminate_gracefully() {
  local name="$1"
  local pid="$2"
  local targets=()
  local child
  while read -r child; do
    [[ -n "${child}" ]] && targets+=("${child}")
  done < <(descendants "${pid}")
  targets+=("${pid}")
  for child in "${targets[@]}"; do
    kill -TERM "${child}" >/dev/null 2>&1 || true
  done
  for _ in $(seq 1 600); do
    local alive=0
    for child in "${targets[@]}"; do
      if kill -0 "${child}" >/dev/null 2>&1; then
        alive=1
        break
      fi
    done
    if [[ "${alive}" == "0" ]]; then
      wait "${pid}" >/dev/null 2>&1 || true
      return 0
    fi
    sleep 0.1
  done
  echo "${name} did not stop after SIGTERM; sending SIGKILL" >&2
  for child in "${targets[@]}"; do
    if command -v jcmd >/dev/null 2>&1 && kill -0 "${child}" >/dev/null 2>&1; then
      jcmd "${child}" Thread.print >"${log_dir}/${name}-${child}-thread-dump.log" 2>&1 || true
    fi
  done
  for child in "${targets[@]}"; do
    kill -KILL "${child}" >/dev/null 2>&1 || true
  done
  wait "${pid}" >/dev/null 2>&1 || true
  return 1
}

static_checks() {
  local tmp
  if [[ "${LOCAL_READINESS_TIMEOUT_SECONDS}" != 3 \
     || "${LOCAL_READINESS_ATTEMPTS}" != 30 ]]; then
    echo "AutomaticTurnDispatch must use a 3s readiness limit" >&2
    return 1
  fi
  tmp="$(mktemp)"
  if grep -RInE 'POST|\.POST\(' Client Server Shared --include='*.java' >"${tmp}"; then
    cat "${tmp}" >&2
    rm -f "${tmp}"
    echo "AutomaticTurnDispatch scenarios must not be triggered through HTTP." >&2
    return 1
  fi
  rm -f "${tmp}"

  tmp="$(mktemp)"
  if ! grep -RInF '.yield(' Client Server Shared --include='*.java' >"${tmp}"; then
    rm -f "${tmp}"
    echo "AutomaticTurnDispatch must exercise the yield terminator." >&2
    return 1
  fi
  rm -f "${tmp}"

  if ! grep -q 'ZLinkStreamConnectorFactory.create' Client/src/main/java/systems/zlink/e2e/automaticturn/client/Program.java; then
    echo "AutomaticTurnDispatch client must create a real stream connector directly." >&2
    return 1
  fi
}

assert_readiness() {
  : >"${log_dir}/readiness.stdout.log"
  : >"${log_dir}/readiness.stderr.log"
  timeout -k 5s 20s "$(client_bin)" --config "${client_config}" --readiness \
    >>"${log_dir}/readiness.stdout.log" 2>>"${log_dir}/readiness.stderr.log"
}

gradle_run() {
  ../../gradlew -PzlinkE2eBuildDir="${e2e_build_dir}" \
    --project-cache-dir "${gradle_cache_dir}" --no-daemon --no-parallel --max-workers=1 "$@" --quiet
}

client_bin() {
  echo "${e2e_build_dir}/Client/install/automatic-turn-dispatch-client/bin/automatic-turn-dispatch-client"
}

delay_bin() {
  echo "${e2e_build_dir}/Server-Delay/install/automatic-turn-dispatch-delay/bin/automatic-turn-dispatch-delay"
}

play_bin() {
  echo "${e2e_build_dir}/Server-Play/install/automatic-turn-dispatch-play/bin/automatic-turn-dispatch-play"
}

session_bin() {
  echo "${e2e_build_dir}/Server-Session/install/automatic-turn-dispatch-session/bin/automatic-turn-dispatch-session"
}

read -r DELAY_PORT ROUTE_A_PORT UNUSED_SPOT_A_PORT ROUTE_B_PORT UNUSED_SPOT_B_PORT STREAM_PORT PLAY_A_HTTP_PORT PLAY_B_HTTP_PORT SESSION_HTTP_PORT SESSION_ROUTE_PORT UNUSED_SESSION_SPOT_PORT _ _ <<<"$(reserve_ports)"
DELAY_ENDPOINT="$(tcp "${DELAY_PORT}")"
ROUTE_A_ENDPOINT="$(tcp "${ROUTE_A_PORT}")"
ROUTE_B_ENDPOINT="$(tcp "${ROUTE_B_PORT}")"
STREAM_ENDPOINT="$(tcp "${STREAM_PORT}")"
PLAY_A_HTTP="$(http "${PLAY_A_HTTP_PORT}")"
PLAY_B_HTTP="$(http "${PLAY_B_HTTP_PORT}")"
SESSION_HTTP="$(http "${SESSION_HTTP_PORT}")"
SESSION_ROUTE_ENDPOINT="$(tcp "${SESSION_ROUTE_PORT}")"

write_config() {
  local path="$1"
  shift
  {
    printf 'redisLocationEndpoint=%s\n' "${redis_location_endpoint}"
    printf 'locationKeyPrefix=%s\n' "${location_key_prefix}"
    printf 'logDirectory=%s\n' "${log_dir}"
    printf '%s\n' "$@"
  } >"${path}"
  chmod 0600 "${path}"
}
write_role_config() {
  local path="$1"
  shift
  {
    printf 'e2e.redis-location-endpoint=%s\n' "${redis_location_endpoint}"
    printf 'e2e.location-key-prefix=%s\n' "${location_key_prefix}"
    printf 'e2e.log-directory=%s\n' "${log_dir}"
    local property
    for property in "$@"; do
      printf 'e2e.%s\n' "${property}"
    done
  } >"${path}"
  chmod 0600 "${path}"
}
write_client_config() {
  local path="$1"
  local shutdown_request_id="${2:-}"
  local shutdown_spot_rid="${3:-}"
  write_config "${path}" \
    "streamEndpoint=${STREAM_ENDPOINT}" \
    "playHttpEndpoint=${PLAY_A_HTTP}" \
    "playBHttpEndpoint=${PLAY_B_HTTP}" \
    "sessionHttpEndpoint=${SESSION_HTTP}" \
    "shutdownRequestId=${shutdown_request_id}" \
    "shutdownSpotRid=${shutdown_spot_rid}"
}
delay_config="${config_dir}/delay.properties"
play_a_config="${config_dir}/play-a.properties"
play_b_config="${config_dir}/play-b.properties"
session_config="${config_dir}/session.properties"
client_config="${config_dir}/client.properties"
write_role_config "${delay_config}" "delay-endpoint=${DELAY_ENDPOINT}"
write_role_config "${play_a_config}" \
  "node-rid=play-a" "route-endpoint=${ROUTE_A_ENDPOINT}" \
  "route-peer-endpoint=${ROUTE_B_ENDPOINT}" \
  "delay-endpoint=${DELAY_ENDPOINT}" "http-endpoint=${PLAY_A_HTTP}"
write_role_config "${play_b_config}" \
  "node-rid=play-b" "route-endpoint=${ROUTE_B_ENDPOINT}" \
  "route-peer-endpoint=${ROUTE_A_ENDPOINT}" \
  "delay-endpoint=${DELAY_ENDPOINT}" "http-endpoint=${PLAY_B_HTTP}"
write_role_config "${session_config}" \
  "route-endpoint=${ROUTE_A_ENDPOINT}" "route-b-endpoint=${ROUTE_B_ENDPOINT}" \
  "session-route-endpoint=${SESSION_ROUTE_ENDPOINT}" \
  "delay-endpoint=${DELAY_ENDPOINT}" "stream-endpoint=${STREAM_ENDPOINT}" \
  "http-endpoint=${SESSION_HTTP}"
write_client_config "${client_config}"

start_initial_role() {
  case "$1" in
    delay)
      "$(delay_bin)" --config "${delay_config}" \
        >"${log_dir}/delay.stdout.log" 2>"${log_dir}/delay.stderr.log" &
      ;;
    play-a)
      "$(play_bin)" --config "${play_a_config}" \
        >"${log_dir}/play-a.stdout.log" 2>"${log_dir}/play-a.stderr.log" &
      ;;
    play-b)
      "$(play_bin)" --config "${play_b_config}" \
        >"${log_dir}/play-b.stdout.log" 2>"${log_dir}/play-b.stderr.log" &
      ;;
    session)
      "$(session_bin)" --config "${session_config}" \
        >"${log_dir}/session.stdout.log" 2>"${log_dir}/session.stderr.log" &
      ;;
  esac
  pids+=("$!")
}

wait_initial_role() {
  case "$1" in
    delay)
      wait_port delay "${DELAY_ENDPOINT}"
      ;;
    play-a)
      wait_port play-a-route "${ROUTE_A_ENDPOINT}"
      wait_http play-a-http "${PLAY_A_HTTP}"
      ;;
    play-b)
      wait_port play-b-route "${ROUTE_B_ENDPOINT}"
      wait_http play-b-http "${PLAY_B_HTTP}"
      ;;
    session)
      wait_port session-route "${SESSION_ROUTE_ENDPOINT}"
      wait_port session-stream "${STREAM_ENDPOINT}"
      wait_http session-http "${SESSION_HTTP}"
      ;;
  esac
}

static_checks
gradle_run installDist

mapfile -t ORDERED_SERVER_ROLES < <(zlink_e2e_order_roles delay play-a play-b session)
for role in "${ORDERED_SERVER_ROLES[@]}"; do
  start_initial_role "${role}"
  wait_initial_role "${role}"
done
assert_readiness

timeout -k 5s 90s "$(client_bin)" --config "${client_config}" "${SCENARIO}" \
  >"${log_dir}/client.stdout.log" 2>"${log_dir}/client.stderr.log"

fetch_evidence "${PLAY_A_HTTP}" "${log_dir}/play-a-evidence.json"
fetch_evidence "${PLAY_B_HTTP}" "${log_dir}/play-b-evidence.json"
fetch_evidence "${SESSION_HTTP}" "${log_dir}/session-evidence.json"
cat "${log_dir}/client.stdout.log"
if [[ "${SCENARIO}" == "all" ]]; then
  grep -q "scenario ATD-A1 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-A2 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-A3 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-A4 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-B1 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-B2 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-B3 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-C1 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-C2 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-C3 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-D1 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-D2 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-D3 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-D4 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-E1 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-E2 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-E4 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-E5 passed" "${log_dir}/client.stdout.log"
else
  grep -q "scenario ${SCENARIO} passed" "${log_dir}/client.stdout.log"
fi
grep -q "automatic-turn-dispatch e2e result=passed" "${log_dir}/client.stdout.log"
grep -Rq "message flow" "${log_dir}"/*-flow.log

if [[ "${SCENARIO}" == "all" ]]; then
  SHUTDOWN_ID="atde3-$(date +%s)-$$"
  SHUTDOWN_SPOT="atd-shutdown-${run_id//[^a-zA-Z0-9]/}"
  shutdown_client_config="${config_dir}/client-shutdown.properties"
  write_client_config "${shutdown_client_config}" "${SHUTDOWN_ID}" "${SHUTDOWN_SPOT}"
  timeout -k 5s 120s "$(client_bin)" \
    --config "${shutdown_client_config}" --shutdown-wait \
      >"${log_dir}/client-shutdown-wait.stdout.log" 2>"${log_dir}/client-shutdown-wait.stderr.log" &
  SHUTDOWN_CLIENT_PID=$!
  wait_evidence_contains \
    "${PLAY_A_HTTP}" \
    "await-released" \
    "${SHUTDOWN_ID}" \
    "ATD-E3 pending await marker was not observed before shutdown."
  fetch_evidence "${PLAY_A_HTTP}" "${log_dir}/play-a-shutdown-before-stop-evidence.json"
  terminate_gracefully play-a "${pids[1]}"
  wait "${SHUTDOWN_CLIENT_PID}"
  cat "${log_dir}/client-shutdown-wait.stdout.log"
  grep -q "automatic-turn-dispatch shutdown wait result=passed" "${log_dir}/client-shutdown-wait.stdout.log"

  "$(play_bin)" --config "${play_a_config}" \
    >"${log_dir}/play-a-restart.stdout.log" 2>"${log_dir}/play-a-restart.stderr.log" &
  pids+=("$!")
  wait_port play-a-route "${ROUTE_A_ENDPOINT}"
  wait_http play-a-http "${PLAY_A_HTTP}"
  assert_readiness

  timeout -k 5s 90s "$(client_bin)" \
    --config "${shutdown_client_config}" --shutdown-recovery \
      >"${log_dir}/client-shutdown-recovery.stdout.log" 2>"${log_dir}/client-shutdown-recovery.stderr.log"
  cat "${log_dir}/client-shutdown-recovery.stdout.log"
  grep -q "automatic-turn-dispatch shutdown recovery result=passed" "${log_dir}/client-shutdown-recovery.stdout.log"
  fetch_evidence "${PLAY_A_HTTP}" "${log_dir}/play-a-restart-evidence.json"
  grep -q "automatic-turn-dispatch shutdown recovery result=passed" "${log_dir}/client-shutdown-recovery.stdout.log"
  echo "scenario ATD-E3 passed"
fi

if [[ "${SCENARIO}" == "all" ]]; then
  cat >"${log_dir}/runner-gates.json" <<'JSON'
{"entries":[
  {"marker":"shutdown-await-cleaned","subject":"atde3-runner","value":"closed-or-cancelled"},
  {"marker":"shutdown-recovery-completed","subject":"atde3-runner","value":"routing-id-reused"},
  {"marker":"static-contract-verified","subject":"atde4-runner","value":"submit-only"},
  {"marker":"marker-schema-verified","subject":"atde5-runner","value":"java-common-parity"}
]}
JSON
  write_marker_report
  grep -q '"ATD-E3"' "${log_dir}/automatic-turn-dispatch-marker-report.json"
  grep -q '"ATD-E4"' "${log_dir}/automatic-turn-dispatch-marker-report.json"
  grep -q '"ATD-E5"' "${log_dir}/automatic-turn-dispatch-marker-report.json"
  echo "automatic-turn-dispatch marker report=${log_dir}/automatic-turn-dispatch-marker-report.json"
fi
