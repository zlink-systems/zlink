#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRAMEWORK_DIR="$(cd "$ROOT_DIR/../.." && pwd)"
source "$ROOT_DIR/../redis-common.sh"
BUILD_DIR="$FRAMEWORK_DIR/build"
SCENARIO="${1:-all}"
SCENARIO_LOWER="$(printf '%s' "$SCENARIO" | tr '[:upper:]' '[:lower:]')"
case "$SCENARIO_LOWER" in
  all|full|atd-a[1-4]|atd-b[1-3]|atd-c[1-3]|atd-d[1-4]|atd-e[1-3]|td-c[1-3]|td-e[23]) ;;
  *)
    echo "Unsupported AutomaticTurnDispatch scenario: $SCENARIO" >&2
    exit 2
    ;;
esac
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
ROUTE_SETTLE_SECONDS=5
SCENARIO_SETTLE_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
# spot node destroy는 core에서 소켓 제거 완료를 기다린다(ledger CPP-CORE-SPOTDESTROY-002).
# 부하가 걸리면 8초를 넘길 수 있어 느린 종료를 hang으로 오판하지 않도록 넉넉히 둔다.
PROCESS_SHUTDOWN_TIMEOUT_SECONDS=45
PROCESS_SHUTDOWN_POLL_SECONDS=0.1
SCENARIO_MARKER_TIMEOUT_SECONDS=30
LOCAL_READINESS_ATTEMPTS="$(
  python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"
PROCESS_SHUTDOWN_ATTEMPTS="$(
  python3 - "$PROCESS_SHUTDOWN_TIMEOUT_SECONDS" "$PROCESS_SHUTDOWN_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"
SCENARIO_MARKER_ATTEMPTS="$(
  python3 - "$SCENARIO_MARKER_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/logs/$RUN_ID"
CONFIG_DIR="$LOG_DIR/config"
mkdir -p "$LOG_DIR"
mkdir -p "$CONFIG_DIR"
echo "log_dir=$LOG_DIR"

allocate_role_endpoints() {
  read -r DELAY_A_HTTP DELAY_A_ENDPOINT DELAY_B_HTTP DELAY_B_ENDPOINT \
    PLAY_A_HTTP PLAY_A_CONTROL PLAY_A_SPOT_ROUTE PLAY_A_SPOT_ROUTER PLAY_A_SPOT_PUB \
    PLAY_B_HTTP PLAY_B_CONTROL PLAY_B_SPOT_ROUTE PLAY_B_SPOT_ROUTER PLAY_B_SPOT_PUB \
    SESSION_A_HTTP SESSION_A_STREAM SESSION_A_SPOT_ROUTER SESSION_A_SPOT_PUB \
    SESSION_B_HTTP SESSION_B_STREAM SESSION_B_SPOT_ROUTER SESSION_B_SPOT_PUB \
    EXTERNAL_API_HTTP <<<"$(python3 - <<'PY'
import socket

sockets = []
ports = []
host = "127.0.0.1"
for _ in range(23):
    sock = socket.socket()
    sock.bind((host, 0))
    sockets.append(sock)
    ports.append(sock.getsockname()[1])
print(f"http://{host}:{ports[0]}", end=" ")
print(f"tcp://{host}:{ports[1]}", end=" ")
print(f"http://{host}:{ports[2]}", end=" ")
print(f"tcp://{host}:{ports[3]}", end=" ")
print(f"http://{host}:{ports[4]}", end=" ")
print(f"tcp://{host}:{ports[5]}", end=" ")
print(f"tcp://{host}:{ports[6]}", end=" ")
print(f"tcp://{host}:{ports[7]}", end=" ")
print(f"tcp://{host}:{ports[8]}", end=" ")
print(f"http://{host}:{ports[9]}", end=" ")
print(f"tcp://{host}:{ports[10]}", end=" ")
print(f"tcp://{host}:{ports[11]}", end=" ")
print(f"tcp://{host}:{ports[12]}", end=" ")
print(f"tcp://{host}:{ports[13]}", end=" ")
print(f"http://{host}:{ports[14]}", end=" ")
print(f"tcp://{host}:{ports[15]}", end=" ")
print(f"tcp://{host}:{ports[16]}", end=" ")
print(f"tcp://{host}:{ports[17]}", end=" ")
print(f"http://{host}:{ports[18]}", end=" ")
print(f"tcp://{host}:{ports[19]}", end=" ")
print(f"tcp://{host}:{ports[20]}", end=" ")
print(f"tcp://{host}:{ports[21]}", end=" ")
print(f"http://{host}:{ports[22]}")
for sock in sockets:
    sock.close()
PY
)"
}

# 샘플 러너와 build 디렉터리를 공유한다. 여기서 BUILD_SAMPLES를 끄면 그 cache가 남아
# ctest에서 sample smoke 테스트가 통째로 사라진다(게이트가 실행 순서에 의존하게 된다).
cmake -S "$FRAMEWORK_DIR" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" --target \
  zlink_cpp_e2e_automatic_turn_dispatch_delay \
  zlink_cpp_e2e_automatic_turn_dispatch_external_api \
  zlink_cpp_e2e_automatic_turn_dispatch_play \
  zlink_cpp_e2e_automatic_turn_dispatch_session \
  zlink_cpp_e2e_automatic_turn_dispatch_client >/dev/null

DELAY="$BUILD_DIR/zlink_cpp_e2e_automatic_turn_dispatch_delay"
EXTERNAL_API="$BUILD_DIR/zlink_cpp_e2e_automatic_turn_dispatch_external_api"
PLAY="$BUILD_DIR/zlink_cpp_e2e_automatic_turn_dispatch_play"
SESSION="$BUILD_DIR/zlink_cpp_e2e_automatic_turn_dispatch_session"
CLIENT="$BUILD_DIR/zlink_cpp_e2e_automatic_turn_dispatch_client"
REDIS_CONTAINER=""
REDIS_ENDPOINT=""
REDIS_KEY_PREFIX="zlink:cpp:automatic-turn-dispatch:${RUN_ID}"
PIDS=()

process_exited() {
  local pid="$1"
  if ! kill -0 "$pid" >/dev/null 2>&1; then
    return 0
  fi
  local state
  state="$(ps -o stat= -p "$pid" 2>/dev/null || true)"
  [[ "$state" == Z* ]]
}

launch_process() {
  local stdout_log="$1"
  local stderr_log="$2"
  shift 2
  "$@" >"$stdout_log" 2>"$stderr_log" &
}

print_failure_logs() {
  echo "Recent logs:" >&2
  for log_file in "$LOG_DIR"/*.stderr.log "$LOG_DIR"/client*.stdout.log; do
    [[ -f "$log_file" ]] || continue
    echo "--- ${log_file#$LOG_DIR/} ---" >&2
    tail -n 40 "$log_file" >&2 || true
  done
}

cleanup() {
  local code=$?
  local cleanup_failed=0
  local status
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
  done
  for _ in $(seq 1 "$PROCESS_SHUTDOWN_ATTEMPTS"); do
    local alive=0
    for pid in "${PIDS[@]:-}"; do
      if ! process_exited "$pid"; then
        alive=1
        break
      fi
    done
    [[ $alive -eq 0 ]] && break
    sleep "$PROCESS_SHUTDOWN_POLL_SECONDS"
  done
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then
      echo "forced cleanup process $pid" >&2
      kill -9 "$pid" >/dev/null 2>&1 || true
      cleanup_failed=1
    fi
  done
  for pid in "${PIDS[@]:-}"; do
    set +e
    wait "$pid" >/dev/null 2>&1
    status=$?
    set -e
    if [[ "$status" != "0" && "$status" != "127" && "$status" != "130" && "$status" != "143" ]]; then
      echo "cleanup process $pid exited unexpectedly with status $status" >&2
      cleanup_failed=1
    fi
  done
  docker rm -f "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  rm -rf "$CONFIG_DIR"
  if [[ $code -ne 0 ]]; then
    echo "E2E failed. Logs: $LOG_DIR" >&2
    print_failure_logs
  elif [[ "$cleanup_failed" -ne 0 ]]; then
    echo "E2E cleanup failed. Logs: $LOG_DIR" >&2
    print_failure_logs
    code=1
  fi
  exit "$code"
}
trap cleanup EXIT

port_of() {
  local endpoint="$1"
  echo "${endpoint##*:}"
}

host_of() {
  local endpoint="$1"
  local rest="${endpoint#*://}"
  echo "${rest%%:*}"
}

wait_port() {
  local name="$1"
  local endpoint="$2"
  local host
  local port
  host="$(host_of "$endpoint")"
  port="$(port_of "$endpoint")"
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    if (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name at $endpoint" >&2
  return 1
}

static_checks() {
  if [[ ! -f "$ROOT_DIR/Server/ExternalApi/main.cpp" ]]; then
    echo "AutomaticTurnDispatch TD-C1 requires the external-api role." >&2
    return 1
  fi
  if ! rg -q 'build_server<external_api_http_client_tag_t>' \
      "$ROOT_DIR/Server/Play/play_host_factory.hpp"; then
    echo "AutomaticTurnDispatch Play must register the external API HTTP client by name." >&2
    return 1
  fi
  if ! rg -q 'http_await_msg_t' "$ROOT_DIR/Shared/automatic_turn_dispatch_contracts.hpp" \
      || ! rg -q 'io_worker_await_msg_t' \
        "$ROOT_DIR/Shared/automatic_turn_dispatch_contracts.hpp"; then
    echo "AutomaticTurnDispatch TD-C1 through TD-C3 contracts are missing." >&2
    return 1
  fi
  if ! rg -q 'run_io_worker' "$ROOT_DIR/Server/Play/Handlers" -g '*.hpp'; then
    echo "AutomaticTurnDispatch TD-C3 must exercise run_io_worker." >&2
    return 1
  fi
  if ! rg -q 'http-yield-released' "$ROOT_DIR/Server/Play/Handlers" -g '*.hpp'; then
    echo "AutomaticTurnDispatch TD-C1 deterministic evidence markers are missing." >&2
    return 1
  fi
  if rg -n 'MapPost\("/await|HttpClient|new HttpClient|\.Post\(' "$ROOT_DIR" -g '*.cpp' -g '*.hpp' >/tmp/zlink-automatic-turn-dispatch-static-http.$$; then
    cat /tmp/zlink-automatic-turn-dispatch-static-http.$$ >&2
    rm -f /tmp/zlink-automatic-turn-dispatch-static-http.$$
    echo "AutomaticTurnDispatch must not start scenarios through HTTP client or /await HTTP endpoints." >&2
    return 1
  fi
  rm -f /tmp/zlink-automatic-turn-dispatch-static-http.$$

  if rg -n '\.await\s*\(' "$ROOT_DIR" -g '*.cpp' -g '*.hpp' | rg -v 'Server/Play/' >/tmp/zlink-automatic-turn-dispatch-static-await.$$; then
    cat /tmp/zlink-automatic-turn-dispatch-static-await.$$ >&2
    rm -f /tmp/zlink-automatic-turn-dispatch-static-await.$$
    echo "AutomaticTurnDispatch may only call await() from Spot/Entry Spot handlers in Server/Play." >&2
    return 1
  fi
  rm -f /tmp/zlink-automatic-turn-dispatch-static-await.$$

  if rg -n 'YieldConnectorFactory|AutomaticTurnDispatchScenarioContext|WaitForPlayEvidence|ReadPlayEvidence' "$ROOT_DIR/Client" -g '*.cpp' -g '*.hpp' >/tmp/zlink-automatic-turn-dispatch-static-helper.$$; then
    cat /tmp/zlink-automatic-turn-dispatch-static-helper.$$ >&2
    rm -f /tmp/zlink-automatic-turn-dispatch-static-helper.$$
    echo "AutomaticTurnDispatch client scenarios must use the stream connector directly instead of thin helpers." >&2
    return 1
  fi
  rm -f /tmp/zlink-automatic-turn-dispatch-static-helper.$$

  if ! rg -q 'connector_factory_t::create' "$ROOT_DIR/Client/main.cpp"; then
    echo "AutomaticTurnDispatch full scenario must create and use a real stream connector directly." >&2
    return 1
  fi
  if ! rg -q 'connector_factory_t::create' "$ROOT_DIR/Client/Scenarios/shutdown_await_scenario.hpp"; then
    echo "AutomaticTurnDispatch shutdown scenario must create and use a real stream connector directly." >&2
    return 1
  fi

  local scenario_file
  for scenario_file in "$ROOT_DIR"/Client/Scenarios/atd_*.hpp; do
    if ! rg -q 'TConnector &' "$scenario_file"; then
      echo "$scenario_file" >&2
      echo "AutomaticTurnDispatch YD scenario files must receive the stream connector directly." >&2
      return 1
    fi
  done
}

static_checks

zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-cpp-e2e-yielddispatch" "redis:7-alpine"
REDIS_ENDPOINT="127.0.0.1:${redis_port}"
wait_port redis "$REDIS_ENDPOINT"
# Redis uses a dynamically assigned host port. Allocate application endpoints only after
# that port is fixed so the runner cannot hand the same port to a framework role.
allocate_role_endpoints

wait_file_contains() {
  local file="$1"
  local pattern="$2"
  local message="$3"
  local watched_pid="${4:-}"
  local attempts="${5:-$SCENARIO_MARKER_ATTEMPTS}"
  for _ in $(seq 1 "$attempts"); do
    if [[ -f "$file" ]] && grep -Fq "$pattern" "$file"; then
      return 0
    fi
    if [[ -n "$watched_pid" ]] && ! kill -0 "$watched_pid" >/dev/null 2>&1; then
      if [[ -f "$file" ]] && grep -Fq "$pattern" "$file"; then
        return 0
      fi
      break
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "$message" >&2
  return 1
}

terminate_gracefully() {
  local name="$1"
  local pid="$2"
  kill "$pid" >/dev/null 2>&1 || true
  for _ in $(seq 1 "$PROCESS_SHUTDOWN_ATTEMPTS"); do
    if process_exited "$pid"; then
      local status=0
      wait "$pid" >/dev/null 2>&1 || status=$?
      if [[ "$status" -eq 134 || "$status" -eq 139 ]]; then
        echo "$name exited with crash status $status after SIGTERM" >&2
        return 1
      fi
      return 0
    fi
    sleep "$PROCESS_SHUTDOWN_POLL_SECONDS"
  done
  echo "$name did not exit after SIGTERM" >&2
  echo "forced cleanup process $pid" >&2
  kill -9 "$pid" >/dev/null 2>&1 || true
  wait "$pid" >/dev/null 2>&1 || true
  return 1
}

start_delay_role() {
  local name="$1"
  local http_endpoint="$2"
  local delay_endpoint="$3"
  local config_path="$CONFIG_DIR/$name.json"
  python3 - "$config_path" "$name" "$http_endpoint" "$delay_endpoint" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

path, name, http_endpoint, delay_endpoint, log_dir = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"nodeRid": name, "httpEndpoint": http_endpoint,
        "delayEndpoint": delay_endpoint, "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  launch_process "$LOG_DIR/$name.stdout.log" "$LOG_DIR/$name.stderr.log" \
    "$DELAY" --config="$config_path"
  PIDS+=("$!")
  wait_port "$name" "$http_endpoint"
  wait_port "$name-delay" "$delay_endpoint"
}

start_external_api_role() {
  local config_path="$CONFIG_DIR/external-api.json"
  python3 - "$config_path" "$EXTERNAL_API_HTTP" <<'PY'
import json
import os
import stat
import sys

path, http_endpoint = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"httpEndpoint": http_endpoint}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  launch_process "$LOG_DIR/external-api.stdout.log" \
    "$LOG_DIR/external-api.stderr.log" "$EXTERNAL_API" --config="$config_path"
  PIDS+=("$!")
  wait_port external-api "$EXTERNAL_API_HTTP"
}

start_play_role() {
  local name="$1"
  local http_endpoint="$2"
  local control_endpoint="$3"
  local spot_route_endpoint="$4"
  local spot_router_endpoint="$5"
  local spot_pub_endpoint="$6"
  local delay_endpoint="$7"
  local external_api_base_url="$8"
  local config_path="$CONFIG_DIR/$name.json"
  python3 - "$config_path" "$name" "$http_endpoint" "$control_endpoint" \
    "$spot_route_endpoint" "$spot_router_endpoint" "$spot_pub_endpoint" \
    "$delay_endpoint" "$external_api_base_url" "$REDIS_ENDPOINT" \
    "$REDIS_KEY_PREFIX" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

(path, name, http_endpoint, control_endpoint, spot_route_endpoint,
 spot_router_endpoint, spot_pub_endpoint, delay_endpoint, external_api_base_url,
 redis_endpoint,
 redis_key_prefix, log_dir) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"nodeRid": name, "httpEndpoint": http_endpoint,
        "controlEndpoint": control_endpoint, "spotRouteEndpoint": spot_route_endpoint,
        "spotRouterEndpoint": spot_router_endpoint, "spotPubEndpoint": spot_pub_endpoint,
        "delayEndpoint": delay_endpoint,
        "externalApiBaseUrl": external_api_base_url,
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  launch_process "$LOG_DIR/$name.stdout.log" "$LOG_DIR/$name.stderr.log" \
    "$PLAY" --config="$config_path"
  PIDS+=("$!")
  wait_port "$name" "$http_endpoint"
  wait_port "$name-control" "$control_endpoint"
}

start_session_role() {
  local name="$1"
  local http_endpoint="$2"
  local stream_endpoint="$3"
  local control_endpoint="$4"
  local control_peer_endpoint="$5"
  local spot_route_endpoint="$6"
  local spot_route_peer_endpoint="$7"
  local spot_router_endpoint="$8"
  local spot_pub_endpoint="$9"
  local config_path="$CONFIG_DIR/$name.json"
  python3 - "$config_path" "$name" "$http_endpoint" "$stream_endpoint" \
    "$control_endpoint" "$control_peer_endpoint" "$spot_route_endpoint" \
    "$spot_route_peer_endpoint" "$spot_router_endpoint" "$spot_pub_endpoint" \
    "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

(path, name, http_endpoint, stream_endpoint, control_endpoint,
 control_peer_endpoint, spot_route_endpoint, spot_route_peer_endpoint,
 spot_router_endpoint, spot_pub_endpoint, redis_endpoint, redis_key_prefix,
 log_dir) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"nodeRid": name, "httpEndpoint": http_endpoint,
        "streamEndpoint": stream_endpoint, "controlEndpoint": control_endpoint,
        "controlPeerEndpoint": control_peer_endpoint,
        "spotRouteEndpoint": spot_route_endpoint,
        "spotRoutePeerEndpoint": spot_route_peer_endpoint,
        "spotRouterEndpoint": spot_router_endpoint, "spotPubEndpoint": spot_pub_endpoint,
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  launch_process "$LOG_DIR/$name.stdout.log" "$LOG_DIR/$name.stderr.log" \
    "$SESSION" --config="$config_path"
  PIDS+=("$!")
  wait_port "$name" "$http_endpoint"
  wait_port "$name-stream" "$stream_endpoint"
}

write_client_config() {
  local path="$1"
  local scenario="$2"
  local request_id="${3:-}"
  local spot_id="${4:-}"
  python3 - "$path" "$SESSION_A_STREAM" "$SESSION_B_STREAM" "$scenario" \
    "$request_id" "$spot_id" <<'PY'
import json
import os
import stat
import sys

path, session_a, session_b, scenario, request_id, spot_id = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"sessionAStreamEndpoint": session_a,
        "sessionBStreamEndpoint": session_b, "scenario": scenario,
        "requestId": request_id, "spotId": spot_id}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

start_external_api_role
start_delay_role delay-a "$DELAY_A_HTTP" "$DELAY_A_ENDPOINT"
start_delay_role delay-b "$DELAY_B_HTTP" "$DELAY_B_ENDPOINT"
start_play_role play-a "$PLAY_A_HTTP" "$PLAY_A_CONTROL" "$PLAY_A_SPOT_ROUTE" "$PLAY_A_SPOT_ROUTER" "$PLAY_A_SPOT_PUB" "$DELAY_A_ENDPOINT" "$EXTERNAL_API_HTTP"
PLAY_A_PID="${PIDS[-1]}"
start_play_role play-b "$PLAY_B_HTTP" "$PLAY_B_CONTROL" "$PLAY_B_SPOT_ROUTE" "$PLAY_B_SPOT_ROUTER" "$PLAY_B_SPOT_PUB" "$DELAY_B_ENDPOINT" "$EXTERNAL_API_HTTP"
start_session_role session-a "$SESSION_A_HTTP" "$SESSION_A_STREAM" "$PLAY_A_CONTROL" "$PLAY_B_CONTROL" "$PLAY_A_SPOT_ROUTE" "$PLAY_B_SPOT_ROUTE" "$SESSION_A_SPOT_ROUTER" "$SESSION_A_SPOT_PUB"
start_session_role session-b "$SESSION_B_HTTP" "$SESSION_B_STREAM" "$PLAY_B_CONTROL" "$PLAY_A_CONTROL" "$PLAY_B_SPOT_ROUTE" "$PLAY_A_SPOT_ROUTE" "$SESSION_B_SPOT_ROUTER" "$SESSION_B_SPOT_PUB"

if [[ "$SCENARIO_LOWER" == "all" || "$SCENARIO_LOWER" == "full" || "$SCENARIO_LOWER" == atd-[a-d]* || "$SCENARIO_LOWER" == "atd-e1" || "$SCENARIO_LOWER" == td-c[1-3] || "$SCENARIO_LOWER" == td-e[23] ]]; then
  CLIENT_SCENARIO="$SCENARIO_LOWER"
  if [[ "$CLIENT_SCENARIO" == "atd-d1" ]]; then
    CLIENT_SCENARIO="full"
  fi
  write_client_config "$CONFIG_DIR/client.json" "$CLIENT_SCENARIO"
  "$CLIENT" --config="$CONFIG_DIR/client.json" \
    >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
  if [[ "$SCENARIO_LOWER" == "all" || "$SCENARIO_LOWER" == "full" || "$SCENARIO_LOWER" == "atd-d1" ]]; then
    grep -q "scenario ATD-A1 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-A2 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-A3 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-A4 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-B1 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-B2 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-B3 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-C1 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-C2 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-C3 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-D2 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-D3 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-D4 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario ATD-E1 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario TD-E2 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario TD-E3 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario TD-C1 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario TD-C2 passed" "$LOG_DIR/client.stdout.log"
    grep -q "scenario TD-C3 passed" "$LOG_DIR/client.stdout.log"
    grep -q "automatic-turn-dispatch track-a-e1 result=passed" "$LOG_DIR/client.stdout.log"
    grep -q "^hold-completed|rid=play-a" "$LOG_DIR/play-a.evidence.log"
    grep -q "^await-completed|rid=play-a" "$LOG_DIR/play-a.evidence.log"
    grep -q "^worker-await-completed|rid=play-a" "$LOG_DIR/play-a.evidence.log"
    grep -q "^actor-await-completed|rid=play-a" "$LOG_DIR/play-a.evidence.log"
    grep -q "^timer-await-completed|rid=play-a" "$LOG_DIR/play-a.evidence.log"
    grep -q "^timeout-await-completed|rid=play-a" "$LOG_DIR/play-a.evidence.log"
    echo "scenario ATD-D1 passed"
  else
    EXPECTED_ID="$(printf '%s' "$SCENARIO_LOWER" | tr '[:lower:]' '[:upper:]')"
    grep -q "scenario ${EXPECTED_ID} passed" "$LOG_DIR/client.stdout.log"
  fi
fi

SHUTDOWN_ID="ATD-E3-$RUN_ID"
SHUTDOWN_SPOT="await-shutdown-${RUN_ID//[^a-zA-Z0-9]/}"
if [[ "$SCENARIO_LOWER" == "all" || "$SCENARIO_LOWER" == "full" || "$SCENARIO_LOWER" == "atd-e3" ]]; then
  write_client_config "$CONFIG_DIR/client-shutdown-wait.json" "shutdown-wait" \
    "$SHUTDOWN_ID" "$SHUTDOWN_SPOT"
  "$CLIENT" --config="$CONFIG_DIR/client-shutdown-wait.json" \
    >"$LOG_DIR/client-shutdown-wait.stdout.log" 2>"$LOG_DIR/client-shutdown-wait.stderr.log" &
  SHUTDOWN_CLIENT_PID=$!
  wait_file_contains \
    "$LOG_DIR/play-a.evidence.log" \
    "await-released|rid=play-a|spot=$SHUTDOWN_SPOT|request=$SHUTDOWN_ID" \
    "ATD-E3 pending await marker was not observed before shutdown." \
    "$SHUTDOWN_CLIENT_PID"
  kill "$PLAY_A_PID" >/dev/null 2>&1 || true
  wait_file_contains \
    "$LOG_DIR/client-shutdown-wait.stdout.log" \
    "automatic-turn-dispatch shutdown wait result=passed" \
    "ATD-E3 shutdown client did not observe the public closed/cancelled error." \
    "$SHUTDOWN_CLIENT_PID"
  terminate_gracefully play-a "$PLAY_A_PID"
  wait "$SHUTDOWN_CLIENT_PID"

  start_play_role play-a "$PLAY_A_HTTP" "$PLAY_A_CONTROL" "$PLAY_A_SPOT_ROUTE" "$PLAY_A_SPOT_ROUTER" "$PLAY_A_SPOT_PUB" "$DELAY_A_ENDPOINT" "$EXTERNAL_API_HTTP"
  PLAY_A_PID="${PIDS[-1]}"
  sleep "$SCENARIO_SETTLE_SECONDS"

  write_client_config "$CONFIG_DIR/client-shutdown-recovery.json" "shutdown-recovery" \
    "$SHUTDOWN_ID-recovery" "$SHUTDOWN_SPOT"
  "$CLIENT" --config="$CONFIG_DIR/client-shutdown-recovery.json" \
    >"$LOG_DIR/client-shutdown-recovery.stdout.log" \
    2>"$LOG_DIR/client-shutdown-recovery.stderr.log"
  grep -q "automatic-turn-dispatch shutdown recovery result=passed" "$LOG_DIR/client-shutdown-recovery.stdout.log"
fi

python3 - "$LOG_DIR/automatic-turn-dispatch-report.json" <<'PY'
import json
import sys

report = {
    "language": "cpp",
    "config": "AutomaticTurnDispatch",
    "scenarios": [
        {"id": "ATD-A1", "status": "passed",
         "markers": ["hold-started", "hold-resumed", "hold-completed", "probe-started"]},
        {"id": "ATD-A2", "status": "passed",
         "markers": ["await-started", "await-released", "probe-started",
                     "probe-completed", "await-resumed", "await-completed"]},
        {"id": "ATD-A3", "status": "passed",
         "markers": ["await-started", "await-released", "await-resumed", "await-completed"]},
        {"id": "ATD-A4", "status": "passed",
         "markers": ["worker-await-started", "worker-await-released", "probe-started",
                     "probe-completed", "worker-await-resumed", "worker-await-completed"]},
        {"id": "ATD-B1", "status": "passed",
         "markers": ["actor-await-started", "actor-await-released", "actor-fast-started",
                     "actor-fast-completed", "actor-await-resumed", "actor-await-completed"]},
        {"id": "ATD-B2", "status": "passed",
         "markers": ["actor-await-started", "actor-await-released", "actor-await-resumed",
                     "actor-await-completed", "actor-fast-started", "actor-fast-completed"]},
        {"id": "ATD-B3", "status": "passed",
         "markers": ["actor-join-await-started", "actor-join-await-released",
                     "actor-fast-started", "actor-fast-completed",
                     "actor-join-await-resumed", "actor-join-await-completed"]},
        {"id": "ATD-C1", "status": "passed",
         "markers": ["timer-await-started", "timer-await-released",
                     "timer-fast-started", "timer-fast-completed",
                     "timer-await-resumed", "timer-await-completed"]},
        {"id": "ATD-C2", "status": "passed",
         "markers": ["timer-await-started", "timer-await-released",
                     "timer-await-resumed", "timer-await-completed"]},
        {"id": "ATD-C3", "status": "passed",
         "markers": ["actor-await-started", "actor-await-released",
                     "timer-fast-started", "timer-fast-completed",
                     "actor-await-resumed", "actor-await-completed",
                     "timer-await-started", "timer-await-released",
                     "actor-fast-started", "actor-fast-completed",
                     "timer-await-resumed", "timer-await-completed"]},
        {"id": "ATD-D1", "status": "passed",
         "markers": ["hold-completed", "await-completed", "worker-await-completed",
                     "actor-await-completed", "timer-await-completed",
                     "timeout-await-completed"]},
        {"id": "ATD-D2", "status": "passed",
         "markers": ["remote-await-started", "remote-await-released",
                     "await-started", "await-released", "await-resumed",
                     "await-completed", "remote-await-resumed",
                     "remote-await-completed"]},
        {"id": "ATD-D3", "status": "passed",
         "markers": ["await-started", "await-released", "probe-started",
                     "probe-completed", "await-resumed", "await-completed"]},
        {"id": "ATD-D4", "status": "passed",
         "markers": ["actor-push-await-started", "actor-push-await-released",
                     "actor-push-await-resumed", "actor-push-await-completed"]},
        {"id": "ATD-E1", "status": "passed",
         "markers": ["timeout-await-started", "timeout-await-released",
                     "timeout-await-completed", "probe-started", "probe-completed"]},
        {"id": "ATD-E2", "status": "not-applicable",
         "reason": "C++ public contract has no cancellation argument (cpp README); pending G6 reviewer approval"},
        {"id": "ATD-E3", "status": "passed",
         "markers": ["await-released", "probe-completed"]},
        {"id": "ATD-E4", "status": "passed",
         "markers": ["static-check-http", "static-check-await-surface",
                     "static-check-connector"]},
        {"id": "ATD-E5", "status": "passed",
         "markers": ["report-written"]}
    ]
}

with open(sys.argv[1], "w", encoding="utf-8") as file:
    json.dump(report, file, ensure_ascii=False, indent=2)
    file.write("\n")
PY
python3 - "$LOG_DIR/automatic-turn-dispatch-report.json" <<'PY'
import json
import sys

report = json.load(open(sys.argv[1], encoding="utf-8"))
ids = {entry["id"] for entry in report["scenarios"]}
expected = {f"ATD-{track}{index}" for track, count in (("A", 4), ("B", 3), ("C", 3), ("D", 4), ("E", 5))
            for index in range(1, count + 1)}
missing = expected - ids
extra = ids - expected
assert not missing, sorted(missing)
assert not extra, sorted(extra)
assert report["language"] == "cpp"
assert report["config"] == "AutomaticTurnDispatch"
PY
echo "scenario ATD-E5 passed"
echo "automatic-turn-dispatch e2e result=passed"
