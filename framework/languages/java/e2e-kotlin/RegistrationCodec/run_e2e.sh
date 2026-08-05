#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-kotlin-config.sh"

cd "$(dirname "${BASH_SOURCE[0]}")"

pids=()
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
SCENARIO="${1:-all}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
export ZLINK_KOTLIN_E2E_BUILD_DIR="${ZLINK_KOTLIN_E2E_BUILD_DIR:-${HOME}/.cache/zlink/kotlin-e2e/RegistrationCodec}"
export ZLINK_KOTLIN_E2E_GRADLE_CACHE="${ZLINK_KOTLIN_E2E_GRADLE_CACHE:-${HOME}/.cache/zlink/kotlin-e2e/RegistrationCodec-gradle-cache}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30

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
  exit "${status}"
}
trap cleanup EXIT

reserve_ports() {
  local count="${1:-3}"
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

tcp() { echo "tcp://127.0.0.1:$1"; }
http() { echo "http://127.0.0.1:$1"; }
port_of() { echo "${1##*:}"; }

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

gradle_run() {
  ../../gradlew --project-cache-dir "${ZLINK_KOTLIN_E2E_GRADLE_CACHE}" --no-daemon "$@" --quiet
}

bin_path() {
  local path="$1"
  local app="$2"
  echo "${ZLINK_KOTLIN_E2E_BUILD_DIR}/${path}/install/${app}/bin/${app}"
}

CLIENT_BIN="$(bin_path Client registration-codec-kotlin-client)"
MAIN_BIN="$(bin_path Server-Main registration-codec-kotlin-main)"
CODEC_REQUESTER_BIN="$(bin_path Server-CodecRequester registration-codec-kotlin-codec-requester)"
JSON_ONLY_BIN="$(bin_path Server-JsonOnlyPeer registration-codec-kotlin-json-only-peer)"
INVALID_BIN="$(bin_path Server-InvalidDuplicate registration-codec-kotlin-invalid-duplicate)"

read -r SERVER_PORT HTTP_PORT INVALID_PORT MISMATCH_PORT MISMATCH_HTTP_PORT REQUESTER_HTTP_PORT <<<"$(reserve_ports 6)"
SERVER_ENDPOINT="$(tcp "${SERVER_PORT}")"
HTTP_ENDPOINT="$(http "${HTTP_PORT}")"
INVALID_ENDPOINT="$(tcp "${INVALID_PORT}")"
MISMATCH_ENDPOINT="$(tcp "${MISMATCH_PORT}")"
MISMATCH_HTTP_ENDPOINT="$(http "${MISMATCH_HTTP_PORT}")"
REQUESTER_HTTP_ENDPOINT="$(http "${REQUESTER_HTTP_PORT}")"

run_invalid_scenario=false
run_main_scenarios=false
run_mismatch_scenario=false
case "${SCENARIO}" in
  all)
    run_invalid_scenario=true
    run_main_scenarios=true
    run_mismatch_scenario=true
    ;;
  RC-A6)
    run_invalid_scenario=true
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

gradle_run installDist

if [[ "${run_invalid_scenario}" == "true" ]]; then
  set +e
  zlink_kotlin_e2e_run "${INVALID_BIN}" \
    --server-endpoint "${INVALID_ENDPOINT}" \
    --log-dir "${log_dir}" \
    >"${log_dir}/invalid-server.stdout.log" 2>"${log_dir}/invalid-server.stderr.log"
  invalid_status="$?"
  set -e
  if [[ "${invalid_status}" == "0" ]]; then
    echo "invalid registration server unexpectedly started" >&2
    exit 1
  fi
  cat "${log_dir}/invalid-server.stdout.log" "${log_dir}/invalid-server.stderr.log" \
    | grep -Eq "duplicate|Duplicate|registration|packet"
  echo "scenario RC-A6 passed"
fi

if [[ "${run_main_scenarios}" == "true" ]]; then
  zlink_kotlin_e2e_run "${MAIN_BIN}" \
    --server-endpoint "${SERVER_ENDPOINT}" \
    --http-endpoint "${HTTP_ENDPOINT}" \
    --log-dir "${log_dir}" \
    --codec-mode all \
    >"${log_dir}/server.stdout.log" 2>"${log_dir}/server.stderr.log" &
  pids+=("$!")
  wait_port server "${SERVER_ENDPOINT}"
  wait_port evidence "${HTTP_ENDPOINT}"

  zlink_kotlin_e2e_run "${CLIENT_BIN}" \
    --http-endpoint "${HTTP_ENDPOINT}" \
    --codec-requester-http-endpoint "${REQUESTER_HTTP_ENDPOINT}" \
    --log-dir "${log_dir}" \
    --mode "${SCENARIO}" \
    >"${log_dir}/client.stdout.log" 2>"${log_dir}/client.stderr.log"

  cat "${log_dir}/client.stdout.log"
  python3 - "${HTTP_ENDPOINT}/evidence" >"${log_dir}/server-evidence.json" <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=5) as response:
    sys.stdout.write(response.read().decode("utf-8"))
PY
  grep -Rq "message flow" "${log_dir}"/*-flow.log
  if [[ "${SCENARIO}" == "all" ]]; then
    grep -q "EchoAutoReq" "${log_dir}/server-evidence.json"
    grep -q "ProtobufEcho" "${log_dir}/server-evidence.json"
    grep -q "PackedEchoReq" "${log_dir}/server-evidence.json"
  fi
fi

stop_current() {
  set +e
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    kill "${pids[$i]}" >/dev/null 2>&1 || true
  done
  wait >/dev/null 2>&1 || true
  pids=()
  set -e
}

stop_current

if [[ "${run_mismatch_scenario}" == "true" ]]; then
  zlink_kotlin_e2e_run "${JSON_ONLY_BIN}" \
    --server-endpoint "${MISMATCH_ENDPOINT}" \
    --http-endpoint "${MISMATCH_HTTP_ENDPOINT}" \
    --log-dir "${log_dir}" \
    --codec-mode json-only \
    >"${log_dir}/mismatch-server.stdout.log" 2>"${log_dir}/mismatch-server.stderr.log" &
  pids+=("$!")
  wait_port mismatch-server "${MISMATCH_ENDPOINT}"
  wait_port mismatch-evidence "${MISMATCH_HTTP_ENDPOINT}"

  zlink_kotlin_e2e_run "${CODEC_REQUESTER_BIN}" \
    --server-endpoint "${MISMATCH_ENDPOINT}" \
    --http-endpoint "${REQUESTER_HTTP_ENDPOINT}" \
    --log-dir "${log_dir}" \
    >"${log_dir}/codec-requester.stdout.log" 2>"${log_dir}/codec-requester.stderr.log" &
  pids+=("$!")
  wait_port codec-requester "${REQUESTER_HTTP_ENDPOINT}"

  zlink_kotlin_e2e_run "${CLIENT_BIN}" \
    --http-endpoint "${MISMATCH_HTTP_ENDPOINT}" \
    --codec-requester-http-endpoint "${REQUESTER_HTTP_ENDPOINT}" \
    --log-dir "${log_dir}" \
    --mode codec-mismatch \
    >"${log_dir}/mismatch-client.stdout.log" 2>"${log_dir}/mismatch-client.stderr.log"

  cat "${log_dir}/mismatch-client.stdout.log"
  grep -q "scenario RC-B5 passed" "${log_dir}/mismatch-client.stdout.log"
fi
echo "registration-codec kotlin e2e result=passed"
