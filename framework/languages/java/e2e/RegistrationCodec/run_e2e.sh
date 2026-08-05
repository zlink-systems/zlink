#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/start-order-common.sh"

cd "$(dirname "${BASH_SOURCE[0]}")"

pids=()
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
config_dir="$(mktemp -d)"
chmod 0700 "${config_dir}"
SCENARIO="${1:-all}"
e2e_start_order="$(zlink_e2e_start_order_mode "$@")"
echo "start_order=${e2e_start_order}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
readonly e2e_build_dir="${HOME}/.cache/zlink/java-e2e/RegistrationCodec"
readonly gradle_cache_dir="${HOME}/.cache/zlink/java-e2e/RegistrationCodec-gradle-cache"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
if rg -n 'java\.net\.http\.HttpClient|HttpClient\.new' \
    "$(pwd)/Client/src/main/java" --glob '*.java'; then
  echo "RegistrationCodec client must use ZLinkHttpClient" >&2
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
  wait >/dev/null 2>&1 || true
  rm -rf "${config_dir}"
  exit "${status}"
}
trap cleanup EXIT

reserve_ports() {
  local count="${1:-5}"
  python3 - "${count}" <<'PY'
import socket
import sys
count = int(sys.argv[1])
sockets = []
ports = []
try:
    for _ in range(count):
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

wait_health() {
  local name="$1"
  local endpoint="$2"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if python3 - "${endpoint}/health" <<'PY'
import sys
import urllib.request

try:
    with urllib.request.urlopen(sys.argv[1], timeout=0.1) as response:
        sys.exit(0 if 200 <= response.status < 300 else 1)
except Exception:
    sys.exit(1)
PY
    then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${name} health at ${endpoint}" >&2
  return 1
}

gradle_run() {
  ../../gradlew -PzlinkE2eBuildDir="${e2e_build_dir}" \
    --project-cache-dir "${gradle_cache_dir}" --no-daemon --no-parallel --max-workers=1 "$@" --quiet
}

client_bin() {
  echo "${e2e_build_dir}/Client/install/registration-codec-client/bin/registration-codec-client"
}

main_bin() {
  echo "${e2e_build_dir}/Server-Main/install/registration-codec-main/bin/registration-codec-main"
}

invalid_bin() {
  echo "${e2e_build_dir}/Server-InvalidDuplicate/install/registration-codec-invalid-duplicate/bin/registration-codec-invalid-duplicate"
}

json_only_bin() {
  echo "${e2e_build_dir}/Server-JsonOnlyPeer/install/registration-codec-json-only-peer/bin/registration-codec-json-only-peer"
}

requester_bin() {
  echo "${e2e_build_dir}/Server-CodecRequester/install/registration-codec-requester/bin/registration-codec-requester"
}

read -r SERVER_PORT HTTP_PORT INVALID_PORT MISMATCH_PORT MISMATCH_HTTP_PORT REQUESTER_HTTP_PORT <<<"$(reserve_ports 6)"
SERVER_ENDPOINT="$(tcp "${SERVER_PORT}")"
HTTP_ENDPOINT="$(http "${HTTP_PORT}")"
INVALID_ENDPOINT="$(tcp "${INVALID_PORT}")"
MISMATCH_ENDPOINT="$(tcp "${MISMATCH_PORT}")"
MISMATCH_HTTP_ENDPOINT="$(http "${MISMATCH_HTTP_PORT}")"
REQUESTER_HTTP_ENDPOINT="$(http "${REQUESTER_HTTP_PORT}")"

write_config() {
  local path="$1"
  shift
  printf '%s\n' "$@" >"${path}"
  chmod 0600 "${path}"
}
main_config="${config_dir}/main.properties"
json_only_config="${config_dir}/json-only.properties"
requester_config="${config_dir}/requester.properties"
invalid_config="${config_dir}/invalid.properties"
client_config="${config_dir}/client.properties"
write_config "${main_config}" \
  "e2e.server-endpoint=${SERVER_ENDPOINT}" "e2e.http-endpoint=${HTTP_ENDPOINT}" "e2e.log-dir=${log_dir}"
write_config "${json_only_config}" \
  "e2e.server-endpoint=${MISMATCH_ENDPOINT}" "e2e.http-endpoint=${MISMATCH_HTTP_ENDPOINT}" "e2e.log-dir=${log_dir}"
write_config "${requester_config}" \
  "e2e.server-endpoint=${MISMATCH_ENDPOINT}" "e2e.http-endpoint=${REQUESTER_HTTP_ENDPOINT}" "e2e.log-dir=${log_dir}"
write_config "${invalid_config}" "e2e.server-endpoint=${INVALID_ENDPOINT}"
write_config "${client_config}" \
  "serverEndpoint=${SERVER_ENDPOINT}" \
  "httpEndpoint=${HTTP_ENDPOINT}" \
  "codecRequesterHttpEndpoint=${REQUESTER_HTTP_ENDPOINT}" \
  "invalidServerEndpoint=${INVALID_ENDPOINT}" \
  "buildDir=${e2e_build_dir}" \
  "logDir=${log_dir}" \
  "invalidServerConfig=${invalid_config}"

run_main_scenarios=false
run_mismatch_scenario=false
case "${SCENARIO}" in
  all)
    run_main_scenarios=true
    run_mismatch_scenario=true
    ;;
  RC-A6)
    ;;
  RC-B5)
    run_mismatch_scenario=true
    ;;
  RC-A1|RC-A2|RC-A3|RC-A4|RC-A5|RC-B1|RC-B2|RC-B3|RC-B4)
    run_main_scenarios=true
    ;;
  *)
    echo "Unknown RegistrationCodec scenario: ${SCENARIO}" >&2
    exit 1
    ;;
esac

rm -rf "${e2e_build_dir}"
gradle_run installDist

start_initial_role() {
  case "$1" in
    main)
      "$(main_bin)" --config "${main_config}" >"${log_dir}/server.stdout.log" 2>"${log_dir}/server.stderr.log" &
      ;;
    json-only)
      "$(json_only_bin)" --config "${json_only_config}" >"${log_dir}/mismatch-server.stdout.log" 2>"${log_dir}/mismatch-server.stderr.log" &
      ;;
    requester)
      "$(requester_bin)" --config "${requester_config}" >"${log_dir}/codec-requester.stdout.log" 2>"${log_dir}/codec-requester.stderr.log" &
      ;;
  esac
  pids+=("$!")
}

SERVER_ROLES=()
if [[ "${run_main_scenarios}" == "true" ]]; then
  SERVER_ROLES+=(main)
fi
if [[ "${run_mismatch_scenario}" == "true" ]]; then
  SERVER_ROLES+=(json-only requester)
fi
mapfile -t ORDERED_SERVER_ROLES < <(zlink_e2e_order_roles "${SERVER_ROLES[@]}")
for role in "${ORDERED_SERVER_ROLES[@]}"; do
  start_initial_role "${role}"
done

if [[ "${run_main_scenarios}" == "true" ]]; then
  wait_port server "${SERVER_ENDPOINT}"
  wait_health server "${HTTP_ENDPOINT}"
fi

if [[ "${run_mismatch_scenario}" == "true" ]]; then
  wait_port mismatch-server "${MISMATCH_ENDPOINT}"
  wait_health mismatch-server "${MISMATCH_HTTP_ENDPOINT}"
  wait_health codec-requester "${REQUESTER_HTTP_ENDPOINT}"
fi

"$(client_bin)" --config "${client_config}" --scenario "${SCENARIO}" \
  >"${log_dir}/client.stdout.log" 2>"${log_dir}/client.stderr.log"

cat "${log_dir}/client.stdout.log"
if [[ "${run_mismatch_scenario}" == "true" ]]; then
  python3 - "${MISMATCH_HTTP_ENDPOINT}/evidence" >"${log_dir}/mismatch-server-evidence.json" <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=5) as response:
    sys.stdout.write(response.read().decode("utf-8"))
PY
  if [[ "${SCENARIO}" == "RC-B5" ]]; then
    grep -q "reason=PAYLOAD_DECODE_FAILED" "${log_dir}/json-only-flow.log"
    if grep -q "UnexpectedHandler" "${log_dir}/mismatch-server-evidence.json"; then
      echo "RC-B5 unsupported codec reached the receiver handler" >&2
      exit 1
    fi
  fi
fi
if [[ "${run_main_scenarios}" == "true" ]]; then
  python3 - "${HTTP_ENDPOINT}/evidence" >"${log_dir}/server-evidence.json" <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=5) as response:
    sys.stdout.write(response.read().decode("utf-8"))
PY

  grep -Rq "message flow" "${log_dir}"/*-flow.log
fi
if [[ "${SCENARIO}" == "all" ]]; then
  grep -q "EchoAuto" "${log_dir}/server-evidence.json"
  grep -q "ProtobufEcho" "${log_dir}/server-evidence.json"
  grep -q "MsgpackEcho" "${log_dir}/server-evidence.json"
  grep -q "scenario RC-A4 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario RC-A6 passed" "${log_dir}/client.stdout.log"
  grep -q "scenario RC-B5 passed" "${log_dir}/client.stdout.log"
else
  grep -q "scenario ${SCENARIO} passed" "${log_dir}/client.stdout.log"
fi
