#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
DELAY_PROJECT="$SCRIPT_DIR/Server/Delay/AutomaticTurnDispatch.Delay.csproj"
EXTERNAL_API_PROJECT="$SCRIPT_DIR/Server/ExternalApi/AutomaticTurnDispatch.ExternalApi.csproj"
PLAY_PROJECT="$SCRIPT_DIR/Server/Play/AutomaticTurnDispatch.Play.csproj"
SESSION_PROJECT="$SCRIPT_DIR/Server/Session/AutomaticTurnDispatch.Session.csproj"
CLIENT_PROJECT="$SCRIPT_DIR/Client/AutomaticTurnDispatch.Client.csproj"
DELAY_DLL="$SCRIPT_DIR/Server/Delay/bin/Debug/net8.0/AutomaticTurnDispatch.Delay.dll"
EXTERNAL_API_DLL="$SCRIPT_DIR/Server/ExternalApi/bin/Debug/net8.0/AutomaticTurnDispatch.ExternalApi.dll"
PLAY_DLL="$SCRIPT_DIR/Server/Play/bin/Debug/net8.0/AutomaticTurnDispatch.Play.dll"
SESSION_DLL="$SCRIPT_DIR/Server/Session/bin/Debug/net8.0/AutomaticTurnDispatch.Session.dll"
CLIENT_DLL="$SCRIPT_DIR/Client/bin/Debug/net8.0/AutomaticTurnDispatch.Client.dll"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$STAMP"
SKIP_BUILD=0
SCENARIO_ARGS=()
CANONICAL_SCENARIOS=(
  TD-A1 TD-A2 TD-A3 TD-A4 TD-A5
  TD-B1 TD-B2 TD-B3 TD-B4
  TD-C1 TD-C2 TD-C3 TD-C4 TD-C5
  TD-D1 TD-D2 TD-D3 TD-D4 TD-D5 TD-D6
  TD-E1 TD-E2 TD-E3 TD-E2A
  TD-F1 TD-F2 TD-F3 TD-F4 TD-F5 TD-F6
  TD-G1 TD-F5A
)
CLIENT_CANONICAL_SCENARIOS=(
  TD-A1 TD-A2 TD-A3 TD-A4 TD-A5
  TD-B1 TD-B2 TD-B3 TD-B4
  TD-C1 TD-C2 TD-C3 TD-C4 TD-C5
  TD-D1 TD-D2 TD-D3 TD-D4 TD-D5 TD-D6
  TD-E1 TD-E2 TD-E3 TD-E2A
  TD-F1 TD-F2 TD-F3 TD-F4 TD-F5 TD-F6
  TD-G1
)
while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --)
      shift
      SCENARIO_ARGS+=("$@")
      break
      ;;
    *)
      SCENARIO_ARGS+=("$1")
      shift
      ;;
  esac
done
if [[ "${#SCENARIO_ARGS[@]}" -eq 0 ]]; then
  SCENARIO="all"
else
  SCENARIO="${SCENARIO_ARGS[*]}"
  SCENARIO="${SCENARIO// /,}"
fi

RUN_CLIENT=0
RUN_SHUTDOWN=0
CLIENT_ALL=0
CLIENT_SCENARIO_IDS=()
declare -A CLIENT_SCENARIO_SEEN=()
IFS=',' read -ra REQUESTED_SELECTORS <<<"$SCENARIO"
for selector in "${REQUESTED_SELECTORS[@]}"; do
  case "$selector" in
    all)
      RUN_CLIENT=1
      RUN_SHUTDOWN=1
      CLIENT_ALL=1
      ;;
    full)
      RUN_CLIENT=1
      CLIENT_ALL=1
      ;;
    shutdown|shutdown-wait|shutdown-recovery)
      RUN_SHUTDOWN=1
      ;;
    TD-A[1-5]|TD-B[1-4]|TD-C[1-5]|TD-D[1-6]|TD-E[1-3]|TD-E2A|TD-F[1-6]|TD-G1)
      RUN_CLIENT=1
      if [[ -z "${CLIENT_SCENARIO_SEEN[$selector]+x}" ]]; then
        CLIENT_SCENARIO_IDS+=("$selector")
        CLIENT_SCENARIO_SEEN["$selector"]=1
      fi
      ;;
    TD-F5A)
      RUN_SHUTDOWN=1
      ;;
    *)
      echo "Unknown scenario selector: $selector" >&2
      exit 64
      ;;
  esac
done
if [[ "$RUN_CLIENT" == "1" ]]; then
  if [[ "$CLIENT_ALL" == "1" ]]; then
    CLIENT_SCENARIO_IDS=("${CLIENT_CANONICAL_SCENARIOS[@]}")
    CLIENT_SCENARIO="full"
  else
    CLIENT_SCENARIO="$(IFS=,; echo "${CLIENT_SCENARIO_IDS[*]}")"
  fi
  EXPECTED_CLIENT_SCENARIO_COUNT="${#CLIENT_SCENARIO_IDS[@]}"
fi

mkdir -p "$LOG_DIR"
CONFIG_DIR="$(mktemp -d)"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
REDIS_READINESS_TIMEOUT_SECONDS=60
HTTP_PROBE_TIMEOUT_SECONDS=3
PROCESS_CLEANUP_TIMEOUT_SECONDS=35
TURN_SHUTDOWN_TIMEOUT_SECONDS=60
SCENARIO_MARKER_TIMEOUT_SECONDS=30
SHUTDOWN_CLIENT_MARKER_TIMEOUT_SECONDS=90
LOCAL_READINESS_ATTEMPTS="$(
  python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"
PROCESS_CLEANUP_ATTEMPTS="$(
  python3 - "$PROCESS_CLEANUP_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"
TURN_SHUTDOWN_ATTEMPTS="$(
  python3 - "$TURN_SHUTDOWN_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
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
SHUTDOWN_CLIENT_MARKER_ATTEMPTS="$(
  python3 - "$SHUTDOWN_CLIENT_MARKER_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"

scenario_selected() {
  local expected="$1"
  [[ "$expected" == "full" && "$RUN_CLIENT" == "1" ]] && return 0
  [[ "$expected" == "shutdown" && "$RUN_SHUTDOWN" == "1" ]] && return 0
  return 1
}

build_projects() {
  dotnet build "$DELAY_PROJECT" --maxcpucount:1 >/dev/null
  dotnet build "$EXTERNAL_API_PROJECT" --maxcpucount:1 >/dev/null
  dotnet build "$PLAY_PROJECT" --maxcpucount:1 >/dev/null
  dotnet build "$SESSION_PROJECT" --maxcpucount:1 >/dev/null
  dotnet build "$CLIENT_PROJECT" --maxcpucount:1 >/dev/null
}

# Portable ripgrep shim: this environment may not have rg installed.
rg() {
  local args=() pattern="" path_args=() quiet=0 invert=0
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -n) shift ;;
      -q) quiet=1; shift ;;
      -v) invert=1; pattern="$2"; shift 2 ;;
      -g) shift 2 ;;
      *) if [[ -z "$pattern" && $invert -eq 0 ]]; then pattern="$1"; else path_args+=("$1"); fi; shift ;;
    esac
  done
  local grep_args=(-rEn --include='*.cs')
  [[ $quiet -eq 1 ]] && grep_args=(-rEq --include='*.cs')
  [[ $invert -eq 1 ]] && { command grep -Ev "$pattern"; return $?; }
  if [[ ${#path_args[@]} -eq 0 ]]; then
    command grep -E "$pattern"
  else
    command grep "${grep_args[@]}" -- "$pattern" "${path_args[@]}"
  fi
}

static_checks() {
  if rg -n 'new HttpClient|ZLinkHttpClient\.Create' "$SCRIPT_DIR/Server/Play" -g '*.cs' >/tmp/zlink-automatic-turn-dispatch-static-http.$$; then
    cat /tmp/zlink-automatic-turn-dispatch-static-http.$$ >&2
    rm -f /tmp/zlink-automatic-turn-dispatch-static-http.$$
    echo "AutomaticTurnDispatch Play handlers must receive the framework HTTP client through DI." >&2
    return 1
  fi
  rm -f /tmp/zlink-automatic-turn-dispatch-static-http.$$

  if ! rg -q 'AddZLinkHttpClient\("external-api"' "$SCRIPT_DIR/Server/Play/PlayHostFactory.cs"; then
    echo "AutomaticTurnDispatch Play must register the external API HTTP client by name." >&2
    return 1
  fi

  if ! rg -q '\.Async(<|\()' "$SCRIPT_DIR/Server/Play" || ! rg -q '\.Yield(<|\()' "$SCRIPT_DIR/Server/Play"; then
    echo "AutomaticTurnDispatch must exercise both Async and Yield terminators." >&2
    return 1
  fi

  if rg -n 'AwaitConnectorFactory|AutomaticTurnDispatchScenarioContext|WaitForPlayEvidenceAsync|ReadPlayEvidenceAsync' "$SCRIPT_DIR/Client" -g '*.cs' >/tmp/zlink-automatic-turn-dispatch-static-helper.$$; then
    cat /tmp/zlink-automatic-turn-dispatch-static-helper.$$ >&2
    rm -f /tmp/zlink-automatic-turn-dispatch-static-helper.$$
    echo "AutomaticTurnDispatch client scenarios must use the stream connector directly instead of thin helpers." >&2
    return 1
  fi
  rm -f /tmp/zlink-automatic-turn-dispatch-static-helper.$$

  if ! rg -q 'ZlinkStreamConnectorFactory\.Create' "$SCRIPT_DIR/Client/Program.cs"; then
    echo "AutomaticTurnDispatch full scenario must create and use a real stream connector directly." >&2
    return 1
  fi

  local scenario_count
  scenario_count="$(find "$SCRIPT_DIR/Client/Scenarios" -maxdepth 1 -name 'Td*Scenario.cs' | wc -l)"
  if [[ "$scenario_count" != "32" ]]; then
    echo "AutomaticTurnDispatch must expose exactly 32 canonical TD scenario files; actual=$scenario_count." >&2
    return 1
  fi

  if ! rg -q 'ZlinkStreamConnectorFactory\.Create' "$SCRIPT_DIR/Client/Scenarios/ShutdownAwaitProbe.cs"; then
    echo "AutomaticTurnDispatch shutdown scenario must create and use a real stream connector directly." >&2
    return 1
  fi

  echo "TD-C5 source-gate=passed blocking-io-in-cpu-worker=absent"
}

PIDS=()
cleanup() {
  set +e
  rm -rf "$CONFIG_DIR"
  if [[ -n "${REDIS_CONTAINER:-}" ]]; then
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  fi
  for pid in "${PIDS[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -INT "-$pid" 2>/dev/null || kill -INT "$pid" 2>/dev/null || true
    fi
  done
  for _ in $(seq 1 "$PROCESS_CLEANUP_ATTEMPTS"); do
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
  pkill -TERM -f "$LOG_DIR" 2>/dev/null || true
  sleep "$LOCAL_READINESS_POLL_SECONDS"
  pkill -KILL -f "$LOG_DIR" 2>/dev/null || true
}
trap cleanup EXIT
trap 'cleanup; exit 143' TERM INT

allocate_ports() {
  local count="$1"
  python3 - "$count" <<'PY'
import random
import socket
import sys

count = int(sys.argv[1])
sockets = []
try:
    chosen = set()
    while len(sockets) < count:
        port = random.randint(20000, 32767)
        if port in chosen:
            continue
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.bind(("127.0.0.1", port))
        except OSError:
            sock.close()
            continue
        chosen.add(port)
        sockets.append(sock)
    print(" ".join(str(sock.getsockname()[1]) for sock in sockets))
finally:
    for sock in sockets:
        sock.close()
PY
}

endpoint_port() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  endpoint="${endpoint#http://}"
  echo "${endpoint##*:}"
}

endpoint_host() {
  local endpoint="$1"
  endpoint="${endpoint#tcp://}"
  endpoint="${endpoint#http://}"
  echo "${endpoint%:*}"
}

wait_port() {
  local name="$1"
  local endpoint="$2"
  local pid="${PIDS[-1]:-}"
  local host
  local port
  host="$(endpoint_host "$endpoint")"
  port="$(endpoint_port "$endpoint")"
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    if (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
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
  local pid="${PIDS[-1]:-}"
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

wait_route_ready() {
  local url="$1"
  local mesh_name="$2"
  local rid="$3"
  local name="$4"
  local deadline_ns
  deadline_ns="$(python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" <<'PY'
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
      -fsS --get \
      --data-urlencode "meshName=$mesh_name" \
      --data-urlencode "rid=$rid" \
      "$url/topology/ready" 2>/dev/null | grep -Fq '"ready":true'; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name route readiness" >&2
  return 1
}

start_server() {
  local name="$1"
  local dll="$2"
  shift 2
  local config="$CONFIG_DIR/${name}.json"
  python3 "$SCRIPT_DIR/../write_role_config.py" "$config" -- "$@"
  setsid dotnet "$dll" --config "$config" >"$LOG_DIR/${name}.stdout.log" 2>"$LOG_DIR/${name}.stderr.log" &
  PIDS+=("$!")
}

terminate_gracefully() {
  local name="$1"
  local pid="$2"
  if ! kill -0 "$pid" 2>/dev/null; then
    return 0
  fi
  kill -TERM "-$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
  for _ in $(seq 1 "$TURN_SHUTDOWN_ATTEMPTS"); do
    local state
    state="$(ps -o stat= -p "$pid" 2>/dev/null || true)"
    if [[ -z "$state" || "$state" == Z* ]]; then
      wait "$pid" 2>/dev/null || true
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "${name} did not stop within ${TURN_SHUTDOWN_TIMEOUT_SECONDS}s after SIGTERM while await was pending" >&2
  return 1
}

request_shutdown() {
  local pid="$1"
  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM "-$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
  fi
}

reap_or_kill_after_shutdown() {
  local name="$1"
  local pid="$2"
  for _ in $(seq 1 "$PROCESS_CLEANUP_ATTEMPTS"); do
    local state
    state="$(ps -o stat= -p "$pid" 2>/dev/null || true)"
    if [[ -z "$state" || "$state" == Z* ]]; then
      wait "$pid" 2>/dev/null || true
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "${name} did not stop after the client observed shutdown; sending SIGKILL" >&2
  kill -KILL "-$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

wait_file_contains() {
  local file="$1"
  local pattern="$2"
  local failure="$3"
  local pid="${4:-}"
  local attempts="${5:-$SCENARIO_MARKER_ATTEMPTS}"
  for _ in $(seq 1 "$attempts"); do
    if [[ -f "$file" ]] && grep -F "$pattern" "$file" >/dev/null 2>&1; then
      return 0
    fi
    if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
      echo "$failure" >&2
      echo "client exited before marker: $pattern" >&2
      return 1
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "$failure" >&2
  echo "missing marker: $pattern" >&2
  return 1
}

wait_process_exit() {
  local name="$1"
  local pid="$2"
  for _ in $(seq 1 "$TURN_SHUTDOWN_ATTEMPTS"); do
    if [[ -r "/proc/$pid/stat" ]]; then
      local state
      state="$(awk '{print $3}' "/proc/$pid/stat")"
      if [[ "$state" == "Z" ]]; then
        wait "$pid"
        return $?
      fi
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid"
      return $?
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "${name} did not exit within ${TURN_SHUTDOWN_TIMEOUT_SECONDS}s after the peer shutdown closed the pending await request" >&2
  return 1
}

echo "log_dir=$LOG_DIR"
if [[ "$SKIP_BUILD" != "1" ]]; then
  build_projects
fi
static_checks

# The run owns its Redis: a dedicated, throwaway container is the shared
# location store every server registers into (no registry process exists).
if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run the AutomaticTurnDispatch E2E (it provisions a dedicated Redis container)." >&2
  exit 1
fi
zlink_redis_start_scoped_assign \
  REDIS_CONTAINER \
  REDIS_ENDPOINT \
  "zlink-redis-dotnet-e2e-automatic-turn-dispatch" \
  "redis:7.2-alpine" \
  "$LOG_DIR"
zlink_redis_wait_ready "$REDIS_CONTAINER" "$REDIS_READINESS_TIMEOUT_SECONDS"
REDIS_KEY_PREFIX="awaitdispatch-e2e:$$:"

read -r EXTERNAL_API_HTTP_PORT <<<"$(allocate_ports 1)"
EXTERNAL_API_HTTP="http://127.0.0.1:${EXTERNAL_API_HTTP_PORT}"
start_server external-api "$EXTERNAL_API_DLL" \
  --http-url "$EXTERNAL_API_HTTP"
wait_health external-api "$EXTERNAL_API_HTTP"

read -r DELAY_A_HTTP_PORT DELAY_A_ENDPOINT_PORT <<<"$(allocate_ports 2)"
DELAY_A_HTTP="http://127.0.0.1:${DELAY_A_HTTP_PORT}"
DELAY_A_ENDPOINT="tcp://127.0.0.1:${DELAY_A_ENDPOINT_PORT}"
start_server delay-a "$DELAY_DLL" \
  --rid delay-a \
  --http-url "$DELAY_A_HTTP" \
  --delay-endpoint "$DELAY_A_ENDPOINT" \
  --external-api-base-url "$EXTERNAL_API_HTTP" \
  --log-dir "$LOG_DIR"
wait_health delay-a "$DELAY_A_HTTP"
wait_port delay-a-channel "$DELAY_A_ENDPOINT"

read -r DELAY_B_HTTP_PORT DELAY_B_ENDPOINT_PORT <<<"$(allocate_ports 2)"
DELAY_B_HTTP="http://127.0.0.1:${DELAY_B_HTTP_PORT}"
DELAY_B_ENDPOINT="tcp://127.0.0.1:${DELAY_B_ENDPOINT_PORT}"
start_server delay-b "$DELAY_DLL" \
  --rid delay-b \
  --http-url "$DELAY_B_HTTP" \
  --delay-endpoint "$DELAY_B_ENDPOINT" \
  --external-api-base-url "$EXTERNAL_API_HTTP" \
  --log-dir "$LOG_DIR"
wait_health delay-b "$DELAY_B_HTTP"
wait_port delay-b-channel "$DELAY_B_ENDPOINT"

read -r PLAY_A_HTTP_PORT PLAY_A_SPOT_ROUTER_PORT PLAY_A_SPOT_PUB_PORT PLAY_A_SPOT_ROUTE_PORT PLAY_A_CONTROL_PORT <<<"$(allocate_ports 5)"
PLAY_A_HTTP="http://127.0.0.1:${PLAY_A_HTTP_PORT}"
PLAY_A_SPOT_ROUTER="tcp://127.0.0.1:${PLAY_A_SPOT_ROUTER_PORT}"
PLAY_A_SPOT_PUB="tcp://127.0.0.1:${PLAY_A_SPOT_PUB_PORT}"
PLAY_A_SPOT_ROUTE="tcp://127.0.0.1:${PLAY_A_SPOT_ROUTE_PORT}"
PLAY_A_CONTROL="tcp://127.0.0.1:${PLAY_A_CONTROL_PORT}"
PLAY_A_ARGS=(
  --rid play-a
  --http-url "$PLAY_A_HTTP"
  --redis-endpoint "$REDIS_ENDPOINT"
  --redis-key-prefix "$REDIS_KEY_PREFIX"
  --control-endpoint "$PLAY_A_CONTROL"
  --delay-endpoint "$DELAY_A_ENDPOINT"
  --external-api-base-url "$EXTERNAL_API_HTTP"
  --spot-router-endpoint "$PLAY_A_SPOT_ROUTER"
  --spot-pub-endpoint "$PLAY_A_SPOT_PUB"
  --spot-route-endpoint "$PLAY_A_SPOT_ROUTE"
  --log-dir "$LOG_DIR"
)
if [[ "$RUN_SHUTDOWN" == "1" ]]; then
  PLAY_A_ARGS+=(--placement-weight 100)
fi
start_server play-a "$PLAY_DLL" "${PLAY_A_ARGS[@]}"
PLAY_A_PID="${PIDS[-1]}"
wait_health play-a "$PLAY_A_HTTP"
wait_port play-a-control "$PLAY_A_CONTROL"
wait_port play-a-spot-router "$PLAY_A_SPOT_ROUTER"
wait_port play-a-spot-route "$PLAY_A_SPOT_ROUTE"

read -r PLAY_B_HTTP_PORT PLAY_B_SPOT_ROUTER_PORT PLAY_B_SPOT_PUB_PORT PLAY_B_SPOT_ROUTE_PORT PLAY_B_CONTROL_PORT <<<"$(allocate_ports 5)"
PLAY_B_HTTP="http://127.0.0.1:${PLAY_B_HTTP_PORT}"
PLAY_B_SPOT_ROUTER="tcp://127.0.0.1:${PLAY_B_SPOT_ROUTER_PORT}"
PLAY_B_SPOT_PUB="tcp://127.0.0.1:${PLAY_B_SPOT_PUB_PORT}"
PLAY_B_SPOT_ROUTE="tcp://127.0.0.1:${PLAY_B_SPOT_ROUTE_PORT}"
PLAY_B_CONTROL="tcp://127.0.0.1:${PLAY_B_CONTROL_PORT}"
PLAY_B_ARGS=(
  --rid play-b
  --http-url "$PLAY_B_HTTP"
  --redis-endpoint "$REDIS_ENDPOINT"
  --redis-key-prefix "$REDIS_KEY_PREFIX"
  --control-endpoint "$PLAY_B_CONTROL"
  --delay-endpoint "$DELAY_B_ENDPOINT"
  --external-api-base-url "$EXTERNAL_API_HTTP"
  --spot-router-endpoint "$PLAY_B_SPOT_ROUTER"
  --spot-pub-endpoint "$PLAY_B_SPOT_PUB"
  --spot-route-endpoint "$PLAY_B_SPOT_ROUTE"
  --log-dir "$LOG_DIR"
)
if [[ "$RUN_SHUTDOWN" == "1" ]]; then
  PLAY_B_ARGS+=(--placement-weight 0)
fi
start_server play-b "$PLAY_DLL" "${PLAY_B_ARGS[@]}"
wait_health play-b "$PLAY_B_HTTP"
wait_port play-b-control "$PLAY_B_CONTROL"
wait_port play-b-spot-router "$PLAY_B_SPOT_ROUTER"
wait_port play-b-spot-route "$PLAY_B_SPOT_ROUTE"

read -r SESSION_A_HTTP_PORT SESSION_A_STREAM_PORT SESSION_A_SPOT_ROUTER_PORT SESSION_A_CONTROL_PORT <<<"$(allocate_ports 4)"
SESSION_A_HTTP="http://127.0.0.1:${SESSION_A_HTTP_PORT}"
SESSION_A_STREAM="tcp://127.0.0.1:${SESSION_A_STREAM_PORT}"
SESSION_A_SPOT_ROUTER="tcp://127.0.0.1:${SESSION_A_SPOT_ROUTER_PORT}"
SESSION_A_CONTROL="tcp://127.0.0.1:${SESSION_A_CONTROL_PORT}"
start_server session-a "$SESSION_DLL" \
  --rid session-a \
  --http-url "$SESSION_A_HTTP" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --control-endpoint "$SESSION_A_CONTROL" \
  --play-control-endpoint "$PLAY_A_CONTROL" \
  --spot-router-endpoint "$SESSION_A_SPOT_ROUTER" \
  --stream-endpoint "$SESSION_A_STREAM" \
  --log-dir "$LOG_DIR"
wait_health session-a "$SESSION_A_HTTP"
wait_port session-a-control "$SESSION_A_CONTROL"
wait_port session-a-spot-router "$SESSION_A_SPOT_ROUTER"
wait_port session-a-stream "$SESSION_A_STREAM"

read -r SESSION_B_HTTP_PORT SESSION_B_STREAM_PORT SESSION_B_SPOT_ROUTER_PORT SESSION_B_CONTROL_PORT <<<"$(allocate_ports 4)"
SESSION_B_HTTP="http://127.0.0.1:${SESSION_B_HTTP_PORT}"
SESSION_B_STREAM="tcp://127.0.0.1:${SESSION_B_STREAM_PORT}"
SESSION_B_SPOT_ROUTER="tcp://127.0.0.1:${SESSION_B_SPOT_ROUTER_PORT}"
SESSION_B_CONTROL="tcp://127.0.0.1:${SESSION_B_CONTROL_PORT}"
start_server session-b "$SESSION_DLL" \
  --rid session-b \
  --http-url "$SESSION_B_HTTP" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --control-endpoint "$SESSION_B_CONTROL" \
  --play-control-endpoint "$PLAY_A_CONTROL" \
  --spot-router-endpoint "$SESSION_B_SPOT_ROUTER" \
  --stream-endpoint "$SESSION_B_STREAM" \
  --log-dir "$LOG_DIR"
wait_health session-b "$SESSION_B_HTTP"
wait_port session-b-control "$SESSION_B_CONTROL"
wait_port session-b-spot-router "$SESSION_B_SPOT_ROUTER"
wait_port session-b-stream "$SESSION_B_STREAM"

wait_route_ready "$PLAY_A_HTTP" await.delay delay-a "play-a to delay-a"
wait_route_ready "$PLAY_B_HTTP" await.delay delay-b "play-b to delay-b"
for session_url in "$SESSION_A_HTTP" "$SESSION_B_HTTP"; do
  for play_rid in play-a play-b; do
    wait_route_ready "$session_url" await.control "$play_rid" "session control to $play_rid"
  done
done

if scenario_selected full; then
  python3 "$SCRIPT_DIR/../write_role_config.py" "$CONFIG_DIR/client-full.json" -- \
    --config-dir "$CONFIG_DIR" \
    --scenario "$CLIENT_SCENARIO" \
    --session-a-stream-endpoint "$SESSION_A_STREAM" \
    --session-b-stream-endpoint "$SESSION_B_STREAM" \
    --play-a-url "$PLAY_A_HTTP" \
    --play-b-url "$PLAY_B_HTTP" \
    --request-id "client-${STAMP//[^a-zA-Z0-9]/}" \
    --spot-rid "await-client-${STAMP//[^a-zA-Z0-9]/}"
  dotnet "$CLIENT_DLL" --config "$CONFIG_DIR/client-full.json" \
    >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
  cat "$LOG_DIR/client.stdout.log"
  expected_client_summary="automatic-turn-dispatch client executed=${EXPECTED_CLIENT_SCENARIO_COUNT} result=passed"
  if ! grep -Fxq "$expected_client_summary" "$LOG_DIR/client.stdout.log"; then
    echo "AutomaticTurnDispatch client did not execute the selected scenario count: expected=${EXPECTED_CLIENT_SCENARIO_COUNT}." >&2
    exit 1
  fi
  actual_client_markers="$(
    grep -Ec '^TD-[A-G][0-9]+[A-Z]? result=passed$' "$LOG_DIR/client.stdout.log" || true
  )"
  if [[ "$actual_client_markers" != "$EXPECTED_CLIENT_SCENARIO_COUNT" ]]; then
    echo "AutomaticTurnDispatch scenario marker count mismatch: expected=${EXPECTED_CLIENT_SCENARIO_COUNT} actual=${actual_client_markers}." >&2
    exit 1
  fi
fi

if scenario_selected shutdown; then
  SHUTDOWN_ID="TD-F5-$(date +%s)-$$"
  SHUTDOWN_SPOT="await-shutdown-${STAMP//[^a-zA-Z0-9]/}"
  python3 "$SCRIPT_DIR/../write_role_config.py" "$CONFIG_DIR/client-shutdown-wait.json" -- \
    --config-dir "$CONFIG_DIR" \
    --scenario shutdown-wait \
    --session-a-stream-endpoint "$SESSION_A_STREAM" \
    --session-b-stream-endpoint "$SESSION_B_STREAM" \
    --play-a-url "$PLAY_A_HTTP" \
    --play-b-url "$PLAY_B_HTTP" \
    --request-id "$SHUTDOWN_ID" \
    --spot-rid "$SHUTDOWN_SPOT"
  dotnet "$CLIENT_DLL" --config "$CONFIG_DIR/client-shutdown-wait.json" \
    >"$LOG_DIR/client-shutdown-wait.stdout.log" 2>"$LOG_DIR/client-shutdown-wait.stderr.log" &
  SHUTDOWN_CLIENT_PID=$!
  wait_file_contains \
    "$LOG_DIR/play-a.evidence.log" \
    "await-held|rid=play-a|spot=$SHUTDOWN_SPOT|request=$SHUTDOWN_ID" \
    "TD-F5 pending Async marker was not observed before shutdown." \
    "$SHUTDOWN_CLIENT_PID"
  request_shutdown "$PLAY_A_PID"
  wait_file_contains \
    "$LOG_DIR/client-shutdown-wait.stdout.log" \
    "automatic-turn-dispatch shutdown wait result=passed" \
    "TD-F5 shutdown client did not observe a public shutdown error." \
    "$SHUTDOWN_CLIENT_PID" \
    "$SHUTDOWN_CLIENT_MARKER_ATTEMPTS"
  shutdown_client_status=0
  wait "$SHUTDOWN_CLIENT_PID" || shutdown_client_status=$?
  if [[ "$shutdown_client_status" != "0" ]]; then
    cat "$LOG_DIR/client-shutdown-wait.stderr.log" >&2
    echo "TD-F5 shutdown wait client exited with status $shutdown_client_status." >&2
    exit "$shutdown_client_status"
  fi
  cat "$LOG_DIR/client-shutdown-wait.stdout.log"
  reap_or_kill_after_shutdown play-a "$PLAY_A_PID"

  PLAY_A_RECOVERY_ARGS=("${PLAY_A_ARGS[@]}")
  PLAY_A_RECOVERY_ARGS[1]=play-a-recovery
  start_server play-a "$PLAY_DLL" "${PLAY_A_RECOVERY_ARGS[@]}"
  PLAY_A_PID="${PIDS[-1]}"
  wait_health play-a "$PLAY_A_HTTP"
  wait_port play-a-control "$PLAY_A_CONTROL"
  wait_port play-a-spot-router "$PLAY_A_SPOT_ROUTER"
  wait_port play-a-spot-route "$PLAY_A_SPOT_ROUTE"
  LOCAL_READINESS_TIMEOUT_SECONDS=10
  wait_route_ready "$PLAY_A_HTTP" await.delay delay-a "restarted play-a to delay-a"
  for session_url in "$SESSION_A_HTTP" "$SESSION_B_HTTP"; do
    wait_route_ready "$session_url" await.control play-a "session control to restarted play-a"
  done

  python3 "$SCRIPT_DIR/../write_role_config.py" "$CONFIG_DIR/client-shutdown-recovery.json" -- \
    --config-dir "$CONFIG_DIR" \
    --scenario shutdown-recovery \
    --session-a-stream-endpoint "$SESSION_A_STREAM" \
    --session-b-stream-endpoint "$SESSION_B_STREAM" \
    --play-a-url "$PLAY_A_HTTP" \
    --play-b-url "$PLAY_B_HTTP" \
    --request-id "${SHUTDOWN_ID}-recovery" \
    --spot-rid "$SHUTDOWN_SPOT"
  dotnet "$CLIENT_DLL" --config "$CONFIG_DIR/client-shutdown-recovery.json" \
    >"$LOG_DIR/client-shutdown-recovery.stdout.log" 2>"$LOG_DIR/client-shutdown-recovery.stderr.log"
  cat "$LOG_DIR/client-shutdown-recovery.stdout.log"
  echo "TD-F5A result=passed"
fi

echo "automatic-turn-dispatch e2e result=passed"
