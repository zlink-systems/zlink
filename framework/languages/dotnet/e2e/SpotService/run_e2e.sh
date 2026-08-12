#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
zlink_dotnet_e2e_acquire_run_lock "$0" "$@"
PLAY_PROJECT="$SCRIPT_DIR/Server/Play/SpotService.Play.csproj"
SESSION_PROJECT="$SCRIPT_DIR/Server/Session/SpotService.Session.csproj"
MULTI_NODE_PROJECT="$SCRIPT_DIR/Server/MultiNode/SpotService.MultiNode.csproj"
GATEWAY_PROJECT="$SCRIPT_DIR/Server/Gateway/SpotService.Gateway.csproj"
CLIENT_PROJECT="$SCRIPT_DIR/Client/SpotService.Client.csproj"
PLAY_DLL="$SCRIPT_DIR/Server/Play/bin/Debug/net8.0/SpotService.Play.dll"
SESSION_DLL="$SCRIPT_DIR/Server/Session/bin/Debug/net8.0/SpotService.Session.dll"
MULTI_NODE_DLL="$SCRIPT_DIR/Server/MultiNode/bin/Debug/net8.0/SpotService.MultiNode.dll"
GATEWAY_DLL="$SCRIPT_DIR/Server/Gateway/bin/Debug/net8.0/SpotService.Gateway.dll"
CLIENT_DLL="$SCRIPT_DIR/Client/bin/Debug/net8.0/SpotService.Client.dll"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$STAMP"
CONFIG_DIR="$(mktemp -d)"
ALL_CHILD=0
SKIP_BUILD=0
E2E_START_ORDER="forward"
SCENARIOS=()
while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --all-child)
      ALL_CHILD=1
      shift
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --start-order)
      [[ "$#" -ge 2 ]] || { echo "--start-order requires a value" >&2; exit 2; }
      E2E_START_ORDER="$2"
      shift 2
      ;;
    --*)
      echo "Unknown option: $1" >&2
      exit 2
      ;;
    *)
      SCENARIOS+=("$1")
      shift
      ;;
  esac
done
if [[ "${#SCENARIOS[@]}" -eq 0 ]]; then
  SCENARIO_SET="all"
else
  SCENARIO_SET="$(IFS=,; echo "${SCENARIOS[*]}")"
fi
NEED_SESSION_NODES=1
NEED_SESSION_B=0
NEED_PLAY_B=1
NEED_TLS_STREAM=0
NEED_MESSAGE_FOLLOW_PROXY=0

scenario_selector_contains() {
  local expected="$1"
  local item
  if [[ "$SCENARIO_SET" == "$expected" ]]; then
    return 0
  fi
  IFS=',' read -ra items <<<"$SCENARIO_SET"
  for item in "${items[@]}"; do
    if [[ "$item" == "$expected" ]]; then
      return 0
    fi
  done
  return 1
}

case "$SCENARIO_SET" in
  all)
    NEED_SESSION_B=1
    NEED_TLS_STREAM=1
    ;;
  sm-e4|sm-e2-e3)
    NEED_SESSION_NODES=0
    ;;
  sm-a7-a8-c4)
    NEED_SESSION_NODES=0
    ;;
  sm-a5|sm-g2|sm-g5|sm-g5a|sm-g5b)
    NEED_SESSION_NODES=0
    if [[ "$SCENARIO_SET" != "sm-g2" && "$SCENARIO_SET" != "sm-g5" && "$SCENARIO_SET" != "sm-g5a" && "$SCENARIO_SET" != "sm-g5b" ]]; then
      NEED_PLAY_B=0
    fi
    ;;
  sm-e1-f4)
    NEED_PLAY_B=0
    ;;
  sm-c6)
    NEED_SESSION_NODES=0
    ;;
  sm-a9|sm-a10|sm-b0a|sm-b10|sm-b11)
    NEED_SESSION_NODES=0
    ;;
  sm-d14)
    NEED_PLAY_B=0
    NEED_TLS_STREAM=1
    ;;
  sm-b8)
    NEED_SESSION_NODES=0
    NEED_PLAY_B=0
    ;;
  sm-a12|sm-a13|sm-a12-a13)
    NEED_SESSION_NODES=0
    NEED_PLAY_B=0
    ;;
  sm-d15)
    NEED_PLAY_B=0
    ;;
  sm-g3|sm-g4)
    NEED_PLAY_B=0
    ;;
  instance-track-a|instance-owner-loss|instance-queue-owner-loss|instance-creating-join)
    NEED_SESSION_NODES=0
    NEED_PLAY_B=1
    ;;
  instance-idle)
    NEED_SESSION_NODES=0
    NEED_PLAY_B=0
    ;;
  sm-f6)
    NEED_SESSION_NODES=0
    NEED_PLAY_B=0
    ;;
  sm-d2-d6|sm-d4a|sm-d4b|sm-d10|sm-d12|sm-g1)
    NEED_SESSION_B=1
    if [[ "$SCENARIO_SET" == "sm-d4b" ]]; then
      NEED_MESSAGE_FOLLOW_PROXY=1
    fi
    ;;
  default-batch)
    NEED_SESSION_B=1
    NEED_TLS_STREAM=1
    NEED_MESSAGE_FOLLOW_PROXY=1
    ;;
  track-g)
    NEED_SESSION_B=1
    ;;
esac
if scenario_selector_contains sm-d2-d6 \
  || scenario_selector_contains sm-d10 \
  || scenario_selector_contains sm-d4a \
  || scenario_selector_contains sm-d4b \
  || scenario_selector_contains sm-d12 \
  || scenario_selector_contains sm-g1; then
  NEED_SESSION_B=1
fi
if scenario_selector_contains sm-d14; then
  NEED_TLS_STREAM=1
fi
mkdir -p "$LOG_DIR"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
REDIS_READINESS_TIMEOUT_SECONDS=60
ROUTE_READINESS_TIMEOUT_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
PROCESS_SHUTDOWN_TIMEOUT_SECONDS=15
CHILD_PROCESS_TIMEOUT_SECONDS=420
CLIENT_PROCESS_TIMEOUT_SECONDS=120
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
  python3 - "$PROCESS_SHUTDOWN_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"

build_projects() {
  if [[ "$SCENARIO_SET" == "sm-q9" || "$SCENARIO_SET" == "sm-f6" ]]; then
    dotnet build "$MULTI_NODE_PROJECT" --maxcpucount:1 >/dev/null
    dotnet build "$CLIENT_PROJECT" --maxcpucount:1 >/dev/null
    return
  fi

  dotnet build "$PLAY_PROJECT" --maxcpucount:1 >/dev/null
  dotnet build "$SESSION_PROJECT" --maxcpucount:1 >/dev/null
  dotnet build "$MULTI_NODE_PROJECT" --maxcpucount:1 >/dev/null
  dotnet build "$GATEWAY_PROJECT" --maxcpucount:1 >/dev/null
  dotnet build "$CLIENT_PROJECT" --maxcpucount:1 >/dev/null
}

if [[ "$SCENARIO_SET" == "all" && "$ALL_CHILD" != "1" ]]; then
  echo "log_dir=$LOG_DIR"
  build_projects
  child_manifest="$LOG_DIR/child-manifest.tsv"
  printf 'operation_group\texit_code\tchild_log_dir\tstdout_log\tstderr_log\n' \
    >"$child_manifest"
  for child_group in default-batch sm-a9 sm-a10 sm-b0a sm-b10 sm-b11 sm-f6 sm-g2 sm-g3 sm-g4 sm-g5a sm-g5b sm-g1 sm-q9; do
    echo "child operation_group=${child_group}"
    child_stdout_log="$LOG_DIR/child-${child_group}.stdout.log"
    child_stderr_log="$LOG_DIR/child-${child_group}.stderr.log"
    child_status=0
    if timeout "${CHILD_PROCESS_TIMEOUT_SECONDS}s" \
        bash "$SCRIPT_DIR/run_e2e.sh" --all-child --skip-build --start-order "$E2E_START_ORDER" "$child_group" \
        >"$child_stdout_log" 2>"$child_stderr_log"; then
      child_status=0
    else
      child_status=$?
    fi
    cat "$child_stdout_log"
    cat "$child_stderr_log" >&2
    child_log_dir="$(sed -n 's/^log_dir=//p' "$child_stdout_log" | /usr/bin/head -1)"
    printf '%s\t%s\t%s\t%s\t%s\n' \
      "$child_group" \
      "$child_status" \
      "$child_log_dir" \
      "$child_stdout_log" \
      "$child_stderr_log" \
      >>"$child_manifest"
    if [[ "$child_status" != "0" ]]; then
      printf 'spot-service e2e result=failed\toperation_group=%s\texit_code=%s\n' \
        "$child_group" "$child_status" \
        >>"$child_manifest"
      exit "$child_status"
    fi
  done
  printf 'spot-service e2e result=passed\n' >>"$child_manifest"
  echo "spot-service e2e result=passed"
  exit 0
fi

PIDS=()
AUX_PIDS=()

cleanup() {
  set +e
  if [[ -n "${PAUSED_PROCESS_GROUP:-}" ]]; then
    kill -CONT -- "-$PAUSED_PROCESS_GROUP" 2>/dev/null || true
  fi
  rm -rf "$CONFIG_DIR"
  if [[ -n "${REDIS_CONTAINER:-}" ]]; then
    zlink_redis_remove_by_id "$REDIS_CONTAINER" || true
  fi
  for pid in "${PIDS[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -INT "-$pid" 2>/dev/null || kill -INT "$pid" 2>/dev/null || true
    fi
  done
  for pid in "${AUX_PIDS[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -INT "-$pid" 2>/dev/null || kill -INT "$pid" 2>/dev/null || true
    fi
  done
  for _ in $(seq 1 "$PROCESS_SHUTDOWN_ATTEMPTS"); do
    local alive=0
    for pid in "${PIDS[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then
        alive=1
        break
      fi
    done
    [[ "$alive" == "0" ]] && break
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  for pid in "${PIDS[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 "-$pid" 2>/dev/null || kill -9 "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
  done
  for pid in "${AUX_PIDS[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 "-$pid" 2>/dev/null || kill -9 "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT
trap 'cleanup; exit 143' TERM INT

PORT_LIST="$(zlink_dotnet_e2e_allocate_ports 140)"
read -r -a PORTS <<<"$PORT_LIST"

PLAY_A_HTTP="http://127.0.0.1:${PORTS[3]}"
PLAY_A_CONTROL="tcp://127.0.0.1:${PORTS[4]}"
PLAY_A_SPOT_ROUTER="tcp://127.0.0.1:${PORTS[5]}"
PLAY_A_SPOT_ROUTER_BIND="$PLAY_A_SPOT_ROUTER"
PLAY_A_SPOT_PUB="tcp://127.0.0.1:${PORTS[6]}"
PLAY_A_EXTERNAL_SPOT="tcp://127.0.0.1:${PORTS[19]}"
PLAY_B_HTTP="http://127.0.0.1:${PORTS[7]}"
PLAY_B_CONTROL="tcp://127.0.0.1:${PORTS[8]}"
PLAY_B_SPOT_ROUTER="tcp://127.0.0.1:${PORTS[9]}"
PLAY_B_SPOT_ROUTER_BIND="$PLAY_B_SPOT_ROUTER"
PLAY_B_SPOT_PUB="tcp://127.0.0.1:${PORTS[10]}"
PLAY_B_EXTERNAL_SPOT="tcp://127.0.0.1:${PORTS[26]}"
SESSION_A_HTTP="http://127.0.0.1:${PORTS[11]}"
SESSION_A_SPOT_ROUTER="tcp://127.0.0.1:${PORTS[12]}"
SESSION_A_SPOT_ROUTER_BIND="$SESSION_A_SPOT_ROUTER"
SESSION_A_STREAM="tcp://127.0.0.1:${PORTS[13]}"
SESSION_A_TLS_STREAM="tls://127.0.0.1:${PORTS[25]}"
SESSION_A_CONTROL="tcp://127.0.0.1:${PORTS[14]}"
SESSION_B_HTTP="http://127.0.0.1:${PORTS[15]}"
SESSION_B_SPOT_ROUTER="tcp://127.0.0.1:${PORTS[16]}"
SESSION_B_STREAM="tcp://127.0.0.1:${PORTS[17]}"
SESSION_B_CONTROL="tcp://127.0.0.1:${PORTS[18]}"
CLIENT_CONTROL="tcp://127.0.0.1:${PORTS[20]}"
CLIENT_EXTERNAL_ROUTE="tcp://127.0.0.1:${PORTS[21]}"
CLIENT_SPOT_ROUTER="tcp://127.0.0.1:${PORTS[22]}"
CLIENT_SPOT_PUB="tcp://127.0.0.1:${PORTS[24]}"
CLIENT_EXTERNAL_ROUTE_B="tcp://127.0.0.1:${PORTS[27]}"
MULTI_A_HTTP="http://127.0.0.1:${PORTS[28]}"
MULTI_ROUTE_A="tcp://127.0.0.1:${PORTS[29]}"
MULTI_ROUTE_B="tcp://127.0.0.1:${PORTS[30]}"
MULTI_SPOT_ROUTER_A="tcp://127.0.0.1:${PORTS[31]}"
MULTI_SPOT_ROUTER_B="tcp://127.0.0.1:${PORTS[32]}"
CLIENT_MULTI_ROUTE_A="tcp://127.0.0.1:${PORTS[33]}"
CLIENT_MULTI_ROUTE_B="tcp://127.0.0.1:${PORTS[34]}"
MULTI_B_HTTP="http://127.0.0.1:${PORTS[35]}"
GATEWAY_HTTP="http://127.0.0.1:${PORTS[36]}"
GATEWAY_SPOT_ROUTER="tcp://127.0.0.1:${PORTS[37]}"
PLAY_A_TRANSPORT_PROXY_ADMIN="http://127.0.0.1:${PORTS[39]}"
PLAY_B_TRANSPORT_PROXY_ADMIN="http://127.0.0.1:${PORTS[40]}"
SESSION_A_TRANSPORT_PROXY_ADMIN="http://127.0.0.1:${PORTS[41]}"
if [[ "$NEED_MESSAGE_FOLLOW_PROXY" == "1" ]]; then
  PLAY_A_SPOT_ROUTER_BIND="tcp://127.0.0.2:${PORTS[5]}"
  PLAY_B_SPOT_ROUTER_BIND="tcp://127.0.0.3:${PORTS[9]}"
  SESSION_A_SPOT_ROUTER_BIND="tcp://127.0.0.4:${PORTS[12]}"
fi
WAIT_SOURCE_PORT_INDEX=38
WAIT_ROLE_PID=""

pid_for_role() {
  local expected_role="$1"
  local index
  for index in "${!ORDERED_SERVER_ROLES[@]}"; do
    if [[ "${ORDERED_SERVER_ROLES[$index]}" == "$expected_role" ]]; then
      printf '%s\n' "${PIDS[$index]}"
      return 0
    fi
  done
  return 1
}

endpoint_port() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  endpoint="${endpoint#http://}"
  endpoint="${endpoint#tls://}"
  echo "${endpoint##*:}"
}

endpoint_host() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  endpoint="${endpoint#http://}"
  endpoint="${endpoint#tls://}"
  echo "${endpoint%:*}"
}

wait_port() {
  local name="$1"
  local endpoint="$2"
  local pid="${WAIT_ROLE_PID:-}"
  local host
  local port
  host="$(endpoint_host "$endpoint")"
  port="$(endpoint_port "$endpoint")"
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    if python3 - "$host" "$port" <<'PY' >/dev/null 2>&1
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    sock.settimeout(0.2)
    sock.connect((host, port))
finally:
    sock.close()
PY
    then
      return 0
    fi
    if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
      echo "${name} exited before readiness at ${endpoint}" >&2
      return 1
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for ${name} at ${endpoint}" >&2
  return 1
}

wait_health() {
  local name="$1"
  local url="$2"
  local pid="${WAIT_ROLE_PID:-}"
  local deadline_ns
  deadline_ns="$(
    python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" <<'PY'
import sys
import time

timeout = float(sys.argv[1])
print(time.monotonic_ns() + int(timeout * 1_000_000_000))
PY
  )"
  while true; do
    local probe_timeout
    probe_timeout="$(
      python3 - "$deadline_ns" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import sys
import time

deadline_ns = int(sys.argv[1])
probe_timeout = float(sys.argv[2])
remaining = (deadline_ns - time.monotonic_ns()) / 1_000_000_000
if remaining <= 0:
    print("0")
else:
    print(f"{min(probe_timeout, remaining):.3f}")
PY
    )"
    if [[ "$probe_timeout" == "0" ]]; then
      break
    fi
    if curl --max-time "$probe_timeout" \
      --connect-timeout "$probe_timeout" \
      -fsS "$url/health" >/dev/null 2>&1; then
      return 0
    fi
    if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
      echo "${name} exited before readiness at ${url}" >&2
      return 1
    fi
    python3 - "$deadline_ns" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import sys
import time

deadline_ns = int(sys.argv[1])
poll = float(sys.argv[2])
remaining = (deadline_ns - time.monotonic_ns()) / 1_000_000_000
if remaining > 0:
    time.sleep(min(poll, remaining))
PY
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for ${name} at ${url}" >&2
  return 1
}

wait_control_route() {
  local source_url="$1"
  local target_rid="$2"
  local name="$3"
  local timeout_seconds="${4:-$ROUTE_READINESS_TIMEOUT_SECONDS}"
  local payload deadline_ns
  payload="{\"value\":\"ready-${name}\"}"
  deadline_ns="$(python3 - "$timeout_seconds" <<'PY'
import sys
import time

print(time.monotonic_ns() + int(float(sys.argv[1]) * 1_000_000_000))
PY
  )"
  while true; do
    local probe_timeout
    probe_timeout="$(python3 - "$deadline_ns" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import sys
import time

remaining = (int(sys.argv[1]) - time.monotonic_ns()) / 1_000_000_000
print("0" if remaining <= 0 else f"{min(float(sys.argv[2]), remaining):.3f}")
PY
    )"
    if [[ "$probe_timeout" == "0" ]]; then
      break
    fi
    if curl --max-time "$probe_timeout" \
      --connect-timeout "$probe_timeout" \
      -fsS \
      -H 'content-type: application/json' \
      -d "$payload" \
      "$source_url/channel/control-ping/$target_rid" >/dev/null 2>&1; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Control route ${name} was not ready within ${timeout_seconds}s via ${source_url} -> ${target_rid}" >&2
  return 1
}

wait_spot_route() {
  local source_url="$1"
  local target_rid="$2"
  local name="$3"
  local timeout_seconds="${4:-$ROUTE_READINESS_TIMEOUT_SECONDS}"
  local deadline_ns
  deadline_ns="$(python3 - "$timeout_seconds" <<'PY'
import sys
import time

print(time.monotonic_ns() + int(float(sys.argv[1]) * 1_000_000_000))
PY
  )"
  while true; do
    local probe_timeout
    probe_timeout="$(python3 - "$deadline_ns" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import sys
import time

remaining = (int(sys.argv[1]) - time.monotonic_ns()) / 1_000_000_000
print("0" if remaining <= 0 else f"{min(float(sys.argv[2]), remaining):.3f}")
PY
    )"
    if [[ "$probe_timeout" == "0" ]]; then
      break
    fi
    if curl --max-time "$probe_timeout" \
      --connect-timeout "$probe_timeout" \
      -fsS "$source_url/channel/spot-peer-ready/$target_rid" >/dev/null 2>&1; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Spot route ${name} was not ready within ${timeout_seconds}s via ${source_url} -> ${target_rid}" >&2
  return 1
}

ordered_roles() {
  python3 - "$E2E_START_ORDER" "$@" <<'PY'
import random
import sys

mode = sys.argv[1]
roles = sys.argv[2:]
if mode in ("", "forward"):
    pass
elif mode == "reverse":
    roles.reverse()
elif mode.startswith("shuffle:"):
    seed_text = mode.split(":", 1)[1]
    if seed_text == "":
        raise SystemExit("start order shuffle requires a seed")
    random.Random(int(seed_text)).shuffle(roles)
else:
    raise SystemExit(f"unsupported start order={mode!r}")
for role in roles:
    print(role)
PY
}

start_server() {
  local name="$1"
  local dll="$2"
  shift 2
  local config="$CONFIG_DIR/$name.json"
  python3 "$SCRIPT_DIR/../write_role_config.py" "$config" -- --role "$name" "$@"
  setsid bash -c '
    set +e
    name="$1"
    dll="$2"
    log_dir="$3"
    config="$4"
    shift 4
    dotnet "$dll" --config "$config" >"$log_dir/${name}.stdout.log" 2>"$log_dir/${name}.stderr.log"
    rc=$?
    echo "server_exit name=${name} exit_code=${rc}" >"$log_dir/${name}.exit.log"
    exit "$rc"
  ' bash "$name" "$dll" "$LOG_DIR" "$config" &
  PIDS+=("$!")
}

start_transport_proxy() {
  local name="$1"
  local listen_port="$2"
  local upstream_host="$3"
  local upstream_port="$4"
  local admin_port="$5"
  setsid python3 "$SCRIPT_DIR/../SpotActorTransfer/Support/stream_marker_proxy.py" \
    --listen-port "$listen_port" \
    --upstream-host "$upstream_host" \
    --upstream-port "$upstream_port" \
    --admin-port "$admin_port" \
    9>&- \
    >>"$LOG_DIR/$name.stdout.log" 2>>"$LOG_DIR/$name.stderr.log" &
  AUX_PIDS+=("$!")
}

assert_servers_alive() {
  local phase="$1"
  local index pid role
  for index in "${!PIDS[@]}"; do
    pid="${PIDS[$index]}"
    role="${ORDERED_SERVER_ROLES[$index]:-unknown}"
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "server exited before ${phase}: role=${role} pid=${pid}" >&2
      if [[ -f "$LOG_DIR/${role}.exit.log" ]]; then
        cat "$LOG_DIR/${role}.exit.log" >&2
      fi
      if [[ -f "$LOG_DIR/${role}.stderr.log" ]]; then
        tail -120 "$LOG_DIR/${role}.stderr.log" >&2
      fi
      return 1
    fi
  done
}

assert_expected_server_exit() {
  local role="$1"
  local expected_exit_code="$2"
  local phase="$3"
  local index pid exit_file
  pid=""
  for index in "${!ORDERED_SERVER_ROLES[@]}"; do
    if [[ "${ORDERED_SERVER_ROLES[$index]}" == "$role" ]]; then
      pid="${PIDS[$index]}"
      break
    fi
  done
  if [[ -z "$pid" ]]; then
    echo "expected server role ${role} was not started for ${phase}" >&2
    return 1
  fi
  if kill -0 "$pid" 2>/dev/null; then
    echo "expected server role ${role} to exit during ${phase}, but it is still alive: pid=${pid}" >&2
    return 1
  fi
  exit_file="$LOG_DIR/${role}.exit.log"
  if [[ ! -f "$exit_file" ]]; then
    echo "expected server exit log is missing for ${role} during ${phase}: ${exit_file}" >&2
    return 1
  fi
  if ! grep -qx "server_exit name=${role} exit_code=${expected_exit_code}" "$exit_file"; then
    echo "unexpected server exit for ${role} during ${phase}" >&2
    cat "$exit_file" >&2
    return 1
  fi
}

wait_for_server_exit() {
  local role="$1"
  local pid="$2"
  for _ in $(seq 1 "$PROCESS_SHUTDOWN_ATTEMPTS"); do
    if [[ -f "$LOG_DIR/${role}.exit.log" ]]; then
      wait "$pid" 2>/dev/null || true
      return 0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "server role ${role} did not exit within the expected startup window" >&2
  return 1
}

assert_b10_invalid_server() {
  local role="$1"
  local pid="$2"
  local http_url="$3"
  wait_for_server_exit "$role" "$pid"
  local exit_line
  exit_line="$(<"$LOG_DIR/${role}.exit.log")"
  if [[ "$exit_line" == *"exit_code=0" ]]; then
    echo "SM-B10 invalid role unexpectedly started successfully: ${role}" >&2
    return 1
  fi
  if curl --max-time "$HTTP_PROBE_TIMEOUT_SECONDS" \
      --connect-timeout "$HTTP_PROBE_TIMEOUT_SECONDS" \
      -fsS "$http_url/health" >/dev/null 2>&1; then
    echo "SM-B10 invalid role exposed an HTTP listener: ${role}" >&2
    return 1
  fi
  if ! grep -Eiq "Location Store" \
      "$LOG_DIR/${role}.stderr.log" "$LOG_DIR/${role}.stdout.log"; then
    echo "SM-B10 invalid role did not report the Location Store configuration error: ${role}" >&2
    tail -120 "$LOG_DIR/${role}.stderr.log" >&2 || true
    return 1
  fi
}

run_b10_client() {
  local config="$CONFIG_DIR/client-sm-b10.json"
  python3 "$SCRIPT_DIR/../write_role_config.py" "$config" -- \
    --config-dir "$CONFIG_DIR" \
    --gateway-url "$GATEWAY_HTTP" \
    --play-a-url "$PLAY_A_HTTP" \
    --play-b-url "$PLAY_B_HTTP" \
    --multi-a-url "$MULTI_A_HTTP" \
    --multi-b-url "$MULTI_B_HTTP" \
    --session-a-url "$SESSION_A_HTTP" \
    --session-a-stream-endpoint "$SESSION_A_STREAM" \
    --session-a-tls-stream-endpoint "$SESSION_A_TLS_STREAM" \
    --session-b-stream-endpoint "$SESSION_B_STREAM" \
    --sm-c6-pause-ack-file "$LOG_DIR/sm-c6-paused" \
    --sm-c6-resume-ack-file "$LOG_DIR/sm-c6-resumed" \
    --play-a-transport-proxy-admin "$PLAY_A_TRANSPORT_PROXY_ADMIN" \
    --play-b-transport-proxy-admin "$PLAY_B_TRANSPORT_PROXY_ADMIN" \
    --session-a-transport-proxy-admin "$SESSION_A_TRANSPORT_PROXY_ADMIN" \
    --b10-control-endpoint "$PLAY_A_CONTROL" \
    --b10-control-rid "b10-manual-control" \
    --operation-group sm-b10
  timeout "${CLIENT_PROCESS_TIMEOUT_SECONDS}s" dotnet "$CLIENT_DLL" --config "$config" \
    2> >(tee -a "$LOG_DIR/client.stderr.log" >&2) \
    | tee -a "$LOG_DIR/client.stdout.log"
}

start_named_server() {
  case "$1" in
    session-a)
      SESSION_A_ARGS=(
        --rid session-a
        --http-url "$SESSION_A_HTTP"
        --redis-endpoint "$REDIS_ENDPOINT"
        --redis-key-prefix "$REDIS_KEY_PREFIX"
        --control-endpoint "$SESSION_A_CONTROL"
        --control-peer-a-endpoint "$PLAY_A_CONTROL"
        --spot-router-endpoint "$SESSION_A_SPOT_ROUTER_BIND"
        --spot-peer-a-endpoint "$PLAY_A_SPOT_ROUTER"
        --stream-endpoint "$SESSION_A_STREAM"
        --evidence-file "$LOG_DIR/session-a.evidence.log"
        --log-dir "$LOG_DIR"
      )
      if [[ "$NEED_TLS_STREAM" == "1" ]]; then
        SESSION_A_ARGS+=(
          --tls-stream-endpoint "$SESSION_A_TLS_STREAM"
          --tls-cert-path "$TLS_CERT"
          --tls-key-path "$TLS_KEY"
        )
      fi
      if [[ "$NEED_MESSAGE_FOLLOW_PROXY" == "1" ]]; then
        SESSION_A_ARGS+=(--spot-router-advertise-host 127.0.0.1)
      fi
      if [[ "$NEED_PLAY_B" == "1" && "$SCENARIO_SET" != "sm-g2" ]]; then
        SESSION_A_ARGS+=(
          --control-peer-b-endpoint "$PLAY_B_CONTROL"
          --spot-peer-b-endpoint "$PLAY_B_SPOT_ROUTER"
        )
      fi
      start_server session-a "$SESSION_DLL" "${SESSION_A_ARGS[@]}"
      ;;
    session-b)
      SESSION_B_ARGS=(
        --rid session-b \
        --http-url "$SESSION_B_HTTP" \
        --redis-endpoint "$REDIS_ENDPOINT" \
        --redis-key-prefix "$REDIS_KEY_PREFIX" \
        --control-endpoint "$SESSION_B_CONTROL" \
        --control-peer-a-endpoint "$PLAY_A_CONTROL" \
        --spot-router-endpoint "$SESSION_B_SPOT_ROUTER" \
        --spot-peer-a-endpoint "$PLAY_A_SPOT_ROUTER" \
        --stream-endpoint "$SESSION_B_STREAM" \
        --evidence-file "$LOG_DIR/session-b.evidence.log" \
        --log-dir "$LOG_DIR"
      )
      if [[ "$NEED_PLAY_B" == "1" && "$SCENARIO_SET" != "sm-g2" ]]; then
        SESSION_B_ARGS+=(
          --control-peer-b-endpoint "$PLAY_B_CONTROL"
          --spot-peer-b-endpoint "$PLAY_B_SPOT_ROUTER"
        )
      fi
      start_server session-b "$SESSION_DLL" "${SESSION_B_ARGS[@]}"
      ;;
    play-a)
      PLAY_A_ARGS=(
        --rid play-a \
        --http-url "$PLAY_A_HTTP" \
        --redis-endpoint "$REDIS_ENDPOINT" \
        --redis-key-prefix "$REDIS_KEY_PREFIX" \
        --control-endpoint "$PLAY_A_CONTROL" \
        --spot-router-endpoint "$PLAY_A_SPOT_ROUTER_BIND" \
        --spot-pub-endpoint "$PLAY_A_SPOT_PUB" \
        --client-spot-pub-endpoint "$CLIENT_SPOT_PUB" \
        --external-spot-endpoint "$PLAY_A_EXTERNAL_SPOT" \
        --evidence-file "$LOG_DIR/play-a.evidence.log" \
        --log-dir "$LOG_DIR"
      )
      if [[ "$NEED_MESSAGE_FOLLOW_PROXY" == "1" ]]; then
        PLAY_A_ARGS+=(
          --spot-router-advertise-host 127.0.0.1
          --message-follow-duration-milliseconds 7000
          --owner-lease-ttl-milliseconds 30000
        )
      fi
      if [[ "$SCENARIO_SET" == "instance-idle" ]]; then
        PLAY_A_ARGS+=(--instance-spot-idle-timeout-milliseconds 100)
      fi
      if [[ "$SCENARIO_SET" == "instance-queue-owner-loss" ]]; then
        PLAY_A_ARGS+=(--instance-handler-gate-file "$LOG_DIR/instance-handler-release")
      fi
      if [[ "$SCENARIO_SET" == "instance-creating-join" ]]; then
        PLAY_A_ARGS+=(--instance-initialization-gate-file "$LOG_DIR/instance-initialization-release")
      fi
      if [[ "$SCENARIO_SET" == "sm-a9" ]]; then
        PLAY_A_ARGS+=(--spot-initialization-gate-file "$LOG_DIR/sm-a9-play-a-gate")
      fi
      if [[ "$SCENARIO_SET" == "sm-b11" ]]; then
        PLAY_A_ARGS+=(--actor-factory-gate-file "$LOG_DIR/sm-b11-play-a-gate")
      fi
      if [[ "$SCENARIO_SET" == "sm-b0a" ]]; then
        PLAY_A_ARGS+=(--actor-creation-race-gate-file "$LOG_DIR/sm-b0a-actor-gate")
      fi
      if [[ "$SCENARIO_SET" == "sm-g5" || "$SCENARIO_SET" == "sm-g5a" || "$SCENARIO_SET" == "sm-g5b" ]]; then
        PLAY_A_ARGS+=(--population-limit 1000)
      fi
      start_server play-a "$PLAY_DLL" "${PLAY_A_ARGS[@]}"
      ;;
    play-b)
      PLAY_B_ARGS=(
        --rid play-b \
        --http-url "$PLAY_B_HTTP" \
        --redis-endpoint "$REDIS_ENDPOINT" \
        --redis-key-prefix "$REDIS_KEY_PREFIX" \
        --control-endpoint "$PLAY_B_CONTROL" \
        --spot-router-endpoint "$PLAY_B_SPOT_ROUTER_BIND" \
        --spot-pub-endpoint "$PLAY_B_SPOT_PUB" \
        --client-spot-pub-endpoint "$PLAY_A_SPOT_PUB" \
        --external-spot-endpoint "$PLAY_B_EXTERNAL_SPOT" \
        --evidence-file "$LOG_DIR/play-b.evidence.log" \
        --log-dir "$LOG_DIR"
      )
      if [[ "$NEED_MESSAGE_FOLLOW_PROXY" == "1" ]]; then
        PLAY_B_ARGS+=(
          --spot-router-advertise-host 127.0.0.1
          --message-follow-duration-milliseconds 7000
          --owner-lease-ttl-milliseconds 30000
        )
      fi
      if [[ "$SCENARIO_SET" == "sm-g5" || "$SCENARIO_SET" == "sm-g5a" || "$SCENARIO_SET" == "sm-g5b" ]]; then
        PLAY_B_ARGS+=(--population-limit 1000)
      fi
      if [[ "$SCENARIO_SET" == "sm-c6" || "$SCENARIO_SET" == "default-batch" ]]; then
        PLAY_B_ARGS+=(
          --application-hwm-bytes 1048576
          --backpressure-gate-file "$LOG_DIR/sm-c6-play-b-gate"
        )
      fi
      if [[ "$SCENARIO_SET" == "instance-queue-owner-loss" ]]; then
        PLAY_B_ARGS+=(--instance-handler-gate-file "$LOG_DIR/instance-handler-release")
      fi
      if [[ "$SCENARIO_SET" == "instance-creating-join" ]]; then
        PLAY_B_ARGS+=(--instance-initialization-gate-file "$LOG_DIR/instance-initialization-release")
      fi
      start_server play-b "$PLAY_DLL" "${PLAY_B_ARGS[@]}"
      ;;
    multi-node-a)
      MULTI_NODE_A_ARGS=(
        --rid multi-node-a \
        --http-url "$MULTI_A_HTTP" \
        --redis-endpoint "$REDIS_ENDPOINT" \
        --redis-key-prefix "$REDIS_KEY_PREFIX" \
        --multi-spot-router-a-endpoint "$MULTI_SPOT_ROUTER_A" \
        --evidence-file "$LOG_DIR/multi-node-a.evidence.log" \
        --log-dir "$LOG_DIR"
      )
      if [[ "$SCENARIO_SET" != "sm-f6" ]]; then
        MULTI_NODE_A_ARGS+=(--multi-route-a-endpoint "$MULTI_ROUTE_A")
      fi
      start_server multi-node-a "$MULTI_NODE_DLL" "${MULTI_NODE_A_ARGS[@]}"
      ;;
    multi-node-b)
      MULTI_NODE_B_ARGS=(
        --rid multi-node-b \
        --http-url "$MULTI_B_HTTP" \
        --redis-endpoint "$REDIS_ENDPOINT" \
        --redis-key-prefix "$REDIS_KEY_PREFIX" \
        --multi-spot-router-b-endpoint "$MULTI_SPOT_ROUTER_B" \
        --evidence-file "$LOG_DIR/multi-node-b.evidence.log" \
        --log-dir "$LOG_DIR"
      )
      if [[ "$SCENARIO_SET" != "sm-f6" ]]; then
        MULTI_NODE_B_ARGS+=(--multi-route-b-endpoint "$MULTI_ROUTE_B")
      fi
      start_server multi-node-b "$MULTI_NODE_DLL" "${MULTI_NODE_B_ARGS[@]}"
      ;;
    gateway)
      GATEWAY_ARGS=(
        --rid gateway \
        --http-url "$GATEWAY_HTTP" \
        --redis-endpoint "$REDIS_ENDPOINT" \
        --redis-key-prefix "$REDIS_KEY_PREFIX" \
        --spot-router-endpoint "$GATEWAY_SPOT_ROUTER" \
        --spot-pub-endpoint "$CLIENT_SPOT_PUB" \
        --external-spot-endpoint "$PLAY_A_EXTERNAL_SPOT" \
        --spot-peer-a-endpoint "$PLAY_A_SPOT_ROUTER" \
        --spot-peer-b-endpoint "$PLAY_B_SPOT_ROUTER" \
        --evidence-file "$LOG_DIR/gateway.evidence.log" \
        --log-dir "$LOG_DIR"
      )
      if [[ "$SCENARIO_SET" == "sm-b0a" ]]; then
        GATEWAY_ARGS+=(
          --actor-creation-race-gate-file "$LOG_DIR/sm-b0a-actor-gate"
        )
      fi
      start_server gateway "$GATEWAY_DLL" "${GATEWAY_ARGS[@]}"
      ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

wait_named_server() {
  WAIT_ROLE_PID="$(pid_for_role "$1")"
  case "$1" in
    session-a)
      wait_health session-a "$SESSION_A_HTTP"
      wait_port session-a-control "$SESSION_A_CONTROL"
      wait_port session-a-spot-router "$SESSION_A_SPOT_ROUTER_BIND"
      wait_port session-a-stream "$SESSION_A_STREAM"
      if [[ "$NEED_TLS_STREAM" == "1" ]]; then
        wait_port session-a-tls-stream "$SESSION_A_TLS_STREAM"
      fi
      ;;
    session-b)
      wait_health session-b "$SESSION_B_HTTP"
      wait_port session-b-control "$SESSION_B_CONTROL"
      wait_port session-b-spot-router "$SESSION_B_SPOT_ROUTER"
      wait_port session-b-stream "$SESSION_B_STREAM"
      ;;
    play-a)
      wait_health play-a "$PLAY_A_HTTP"
      wait_port play-a-control "$PLAY_A_CONTROL"
      wait_port play-a-spot-router "$PLAY_A_SPOT_ROUTER_BIND"
      wait_port play-a-external-spot "$PLAY_A_EXTERNAL_SPOT"
      ;;
    play-b)
      wait_health play-b "$PLAY_B_HTTP"
      wait_port play-b-control "$PLAY_B_CONTROL"
      wait_port play-b-spot-router "$PLAY_B_SPOT_ROUTER_BIND"
      wait_port play-b-external-spot "$PLAY_B_EXTERNAL_SPOT"
      ;;
    multi-node-a)
      wait_health multi-node-a "$MULTI_A_HTTP"
      if [[ "$SCENARIO_SET" != "sm-f6" ]]; then
        wait_port multi-route-a "$MULTI_ROUTE_A"
      fi
      wait_port multi-spot-router-a "$MULTI_SPOT_ROUTER_A"
      ;;
    multi-node-b)
      wait_health multi-node-b "$MULTI_B_HTTP"
      if [[ "$SCENARIO_SET" != "sm-f6" ]]; then
        wait_port multi-route-b "$MULTI_ROUTE_B"
      fi
      wait_port multi-spot-router-b "$MULTI_SPOT_ROUTER_B"
      ;;
    gateway)
      wait_health gateway "$GATEWAY_HTTP"
      wait_port gateway-spot-router "$GATEWAY_SPOT_ROUTER"
      ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

echo "log_dir=$LOG_DIR"
echo "start_order=$E2E_START_ORDER"
if [[ "$SKIP_BUILD" != "1" ]]; then
  build_projects
fi
TLS_CERT="$LOG_DIR/session-a-tls.crt"
TLS_KEY="$LOG_DIR/session-a-tls.key"
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "$TLS_KEY" \
  -out "$TLS_CERT" \
  -days 1 \
  -subj "/CN=localhost" >/dev/null 2>&1

if [[ "$SCENARIO_SET" == "sm-b10" ]]; then
  PIDS=()
  ORDERED_SERVER_ROLES=(b10-invalid-client)
  start_server b10-invalid-client "$PLAY_DLL" \
    --rid b10-invalid-client \
    --http-url "http://127.0.0.1:${PORTS[44]}" \
    --control-endpoint "tcp://127.0.0.1:${PORTS[45]}" \
    --spot-router-endpoint "tcp://127.0.0.1:${PORTS[46]}" \
    --b10-mode client \
    --evidence-file "$LOG_DIR/b10-invalid-client.evidence.log" \
    --log-dir "$LOG_DIR"
  invalid_pid="${PIDS[0]}"
  assert_b10_invalid_server b10-invalid-client "$invalid_pid" "http://127.0.0.1:${PORTS[44]}"

  PIDS=()
  ORDERED_SERVER_ROLES=(b10-invalid-server)
  start_server b10-invalid-server "$PLAY_DLL" \
    --rid b10-invalid-server \
    --http-url "http://127.0.0.1:${PORTS[47]}" \
    --control-endpoint "tcp://127.0.0.1:${PORTS[48]}" \
    --spot-router-endpoint "tcp://127.0.0.1:${PORTS[49]}" \
    --b10-mode server \
    --evidence-file "$LOG_DIR/b10-invalid-server.evidence.log" \
    --log-dir "$LOG_DIR"
  invalid_pid="${PIDS[0]}"
  assert_b10_invalid_server b10-invalid-server "$invalid_pid" "http://127.0.0.1:${PORTS[47]}"

  PIDS=()
  ORDERED_SERVER_ROLES=(b10-manual)
  start_server b10-manual "$PLAY_DLL" \
    --rid b10-manual \
    --http-url "$PLAY_A_HTTP" \
    --control-endpoint "$PLAY_A_CONTROL" \
    --b10-mode manual \
    --evidence-file "$LOG_DIR/b10-manual.evidence.log" \
    --log-dir "$LOG_DIR"
  WAIT_ROLE_PID="${PIDS[0]}"
  wait_health b10-manual "$PLAY_A_HTTP"
  wait_port b10-manual-control "$PLAY_A_CONTROL"
  run_b10_client
  assert_servers_alive "SM-B10 completion"
  echo "spot-service e2e result=passed"
  exit 0
fi

# The run owns its Redis: a dedicated, throwaway container is the shared
# location store every server registers into (no registry process exists).
if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run the SpotService E2E (it provisions a dedicated Redis container)." >&2
  exit 1
fi
zlink_redis_start_scoped_assign \
  REDIS_CONTAINER \
  REDIS_ENDPOINT \
  "zlink-redis-dotnet-e2e-spot-service" \
  "redis:7.2-alpine" \
  "$LOG_DIR"
REDIS_KEY_PREFIX="spotservice-e2e:$$:"
zlink_redis_wait_ready "$REDIS_CONTAINER" "$REDIS_READINESS_TIMEOUT_SECONDS"

if [[ "$NEED_MESSAGE_FOLLOW_PROXY" == "1" ]]; then
  start_transport_proxy play-a-transport-proxy "${PORTS[5]}" \
    127.0.0.2 "${PORTS[5]}" "${PORTS[39]}"
  start_transport_proxy play-b-transport-proxy "${PORTS[9]}" \
    127.0.0.3 "${PORTS[9]}" "${PORTS[40]}"
  start_transport_proxy session-a-transport-proxy "${PORTS[12]}" \
    127.0.0.4 "${PORTS[12]}" "${PORTS[41]}"
  WAIT_ROLE_PID=""
  wait_health play-a-transport-proxy "$PLAY_A_TRANSPORT_PROXY_ADMIN"
  wait_health play-b-transport-proxy "$PLAY_B_TRANSPORT_PROXY_ADMIN"
  wait_health session-a-transport-proxy "$SESSION_A_TRANSPORT_PROXY_ADMIN"
fi

SERVER_ROLES=()
if [[ "$SCENARIO_SET" != "sm-q9" && "$NEED_SESSION_NODES" == "1" ]]; then
  SERVER_ROLES+=(session-a)
  if [[ "$NEED_SESSION_B" == "1" ]]; then
    SERVER_ROLES+=(session-b)
  fi
fi

if [[ "$SCENARIO_SET" != "sm-q9" && "$SCENARIO_SET" != "sm-f6" ]]; then
  SERVER_ROLES+=(play-a)
  if [[ "$NEED_PLAY_B" == "1" && "$SCENARIO_SET" != "sm-g2" ]]; then
    SERVER_ROLES+=(play-b)
  fi
fi

if scenario_selector_contains sm-q9 || scenario_selector_contains sm-f6; then
  SERVER_ROLES+=(multi-node-a multi-node-b)
fi

if [[ "$SCENARIO_SET" != "sm-q9" && "$SCENARIO_SET" != "sm-f6" ]]; then
  SERVER_ROLES+=(gateway)
fi

mapfile -t ORDERED_SERVER_ROLES < <(ordered_roles "${SERVER_ROLES[@]}")
for role in "${ORDERED_SERVER_ROLES[@]}"; do
  start_named_server "$role"
done
for role in "${SERVER_ROLES[@]}"; do
  wait_named_server "$role"
done

if [[ "$SCENARIO_SET" != "sm-q9" && "$NEED_SESSION_NODES" == "1" ]]; then
  wait_control_route "$SESSION_A_HTTP" play-a session-a-play-a
  wait_spot_route "$SESSION_A_HTTP" play-a session-a-play-a
    if [[ "$NEED_PLAY_B" == "1" && "$SCENARIO_SET" != "sm-g2" ]]; then
    wait_control_route "$SESSION_A_HTTP" play-b session-a-play-b
    wait_spot_route "$SESSION_A_HTTP" play-b session-a-play-b
  fi
  if [[ "$NEED_SESSION_B" == "1" ]]; then
    wait_control_route "$SESSION_B_HTTP" play-a session-b-play-a
    wait_spot_route "$SESSION_B_HTTP" play-a session-b-play-a
    wait_control_route "$SESSION_B_HTTP" play-b session-b-play-b
    wait_spot_route "$SESSION_B_HTTP" play-b session-b-play-b
  fi
fi

wait_for_log_after() {
  local name="$1" pattern="$2" first_line="$3" attempts="${4:-200}"
  for ((i = 0; i < attempts; i++)); do
    if tail -n +"$first_line" "$LOG_DIR/$name.log" 2>/dev/null | grep -q "$pattern"; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting for '$pattern' in $name after line $first_line" >&2
  return 1
}

wait_for_log_in_either_after() {
  local first_name="$1" second_name="$2" pattern="$3"
  local first_line="$4" second_line="$5" attempts="${6:-200}"
  for ((i = 0; i < attempts; i++)); do
    if tail -n +"$first_line" "$LOG_DIR/$first_name.log" 2>/dev/null | grep -q "$pattern" \
      || tail -n +"$second_line" "$LOG_DIR/$second_name.log" 2>/dev/null | grep -q "$pattern"; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting for '$pattern' in $first_name or $second_name" >&2
  return 1
}

crash_named_role() {
  local role="$1" wrapper_pid child_pid
  wrapper_pid="$(pid_for_role "$role")"
  child_pid="$(pgrep -P "$wrapper_pid" -n || true)"
  if [[ -z "$child_pid" ]]; then
    echo "$role dotnet child was not found under wrapper pid=$wrapper_pid" >&2
    return 1
  fi
  kill -KILL "$child_pid"
  for _ in $(seq 1 "$PROCESS_SHUTDOWN_ATTEMPTS"); do
    kill -0 "$wrapper_pid" 2>/dev/null || break
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  assert_expected_server_exit "$role" 137 "SIGKILL"
}

crash_play_a() {
  crash_named_role play-a
}

restart_named_role() {
  local role="$1" index new_pid last_index
  for index in "${!ORDERED_SERVER_ROLES[@]}"; do
    if [[ "${ORDERED_SERVER_ROLES[$index]}" == "$role" ]]; then
      rm -f "$LOG_DIR/$role.exit.log"
      start_named_server "$role"
      last_index=$((${#PIDS[@]} - 1))
      new_pid="${PIDS[$last_index]}"
      PIDS[$index]="$new_pid"
      unset "PIDS[$last_index]"
      wait_named_server "$role"
      return 0
    fi
  done
  echo "$role role index was not found for restart" >&2
  return 1
}

restart_play_a() {
  restart_named_role play-a
}

stop_play_a() {
  local pid
  pid="$(pid_for_role play-a)"
  curl --max-time "$HTTP_PROBE_TIMEOUT_SECONDS" \
    --connect-timeout "$HTTP_PROBE_TIMEOUT_SECONDS" \
    -fsS -X POST "$PLAY_A_HTTP/shutdown" >/dev/null
  for _ in $(seq 1 "$PROCESS_SHUTDOWN_ATTEMPTS"); do
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "play-a did not stop during lifecycle restart: pid=$pid" >&2
  return 1
}

run_client() {
  local operation_group="$1"
  assert_servers_alive "client ${operation_group}"
  echo "client operation_group=${operation_group}" >>"$LOG_DIR/client.stdout.log"
  local config="$CONFIG_DIR/client-$operation_group.json"
  python3 "$SCRIPT_DIR/../write_role_config.py" "$config" -- \
    --config-dir "$CONFIG_DIR" \
	    --gateway-url "$GATEWAY_HTTP" \
	    --play-a-url "$PLAY_A_HTTP" \
	    --play-b-url "$PLAY_B_HTTP" \
	    --multi-a-url "$MULTI_A_HTTP" \
	    --multi-b-url "$MULTI_B_HTTP" \
	    --session-a-url "$SESSION_A_HTTP" \
    --session-a-stream-endpoint "$SESSION_A_STREAM" \
    --session-a-tls-stream-endpoint "$SESSION_A_TLS_STREAM" \
    --session-b-stream-endpoint "$SESSION_B_STREAM" \
    --sm-c6-pause-ack-file "$LOG_DIR/sm-c6-paused" \
    --sm-c6-resume-ack-file "$LOG_DIR/sm-c6-resumed" \
    --play-a-transport-proxy-admin "$PLAY_A_TRANSPORT_PROXY_ADMIN" \
    --play-b-transport-proxy-admin "$PLAY_B_TRANSPORT_PROXY_ADMIN" \
    --session-a-transport-proxy-admin "$SESSION_A_TRANSPORT_PROXY_ADMIN" \
    --instance-owner-loss-crash-ack-file "$LOG_DIR/instance-owner-loss-crashed" \
    --instance-owner-loss-restart-ack-file "$LOG_DIR/instance-owner-loss-restarted" \
    --instance-creating-release-ack-file "$LOG_DIR/instance-initialization-release" \
    --operation-group "$operation_group"
  if [[ "$operation_group" == "instance-creating-join" ]]; then
    local first_line client_pid client_status marker spot initialize_count first_count follow_up_count
    first_line=$(($(wc -l <"$LOG_DIR/client.stdout.log") + 1))
    timeout "${CLIENT_PROCESS_TIMEOUT_SECONDS}s" dotnet "$CLIENT_DLL" --config "$config" \
      2> >(tee -a "$LOG_DIR/client.stderr.log" >&2) \
      | tee -a "$LOG_DIR/client.stdout.log" &
    client_pid=$!
    wait_for_log_after client.stdout "instance-creating-join release-ready" "$first_line" 300
    marker="$(tail -n +"$first_line" "$LOG_DIR/client.stdout.log" \
      | grep "instance-creating-join release-ready" | tail -1)"
    spot="$(sed -n 's/.*|spot=\([^|]*\).*/\1/p' <<<"$marker")"
    [[ -n "$spot" ]] || {
      echo "Instance creating marker has no spot id: $marker" >&2
      wait "$client_pid" || true
      return 1
    }
    touch "$LOG_DIR/instance-initialization-release"
    set +e
    wait "$client_pid"
    client_status=$?
    set -e
    [[ "$client_status" -eq 0 ]] || return "$client_status"
    initialize_count="$(grep -h "instance-initialize.*spot=$spot" \
      "$LOG_DIR/play-a.evidence.log" "$LOG_DIR/play-b.evidence.log" \
      | grep -v "instance-initialize-gate" | wc -l)"
    first_count="$(grep -h "instance-request.*spot=$spot.*operation=creating-first" \
      "$LOG_DIR/play-a.evidence.log" "$LOG_DIR/play-b.evidence.log" | wc -l)"
    follow_up_count="$(grep -h "instance-request.*spot=$spot.*operation=creating-follow-up" \
      "$LOG_DIR/play-a.evidence.log" "$LOG_DIR/play-b.evidence.log" | wc -l)"
    [[ "$initialize_count" -eq 1 && "$first_count" -eq 1 && "$follow_up_count" -eq 1 ]] || {
      echo "Creating join evidence mismatch: initialize=$initialize_count first=$first_count follow_up=$follow_up_count" >&2
      return 1
    }
  elif [[ "$operation_group" == "instance-queue-owner-loss" ]]; then
    local first_line client_pid client_status marker owner spot initialize_count
    first_line=$(($(wc -l <"$LOG_DIR/client.stdout.log") + 1))
    timeout "${CLIENT_PROCESS_TIMEOUT_SECONDS}s" dotnet "$CLIENT_DLL" --config "$config" \
      2> >(tee -a "$LOG_DIR/client.stderr.log" >&2) \
      | tee -a "$LOG_DIR/client.stdout.log" &
    client_pid=$!
    wait_for_log_after client.stdout "instance-queue-owner-loss crash-ready" "$first_line" 300
    marker="$(tail -n +"$first_line" "$LOG_DIR/client.stdout.log" \
      | grep "instance-queue-owner-loss crash-ready" | tail -1)"
    owner="$(sed -n 's/.*|owner=\([^|]*\).*/\1/p' <<<"$marker")"
    spot="$(sed -n 's/.*|spot=\([^|]*\).*/\1/p' <<<"$marker")"
    [[ "$owner" == "play-a" || "$owner" == "play-b" ]] || {
      echo "Instance queue owner-loss marker has invalid owner: $marker" >&2
      wait "$client_pid" || true
      return 1
    }
    crash_named_role "$owner"
    touch "$LOG_DIR/instance-owner-loss-crashed"
    wait_for_log_after client.stdout "instance-queue-owner-loss restart-ready" "$first_line" 300
    restart_named_role "$owner"
    touch "$LOG_DIR/instance-owner-loss-restarted"
    set +e
    wait "$client_pid"
    client_status=$?
    set -e
    [[ "$client_status" -eq 0 ]] || return "$client_status"
    if grep -h -E "operation=(queued-first|queued-follow-up|after-owner-restart)" \
      "$LOG_DIR/play-a.evidence.log" "$LOG_DIR/play-b.evidence.log" | grep -q .; then
      echo "IS-E2E-35 replay or post-restart handler evidence was created" >&2
      return 1
    fi
    initialize_count="$(grep -h "instance-initialize.*spot=$spot" \
      "$LOG_DIR/play-a.evidence.log" "$LOG_DIR/play-b.evidence.log" | wc -l)"
    [[ "$initialize_count" -eq 1 ]] || {
      echo "IS-E2E-35 expected one factory initialization, got $initialize_count" >&2
      return 1
    }
  elif [[ "$operation_group" == "instance-owner-loss" ]]; then
    local first_line client_pid client_status marker owner spot initialize_count
    first_line=$(($(wc -l <"$LOG_DIR/client.stdout.log") + 1))
    timeout "${CLIENT_PROCESS_TIMEOUT_SECONDS}s" dotnet "$CLIENT_DLL" --config "$config" \
      2> >(tee -a "$LOG_DIR/client.stderr.log" >&2) \
      | tee -a "$LOG_DIR/client.stdout.log" &
    client_pid=$!
    wait_for_log_after client.stdout "instance-owner-loss crash-ready" "$first_line" 300
    marker="$(tail -n +"$first_line" "$LOG_DIR/client.stdout.log" \
      | grep "instance-owner-loss crash-ready" | tail -1)"
    owner="$(sed -n 's/.*|owner=\([^|]*\).*/\1/p' <<<"$marker")"
    spot="$(sed -n 's/.*|spot=\([^|]*\).*/\1/p' <<<"$marker")"
    [[ "$owner" == "play-a" || "$owner" == "play-b" ]] || {
      echo "Instance owner-loss marker has invalid owner: $marker" >&2
      wait "$client_pid" || true
      return 1
    }
    [[ -n "$spot" ]] || {
      echo "Instance owner-loss marker has no spot id: $marker" >&2
      wait "$client_pid" || true
      return 1
    }
    crash_named_role "$owner"
    touch "$LOG_DIR/instance-owner-loss-crashed"
    set +e
    wait "$client_pid"
    client_status=$?
    set -e
    [[ "$client_status" -eq 0 ]] || return "$client_status"
    if grep -h "operation=after-ready-crash" \
      "$LOG_DIR/play-a.evidence.log" "$LOG_DIR/play-b.evidence.log" | grep -q .; then
      echo "IS-E2E-05 post-crash handler evidence was created" >&2
      return 1
    fi
    initialize_count="$(grep -h "instance-initialize.*spot=$spot" \
      "$LOG_DIR/play-a.evidence.log" "$LOG_DIR/play-b.evidence.log" | wc -l)"
    [[ "$initialize_count" -eq 1 ]] || {
      echo "IS-E2E-05 expected one factory initialization, got $initialize_count" >&2
      return 1
    }
  elif [[ "$operation_group" == "sm-g1" ]]; then
    local first_line client_pid client_status next_line
    first_line=$(($(wc -l <"$LOG_DIR/client.stdout.log") + 1))
    timeout "${CLIENT_PROCESS_TIMEOUT_SECONDS}s" dotnet "$CLIENT_DLL" --config "$config" \
      2> >(tee -a "$LOG_DIR/client.stderr.log" >&2) \
      | tee -a "$LOG_DIR/client.stdout.log" &
    client_pid=$!
    wait_for_log_after client.stdout "spot-service sm-g1 crash-1-ready" "$first_line" 300
    crash_play_a
    next_line=$(($(wc -l <"$LOG_DIR/client.stdout.log") + 1))
    wait_for_log_after client.stdout "spot-service sm-g1 restart-1-ready" "$first_line" 300
    restart_play_a
    wait_control_route "$SESSION_A_HTTP" play-a session-a-play-a-restarted 15
    wait_spot_route "$SESSION_A_HTTP" play-a session-a-play-a-restarted 15
    if ! wait_for_log_after client.stdout "spot-service sm-g1 crash-2-ready" "$next_line" 300; then
      curl -fsS "$PLAY_A_HTTP/mesh-snapshot" || true
      wait "$client_pid" || true
      return 1
    fi
    crash_play_a
    set +e
    wait "$client_pid"
    client_status=$?
    set -e
    [[ "$client_status" -eq 0 ]] || return "$client_status"
  elif [[ "$operation_group" == "sm-g2" ]]; then
    local first_line client_pid client_status
    first_line=$(($(wc -l <"$LOG_DIR/client.stdout.log") + 1))
    timeout "${CLIENT_PROCESS_TIMEOUT_SECONDS}s" dotnet "$CLIENT_DLL" --config "$config" \
      2> >(tee -a "$LOG_DIR/client.stderr.log" >&2) \
      | tee -a "$LOG_DIR/client.stdout.log" &
    client_pid=$!
    if ! wait_for_log_after client.stdout "spot-service sm-g2 scale-out-ready" "$first_line" 200; then
      wait "$client_pid" || true
      return 1
    fi
    SERVER_ROLES+=(play-b)
    ORDERED_SERVER_ROLES+=(play-b)
    start_named_server play-b
    wait_named_server play-b
    set +e
    wait "$client_pid"
    client_status=$?
    set -e
    [[ "$client_status" -eq 0 ]] || return "$client_status"
  elif [[ "$operation_group" == *"d13"* ]]; then
    local first_line play_a_evidence_first_line play_b_evidence_first_line
    local client_pid session_pid session_pgid client_status
    first_line=$(($(wc -l <"$LOG_DIR/client.stdout.log") + 1))
    play_a_evidence_first_line=$(($(wc -l <"$LOG_DIR/play-a.evidence.log") + 1))
    play_b_evidence_first_line=$(($(wc -l <"$LOG_DIR/play-b.evidence.log") + 1))
    timeout "${CLIENT_PROCESS_TIMEOUT_SECONDS}s" dotnet "$CLIENT_DLL" --config "$config" \
      2> >(tee -a "$LOG_DIR/client.stderr.log" >&2) \
      | tee -a "$LOG_DIR/client.stdout.log" &
    client_pid=$!
    if ! wait_for_log_after client.stdout "spot-service sm-d13 heartbeat-stop armed" "$first_line" 600; then
      wait "$client_pid" || true
      return 1
    fi
    session_pid="$(pid_for_role session-a)"
    session_pgid="$(ps -o pgid= -p "$session_pid" | tr -d ' ')"
    if [[ "$session_pgid" != "$session_pid" ]]; then
      echo "session-a process group mismatch: pid=$session_pid pgid=$session_pgid" >&2
      wait "$client_pid" || true
      return 1
    fi
    kill -STOP -- "-$session_pid"
    set +e
    wait "$client_pid"
    client_status=$?
    set -e
    kill -CONT -- "-$session_pid" 2>/dev/null || true
    [[ "$client_status" -eq 0 ]] || return "$client_status"
    # GetOrCreate is coordinated through play-a, but the Location Store may
    # place the Actor on either eligible Object Server. Verify the callback at
    # the committed owner instead of treating the coordinator as the owner.
    wait_for_log_in_either_after \
      play-a.evidence play-b.evidence \
      "entry-disconnected|rid=play-[ab]|actor=actor-sm-d13" \
      "$play_a_evidence_first_line" "$play_b_evidence_first_line" 600
  elif [[ "$operation_group" == "sm-c6" ]]; then
    local first_line client_pid client_status
    first_line=$(($(wc -l <"$LOG_DIR/client.stdout.log") + 1))
    timeout "${CLIENT_PROCESS_TIMEOUT_SECONDS}s" dotnet "$CLIENT_DLL" --config "$config" \
      2> >(tee -a "$LOG_DIR/client.stderr.log" >&2) \
      | tee -a "$LOG_DIR/client.stdout.log" &
    client_pid=$!
    if ! wait_for_log_after client.stdout "spot-service sm-c6 pause-play-b-ready" "$first_line" 300; then
      wait "$client_pid" || true
      return 1
    fi
    touch "$LOG_DIR/sm-c6-paused"
    if ! wait_for_log_after client.stdout \
      "spot-service sm-c6 resume-play-b-ready" "$first_line" 1200; then
      wait "$client_pid" || true
      return 1
    fi
    touch "$LOG_DIR/sm-c6-resumed"
    touch "$LOG_DIR/sm-c6-play-b-gate"
    set +e
    wait "$client_pid"
    client_status=$?
    set -e
    [[ "$client_status" -eq 0 ]] || return "$client_status"
  elif [[ "$operation_group" == "sm-a9" || "$operation_group" == "sm-b11" ]]; then
    local first_line client_pid client_status ready_pattern gate_file
    first_line=$(($(wc -l <"$LOG_DIR/client.stdout.log") + 1))
    timeout "${CLIENT_PROCESS_TIMEOUT_SECONDS}s" dotnet "$CLIENT_DLL" --config "$config" \
      2> >(tee -a "$LOG_DIR/client.stderr.log" >&2) \
      | tee -a "$LOG_DIR/client.stdout.log" &
    client_pid=$!
    if [[ "$operation_group" == "sm-a9" ]]; then
      ready_pattern="spot-service sm-a9 release-play-a-ready"
      gate_file="$LOG_DIR/sm-a9-play-a-gate"
    else
      ready_pattern="spot-service sm-b11 release-play-a-ready"
      gate_file="$LOG_DIR/sm-b11-play-a-gate"
    fi
    if ! wait_for_log_after client.stdout "$ready_pattern" "$first_line" 300; then
      wait "$client_pid" || true
      return 1
    fi
    touch "$gate_file"
    set +e
    wait "$client_pid"
    client_status=$?
    set -e
    [[ "$client_status" -eq 0 ]] || return "$client_status"
  elif [[ "$operation_group" == "sm-a10" ]]; then
    local first_line client_pid client_status
    first_line=$(($(wc -l <"$LOG_DIR/client.stdout.log") + 1))
    timeout "${CLIENT_PROCESS_TIMEOUT_SECONDS}s" dotnet "$CLIENT_DLL" --config "$config" \
      2> >(tee -a "$LOG_DIR/client.stderr.log" >&2) \
      | tee -a "$LOG_DIR/client.stdout.log" &
    client_pid=$!
    if ! wait_for_log_after client.stdout "spot-service sm-a10 restart-play-a-ready" "$first_line" 300; then
      wait "$client_pid" || true
      return 1
    fi
    stop_play_a
    restart_play_a
    set +e
    wait "$client_pid"
    client_status=$?
    set -e
    [[ "$client_status" -eq 0 ]] || return "$client_status"
  else
    timeout "${CLIENT_PROCESS_TIMEOUT_SECONDS}s" dotnet "$CLIENT_DLL" --config "$config" \
      2> >(tee -a "$LOG_DIR/client.stderr.log" >&2) \
      | tee -a "$LOG_DIR/client.stdout.log"
  fi
  if [[ "$operation_group" == "sm-g1" || "$operation_group" == "instance-owner-loss" \
    || "$operation_group" == "instance-queue-owner-loss" ]]; then
    : # Both expected SIGKILL exits are asserted at their deterministic gates.
  else
    assert_servers_alive "client ${operation_group} completion"
  fi
}

run_client_list() {
  local item
  IFS=',' read -ra items <<<"$SCENARIO_SET"
  for item in "${items[@]}"; do
    run_client "$item"
  done
}

if [[ "$SCENARIO_SET" == "track-g" ]]; then
  run_client sm-g2
  run_client sm-g3
  run_client sm-g4
  run_client sm-g5a
  run_client sm-g5b
  run_client sm-g1
elif [[ "$SCENARIO_SET" == "all" || "$SCENARIO_SET" == "default-batch" ]]; then
  run_client sm-b1-b2-b3
  run_client sm-b0
  run_client sm-b6
  run_client sm-b8
  run_client sm-b9
  run_client sm-d2-d6
  run_client sm-d3
  run_client sm-d4
  run_client sm-d4a
  run_client sm-d4b
  run_client sm-d5
  run_client sm-d5a
  run_client sm-d7
  run_client sm-d8
  run_client sm-d9-d11-d13
  run_client sm-d10
  run_client sm-d12
  run_client sm-d14
  run_client sm-d15
  run_client sm-c1-c2
  run_client sm-c3
  run_client sm-c5
  run_client sm-c6
  run_client sm-f3-f5
  run_client sm-e4
  run_client sm-e1-f4
  run_client sm-e2-e3
  run_client sm-a7-a8-c4
  run_client sm-a11
  run_client sm-a12-a13
  run_client sm-a3-a6-b4-b7
  run_client sm-a5
  run_client sm-a1-a2-a4-f1-f2
  if [[ "$SCENARIO_SET" == "all" ]]; then
    run_client sm-f6
    run_client sm-q9
    run_client sm-g2
    run_client sm-g3
    run_client sm-g4
    run_client sm-g5a
    run_client sm-g5b
    run_client sm-g1
  fi
else
  run_client_list
fi
