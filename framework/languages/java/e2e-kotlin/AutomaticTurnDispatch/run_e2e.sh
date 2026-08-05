#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"

cd "$(dirname "${BASH_SOURCE[0]}")"

pids=()
play_a_pid=""
redis_container=""
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
mkdir -p "${log_dir}/control"
echo "log_dir=${log_dir}"
SCENARIO="${1:-all}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
export ZLINK_KOTLIN_E2E_BUILD_DIR="${ZLINK_KOTLIN_E2E_BUILD_DIR:-${HOME}/.cache/zlink/kotlin-e2e/AutomaticTurnDispatch}"
export ZLINK_KOTLIN_E2E_GRADLE_CACHE="${ZLINK_KOTLIN_E2E_GRADLE_CACHE:-${HOME}/.cache/zlink/kotlin-e2e/AutomaticTurnDispatch-gradle-cache}"
location_key_prefix="zlink:e2e:kotlin-automatic-turn-dispatch:${run_id}"
config_dir="$(mktemp -d)"
chmod 0700 "${config_dir}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
PLAY_B_START_DELAY_SECONDS="${ZLINK_KOTLIN_E2E_PLAY_B_START_DELAY_SECONDS:-0}"
PROCESS_STOP_ATTEMPTS=600

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
    for _ in $(seq 1 "${PROCESS_STOP_ATTEMPTS}"); do
      if ! kill -0 "${pid}" >/dev/null 2>&1; then
        wait "${pid}" >/dev/null 2>&1 || true
        break
      fi
      sleep "${LOCAL_READINESS_POLL_SECONDS}"
    done
    if kill -0 "${pid}" >/dev/null 2>&1; then
      for child in $(descendants "${pid}"); do
        kill -9 "${child}" >/dev/null 2>&1 || true
      done
      kill -9 "${pid}" >/dev/null 2>&1 || true
      wait "${pid}" >/dev/null 2>&1 || true
    fi
  done
  if [[ -n "${redis_container}" ]]; then
    docker rm -fv "${redis_container}" >/dev/null 2>&1 || true
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
    for _ in range(8):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
    print(" ".join(f"tcp://127.0.0.1:{port}" for port in ports))
finally:
    for sock in sockets:
        sock.close()
PY
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
  echo "Timed out after ${LOCAL_READINESS_TIMEOUT_SECONDS}s waiting for ${name} at ${endpoint}" >&2
  return 1
}

start_redis_if_needed() {
  local redis_endpoint
  local redis_port
  zlink_redis_start_scoped_assign redis_container redis_port \
    "zlink-redis-kotlin-e2e" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}" "127.0.0.1::6379"
  redis_endpoint="127.0.0.1:${redis_port}"
  redis_location_endpoint="${redis_endpoint}"
  wait_port redis "tcp://${redis_endpoint}"
}

gradle_run() {
  ../../gradlew --project-cache-dir "${ZLINK_KOTLIN_E2E_GRADLE_CACHE}" --no-daemon --no-parallel --max-workers=1 "$@" --quiet
}

static_checks() {
  local tmp
  if [[ "${LOCAL_READINESS_TIMEOUT_SECONDS}" != 3 \
     || "${LOCAL_READINESS_ATTEMPTS}" != 30 ]]; then
    echo "AutomaticTurnDispatch must use a 3s readiness limit" >&2
    return 1
  fi
  tmp="$(mktemp)"
  if rg -n 'receivedCount\([^)]*\)\s*==\s*0' \
      Client/src/main/java -g '*.java' >"${tmp}"; then
    cat "${tmp}" >&2
    rm -f "${tmp}"
    echo "AutomaticTurnDispatch negative push assertions must use expectNone." >&2
    return 1
  fi

  if rg -n 'HttpClient|HttpURLConnection|RestTemplate|WebClient|@RestController|@RequestMapping|@PostMapping|@GetMapping' \
      Client Server Shared -g '*.java' -g '*.kt' >"${tmp}"; then
    cat "${tmp}" >&2
    rm -f "${tmp}"
    echo "AutomaticTurnDispatch scenarios must not use HTTP clients or HTTP trigger endpoints." >&2
    return 1
  fi
  rm -f "${tmp}"

  tmp="$(mktemp)"
  if rg -n 'YieldDispatch|yielddispatch|YD-' \
      Client Server Shared -g '*.java' -g '*.kt' >"${tmp}"; then
    cat "${tmp}" >&2
    rm -f "${tmp}"
    echo "AutomaticTurnDispatch must use the execution-turn scenario namespace." >&2
    return 1
  fi
  rm -f "${tmp}"

  if ! rg -q '\.yield\(' Server/Play/src/main/java -g '*.java'; then
    echo "AutomaticTurnDispatch must exercise the yield terminator." >&2
    return 1
  fi

  if ! rg -q 'ZLinkStreamConnectorFactory\.create' Client/src/main/java/systems/zlink/e2e/kotlin/automaticturn/support/ClientStreamSupport.java; then
    echo "AutomaticTurnDispatch client must create a real stream connector." >&2
    return 1
  fi

  local scenario_file
  for scenario_file in Client/src/main/java/systems/zlink/e2e/kotlin/automaticturn/scenarios/Atd*.java; do
    if ! rg -q 'ZLinkStreamConnector' "${scenario_file}"; then
      echo "${scenario_file}" >&2
      echo "AutomaticTurnDispatch ATD scenario files must receive the stream connector directly." >&2
      return 1
    fi
    if rg -n 'createConnector\(|ZLinkStreamConnectorFactory|\.connect\(\)|\.close\(\)' "${scenario_file}" >"${tmp}"; then
      cat "${tmp}" >&2
      rm -f "${tmp}"
      echo "AutomaticTurnDispatch ATD scenario files must not create or own stream connector lifecycle." >&2
      return 1
    fi
  done
  rm -f "${tmp}"
}

client_bin() {
  echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Client/install/automatic-turn-dispatch-kotlin-client/bin/automatic-turn-dispatch-kotlin-client"
}

delay_bin() {
  echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Server-Delay/install/automatic-turn-dispatch-kotlin-delay/bin/automatic-turn-dispatch-kotlin-delay"
}

play_bin() {
  echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Server-Play/install/automatic-turn-dispatch-kotlin-play/bin/automatic-turn-dispatch-kotlin-play"
}

session_bin() {
  echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Server-Session/install/automatic-turn-dispatch-kotlin-session/bin/automatic-turn-dispatch-kotlin-session"
}

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

start_delay() {
  local config_path="${config_dir}/delay.properties"
  write_config "${config_path}" "nodeRid=delay-a" "delayEndpoint=${DELAY}"
  "$(delay_bin)" --config "${config_path}" \
    >"${log_dir}/delay.stdout.log" 2>"${log_dir}/delay.stderr.log" &
  pids+=("$!")
  wait_port delay "${DELAY}"
}

start_play() {
  local node_rid="$1"
  local play_route_endpoint="$2"
  local log_name="$3"
  local config_path="${config_dir}/${log_name}.properties"
  write_config "${config_path}" \
    "nodeRid=${node_rid}" "delayEndpoint=${DELAY}" \
    "playRouteEndpoint=${play_route_endpoint}" \
    "sessionRouteEndpoint=${SESSION_ROUTE}"
  "$(play_bin)" --config "${config_path}" \
    >"${log_dir}/${log_name}.stdout.log" 2>"${log_dir}/${log_name}.stderr.log" &
  pids+=("$!")
  if [[ "${log_name}" == "play" ]]; then
    play_a_pid="$!"
  fi
  wait_port "${log_name}-route" "${play_route_endpoint}"
}

stop_recorded_pid() {
  local pid="$1"
  if [[ -z "${pid}" ]] || ! kill -0 "${pid}" >/dev/null 2>&1; then
    return 0
  fi
  for child in $(descendants "${pid}"); do
    kill "${child}" >/dev/null 2>&1 || true
  done
  kill "${pid}" >/dev/null 2>&1 || true
  for _ in $(seq 1 "${PROCESS_STOP_ATTEMPTS}"); do
    if ! kill -0 "${pid}" >/dev/null 2>&1; then
      wait "${pid}" >/dev/null 2>&1 || true
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  for child in $(descendants "${pid}"); do
    kill -9 "${child}" >/dev/null 2>&1 || true
  done
  kill -9 "${pid}" >/dev/null 2>&1 || true
  wait "${pid}" >/dev/null 2>&1 || true
}

start_session() {
  local config_path="${config_dir}/session.properties"
  write_config "${config_path}" \
    "nodeRid=session-a" \
    "playRouteEndpoint=${PLAY_ROUTE}" "playBRouteEndpoint=${PLAY_B_ROUTE}" \
    "sessionRouteEndpoint=${SESSION_ROUTE}" "streamEndpoint=${STREAM}"
  "$(session_bin)" --config "${config_path}" \
    >"${log_dir}/session.stdout.log" 2>"${log_dir}/session.stderr.log" &
  pids+=("$!")
  wait_port session-route "${SESSION_ROUTE}"
  wait_port stream "${STREAM}"
}

run_client() {
  local config_path="${config_dir}/client.properties"
  write_config "${config_path}" "streamEndpoint=${STREAM}" "controlDirectory=${log_dir}/control"
  timeout -k 5s 45s "$(client_bin)" --config "${config_path}" "${SCENARIO}" \
    >"${log_dir}/client.stdout.log" 2>"${log_dir}/client.stderr.log"
}

run_d2_client() {
  local client_scenario="${SCENARIO}"
  if [[ "${client_scenario}" == "all" ]]; then
    client_scenario="d2"
  fi
  local config_path="${config_dir}/client-d2.properties"
  write_config "${config_path}" "streamEndpoint=${STREAM}" "controlDirectory=${log_dir}/control"
  timeout -k 5s 45s "$(client_bin)" --config "${config_path}" "${client_scenario}" \
    >"${log_dir}/client-d2.stdout.log" 2>"${log_dir}/client-d2.stderr.log"
}

run_e3_client() {
  local client_e3_pid
  local config_path="${config_dir}/client-e3.properties"
  write_config "${config_path}" "streamEndpoint=${STREAM}" "controlDirectory=${log_dir}/control"
  timeout -k 5s 120s "$(client_bin)" --config "${config_path}" ATD-E3 \
    >"${log_dir}/client-e3.stdout.log" 2>"${log_dir}/client-e3.stderr.log" &
  client_e3_pid="$!"
  pids+=("${client_e3_pid}")

  local ready_file="${log_dir}/control/atd-e3-ready-to-stop-play"
  local restarted_file="${log_dir}/control/atd-e3-play-restarted"
  for _ in $(seq 1 100); do
    [[ -f "${ready_file}" ]] && break
    sleep 0.1
  done
  if [[ ! -f "${ready_file}" ]]; then
    echo "Timed out waiting for ATD-E3 shutdown-ready marker" >&2
    return 1
  fi

  stop_recorded_pid "${play_a_pid}"
  start_play play-a "${PLAY_ROUTE}" play-restarted
  touch "${restarted_file}"
  wait "${client_e3_pid}"
}

static_checks
start_redis_if_needed
gradle_run :Client:installDist :Server:Delay:installDist :Server:Play:installDist :Server:Session:installDist
local_package_root="${ZLINK_LOCAL_PACKAGE_ROOT:-${repo_root}/.artifacts/wsl}"
bindings_version="$(python3 - "${repo_root}/framework/languages/java/gradle/libs.versions.toml" <<'PY'
import sys
import tomllib

with open(sys.argv[1], "rb") as catalog:
    print(tomllib.load(catalog)["versions"]["zlinkBindings"])
PY
)"
bindings_jar="${local_package_root}/maven/systems/zlink/zlink/${bindings_version}/zlink-${bindings_version}.jar"
python3 - "${bindings_jar}" <<'PY'
import sys
import zipfile
path = sys.argv[1]
with zipfile.ZipFile(path) as jar:
    bad = jar.testzip()
    if bad is not None:
        raise SystemExit(f"bad jar entry: {bad}")
PY
for dist in \
  "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Client/install/automatic-turn-dispatch-kotlin-client" \
  "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Server-Delay/install/automatic-turn-dispatch-kotlin-delay" \
  "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Server-Play/install/automatic-turn-dispatch-kotlin-play" \
  "${ZLINK_KOTLIN_E2E_BUILD_DIR}/Server-Session/install/automatic-turn-dispatch-kotlin-session"; do
  installed_bindings="${dist}/lib/$(basename "${bindings_jar}")"
  if [[ ! -f "${installed_bindings}" ]]; then
    echo "Missing local package binding jar in ${dist}/lib: $(basename "${bindings_jar}")" >&2
    exit 1
  fi
  python3 - "${installed_bindings}" <<'PY'
import sys
import zipfile
path = sys.argv[1]
with zipfile.ZipFile(path) as jar:
    bad = jar.testzip()
    if bad is not None:
        raise SystemExit(f"bad installed jar entry in {path}: {bad}")
PY
done
read -r DELAY UNUSED_SPOT UNUSED_SESSION_SPOT PLAY_ROUTE SESSION_ROUTE STREAM UNUSED_SPOT_B PLAY_B_ROUTE <<<"$(reserve_ports)"
start_delay
start_play play-a "${PLAY_ROUTE}" play
start_session
case "${SCENARIO}" in
  all)
    run_client
    start_play play-b "${PLAY_B_ROUTE}" play-b
    run_d2_client
    run_e3_client
    ;;
  ATD-D2|ATD-D3|d2)
    sleep "${PLAY_B_START_DELAY_SECONDS}"
    start_play play-b "${PLAY_B_ROUTE}" play-b
    run_d2_client
    ;;
  ATD-E3)
    run_e3_client
    ;;
  *)
    run_client
    ;;
esac

[[ ! -f "${log_dir}/client.stdout.log" ]] || cat "${log_dir}/client.stdout.log"
[[ ! -f "${log_dir}/client-d2.stdout.log" ]] || cat "${log_dir}/client-d2.stdout.log"
[[ ! -f "${log_dir}/client-e3.stdout.log" ]] || cat "${log_dir}/client-e3.stdout.log"
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
  grep -q "scenario ATD-D4 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-D1 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-E1 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-E2 passed" "${log_dir}/client.stdout.log"
  grep -q "automatic-turn-dispatch kotlin e2e result=passed" "${log_dir}/client.stdout.log"
  grep -q "scenario ATD-D2 passed" "${log_dir}/client-d2.stdout.log"
  grep -q "scenario ATD-D3 passed" "${log_dir}/client-d2.stdout.log"
  grep -q "automatic-turn-dispatch kotlin e2e result=passed" "${log_dir}/client-d2.stdout.log"
  grep -q "scenario ATD-E3 passed" "${log_dir}/client-e3.stdout.log"
  grep -q "automatic-turn-dispatch kotlin e2e result=passed" "${log_dir}/client-e3.stdout.log"
elif [[ "${SCENARIO}" == "ATD-D2" || "${SCENARIO}" == "ATD-D3" ]]; then
  grep -q "scenario ${SCENARIO} passed" "${log_dir}/client-d2.stdout.log"
  grep -q "automatic-turn-dispatch kotlin e2e result=passed" "${log_dir}/client-d2.stdout.log"
elif [[ "${SCENARIO}" == "ATD-E3" ]]; then
  grep -q "scenario ATD-E3 passed" "${log_dir}/client-e3.stdout.log"
  grep -q "automatic-turn-dispatch kotlin e2e result=passed" "${log_dir}/client-e3.stdout.log"
else
  grep -q "scenario ${SCENARIO} passed" "${log_dir}/client.stdout.log"
  grep -q "automatic-turn-dispatch kotlin e2e result=passed" "${log_dir}/client.stdout.log"
fi
echo "automatic-turn-dispatch kotlin e2e result=passed"
