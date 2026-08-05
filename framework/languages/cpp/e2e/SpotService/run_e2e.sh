#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
BUILD_DIR="$CPP_DIR/build"
SCENARIO="all"
E2E_START_ORDER="forward"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
REDIS_READINESS_TIMEOUT_SECONDS=60
ROUTE_SETTLE_SECONDS=5
SCENARIO_SETTLE_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
CONTROL_PING_READINESS_TIMEOUT_SECONDS=3
CHILD_SWEEP_SETTLE_SECONDS=1
# spot node destroy는 core에서 소켓 제거 완료를 기다린다(ctx_t::wait_for_socket_removal).
# 부하가 걸린 연속 실행에서는 이 대기가 10초를 넘기는 경우가 있고, 프로세스는 그 뒤 정상
# 종료한다(60초 유예로 확인). 느린 종료를 강제 kill로 오판하지 않도록 유예를 넉넉히 둔다.
PROCESS_SHUTDOWN_TIMEOUT_SECONDS=45
PROCESS_SHUTDOWN_POLL_SECONDS=0.1
BIND_HOST="127.0.0.2"
PORT_BASE=""
PORT_RANGE=20000
SKIP_BUILD=0
INTERNAL_REDIS_ENDPOINT=""
INTERNAL_REDIS_CONTAINER=""
GDB_ROLES=""
if (($# > 0)) && [[ "$1" != --* ]]; then
  SCENARIO="$1"
  shift
fi
while (($# > 0)); do
  case "$1" in
    --start-order=*) E2E_START_ORDER="${1#*=}" ;;
    --redis-endpoint=*) INTERNAL_REDIS_ENDPOINT="${1#*=}" ;;
    --redis-container=*) INTERNAL_REDIS_CONTAINER="${1#*=}" ;;
    --skip-build) SKIP_BUILD=1 ;;
    --bind-host=*) BIND_HOST="${1#*=}" ;;
    --port-base=*) PORT_BASE="${1#*=}" ;;
    --port-range=*) PORT_RANGE="${1#*=}" ;;
    --gdb-roles=*) GDB_ROLES="${1#*=}" ;;
    *) echo "Unknown SpotService runner option: $1" >&2; exit 2 ;;
  esac
  shift
done
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

read -r ROUTE_A ROUTE_B ROUTE_SESSION_A ROUTE_SESSION_B ROUTE_CLIENT ROUTE_STREAM_CLIENT SPOT_A SPOT_B SPOT_SESSION_A SPOT_SESSION_B SPOT_CLIENT PUB_A PUB_B PUB_SESSION_A PUB_SESSION_B PUB_CLIENT PUBLISHER_CLIENT API_CLIENT STREAM_A STREAM_B MULTI_ROUTE_A MULTI_ROUTE_B MULTI_ROUTE_CLIENT_A MULTI_ROUTE_CLIENT_B MULTI_SPOT_A MULTI_SPOT_B MULTI_PUB_A MULTI_PUB_B STREAM_TLS_A HTTP_A HTTP_B HTTP_SESSION_A HTTP_SESSION_B HTTP_GATEWAY HTTP_MULTI_A HTTP_MULTI_B <<<"$(python3 - "$BIND_HOST" "$PORT_BASE" "$PORT_RANGE" <<'PY'
import random
import socket
import sys

sockets = []
ports = []
host, base, port_range_text = sys.argv[1:]
port_range = int(port_range_text)
start = int(base) if base else 10000
stop = start + port_range if base else 30000
available = list(range(start, stop))
for port in random.sample(available, len(available)):
    s = socket.socket()
    try:
        s.bind((host, port))
    except OSError:
        s.close()
        continue
    sockets.append(s)
    ports.append(s.getsockname()[1])
    if len(ports) == 36:
        break
if len(ports) != 36:
    raise SystemExit(f"failed to allocate 36 local ports, allocated {len(ports)}")
print(" ".join(f"tcp://{host}:{p}" for p in ports[:28]), end=" ")
print(f"tls://{host}:{ports[28]}", end=" ")
print(" ".join(f"http://{host}:{p}" for p in ports[29:]))
for s in sockets:
    s.close()
PY
)"

RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$RUN_ID"
CONFIG_DIR="$LOG_DIR/config"
mkdir -p "$CONFIG_DIR"
echo "log_dir=$LOG_DIR"
echo "start_order=$E2E_START_ORDER"
cat >"$LOG_DIR/endpoints.env" <<EOF
ROUTE_A=$ROUTE_A
ROUTE_B=$ROUTE_B
ROUTE_SESSION_A=$ROUTE_SESSION_A
ROUTE_SESSION_B=$ROUTE_SESSION_B
ROUTE_CLIENT=$ROUTE_CLIENT
ROUTE_STREAM_CLIENT=$ROUTE_STREAM_CLIENT
SPOT_A=$SPOT_A
SPOT_B=$SPOT_B
SPOT_SESSION_A=$SPOT_SESSION_A
SPOT_SESSION_B=$SPOT_SESSION_B
SPOT_CLIENT=$SPOT_CLIENT
PUB_A=$PUB_A
PUB_B=$PUB_B
PUB_SESSION_A=$PUB_SESSION_A
PUB_SESSION_B=$PUB_SESSION_B
PUB_CLIENT=$PUB_CLIENT
PUBLISHER_CLIENT=$PUBLISHER_CLIENT
API_CLIENT=$API_CLIENT
STREAM_A=$STREAM_A
STREAM_B=$STREAM_B
MULTI_ROUTE_A=$MULTI_ROUTE_A
MULTI_ROUTE_B=$MULTI_ROUTE_B
MULTI_ROUTE_CLIENT_A=$MULTI_ROUTE_CLIENT_A
MULTI_ROUTE_CLIENT_B=$MULTI_ROUTE_CLIENT_B
MULTI_SPOT_A=$MULTI_SPOT_A
MULTI_SPOT_B=$MULTI_SPOT_B
MULTI_PUB_A=$MULTI_PUB_A
MULTI_PUB_B=$MULTI_PUB_B
STREAM_TLS_A=$STREAM_TLS_A
HTTP_A=$HTTP_A
HTTP_B=$HTTP_B
HTTP_SESSION_A=$HTTP_SESSION_A
HTTP_SESSION_B=$HTTP_SESSION_B
HTTP_GATEWAY=$HTTP_GATEWAY
HTTP_MULTI_A=$HTTP_MULTI_A
HTTP_MULTI_B=$HTTP_MULTI_B
EOF

REDIS_CONTAINER=""
REDIS_CONTAINER_OWNED=0
REDIS_KEY_PREFIX="zlink:e2e:cfg2:$(date +%s)-$$"
if [[ -n "$INTERNAL_REDIS_CONTAINER" && -n "$INTERNAL_REDIS_ENDPOINT" ]]; then
  REDIS_ENDPOINT="$INTERNAL_REDIS_ENDPOINT"
  REDIS_CONTAINER="$INTERNAL_REDIS_CONTAINER"
  echo "redis endpoint=$REDIS_ENDPOINT (existing owned container $REDIS_CONTAINER)"
else
  zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
    "zlink-redis-cpp-e2e-spotservice" "redis:7-alpine"
  REDIS_CONTAINER_OWNED=1
  REDIS_ENDPOINT="127.0.0.1:${redis_port}"
  echo "redis endpoint=$REDIS_ENDPOINT (container $REDIS_CONTAINER)"
fi
REDIS_HOST="${REDIS_ENDPOINT%:*}"
REDIS_TCP_PORT="${REDIS_ENDPOINT##*:}"
if ! zlink_redis_wait_ready "$REDIS_CONTAINER" "$REDIS_READINESS_TIMEOUT_SECONDS"; then
  if [[ -n "$REDIS_CONTAINER" && "$REDIS_CONTAINER_OWNED" == "1" ]]; then
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
    REDIS_CONTAINER=""
  fi
  exit 1
fi
if ! python3 - "$REDIS_HOST" "$REDIS_TCP_PORT" "$REDIS_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import socket
import sys
import time

host, port = sys.argv[1], int(sys.argv[2])
timeout = float(sys.argv[3])
poll = float(sys.argv[4])
deadline = time.monotonic() + timeout
while time.monotonic() < deadline:
    try:
        with socket.create_connection((host, port), timeout=1):
            raise SystemExit(0)
    except OSError:
        time.sleep(poll)
raise SystemExit(f"Timed out waiting {timeout:g}s for Redis at {host}:{port}")
PY
then
  if [[ -n "$REDIS_CONTAINER" && "$REDIS_CONTAINER_OWNED" == "1" ]]; then
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
    REDIS_CONTAINER=""
  fi
  exit 1
fi
echo "redis key prefix=$REDIS_KEY_PREFIX"

declare -A PORT_GUARDS=()

guard_port() {
  local endpoint="$1"
  local port
  port="$(port_of "$endpoint")"
  if [[ -n "${PORT_GUARDS[$port]:-}" ]]; then
    return 0
  fi
  local ready_file="$LOG_DIR/port-guard-$port.ready"
  python3 - "$port" "$ready_file" "$(host_of "$endpoint")" <<'PY' &
import signal
import socket
import sys
import time

port = int(sys.argv[1])
ready_file = sys.argv[2]
host = sys.argv[3]
sock = socket.socket()
sock.bind((host, port))
sock.listen(1)
with open(ready_file, "w", encoding="utf-8") as ready:
    ready.write(str(port))

running = True
def stop(_signum, _frame):
    global running
    running = False

signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
while running:
    time.sleep(0.1)
sock.close()
PY
  local pid="$!"
  PORT_GUARDS[$port]="$pid"
  wait_file "port-guard-$port" "$ready_file"
}

guard_all_ports() {
  local endpoint
  for endpoint in \
    "$ROUTE_A" "$ROUTE_B" "$ROUTE_SESSION_A" "$ROUTE_SESSION_B" \
    "$SPOT_A" "$SPOT_B" "$SPOT_SESSION_A" "$SPOT_SESSION_B" \
    "$PUB_A" "$PUB_B" "$PUB_SESSION_A" "$PUB_SESSION_B" \
    "$PUBLISHER_CLIENT" "$API_CLIENT" \
    "$STREAM_A" "$STREAM_B" \
    "$MULTI_ROUTE_A" "$MULTI_ROUTE_B" \
    "$MULTI_SPOT_A" "$MULTI_SPOT_B" "$MULTI_PUB_A" "$MULTI_PUB_B" \
    "$STREAM_TLS_A" \
    "$HTTP_A" "$HTTP_B" "$HTTP_SESSION_A" "$HTTP_SESSION_B" "$HTTP_GATEWAY" \
    "$HTTP_MULTI_A" "$HTTP_MULTI_B"; do
    guard_port "$endpoint"
  done
}

release_port_guard() {
  local endpoint="$1"
  if [[ -z "$endpoint" ]]; then
    return 0
  fi
  local port
  port="$(port_of "$endpoint")"
  local pid="${PORT_GUARDS[$port]:-}"
  if [[ -z "$pid" ]]; then
    return 0
  fi
  kill "$pid" >/dev/null 2>&1 || true
  wait "$pid" >/dev/null 2>&1 || true
  unset "PORT_GUARDS[$port]"
  rm -f "$LOG_DIR/port-guard-$port.ready"
}

release_port_guards() {
  local endpoint
  for endpoint in "$@"; do
    release_port_guard "$endpoint"
  done
}

if [[ "$SKIP_BUILD" != "1" ]]; then
  cmake -S "$CPP_DIR" -B "$BUILD_DIR" >/dev/null
  cmake --build "$BUILD_DIR" --target \
    zlink_cpp_e2e_spot_service_play \
    zlink_cpp_e2e_spot_service_session \
    zlink_cpp_e2e_spot_service_gateway \
    zlink_cpp_e2e_spot_service_multinode \
    zlink_cpp_e2e_spot_service_multinode_requester \
    zlink_cpp_e2e_spot_service_client >/dev/null
fi

PLAY_SERVER="$BUILD_DIR/zlink_cpp_e2e_spot_service_play"
SESSION_SERVER="$BUILD_DIR/zlink_cpp_e2e_spot_service_session"
GATEWAY_SERVER="$BUILD_DIR/zlink_cpp_e2e_spot_service_gateway"
MULTI_NODE_SERVER="$BUILD_DIR/zlink_cpp_e2e_spot_service_multinode"
MULTI_NODE_REQUESTER="$BUILD_DIR/zlink_cpp_e2e_spot_service_multinode_requester"
CLIENT="$BUILD_DIR/zlink_cpp_e2e_spot_service_client"
PIDS=()
declare -A ROLE_PIDS=()
PLAY_A_PID=""
PLAY_B_PID=""
SESSION_A_PID=""
GATEWAY_PID=""

role_uses_gdb() {
  local role="$1"
  local roles="$GDB_ROLES"
  [[ "$roles" == "all" || ",$roles," == *",$role,"* ]]
}

run_server_binary() {
  local role="$1"
  local binary="$2"
  shift 2
  if role_uses_gdb "$role"; then
    exec gdb --batch -q \
      -ex "set pagination off" \
      -ex "run" \
      -ex "thread apply all bt full" \
      --args "$binary" "$@"
  fi
  exec "$binary" "$@"
}

run_foreground_binary() {
  local role="$1"
  local binary="$2"
  shift 2
  if role_uses_gdb "$role"; then
    gdb --batch -q \
      -ex "set pagination off" \
      -ex "run" \
      -ex "thread apply all bt full" \
      --args "$binary" "$@"
    return $?
  fi
  "$binary" "$@"
}

record_server_pid() {
  local role="$1"
  local pid="$2"
  PIDS+=("$pid")
  ROLE_PIDS["$role"]="$pid"
}

server_pid_for_role() {
  local role="$1"
  echo "${ROLE_PIDS[$role]:-}"
}

is_process_alive() {
  local pid="$1"
  if [[ -z "$pid" ]]; then
    return 1
  fi
  if ! kill -0 "$pid" >/dev/null 2>&1; then
    return 1
  fi
  [[ "$(ps -o stat= -p "$pid" 2>/dev/null | tr -d ' ')" != Z* ]]
}

status_allowed() {
  local status="$1"
  shift
  local allowed
  for allowed in "$@"; do
    if [[ "$status" -eq "$allowed" ]]; then
      return 0
    fi
  done
  return 1
}

wait_pid_status() {
  local pid="$1"
  local label="$2"
  shift 2
  local status
  if [[ -z "$pid" ]]; then
    return 0
  fi
  set +e
  wait "$pid"
  status=$?
  set -e
  if [[ "$status" -eq 127 ]]; then
    return 0
  fi
  if status_allowed "$status" "$@"; then
    return 0
  fi
  echo "$label exited unexpectedly with status $status" >&2
  return 1
}

dump_server_logs() {
  local role="$1"
  local stdout_log="$LOG_DIR/$role.stdout.log"
  local stderr_log="$LOG_DIR/$role.stderr.log"
  if [[ -s "$stdout_log" ]]; then
    echo "--- $stdout_log ---" >&2
    tail -200 "$stdout_log" >&2 || true
  fi
  if [[ -s "$stderr_log" ]]; then
    echo "--- $stderr_log ---" >&2
    tail -200 "$stderr_log" >&2 || true
  fi
}

role_for_pid() {
  local target_pid="$1"
  local role
  for role in "${!ROLE_PIDS[@]}"; do
    if [[ "${ROLE_PIDS[$role]}" == "$target_pid" ]]; then
      echo "$role"
      return 0
    fi
  done
  echo "pid-$target_pid"
}

forget_server_pid() {
  local target_pid="$1"
  local retained=()
  local pid
  local role
  for pid in "${PIDS[@]:-}"; do
    if [[ "$pid" != "$target_pid" ]]; then
      retained+=("$pid")
    fi
  done
  PIDS=("${retained[@]}")
  for role in "${!ROLE_PIDS[@]}"; do
    if [[ "${ROLE_PIDS[$role]}" == "$target_pid" ]]; then
      unset 'ROLE_PIDS[$role]'
    fi
  done
  if [[ "${PLAY_A_PID:-}" == "$target_pid" ]]; then
    PLAY_A_PID=""
  fi
  if [[ "${PLAY_B_PID:-}" == "$target_pid" ]]; then
    PLAY_B_PID=""
  fi
  if [[ "${SESSION_A_PID:-}" == "$target_pid" ]]; then
    SESSION_A_PID=""
  fi
  if [[ "${GATEWAY_PID:-}" == "$target_pid" ]]; then
    GATEWAY_PID=""
  fi
}

dump_process_backtrace() {
  local pid="$1"
  local role
  role="$(role_for_pid "$pid")"
  local threads_log="$LOG_DIR/$role.cleanup-threads.log"
  if [[ -d "/proc/$pid/task" ]]; then
    {
      for task in /proc/"$pid"/task/*; do
        [[ -d "$task" ]] || continue
        local tid
        tid="${task##*/}"
        printf 'tid=%s comm=%s state=%s wchan=%s syscall=%s\n' \
          "$tid" \
          "$(cat "$task/comm" 2>/dev/null || true)" \
          "$(awk '/^State:/ {print $2}' "$task/status" 2>/dev/null || true)" \
          "$(cat "$task/wchan" 2>/dev/null || true)" \
          "$(cat "$task/syscall" 2>/dev/null || true)"
      done
    } >"$threads_log" 2>&1 || true
    echo "cleanup threads for $role pid $pid: $threads_log" >&2
  fi
  if ! command -v gdb >/dev/null 2>&1; then
    echo "gdb not found; cannot dump backtrace for $role pid $pid" >&2
    return 0
  fi
  local bt_log="$LOG_DIR/$role.cleanup-backtrace.log"
  gdb --batch -q \
    -ex "set pagination off" \
    -ex "thread apply all bt full" \
    -p "$pid" >"$bt_log" 2>&1 || true
  if grep -q "ptrace:" "$bt_log" \
    && command -v sudo >/dev/null 2>&1 \
    && sudo -n true >/dev/null 2>&1; then
    sudo -n gdb --batch -q \
      -ex "set pagination off" \
      -ex "thread apply all bt full" \
      -p "$pid" >"$bt_log" 2>&1 || true
  fi
  echo "cleanup backtrace for $role pid $pid: $bt_log" >&2
}

stop_all_processes() {
  local cleanup_failed=0
  local state
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
  done
  for pid in "${PIDS[@]:-}"; do
    for _ in $(seq 1 "$PROCESS_SHUTDOWN_ATTEMPTS"); do
      if ! kill -0 "$pid" >/dev/null 2>&1; then
        break
      fi
      state="$(ps -o stat= -p "$pid" 2>/dev/null | tr -d ' ')"
      if [[ "$state" == Z* ]]; then
        break
      fi
      sleep "$PROCESS_SHUTDOWN_POLL_SECONDS"
    done
    if kill -0 "$pid" >/dev/null 2>&1 \
      && [[ "$(ps -o stat= -p "$pid" 2>/dev/null | tr -d ' ')" != Z* ]]; then
      dump_process_backtrace "$pid"
      kill -9 "$pid" >/dev/null 2>&1 || true
      if ! wait_pid_status "$pid" "forced cleanup process $pid" 137; then
        cleanup_failed=1
      else
        echo "forced cleanup process $pid exited with status 137" >&2
        cleanup_failed=1
      fi
    elif ! wait_pid_status "$pid" "cleanup process $pid" 0 130 143; then
      cleanup_failed=1
    fi
  done
  for pid in "${PORT_GUARDS[@]:-}"; do
    kill "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
  done
  PORT_GUARDS=()
  wait >/dev/null 2>&1 || true
  PIDS=()
  ROLE_PIDS=()
  PLAY_A_PID=""
  PLAY_B_PID=""
  SESSION_A_PID=""
  GATEWAY_PID=""
  return "$cleanup_failed"
}

cleanup() {
  local code=$?
  local cleanup_failed=0
  if ! stop_all_processes; then
    cleanup_failed=1
  fi
  if [[ -n "$REDIS_CONTAINER" && "$REDIS_CONTAINER_OWNED" == "1" ]]; then
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  fi
  rm -rf "$CONFIG_DIR"
  if [[ $code -ne 0 ]]; then
    echo "E2E failed. Logs: $LOG_DIR" >&2
  elif [[ $cleanup_failed -ne 0 ]]; then
    echo "E2E cleanup failed. Logs: $LOG_DIR" >&2
    code=1
  fi
  exit "$code"
}
trap cleanup EXIT

port_of() {
  echo "${1##*:}"
}

host_of() {
  local endpoint="$1"
  local address="${endpoint#*://}"
  echo "${address%:*}"
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

wait_port_closed() {
  local name="$1"
  local endpoint="$2"
  local host
  local port
  host="$(host_of "$endpoint")"
  port="$(port_of "$endpoint")"
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    if ! (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name to close at $endpoint" >&2
  return 1
}

wait_tls_handshake() {
  local name="$1"
  local endpoint="$2"
  local host
  local port
  host="$(host_of "$endpoint")"
  port="$(port_of "$endpoint")"
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    if timeout 2s openssl s_client -connect "${host}:${port}" -servername "$host" \
      </dev/null >/dev/null 2>&1; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name TLS handshake at $endpoint" >&2
  return 1
}

wait_http_health() {
  local name="$1"
  local endpoint="$2"
  local pid="${3:-}"
  local last_error_file="$LOG_DIR/$name.health.last-error"
  rm -f "$last_error_file"
  local attempts
  for attempts in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    if ! is_process_alive "$pid"; then
      echo "Server $name exited before health became ready at $endpoint/health (pid=${pid:-unknown})." >&2
      dump_server_logs "$name"
      return 1
    fi
    if python3 - "$endpoint/health" "$HTTP_PROBE_TIMEOUT_SECONDS" "$last_error_file" <<'PY'
import sys
import urllib.request

url = sys.argv[1]
probe_timeout_seconds = float(sys.argv[2])
last_error_file = sys.argv[3]
try:
    with urllib.request.urlopen(url, timeout=probe_timeout_seconds) as response:
        if 200 <= response.status < 300:
            sys.exit(0)
        last_error = f"HTTP {response.status}"
except Exception as error:
    last_error = str(error)
with open(last_error_file, "w", encoding="utf-8") as handle:
    handle.write(last_error)
sys.exit(1)
PY
    then
      rm -f "$last_error_file"
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  local last_error=""
  if [[ -f "$last_error_file" ]]; then
    last_error="$(cat "$last_error_file")"
  fi
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name health at $endpoint/health: $last_error" >&2
  dump_server_logs "$name"
  return 1
}

allocate_tcp_endpoint() {
  python3 - "$BIND_HOST" "$PORT_BASE" "$PORT_RANGE" <<'PY'
import random
import socket
import sys

host, base, port_range_text = sys.argv[1:]
port_range = int(port_range_text)
start = int(base) if base else 10000
stop = start + port_range if base else 30000
available = list(range(start, stop))
for port in random.sample(available, len(available)):
    sock = socket.socket()
    try:
        sock.bind((host, port))
    except OSError:
        sock.close()
        continue
    sock.close()
    print(f"tcp://{host}:{port}")
    raise SystemExit(0)
raise SystemExit("failed to allocate local TCP endpoint")
PY
}

wait_file() {
  local name="$1"
  local path="$2"
  local attempts="${3:-$LOCAL_READINESS_ATTEMPTS}"
  for _ in $(seq 1 "$attempts"); do
    if [[ -f "$path" ]]; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name file: $path" >&2
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
        raise SystemExit("E2E_START_ORDER shuffle requires a seed")
    random.Random(int(seed_text)).shuffle(roles)
else:
    raise SystemExit(f"unsupported E2E_START_ORDER={mode!r}")
for role in roles:
    print(role)
PY
}

ensure_location_store() {
  python3 - "$REDIS_HOST" "$REDIS_TCP_PORT" <<'PY'
import socket
import sys

host, port = sys.argv[1], int(sys.argv[2])
with socket.create_connection((host, port), timeout=3):
    pass
PY
}

do_start_play() {
  local rid="$1"
  local route="$2"
  local spot="$3"
  local pubsub="$4"
  local http="$5"
  local api_server="${6:-}"
  local route_mesh_enabled="${7:-true}"
  if [[ "$api_server" == routeMeshEnabled=* ]]; then
    route_mesh_enabled="${api_server#routeMeshEnabled=}"
    api_server=""
  fi
  local peer_pubsub="$PUB_B,$PUB_CLIENT"
  if [[ "$rid" == "play-b" ]]; then
    peer_pubsub="$PUB_A"
  fi
  local route_peers="$ROUTE_B,$ROUTE_SESSION_A,$ROUTE_SESSION_B"
  local spot_peers="$SPOT_B,$SPOT_SESSION_A,$SPOT_SESSION_B"
  if [[ "$rid" == "play-b" ]]; then
    route_peers="$ROUTE_A,$ROUTE_SESSION_A,$ROUTE_SESSION_B"
    spot_peers="$SPOT_A,$SPOT_SESSION_A,$SPOT_SESSION_B"
  fi
  if [[ "$route_mesh_enabled" == "false" ]]; then
    spot_peers=""
  fi
  release_port_guards "$route" "$spot" "$pubsub" "$http" "$api_server" "$API_CLIENT" \
    "$PUBLISHER_CLIENT"
  local config_path="$CONFIG_DIR/$rid.json"
  python3 - "$config_path" "$rid" "$route" "$route_peers" "$spot" "$spot_peers" \
    "$pubsub" "$peer_pubsub" "$API_CLIENT" \
    "$api_server" "$PUBLISHER_CLIENT" "$http" "$HTTP_A" "$HTTP_B" \
    "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$LOG_DIR" "$route_mesh_enabled" <<'PY'
import json, os, stat, sys
(path, rid, route, route_peers, spot, spot_peers, pubsub, peer_pubsub, api_peer, api,
 publisher, http, play_a_http, play_b_http, redis_endpoint, redis_key_prefix, log_dir,
 route_mesh_enabled) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"nodeRid": rid, "routeEndpoint": route,
        "routePeerEndpoints": route_peers, "spotRouterEndpoint": spot,
        "spotPeerEndpoints": spot_peers, "pubsubEndpoint": pubsub,
        "peerPubsubEndpoints": peer_pubsub,
        "apiPeerEndpoint": api_peer, "apiEndpoint": api,
        "publisherEndpoint": publisher, "httpEndpoint": http,
        "playHttpEndpoints": {"playA": play_a_http, "playB": play_b_http},
        "routeMeshEnabled": route_mesh_enabled,
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  run_server_binary "$rid" "$PLAY_SERVER" --config="$config_path" \
    >"$LOG_DIR/$rid.stdout.log" 2>"$LOG_DIR/$rid.stderr.log" &
  local pid="$!"
  record_server_pid "$rid" "$pid"
  if [[ "$rid" == "play-a" ]]; then
    PLAY_A_PID="$pid"
  fi
  if [[ "$rid" == "play-b" ]]; then
    PLAY_B_PID="$pid"
  fi
}

do_start_session() {
  local rid="$1"
  local route="$2"
  local spot="$3"
  local pubsub="$4"
  local stream="$5"
  local http="$6"
  local tls_stream="${7:-}"
  local tls_cert="${8:-}"
  local tls_key="${9:-}"
  local route_mesh_enabled="${10:-true}"
  if [[ "$tls_stream" == routeMeshEnabled=* ]]; then
    route_mesh_enabled="${tls_stream#routeMeshEnabled=}"
    tls_stream=""
  fi
  if [[ "$stream" == "__none__" ]]; then
    stream=""
  fi
  local route_peers="$ROUTE_A,$ROUTE_B,$ROUTE_SESSION_B"
  local spot_peers="$SPOT_A,$SPOT_B,$SPOT_SESSION_B"
  if [[ "$rid" == "session-b" ]]; then
    route_peers="$ROUTE_A,$ROUTE_B,$ROUTE_SESSION_A"
    spot_peers="$SPOT_A,$SPOT_B,$SPOT_SESSION_A"
  fi
  if [[ "$route_mesh_enabled" == "false" ]]; then
    spot_peers=""
  fi
  release_port_guards "$route" "$spot" "$pubsub" "$stream" "$http" "$tls_stream"
  local config_path="$CONFIG_DIR/$rid.json"
  python3 - "$config_path" "$rid" "$route" "$route_peers" "$spot" "$spot_peers" \
    "$pubsub" "$stream" \
    "$tls_stream" "$tls_cert" "$tls_key" "$http" "$REDIS_ENDPOINT" \
    "$REDIS_KEY_PREFIX" "$LOG_DIR" "$route_mesh_enabled" <<'PY'
import json, os, stat, sys
(path, rid, route, route_peers, spot, spot_peers, pubsub, stream, tls_stream, tls_cert,
 tls_key, http, redis_endpoint, redis_key_prefix, log_dir, route_mesh_enabled) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"nodeRid": rid, "routeEndpoint": route,
        "routePeerEndpoints": route_peers, "spotRouterEndpoint": spot,
        "spotPeerEndpoints": spot_peers, "pubsubEndpoint": pubsub,
        "streamEndpoint": stream, "tls": {"streamEndpoint": tls_stream,
        "certPath": tls_cert, "keyPath": tls_key}, "httpEndpoint": http,
        "routeMeshEnabled": route_mesh_enabled,
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  run_server_binary "$rid" "$SESSION_SERVER" --config="$config_path" \
    >"$LOG_DIR/$rid.stdout.log" 2>"$LOG_DIR/$rid.stderr.log" &
  local pid="$!"
  record_server_pid "$rid" "$pid"
  if [[ "$rid" == "session-a" ]]; then
    SESSION_A_PID="$pid"
  fi
}

generate_tls_cert() {
  local cert="$1"
  local key="$2"
  if ! command -v openssl >/dev/null 2>&1; then
    echo "openssl is required for SM-D14 TLS certificate generation" >&2
    return 1
  fi
  openssl req -x509 -newkey rsa:2048 -nodes -days 7 \
    -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
    -keyout "$key" \
    -out "$cert" >/dev/null 2>&1
}

do_start_gateway() {
  local rid="$1"
  local route="$2"
  local spot="$3"
  local pubsub="$4"
  local http="$5"
  release_port_guards "$route" "$spot" "$pubsub" "$http"
  local config_path="$CONFIG_DIR/$rid.json"
  python3 - "$config_path" "$rid" "$route" "$spot" "$pubsub" "$http" \
    "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$LOG_DIR" <<'PY'
import json, os, stat, sys
path, rid, route, spot, pubsub, http, redis_endpoint, redis_key_prefix, log_dir = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"nodeRid": rid, "routeEndpoint": route,
        "spotRouterEndpoint": spot, "pubsubEndpoint": pubsub,
        "httpEndpoint": http,
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  run_server_binary "$rid" "$GATEWAY_SERVER" --config="$config_path" \
    >"$LOG_DIR/$rid.stdout.log" 2>"$LOG_DIR/$rid.stderr.log" &
  local pid="$!"
  record_server_pid "$rid" "$pid"
  GATEWAY_PID="$pid"
}

do_start_multi_node() {
  local rid="$1"
  local route="$2"
  local peer_route="$3"
  local spot="$4"
  local pubsub="$5"
  local http="$6"
  if [[ "$peer_route" == "__none__" ]]; then
    peer_route=""
  fi
  release_port_guards "$route" "$spot" "$pubsub" "$http"
  local config_path="$CONFIG_DIR/$rid.json"
  python3 - "$config_path" "$rid" "$route" "$spot" "$pubsub" "$http" \
    "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$LOG_DIR" "$peer_route" <<'PY'
import json, os, stat, sys
(path, rid, route, spot, pubsub, http, redis_endpoint, redis_key_prefix,
 log_dir, peer_route) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"nodeRid": rid, "routeEndpoint": route,
        "spotRouterEndpoint": spot, "pubsubEndpoint": pubsub,
        "httpEndpoint": http, "disableRouteMesh": "true" if not peer_route else "false",
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  run_server_binary "$rid" "$MULTI_NODE_SERVER" --config="$config_path" \
    >"$LOG_DIR/$rid.stdout.log" 2>"$LOG_DIR/$rid.stderr.log" &
  local pid="$!"
  record_server_pid "$rid" "$pid"
}

do_start_multi_node_requester() {
  local rid="$1"
  local route="$2"
  local route_client="$3"
  local spot="$4"
  local http="$5"
  release_port_guards "$route_client" "$spot" "$http"
  local config_path="$CONFIG_DIR/$rid-requester.json"
  python3 - "$config_path" "$rid" "$route_client" "$spot" "$http" \
    "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$LOG_DIR" <<'PY'
import json, os, stat, sys
path, rid, route_client, spot, http, redis_endpoint, redis_key_prefix, log_dir = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"nodeRid": rid, "routeClientEndpoint": route_client,
        "spotRouterEndpoint": spot, "httpEndpoint": http,
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  run_server_binary "$rid-requester" "$MULTI_NODE_REQUESTER" --config="$config_path" \
    >"$LOG_DIR/$rid-requester.stdout.log" 2>"$LOG_DIR/$rid-requester.stderr.log" &
  record_server_pid "$rid-requester" "$!"
}

PENDING_SERVER_ROLES=()
declare -A PENDING_SERVER_SPECS=()
SERVER_ROLES_STARTED=0

queue_server_role() {
  local role="$1"
  shift
  PENDING_SERVER_ROLES+=("$role")
  PENDING_SERVER_SPECS["$role"]="$*"
}

start_play() {
  queue_server_role "$1" "play" "$@"
}

start_session() {
  queue_server_role "$1" "session" "$@"
}

start_gateway() {
  queue_server_role "$1" "gateway" "$@"
}

start_multi_node() {
  queue_server_role "$1" "multi-node" "$@"
}

start_multi_node_requester() {
  queue_server_role "$1-requester" "multi-node-requester" "$@"
}

start_queued_server_role() {
  local role="$1"
  local spec="${PENDING_SERVER_SPECS[$role]}"
  local kind
  read -r kind _ <<<"$spec"
  case "$kind" in
    play) do_start_play ${spec#play } ;;
    session) do_start_session ${spec#session } ;;
    gateway) do_start_gateway ${spec#gateway } ;;
    multi-node) do_start_multi_node ${spec#multi-node } ;;
    multi-node-requester) do_start_multi_node_requester ${spec#multi-node-requester } ;;
    *) echo "Unknown server role '$role'" >&2; return 1 ;;
  esac
}

wait_named_server() {
  case "$1" in
    play-a)
      wait_http_health play-a "$HTTP_A" "$(server_pid_for_role play-a)"
      ;;
    play-b)
      wait_http_health play-b "$HTTP_B" "$(server_pid_for_role play-b)"
      ;;
    session-a)
      wait_http_health session-a "$HTTP_SESSION_A" "$(server_pid_for_role session-a)"
      ;;
    session-b)
      wait_http_health session-b "$HTTP_SESSION_B" "$(server_pid_for_role session-b)"
      ;;
    gateway)
      wait_http_health gateway "$HTTP_GATEWAY" "$(server_pid_for_role gateway)"
      ;;
    multi-a)
      wait_http_health multi-a "$HTTP_MULTI_A" "$(server_pid_for_role multi-a)"
      if [[ "$SCENARIO" != "SM-F6" && "$SCENARIO" != "sm-f6" ]]; then
        wait_port multi-a-route "$MULTI_ROUTE_A"
      fi
      wait_port multi-a-spot-router "$MULTI_SPOT_A"
      wait_port multi-a-pubsub "$MULTI_PUB_A"
      ;;
    multi-b)
      wait_http_health multi-b "$HTTP_MULTI_B" "$(server_pid_for_role multi-b)"
      if [[ "$SCENARIO" != "SM-F6" && "$SCENARIO" != "sm-f6" ]]; then
        wait_port multi-b-route "$MULTI_ROUTE_B"
      fi
      wait_port multi-b-spot-router "$MULTI_SPOT_B"
      wait_port multi-b-pubsub "$MULTI_PUB_B"
      ;;
    multi-a-requester)
      wait_http_health multi-a-requester "$HTTP_A" "$(server_pid_for_role multi-a-requester)"
      ;;
    multi-b-requester)
      wait_http_health multi-b-requester "$HTTP_B" "$(server_pid_for_role multi-b-requester)"
      ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

ensure_servers_started_and_ready() {
  if [[ "$SERVER_ROLES_STARTED" == "1" ]]; then
    return 0
  fi
  mapfile -t ORDERED_SERVER_ROLES < <(ordered_roles "${PENDING_SERVER_ROLES[@]}")
  for role in "${ORDERED_SERVER_ROLES[@]}"; do
    start_queued_server_role "$role"
  done
  for role in "${PENDING_SERVER_ROLES[@]}"; do
    wait_named_server "$role"
  done
  SERVER_ROLES_STARTED=1
}

fetch_evidence() {
  ensure_servers_started_and_ready
  local name="$1"
  local http="$2"
  python3 - "$http/evidence" "$HTTP_PROBE_TIMEOUT_SECONDS" >"$LOG_DIR/$name-evidence.json" <<'PY'
import sys
import urllib.request

url = sys.argv[1]
timeout_seconds = float(sys.argv[2])
with urllib.request.urlopen(url, timeout=timeout_seconds) as response:
    sys.stdout.write(response.read().decode("utf-8"))
PY
}

wait_evidence() {
  ensure_servers_started_and_ready
  local name="$1"
  local http="$2"
  shift 2
  python3 - "$http/evidence/wait" "$HTTP_PROBE_TIMEOUT_SECONDS" "$@" \
    >"$LOG_DIR/$name-evidence-wait.json" 2>"$LOG_DIR/$name-evidence-wait.stderr.log" <<'PY'
import json
import sys
import urllib.request

url = sys.argv[1]
timeout_seconds = float(sys.argv[2])
payload = json.dumps({
    "contains_all": sys.argv[3:],
    "timeout_milliseconds": int(timeout_seconds * 1000),
}).encode("utf-8")
request = urllib.request.Request(
    url,
    data=payload,
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request, timeout=timeout_seconds + 1) as response:
    sys.stdout.write(response.read().decode("utf-8"))
PY
}

run_control_ping() {
  ensure_servers_started_and_ready
  local output="$1"
  local http="$2"
  local target="$3"
  local value="$4"
  local path="${5:-/channel/control-ping}"
  local mesh_name="${6:-}"
  python3 - "$http$path" "$target" "$value" "$mesh_name" "$HTTP_PROBE_TIMEOUT_SECONDS" >"$LOG_DIR/$output.stdout.log" 2>"$LOG_DIR/$output.stderr.log" <<'PY'
import json
import sys
import urllib.error
import urllib.request

url = sys.argv[1]
target = sys.argv[2]
value = sys.argv[3]
mesh_name = sys.argv[4]
timeout_seconds = float(sys.argv[5])
body = json.dumps({
    "target_node_rid": target,
    "value": value,
    "mesh_name": mesh_name,
}).encode("utf-8")
request = urllib.request.Request(
    url,
    data=body,
    headers={"content-type": "application/json"},
    method="POST",
)
try:
    response = urllib.request.urlopen(request, timeout=timeout_seconds)
except urllib.error.HTTPError as error:
    sys.stderr.write(error.read().decode("utf-8", errors="replace"))
    raise
with response:
    payload = json.loads(response.read().decode("utf-8"))
assert payload["node_rid"] == target
assert payload["value"] == value
print(f"control-ping {target} passed")
PY
}

wait_control_ping() {
  local output="$1"
  local http="$2"
  local target="$3"
  local value="$4"
  local path="${5:-/channel/control-ping}"
  local mesh_name="${6:-}"
  local deadline
  deadline="$(python3 - "$CONTROL_PING_READINESS_TIMEOUT_SECONDS" <<'PY'
import sys
import time
print(time.monotonic() + float(sys.argv[1]))
PY
)"
  local status=1
  while true; do
    if run_control_ping "$output" "$http" "$target" "$value" "$path" "$mesh_name"; then
      status=0
      break
    fi
    if ! python3 - "$deadline" <<'PY'
import sys
import time
raise SystemExit(0 if time.monotonic() < float(sys.argv[1]) else 1)
PY
    then
      break
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  cat "$LOG_DIR/$output.stdout.log"
  if [[ "$status" -ne 0 ]]; then
    cat "$LOG_DIR/$output.stderr.log" >&2
    return "$status"
  fi
}

settle_scenario() {
  sleep "$SCENARIO_SETTLE_SECONDS"
}

run_client_from_options() {
  local config_path="$CONFIG_DIR/client-$(date +%s%N)-$$.json"
  python3 - "$config_path" "$@" <<'PY'
import json
import os
import stat
import sys

path, *overrides = sys.argv[1:]
allowed = {"logDir", "routeEndpoint", "routeAEndpoint", "routeBEndpoint",
    "multiRouteClientAEndpoint", "multiRouteClientBEndpoint",
    "multiRouteAEndpoint", "multiRouteBEndpoint", "spotRouterEndpoint",
    "pubsubEndpoint", "publisherEndpoint", "apiEndpoint", "streamEndpoint",
    "alternateStreamEndpoint", "tlsStreamEndpoint", "scenarioMode",
    "playHttpEndpoint", "playBHttpEndpoint", "multiAHttpEndpoint",
    "multiBHttpEndpoint", "multiARequestHttpEndpoint",
    "multiBRequestHttpEndpoint", "sessionHttpEndpoint", "gatewayHttpEndpoint",
    "clientRid", "runId", "crashReadyFile", "crashGoFile", "crashObservedFile"}
configuration = {}
for override in overrides:
    key, separator, value = override.partition("=")
    if not separator or key not in allowed:
        raise SystemExit(f"unknown SpotService client configuration: {override}")
    configuration[key] = value
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": configuration}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$CLIENT" --config="$config_path"
}

run_base_client() {
  ensure_servers_started_and_ready
  local mode="$1"
  local output="$2"
  local route_endpoint="${3:-$ROUTE_CLIENT}"
  local route_a_endpoint="${4:-$ROUTE_A}"
  local route_b_endpoint="${5:-$ROUTE_B}"
  local status=0
  ensure_servers_started_and_ready
  run_client_from_options \
    routeEndpoint="$route_endpoint" \
    routeAEndpoint="$route_a_endpoint" \
    routeBEndpoint="$route_b_endpoint" \
    multiRouteClientAEndpoint="$MULTI_ROUTE_CLIENT_A" \
    multiRouteClientBEndpoint="$MULTI_ROUTE_CLIENT_B" \
    multiRouteAEndpoint="$MULTI_ROUTE_A" \
    multiRouteBEndpoint="$MULTI_ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    scenarioMode="$mode" \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    multiAHttpEndpoint="$HTTP_MULTI_A" \
    multiBHttpEndpoint="$HTTP_MULTI_B" \
    multiARequestHttpEndpoint="$HTTP_A" \
    multiBRequestHttpEndpoint="$HTTP_B" \
    clientRid="client-$mode" \
    runId="$RUN_ID" \
    logDir="$LOG_DIR" \
    >"$LOG_DIR/$output.stdout.log" 2>"$LOG_DIR/$output.stderr.log" || status=$?
  cat "$LOG_DIR/$output.stdout.log"
  return "$status"
}

wait_route_ready() {
  local output="$1"
  local status=0
  sleep "$ROUTE_SETTLE_SECONDS"
  run_base_client route-ready-play-a "$output" "$ROUTE_CLIENT" "$ROUTE_A" "" || status=$?
  wait_port_closed "$output-route-client" "$ROUTE_CLIENT" || true
  return "$status"
}

wait_play_b_route_ready() {
  local output="$1"
  local status=0
  sleep "$ROUTE_SETTLE_SECONDS"
  run_base_client route-ready-play-b "$output" "$ROUTE_CLIENT" "" "$ROUTE_B" || status=$?
  wait_port_closed "$output-route-client" "$ROUTE_CLIENT" || true
  return "$status"
}

wait_route_ready_on_endpoint() {
  local output="$1"
  local route_endpoint="$2"
  local status=0
  sleep "$ROUTE_SETTLE_SECONDS"
  run_base_client route-ready "$output" "$route_endpoint" || status=$?
  wait_port_closed "$output-route-client" "$route_endpoint" || true
  return "$status"
}

wait_play_b_route_ready_on_endpoint() {
  local output="$1"
  local route_endpoint="$2"
  local status=0
  sleep "$ROUTE_SETTLE_SECONDS"
  run_base_client route-ready-play-b "$output" "$route_endpoint" "" "$ROUTE_B" || status=$?
  wait_port_closed "$output-route-client" "$route_endpoint" || true
  return "$status"
}

if [[ "$SCENARIO" == "SM-A1-A2-A4-F1-F2" || "$SCENARIO" == "sm-a1-a2-a4-f1-f2" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  ensure_servers_started_and_ready
  wait_control_ping sm-a1-a2-a4-f1-f2-play-b-play-a-ready "$HTTP_B" play-a \
    "sm-a1-a2-a4-f1-f2-play-b-play-a-ready"
  run_base_client sm-a1-a2-a4-f1-f2 client-sm-a1-a2-a4-f1-f2
  fetch_evidence play-a-sm-a1-a2-a4-f1-f2 "$HTTP_A"
  fetch_evidence play-b-sm-a1-a2-a4-f1-f2 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-a1-a2-a4-f1-f2-evidence.json" "$LOG_DIR/play-b-sm-a1-a2-a4-f1-f2-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
spot = "user:play-a:spot-owner-order-sm-a4"

def has(snapshot, marker, value=None, actor_id=None):
    return any(item["marker"] == marker
               and item["spot_id"] == spot
               and (value is None or item["value"] == value)
               and (actor_id is None or item["actor_id"] == actor_id)
               for item in snapshot["entries"])

values = [entry["value"]
          for entry in play_a["entries"]
          if entry["marker"] == "StateRouted"
          and entry["spot_id"] == spot]
assert has(play_a, "SpotInitialized")
assert values == ["0", "7", "12"], values
assert has(play_a, "SpotToSpotMsg", "sm-f1-command", "sm-c1-client")
assert has(play_a, "SpotToSpotMsg", "sm-f2-command", "sm-c1-client")
assert not any(entry["spot_id"] == spot for entry in play_b["entries"])
print("scenario SM-A1/A2/A4/F1/F2 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-Q9" || "$SCENARIO" == "sm-q9" ]]; then
  ensure_location_store
  mapfile -t Q9_SERVER_ROLES < <(ordered_roles multi-a multi-b)
  for role in "${Q9_SERVER_ROLES[@]}"; do
    case "$role" in
      multi-a)
        do_start_multi_node multi-a "$MULTI_ROUTE_A" "$MULTI_ROUTE_B" "$MULTI_SPOT_A" "$MULTI_PUB_A" "$HTTP_MULTI_A"
        ;;
      multi-b)
        do_start_multi_node multi-b "$MULTI_ROUTE_B" "$MULTI_ROUTE_A" "$MULTI_SPOT_B" "$MULTI_PUB_B" "$HTTP_MULTI_B"
        ;;
    esac
  done
  for role in "${Q9_SERVER_ROLES[@]}"; do
    wait_named_server "$role"
  done
  do_start_multi_node_requester multi-a "$MULTI_ROUTE_A" "$MULTI_ROUTE_CLIENT_A" "$SPOT_SESSION_A" "$HTTP_A"
  do_start_multi_node_requester multi-b "$MULTI_ROUTE_B" "$MULTI_ROUTE_CLIENT_B" "$SPOT_SESSION_B" "$HTTP_B"
  wait_named_server multi-a-requester
  wait_named_server multi-b-requester
  SERVER_ROLES_STARTED=1
  wait_control_ping sm-q9-requester-a-route-ready "$HTTP_A" multi-a \
    "sm-q9-requester-a-route-ready" "/route/control-ping"
  wait_control_ping sm-q9-requester-b-route-ready "$HTTP_B" multi-b \
    "sm-q9-requester-b-route-ready" "/route/control-ping"
  run_base_client sm-q9 client-sm-q9
  grep -q "operation SpotService.sm-q9 passed" "$LOG_DIR/client-sm-q9.stdout.log"
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-F6" || "$SCENARIO" == "sm-f6" ]]; then
  ensure_location_store
  mapfile -t F6_SERVER_ROLES < <(ordered_roles multi-a multi-b)
  for role in "${F6_SERVER_ROLES[@]}"; do
    case "$role" in
      multi-a)
        do_start_multi_node multi-a "$MULTI_ROUTE_A" "__none__" "$MULTI_SPOT_A" "$MULTI_PUB_A" "$HTTP_MULTI_A"
        ;;
      multi-b)
        do_start_multi_node multi-b "$MULTI_ROUTE_B" "__none__" "$MULTI_SPOT_B" "$MULTI_PUB_B" "$HTTP_MULTI_B"
        ;;
      *) echo "Unknown SM-F6 server role '$role'" >&2; exit 1 ;;
    esac
  done
  wait_http_health multi-a "$HTTP_MULTI_A" "$(server_pid_for_role multi-a)"
  wait_port multi-a-spot-router "$MULTI_SPOT_A"
  wait_port multi-a-pubsub "$MULTI_PUB_A"
  wait_http_health multi-b "$HTTP_MULTI_B" "$(server_pid_for_role multi-b)"
  wait_port multi-b-spot-router "$MULTI_SPOT_B"
  wait_port multi-b-pubsub "$MULTI_PUB_B"
  sleep "$ROUTE_SETTLE_SECONDS"
  run_base_client sm-f6 client-sm-f6
  python3 - "$HTTP_MULTI_B/evidence" "$HTTP_PROBE_TIMEOUT_SECONDS" "$RUN_ID" <<'PY'
import json
import sys
import time
import urllib.request

url = sys.argv[1]
timeout_seconds = float(sys.argv[2])
run_id = sys.argv[3]
target_spot = f"spot-sm-f6-target-cpp-{run_id}"
expected_value = f"sm-f6-send-sm-f6-cpp-{run_id}"
deadline = time.monotonic() + 15.0
last = None
while time.monotonic() < deadline:
    with urllib.request.urlopen(url, timeout=timeout_seconds) as response:
        last = json.loads(response.read().decode("utf-8"))
    if any(item["marker"] == "SpotStateCommand"
           and item["spot_id"] == target_spot
           and item["value"] == expected_value
           for item in last["entries"]):
        break
    time.sleep(0.1)
else:
    raise AssertionError(last)
PY
  fetch_evidence multi-b-sm-f6 "$HTTP_MULTI_B"
  python3 - "$LOG_DIR/multi-b-sm-f6-evidence.json" "$RUN_ID" <<'PY'
import json
import sys

multi_b = json.load(open(sys.argv[1], encoding="utf-8"))
run_id = sys.argv[2]
spot = f"spot-sm-f6-target-cpp-{run_id}"
actor = f"actor-sm-f6-cpp-{run_id}"
send_value = f"sm-f6-send-sm-f6-cpp-{run_id}"

def has(marker, value=None, actor_id=None):
    return any(item["marker"] == marker
               and item["spot_id"] == spot
               and (value is None or item["value"] == value)
               and (actor_id is None or item["actor_id"] == actor_id)
               for item in multi_b["entries"])

assert has("MultiStateRequest", "7")
assert has("SpotStateCommand", send_value)
assert has("SpotActorJoined", None, actor)
print("scenario SM-F6 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

run_stream_route_ready_client() {
  ensure_servers_started_and_ready
  local output="$1"
  local status=0
  ensure_servers_started_and_ready
  run_client_from_options \
    routeEndpoint="$ROUTE_STREAM_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    scenarioMode=route-ready \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-stream-route-ready" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/$output.stdout.log" 2>"$LOG_DIR/$output.stderr.log" || status=$?
  cat "$LOG_DIR/$output.stdout.log"
  return "$status"
}

wait_stream_route_ready() {
  local output="$1"
  local status=0
  sleep "$ROUTE_SETTLE_SECONDS"
  run_stream_route_ready_client "$output" || status=$?
  wait_port_closed "$output-route-client" "$ROUTE_STREAM_CLIENT" || true
  return "$status"
}

run_focused_from_all() {
  local scenario="$1"
  local scenario_index="$2"
  local attempt
  local child_output
  local child_status
  local child_log_dir
  for attempt in 1 2; do
    child_output="$LOG_DIR/child-$scenario-attempt-$attempt.output.log"
    set +e
    "$0" "$scenario" --skip-build --start-order="$E2E_START_ORDER" \
      --redis-endpoint="$REDIS_ENDPOINT" --redis-container="$REDIS_CONTAINER" \
      2>&1 | tee "$child_output"
    child_status="${PIPESTATUS[0]}"
    set -e
    child_log_dir="$(sed -n 's/^log_dir=//p' "$child_output" | tail -1)"
    if [[ -n "$child_log_dir" && -d "$child_log_dir" ]]; then
      echo "$scenario $child_log_dir" >>"$LOG_DIR/child-runs.log"
    fi
    if [[ "$child_status" -eq 0 ]]; then
      return 0
    fi
    if [[ "$attempt" -eq 1 ]] \
      && grep -Eqi 'errno=98|address already in use|Address already in use|EADDRINUSE' \
        "$child_output" "$child_log_dir"/*.stderr.log 2>/dev/null; then
      echo "scenario $scenario retrying after transient bind/address-in-use failure" >&2
      continue
    fi
    if [[ -z "$child_log_dir" || ! -d "$child_log_dir" ]]; then
      echo "missing child log directory for $scenario" >&2
    fi
    exit "$child_status"
  done
}

if [[ "$SCENARIO" == "SM-A1" || "$SCENARIO" == "sm-a1" ]]; then
  if ! rg -q 'list_spot_locations' \
      "$SCRIPT_DIR/Server/Play/Endpoints/operational_endpoints.hpp"; then
    echo "SM-A1 contract gate failed: server does not query spot location rows" >&2
    exit 1
  fi
  if ! rg -q 'spot location row mismatch' \
      "$SCRIPT_DIR/Client/Scenarios/sm_a1_scenario.hpp"; then
    echo "SM-A1 contract gate failed: client does not validate the spot location row" >&2
    exit 1
  fi
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  sleep "$ROUTE_SETTLE_SECONDS"
  run_base_client sm-a1 client-sm-a1
  fetch_evidence play-a-sm-a1 "$HTTP_A"
  python3 - "$LOG_DIR/play-a-sm-a1-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
assert any(entry["marker"] == "EntryJoin"
           and entry["actor_id"] == "alice"
           and entry["value"] == "a-room"
           for entry in play_a["entries"])
assert any(entry["marker"] == "ActorJoined"
           and entry["actor_id"] == "alice"
           and entry["spot_id"] == "user:play-a:a-room"
           for entry in play_a["entries"])
print("scenario SM-A1 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-A2" || "$SCENARIO" == "sm-a2" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  sleep "$ROUTE_SETTLE_SECONDS"
  run_base_client sm-a2 client-sm-a2
  fetch_evidence play-a-sm-a2 "$HTTP_A"
  python3 - "$LOG_DIR/play-a-sm-a2-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
values = [entry["value"]
          for entry in play_a["entries"]
          if entry["marker"] == "StateMutated"
          and entry["actor_id"] == "alice"
          and entry["spot_id"] == "user:play-a:a-room"]
assert len(values) == 4, values
assert values[:2] == ["3", "7"], values
assert values[-1] == "18", values
assert values[2] in ("12", "13"), values
print("scenario SM-A2 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-A3" || "$SCENARIO" == "sm-a3" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-a3-play-b-play-a "$HTTP_B" play-a "sm-a3-play-b-play-a-ready"
  run_base_client sm-a3 client-sm-a3
  fetch_evidence play-a-sm-a3 "$HTTP_A"
  fetch_evidence play-b-sm-a3 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-a3-evidence.json" "$LOG_DIR/play-b-sm-a3-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
spot = "user:play-a:sm-a3-route"
assert any(entry["marker"] == "SpotToSpotRequest"
           and entry["actor_id"] == "sm-a3-client"
           and entry["spot_id"] == spot
           and entry["value"] == "route-direct"
           for entry in play_a["entries"])
assert not any(entry["spot_id"] == spot for entry in play_b["entries"])
print("scenario SM-A3 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-A4" || "$SCENARIO" == "sm-a4" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  run_base_client sm-a4 client-sm-a4
  fetch_evidence play-a-sm-a4 "$HTTP_A"
  fetch_evidence play-b-sm-a4 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-a4-evidence.json" "$LOG_DIR/play-b-sm-a4-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
spot = "user:play-a:sm-a4-owner"
values = [entry["value"]
          for entry in play_a["entries"]
          if entry["marker"] == "StateRouted"
          and entry["spot_id"] == spot]
assert len(values) == 2, values
assert all(value == "0" for value in values), values
assert not any(entry["spot_id"] == spot for entry in play_b["entries"])
print("scenario SM-A4 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-A5" || "$SCENARIO" == "sm-a5" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  ensure_servers_started_and_ready
  wait_control_ping sm-a5-play-b-play-a "$HTTP_B" play-a "sm-a5-play-b-play-a-ready"
  run_base_client sm-a5 client-sm-a5
  fetch_evidence play-a-sm-a5 "$HTTP_A"
  fetch_evidence play-b-sm-a5 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-a5-evidence.json" "$LOG_DIR/play-b-sm-a5-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
spot = "user:play-a:sm-a5-stage"

def has(snapshot, marker, value=None):
    return any(entry["marker"] == marker
               and entry["spot_id"] == spot
               and (value is None or entry["value"] == value)
               for entry in snapshot["entries"])

assert has(play_a, "SpotInitialized")
assert has(play_a, "StateRouted", "0")
assert has(play_a, "StageRequest", "sm-a5-stage:9")
assert has(play_a, "StageTimer", "sm-a5-stage-timer:1")
assert has(play_a, "SpotClosing")
assert not any(entry["spot_id"] == spot for entry in play_b["entries"])
print("scenario SM-A5 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-A6" || "$SCENARIO" == "sm-a6" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  sleep "$ROUTE_SETTLE_SECONDS"
  run_base_client sm-a6 client-sm-a6
  fetch_evidence play-a-sm-a6 "$HTTP_A"
  fetch_evidence play-b-sm-a6 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-a6-evidence.json" "$LOG_DIR/play-b-sm-a6-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
life = "user:play-a:sm-a6-life"
busy = "user:play-a:sm-a6-busy"
initialized = [entry for entry in play_a["entries"]
               if entry["marker"] == "SpotInitialized"
               and entry["spot_id"] == life]
closing = [entry for entry in play_a["entries"]
           if entry["marker"] == "SpotClosing"
           and entry["spot_id"] == life]
closed = [entry for entry in play_a["entries"]
          if entry["marker"] == "SpotLifecycleClosed"
          and entry["spot_id"] == life
          and entry["value"] == "closed"]
assert len(initialized) == 1, initialized
assert len(closing) == 1, closing
assert len(closed) == 1, closed
assert any(entry["marker"] == "ActorJoined"
           and entry["actor_id"] == "sm-a6-actor"
           and entry["spot_id"] == busy
           for entry in play_a["entries"])
assert any(entry["marker"] == "SpotCloseRequested"
           and entry["spot_id"] == busy
           and entry["value"] == "not-closed"
           for entry in play_a["entries"])
assert not any(entry["marker"] == "SpotClosing" and entry["spot_id"] == busy
               for entry in play_a["entries"])
assert not any(entry["spot_id"] in (life, busy) for entry in play_b["entries"])
print("scenario SM-A6 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-A7" || "$SCENARIO" == "sm-a7" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  run_base_client sm-a7 client-sm-a7
  fetch_evidence play-a-sm-a7 "$HTTP_A"
  fetch_evidence play-b-sm-a7 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-a7-evidence.json" "$LOG_DIR/play-b-sm-a7-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
spot = "user:play-a:sm-a7-mismatch"
mismatches = [entry for entry in play_a["entries"]
              if entry["marker"] == "SpotTypeMismatch"
              and entry["spot_id"] == spot
              and entry["value"] == "user"]
state_values = [entry["value"] for entry in play_a["entries"]
                if entry["marker"] == "StateMutated"
                and entry["spot_id"] == spot]
assert len(mismatches) == 1, mismatches
assert state_values == ["17", "17"], state_values
assert not any(entry["spot_id"] == spot for entry in play_b["entries"])
print("scenario SM-A7 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-A8" || "$SCENARIO" == "sm-a8" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  run_base_client sm-a8 client-sm-a8
  fetch_evidence play-a-sm-a8 "$HTTP_A"
  fetch_evidence play-b-sm-a8 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-a8-evidence.json" "$LOG_DIR/play-b-sm-a8-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
spot = "user:play-a:sm-a8-worker"
entries = play_a["entries"]

def indices(marker, value=None):
    result = []
    for index, entry in enumerate(entries):
        if entry["marker"] != marker or entry["spot_id"] != spot:
            continue
        if value is not None and entry["value"] != value:
            continue
        result.append(index)
    return result

started = indices("WorkerStarted", "sm-a8-worker")
interleaved = indices("StateRouted", "1")
completed = indices("WorkerCompleted", "sm-a8-worker")
assert len(started) == 1, started
assert len(interleaved) == 1, interleaved
assert len(completed) == 1, completed
assert started[0] < interleaved[0] < completed[0], (started, interleaved, completed)
assert not any(entry["spot_id"] == spot for entry in play_b["entries"])
print("scenario SM-A8 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-B1" || "$SCENARIO" == "sm-b1" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  sleep "$ROUTE_SETTLE_SECONDS"
  run_base_client sm-b1 client-sm-b1
  fetch_evidence play-a-sm-b1 "$HTTP_A"
  fetch_evidence play-b-sm-b1 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-b1-evidence.json" "$LOG_DIR/play-b-sm-b1-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
actor = "sm-b1-local"
spot = "user:play-a:sm-b1-local"
markers = ["ActorCreated", "EntryActorJoined", "ActorJoined", "ActorJoinedCallback",
           "StateMutated"]
indices = []
for marker in markers:
    matches = [index for index, entry in enumerate(play_a["entries"])
               if entry["marker"] == marker
               and entry["actor_id"] == actor
               and (marker in ("ActorCreated", "EntryActorJoined")
                    or entry["spot_id"] == spot)]
    assert len(matches) == 1, (marker, matches)
    indices.append(matches[0])
assert indices == sorted(indices), indices
assert any(entry["marker"] == "StateMutated"
           and entry["actor_id"] == actor
           and entry["spot_id"] == spot
           and entry["value"] == "1"
           for entry in play_a["entries"])
assert not any(entry["actor_id"] == actor or entry["spot_id"] == spot
               for entry in play_b["entries"])
print("scenario SM-B1 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-B2" || "$SCENARIO" == "sm-b2" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  run_base_client sm-b2 client-sm-b2
  fetch_evidence play-a-sm-b2 "$HTTP_A"
  fetch_evidence play-b-sm-b2 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-b2-evidence.json" "$LOG_DIR/play-b-sm-b2-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
actor = "sm-b2-remote"
entry = "play-b"
spot = "user:play-b:b-sm-b2-remote"
markers = ["ActorCreated", "EntryActorJoined", "ActorJoined", "ActorJoinedCallback",
           "StateMutated"]
indices = []
for marker in markers:
    matches = [index for index, item in enumerate(play_b["entries"])
               if item["marker"] == marker
               and item["actor_id"] == actor
               and (item["spot_id"] == entry or item["spot_id"] == spot)]
    assert len(matches) == 1, (marker, matches)
    indices.append(matches[0])
assert indices == sorted(indices), indices
assert any(item["marker"] == "EntryJoin"
           and item["actor_id"] == actor
           and item["spot_id"] == entry
           and item["value"] == "b-sm-b2-remote"
           for item in play_b["entries"])
assert any(item["marker"] == "StateMutated"
           and item["actor_id"] == actor
           and item["spot_id"] == spot
           and item["value"] == "2"
           for item in play_b["entries"])
assert not any(item["actor_id"] == actor or item["spot_id"] == spot
               for item in play_a["entries"])
print("scenario SM-B2 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-B3" || "$SCENARIO" == "sm-b3" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  run_base_client sm-b3 client-sm-b3
  fetch_evidence play-a-sm-b3 "$HTTP_A"
  fetch_evidence play-b-sm-b3 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-b3-evidence.json" "$LOG_DIR/play-b-sm-b3-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
actor = "sm-b3-complex"
entry = "play-a"
spot = "user:play-a:sm-b3-complex"
markers = ["ActorCreated", "EntryActorJoined", "ActorJoined", "ActorJoinedCallback",
           "ActorComplex"]
indices = []
for marker in markers:
    matches = [index for index, item in enumerate(play_a["entries"])
               if item["marker"] == marker
               and item["actor_id"] == actor
               and (item["spot_id"] == entry or item["spot_id"] == spot)]
    assert len(matches) == 1, (marker, matches)
    indices.append(matches[0])
assert indices == sorted(indices), indices
assert any(item["marker"] == "EntryJoin"
           and item["actor_id"] == actor
           and item["spot_id"] == entry
           and item["value"] == "sm-b3-complex"
           for item in play_a["entries"])
assert any(item["marker"] == "ActorComplex"
           and item["actor_id"] == actor
           and item["spot_id"] == spot
           and item["value"] == "Ada Lovelace|42|analyst|west"
           for item in play_a["entries"])
assert not any(item["actor_id"] == actor or item["spot_id"] == spot
               for item in play_b["entries"])
print("scenario SM-B3 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-B4" || "$SCENARIO" == "sm-b4" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  run_base_client sm-b4 client-sm-b4
  fetch_evidence play-a-sm-b4 "$HTTP_A"
  fetch_evidence play-b-sm-b4 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-b4-evidence.json" "$LOG_DIR/play-b-sm-b4-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
actor = "sm-b4-remote"
spot = "user:play-b:b-sm-b4-remote"
assert not any(item["actor_id"] == actor or item["spot_id"] == spot
               for item in play_a["entries"])
assert any(item["marker"] == "RemoteActorRequestSent"
           and item["actor_id"] == actor
           and item["value"] == "play-b:14"
           for item in play_b["entries"])
assert any(item["marker"] == "RemoteActorRequestReply"
           and item["actor_id"] == actor
           and item["value"] == "play-b:14"
           for item in play_b["entries"])
assert any(item["marker"] == "ActorEnsured"
           and item["actor_id"] == actor
           for item in play_b["entries"])
assert any(item["marker"] == "EntryJoin"
           and item["actor_id"] == actor
           and item["value"] == "b-sm-b4-remote"
           for item in play_b["entries"])
assert any(item["marker"] == "ActorJoined"
           and item["actor_id"] == actor
           and item["spot_id"] == spot
           and item["value"] == "b-sm-b4-remote"
           for item in play_b["entries"])
state = [index for index, item in enumerate(play_b["entries"])
         if item["marker"] == "StateMutated"
         and item["actor_id"] == actor
         and item["spot_id"] == spot
         and item["value"] == "14"]
assert len(state) == 1, state
print("scenario SM-B4 evidence passed")
PY
  grep -q "phase=sent surface=spot_actor kind=actor_request.*actor=sm-b4-remote" "$LOG_DIR/play-b-flow.log"
  grep -q "phase=reply_received surface=spot_actor kind=actor_request.*actor=sm-b4-remote" "$LOG_DIR/play-b-flow.log"
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-B5" || "$SCENARIO" == "sm-b5" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  run_base_client sm-b5 client-sm-b5
  fetch_evidence play-a-sm-b5 "$HTTP_A"
  fetch_evidence play-b-sm-b5 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-b5-evidence.json" "$LOG_DIR/play-b-sm-b5-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
actor = "sm-b5-missing"
spot = "user:play-a:sm-b5-missing"
assert any(item["marker"] == "ActorJoined"
           and item["actor_id"] == actor
           and item["spot_id"] == spot
           for item in play_a["entries"])
assert not any((item["actor_id"] == actor or item["spot_id"] == spot)
               for item in play_b["entries"])
print("scenario SM-B5 evidence passed")
PY
  grep -q "surface=spot_actor.*reason=handler_missing.*action=reply_error.*packet=MissingActorPacket" \
    "$LOG_DIR/play-a-flow.log"
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-B6" || "$SCENARIO" == "sm-b6" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  ensure_servers_started_and_ready
  wait_control_ping sm-b6-session-a-play-a "$HTTP_SESSION_A" play-a "sm-b6-session-a-play-a-ready"
  status=0
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-b6 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-b6" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-b6.stdout.log" 2>"$LOG_DIR/client-sm-b6.stderr.log" || status=$?
  cat "$LOG_DIR/client-sm-b6.stdout.log"
  if [[ "${status:-0}" -ne 0 ]]; then
    cat "$LOG_DIR/client-sm-b6.stderr.log" >&2
    exit "$status"
  fi
  wait_evidence session-a-sm-b6 "$HTTP_SESSION_A" \
    StreamDisconnectNotified sm-b6-disconnect-d5-notified \
    StreamUnbound sm-b6-left
  wait_evidence play-a-sm-b6 "$HTTP_A" \
    ActorDisconnected sm-b6-disconnect-d5-notified
  fetch_evidence play-a-sm-b6 "$HTTP_A"
  fetch_evidence play-b-sm-b6 "$HTTP_B"
  fetch_evidence session-a-sm-b6 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-b6-evidence.json" "$LOG_DIR/play-b-sm-b6-evidence.json" "$LOG_DIR/session-a-sm-b6-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
left = "sm-b6-left"
disconnected = "sm-b6-disconnect-d5-notified"
left_spot = "user:play-a:sm-b6-left"
disconnect_spot = "user:play-a:sm-b6-disconnect"

def count(snapshot, marker, actor):
    return sum(1 for item in snapshot["entries"]
               if item["marker"] == marker and item["actor_id"] == actor)

def has(snapshot, marker, actor, spot=None):
    return any(item["marker"] == marker
               and item["actor_id"] == actor
               and (spot is None or item["spot_id"] == spot)
               for item in snapshot["entries"])

assert count(play_a, "ActorLeft", left) == 1
assert has(play_a, "ActorLeft", left, left_spot)
assert count(play_a, "ActorDisconnected", left) == 0
assert count(play_a, "ActorDisconnected", disconnected) == 1
assert has(play_a, "ActorDisconnected", disconnected, disconnect_spot)
assert count(play_a, "ActorLeft", disconnected) == 0
assert not any(item["actor_id"] in (left, disconnected) for item in play_b["entries"])
assert has(session_a, "StreamDisconnectNotified", disconnected)
assert has(session_a, "StreamUnbound", disconnected)
assert has(session_a, "StreamUnbound", left)
print("scenario SM-B6 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-B7" || "$SCENARIO" == "sm-b7" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  ensure_servers_started_and_ready
  wait_control_ping sm-b7-session-a-play-a "$HTTP_SESSION_A" play-a "sm-b7-session-a-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-b7 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-b7" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-b7.stdout.log" 2>"$LOG_DIR/client-sm-b7.stderr.log"
  cat "$LOG_DIR/client-sm-b7.stdout.log"
  fetch_evidence play-a-sm-b7 "$HTTP_A"
  fetch_evidence play-b-sm-b7 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-b7-evidence.json" "$LOG_DIR/play-b-sm-b7-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
actor = "sm-b7-order"
entry = "play-a"
spot = "user:play-a:sm-b7-order"
entries = play_a["entries"]

def first_index(marker, spot_id=None, value=None):
    matches = [index for index, item in enumerate(entries)
               if item["marker"] == marker
               and item["actor_id"] == actor
               and (spot_id is None or item["spot_id"] == spot_id)
               and (value is None or item["value"] == value)]
    assert len(matches) == 1, (marker, spot_id, value, matches)
    return matches[0]

created = first_index("ActorCreated", entry)
entry_joined = first_index("EntryActorJoined", entry)
joined = first_index("ActorJoined", spot, "sm-b7-order")
joined_callback = first_index("ActorJoinedCallback", spot)
first_ping = first_index("ActorPing", spot, "order-1:1")
second_ping = first_index("ActorPing", spot, "order-2:2")
assert [created, entry_joined, joined, joined_callback, first_ping, second_ping] == sorted(
    [created, entry_joined, joined, joined_callback, first_ping, second_ping])
assert not any(item["actor_id"] == actor or item["spot_id"] == spot
               for item in play_b["entries"])
print("scenario SM-B7 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-B8" || "$SCENARIO" == "sm-b8" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  ensure_servers_started_and_ready
  wait_control_ping sm-b8-session-a-play-a "$HTTP_SESSION_A" play-a "sm-b8-session-a-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-b8 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-b8" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-b8.stdout.log" 2>"$LOG_DIR/client-sm-b8.stderr.log"
  cat "$LOG_DIR/client-sm-b8.stdout.log"
  fetch_evidence play-a-sm-b8 "$HTTP_A"
  fetch_evidence play-b-sm-b8 "$HTTP_B"
  fetch_evidence session-a-sm-b8 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-b8-evidence.json" "$LOG_DIR/play-b-sm-b8-evidence.json" "$LOG_DIR/session-a-sm-b8-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
actor = "actor-sm-b8-destroy"
entry = "play-a"
entries = play_a["entries"]

def indexes(marker, spot_id=None, value=None):
    return [index for index, item in enumerate(entries)
            if item["marker"] == marker
            and item["actor_id"] == actor
            and (spot_id is None or item["spot_id"] == spot_id)
            and (value is None or item["value"] == value)]

created = indexes("ActorCreated", entry)
joined = indexes("EntryActorJoined", entry)
destroyed = indexes("ActorDestroyed", entry, "destroy")
assert len(created) == 1, created
assert len(joined) == 1, joined
assert len(destroyed) == 1, destroyed
assert created[0] < joined[0] < destroyed[0]
assert not indexes("EntryActorPing", entry, "after-destroy:1")
assert not any(item["actor_id"] == actor or item["spot_id"] == entry
               for item in play_b["entries"])
assert any(item["marker"] == "StreamUnbound" and item["actor_id"] == actor
           for item in session_a["entries"])
print("scenario SM-B8 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-B9" || "$SCENARIO" == "sm-b9" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  ensure_servers_started_and_ready
  wait_control_ping sm-b9-session-a-play-a "$HTTP_SESSION_A" play-a "sm-b9-session-a-play-a-ready"
  wait_control_ping sm-b9-session-a-play-b "$HTTP_SESSION_A" play-b "sm-b9-session-a-play-b-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-b9 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-b9" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-b9.stdout.log" 2>"$LOG_DIR/client-sm-b9.stderr.log"
  cat "$LOG_DIR/client-sm-b9.stdout.log"
  fetch_evidence play-a-sm-b9 "$HTTP_A"
  fetch_evidence play-b-sm-b9 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-b9-evidence.json" "$LOG_DIR/play-b-sm-b9-evidence.json" <<'PY'
import json
import sys

checks = [
    (json.load(open(sys.argv[1], encoding="utf-8")), "spot-sm-b9-local-cpp", "actor-sm-b9-local-cpp"),
    (json.load(open(sys.argv[2], encoding="utf-8")), "spot-sm-b9-remote-cpp", "actor-sm-b9-remote-cpp"),
]
for snapshot, spot, actor in checks:
    rejected = actor + "-rejected"
    assert any(item["marker"] == "SpotActorJoinAdmitted"
               and item["spot_id"] == spot
               and item["actor_id"] == actor
               and item["value"] == "allowed"
               for item in snapshot["entries"])
    assert any(item["marker"] == "SpotActorJoined"
               and item["spot_id"] == spot
               and item["actor_id"] == actor
               for item in snapshot["entries"])
    assert any(item["marker"] == "SpotActorJoinRejected"
               and item["spot_id"] == spot
               and item["actor_id"] == rejected
               and item["value"] == "capacity"
               for item in snapshot["entries"])
    assert not any(item["marker"] == "SpotActorJoined"
                   and item["spot_id"] == spot
                   and item["actor_id"] == rejected
                   for item in snapshot["entries"])
print("scenario SM-B9 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-C1" || "$SCENARIO" == "sm-c1" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-c1-play-a-play-b "$HTTP_A" play-b "sm-c1-play-a-play-b-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    scenarioMode=sm-c1 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-c1" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-c1.stdout.log" 2>"$LOG_DIR/client-sm-c1.stderr.log"
  cat "$LOG_DIR/client-sm-c1.stdout.log"
  sleep "$SCENARIO_SETTLE_SECONDS"
  fetch_evidence play-a-sm-c1 "$HTTP_A"
  fetch_evidence play-b-sm-c1 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-c1-evidence.json" "$LOG_DIR/play-b-sm-c1-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
spot = "user:play-a:sm-c1-channel"
entries = play_a["entries"]

def has(marker, value=None, actor_id=None):
    return any(item["marker"] == marker
               and item["spot_id"] == spot
               and (value is None or item["value"] == value)
               and (actor_id is None or item["actor_id"] == actor_id)
               for item in entries)

assert has("SpotInitialized")
assert has("SpotToSpotRequest", "sm-c1-request", "sm-c1-client")
assert has("SpotToSpotMsg", "sm-c1-send", "sm-c1-client")
assert has("MeshMsgReceived", "evt-sm-c1:sm-c1-publish")
assert has("SpotToSpotRequest", "sm-c1-after-timeout", "sm-c1-client")
assert not any(item["spot_id"] == spot for item in play_b["entries"])
print("scenario SM-C1 evidence passed")
PY
  grep -q "surface=spot_route.*reason=handler_missing.*action=reply_error.*packet=MissingSpotReq" \
    "$LOG_DIR/play-a-flow.log"
  grep -q "surface=spot_route.*reason=handler_missing.*action=drop.*packet=MissingSpotMsg" \
    "$LOG_DIR/play-a-flow.log"
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-C2" || "$SCENARIO" == "sm-c2" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A" "$API_CLIENT"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-c2-play-a-play-b "$HTTP_A" play-b "sm-c2-play-a-play-b-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    scenarioMode=sm-c2 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-c2" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-c2.stdout.log" 2>"$LOG_DIR/client-sm-c2.stderr.log"
  cat "$LOG_DIR/client-sm-c2.stdout.log"
  sleep "$SCENARIO_SETTLE_SECONDS"
  fetch_evidence play-a-sm-c2 "$HTTP_A"
  fetch_evidence play-b-sm-c2 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-c2-evidence.json" "$LOG_DIR/play-b-sm-c2-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
spot = "user:play-b:sm-c2-outbound"

def has(snapshot, marker, value=None, spot_id=None):
    return any(item["marker"] == marker
               and (value is None or item["value"] == value)
               and (spot_id is None or item["spot_id"] == spot_id)
               for item in snapshot["entries"])

assert has(play_b, "SpotInitialized", spot_id=spot)
assert has(play_b, "SpotOutbound", "echo-sm-c2|notify-sm-c2|timeout=true", spot)
assert has(play_b, "MeshMsgReceived", "evt-sm-c2:sm-c2-publish", spot)
assert has(play_b, "SpotOutboundNegative", "requestFailed=true", spot)
assert has(play_a, "ChannelEcho", "sm-c2")
assert has(play_a, "ChannelMsg", "notify-sm-c2")
assert has(play_a, "ChannelSlow", "sm-c2")
assert not any(item["spot_id"] == spot for item in play_a["entries"])
print("scenario SM-C2 evidence passed")
PY
  grep -q "surface=channel.*reason=handler_missing.*action=reply_error.*packet=MissingChannelReq" \
    "$LOG_DIR/play-a-flow.log"
  grep -q "surface=channel.*reason=handler_missing.*action=drop.*packet=MissingChannelMsg" \
    "$LOG_DIR/play-a-flow.log"
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-C3" || "$SCENARIO" == "sm-c3" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-c3-play-a-play-b "$HTTP_A" play-b "sm-c3-play-a-play-b-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    scenarioMode=sm-c3 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-c3" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-c3.stdout.log" 2>"$LOG_DIR/client-sm-c3.stderr.log"
  cat "$LOG_DIR/client-sm-c3.stdout.log"
  sleep "$SCENARIO_SETTLE_SECONDS"
  fetch_evidence play-a-sm-c3 "$HTTP_A"
  fetch_evidence play-b-sm-c3 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-c3-evidence.json" "$LOG_DIR/play-b-sm-c3-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
source = "user:play-b:sm-c3-source"
target = "user:play-a:sm-c3-target"

def has(snapshot, marker, value=None, spot_id=None, actor_id=None):
    return any(item["marker"] == marker
               and (value is None or item["value"] == value)
               and (spot_id is None or item["spot_id"] == spot_id)
               and (actor_id is None or item["actor_id"] == actor_id)
               for item in snapshot["entries"])

assert has(play_b, "SpotInitialized", spot_id=source)
assert has(play_a, "SpotInitialized", spot_id=target)
assert has(play_b, "SpotToSpotOutbound",
           f"target={target}|value=sm-c3-direct:reply", source)
assert has(play_a, "SpotToSpotRequest", "sm-c3-direct", target, source)
assert has(play_a, "SpotToSpotMsg", "sm-c3-send-direct", target, source)
assert has(play_a, "MeshMsgReceived", "evt-sm-c3:sm-c3-publish-direct", target)
assert has(play_b, "SpotToSpotTimeout", f"target={target}|failed=true", source)
assert has(play_b, "SpotToSpotNegative", f"target={target}|requestFailed=true", source)
print("scenario SM-C3 evidence passed")
PY
  grep -q "surface=spot_route.*reason=handler_missing.*action=reply_error.*packet=MissingSpotReq" \
    "$LOG_DIR/play-a-flow.log"
  grep -q "surface=spot_route.*reason=handler_missing.*action=drop.*packet=MissingSpotMsg" \
    "$LOG_DIR/play-a-flow.log"
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-C4" || "$SCENARIO" == "sm-c4" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_gateway gateway "$ROUTE_CLIENT" "$SPOT_CLIENT" "$PUB_CLIENT" "$HTTP_GATEWAY"
  settle_scenario
  ensure_servers_started_and_ready
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    scenarioMode=sm-c4 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    gatewayHttpEndpoint="$HTTP_GATEWAY" \
    clientRid="client-sm-c4" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-c4.stdout.log" 2>"$LOG_DIR/client-sm-c4.stderr.log"
  cat "$LOG_DIR/client-sm-c4.stdout.log"
  sleep "$SCENARIO_SETTLE_SECONDS"
  fetch_evidence play-a-sm-c4 "$HTTP_A"
  fetch_evidence gateway-sm-c4 "$HTTP_GATEWAY"
  python3 - "$LOG_DIR/play-a-sm-c4-evidence.json" "$LOG_DIR/gateway-sm-c4-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
gateway = json.load(open(sys.argv[2], encoding="utf-8"))
subscribed = "user:play-a:sm-c4-subscribed"
unsubscribed = "user:play-a:sm-c4-unsubscribed"

def count_event(spot_id):
    return sum(1 for item in play_a["entries"]
               if item["marker"] == "MeshMsgReceived"
               and item["spot_id"] == spot_id
               and item["value"] == "evt-sm-c4:sm-c4-publish")

assert any(item["marker"] == "SpotInitialized" and item["spot_id"] == subscribed
           for item in play_a["entries"])
assert count_event(subscribed) >= 1
assert count_event(unsubscribed) == 0
assert any(item["marker"] == "SpotPublish"
           and item["spot_id"] == subscribed
           and item["value"] == "publisher=gateway|marker=sm-c4-publish"
           for item in gateway["entries"])
assert not any(item["marker"] == "SpotInitialized" for item in gateway["entries"])
print("scenario SM-C4 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-C5" || "$SCENARIO" == "sm-c5" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-c5-play-a-play-b "$HTTP_A" play-b "sm-c5-play-a-play-b-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    scenarioMode=sm-c5 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-c5" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-c5.stdout.log" 2>"$LOG_DIR/client-sm-c5.stderr.log"
  cat "$LOG_DIR/client-sm-c5.stdout.log"
  fetch_evidence play-b-sm-c5 "$HTTP_B"
  python3 - "$LOG_DIR/play-b-sm-c5-evidence.json" <<'PY'
import json
import sys

play_b = json.load(open(sys.argv[1], encoding="utf-8"))
assert any(item["marker"] == "MeshMsgReceived"
           and item["spot_id"] == "spot-sm-c5-target-cpp"
           and item["value"] == "evt-sm-c3:sm-c3-publish-sm-c5-cpp"
           for item in play_b["entries"])
print("scenario SM-C5 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-F1" || "$SCENARIO" == "sm-f1" \
   || "$SCENARIO" == "SM-F2" || "$SCENARIO" == "sm-f2" \
   || "$SCENARIO" == "SM-F3" || "$SCENARIO" == "sm-f3" \
   || "$SCENARIO" == "SM-F4" || "$SCENARIO" == "sm-f4" \
   || "$SCENARIO" == "SM-F5" || "$SCENARIO" == "sm-f5" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  scenario_lower="$(printf '%s' "$SCENARIO" | tr '[:upper:]' '[:lower:]')"
  run_base_client "$scenario_lower" "client-$scenario_lower"
  fetch_evidence "play-b-$scenario_lower" "$HTTP_B"
  python3 - "$scenario_lower" "$LOG_DIR/play-b-$scenario_lower-evidence.json" <<'PY'
import json
import sys

scenario = sys.argv[1]
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
spot = "user:play-b:b-room"

def has(marker, value=None, actor_id=None, target_spot=spot):
    return any(item["marker"] == marker
               and item["spot_id"] == target_spot
               and (value is None or item["value"] == value)
               and (actor_id is None or item["actor_id"] == actor_id)
               for item in play_b["entries"])

if scenario == "sm-f1":
    assert has("SpotToSpotRequest", "route-direct", "external-client")
    assert has("SpotToSpotMsg", "route-direct:command", "external-client")
elif scenario == "sm-f2":
    assert has("SpotToSpotRequest", "route-direct-f2", "external-client")
    assert has("SpotToSpotMsg", "route-direct-f2:command", "external-client")
elif scenario == "sm-f3":
    assert has("SpotToSpotRequest", "route-mixed", "external-client")
elif scenario == "sm-f4":
    assert has("SpotToSpotRequest", "route-recovery", "external-client")
elif scenario == "sm-f5":
    assert has("SpotToSpotRequest", "route-recovery", "external-client")
    f5_spot = "user:play-b:b-sm-f5-close"
    assert has("SpotToSpotRequest", "spot-before-close-f5", "external-client", f5_spot)
    assert has("SpotCloseRequested", "closed", target_spot=f5_spot)
    assert not has("SpotToSpotRequest", "spot-after-close-f5", "external-client", f5_spot)
else:
    raise AssertionError(f"unexpected scenario {scenario}")
print(f"scenario {scenario.upper()} evidence passed")
PY
  if [[ "$scenario_lower" == "sm-f4" || "$scenario_lower" == "sm-f5" ]]; then
    grep -q "surface=spot_route.*packet=DirectSpotReq.*reason=handler_missing.*action=reply_error" \
      "$LOG_DIR/play-a-flow.log"
    grep -q "surface=spot_route.*packet=DirectSpotMsg.*reason=handler_missing.*action=drop" \
      "$LOG_DIR/play-b-flow.log"
  fi
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-G1" || "$SCENARIO" == "sm-g1" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  start_session session-b "$ROUTE_SESSION_B" "$SPOT_SESSION_B" "$PUB_SESSION_B" "$STREAM_B" "$HTTP_SESSION_B"
  ensure_servers_started_and_ready
  wait_control_ping sm-g1-session-a-play-a "$HTTP_SESSION_A" play-a "sm-g1-session-a-play-a-ready"
  wait_control_ping sm-g1-session-b-play-b "$HTTP_SESSION_B" play-b "sm-g1-session-b-play-b-ready"
  CRASH_SETUP_ROUTE="$(allocate_tcp_endpoint)"

  CRASH_READY="$LOG_DIR/sm-g1-ready"
  CRASH_GO="$LOG_DIR/sm-g1-go"
  CRASH_OBSERVED="$LOG_DIR/sm-g1-observed"

  run_client_from_options \
    routeEndpoint="$CRASH_SETUP_ROUTE" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    alternateStreamEndpoint="$STREAM_B" \
    scenarioMode=crash-setup \
    clientRid="client-sm-g1-setup" \
    crashReadyFile="$CRASH_READY" \
    crashGoFile="$CRASH_GO" \
    crashObservedFile="$CRASH_OBSERVED" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-g1-setup.stdout.log" 2>"$LOG_DIR/client-sm-g1-setup.stderr.log" &
  CRASH_CLIENT_PID="$!"
  PIDS+=("$CRASH_CLIENT_PID")

  wait_file "SM-G1 ready" "$CRASH_READY"
  kill -9 "$PLAY_A_PID" >/dev/null 2>&1 || true
  wait "$PLAY_A_PID" >/dev/null 2>&1 || true
  forget_server_pid "$PLAY_A_PID"
  touch "$CRASH_GO"
  wait_file "SM-G1 crash observed" "$CRASH_OBSERVED"
  wait "$CRASH_CLIENT_PID"
  cat "$LOG_DIR/client-sm-g1-setup.stdout.log"
  CRASH_RECOVER_ROUTE="$(allocate_tcp_endpoint)"
  ensure_servers_started_and_ready
  run_client_from_options \
    routeEndpoint="$CRASH_RECOVER_ROUTE" \
    routeAEndpoint="" \
    routeBEndpoint="" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_B" \
    scenarioMode=crash-recover \
    clientRid="client-sm-g1-recover" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-g1-recover.stdout.log" 2>"$LOG_DIR/client-sm-g1-recover.stderr.log"
  cat "$LOG_DIR/client-sm-g1-recover.stdout.log"

  fetch_evidence play-b-sm-g1 "$HTTP_B"
  fetch_evidence session-a-sm-g1 "$HTTP_SESSION_A"
  fetch_evidence session-b-sm-g1 "$HTTP_SESSION_B"
  python3 - "$LOG_DIR/play-b-sm-g1-evidence.json" "$LOG_DIR/session-a-sm-g1-evidence.json" "$LOG_DIR/session-b-sm-g1-evidence.json" <<'PY'
import json
import sys

play_b = json.load(open(sys.argv[1], encoding="utf-8"))
session_a = json.load(open(sys.argv[2], encoding="utf-8"))
session_b = json.load(open(sys.argv[3], encoding="utf-8"))

def has(snapshot, marker, actor=None):
    return any(entry["marker"] == marker and (actor is None or entry["actor_id"] == actor)
               for entry in snapshot["entries"])

def has_value(snapshot, marker, actor, value):
    return any(entry["marker"] == marker and entry["actor_id"] == actor and entry["value"] == value
               for entry in snapshot["entries"])

assert has_value(play_b, "EntryActorPing", "actor-sm-g1-survivor", "after-crash:2")
assert has_value(play_b, "EntryActorPing", "actor-sm-g1-crash", "rebound:1")
assert has(session_a, "StreamBound", "actor-sm-g1-crash")
assert has(session_b, "StreamBound", "actor-sm-g1-survivor")
assert has(session_b, "StreamBound", "actor-sm-g1-crash")
print("scenario SM-G1 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D1" || "$SCENARIO" == "sm-d1" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-d1-session-a-play-a-ready "$HTTP_SESSION_A" play-a \
    "sm-d1-session-a-play-a-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-d1 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d1" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d1.stdout.log" 2>"$LOG_DIR/client-sm-d1.stderr.log"
  cat "$LOG_DIR/client-sm-d1.stdout.log"
  fetch_evidence play-a-sm-d1 "$HTTP_A"
  fetch_evidence play-b-sm-d1 "$HTTP_B"
  fetch_evidence session-a-sm-d1 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-d1-evidence.json" "$LOG_DIR/play-b-sm-d1-evidence.json" "$LOG_DIR/session-a-sm-d1-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
actor = "actor-sm-d1"
spot = "user:play-a:sm-d1-local"

assert any(item["marker"] == "StreamBound"
           and item["actor_id"] == actor
           and item["value"].startswith("play-a:")
           for item in session_a["entries"])
assert any(item["marker"] == "ActorPing"
           and item["actor_id"] == actor
           and item["spot_id"] == spot
           and item["value"] == "local-relay:1"
           for item in play_a["entries"])
assert any(item["marker"] == "ActorPushedSession"
           and item["actor_id"] == actor
           and item["spot_id"] == spot
           and item["value"] == "push-local"
           for item in play_a["entries"])
assert not any(item["actor_id"] == actor or item["spot_id"] == spot
               for item in play_b["entries"])
print("scenario SM-D1 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D2" || "$SCENARIO" == "sm-d2" ]]; then
  ensure_location_store
  routeMeshEnabled=false
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A" "routeMeshEnabled=$routeMeshEnabled"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B" "routeMeshEnabled=$routeMeshEnabled"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" \
    "$STREAM_A" "$HTTP_SESSION_A" "routeMeshEnabled=$routeMeshEnabled"
  ensure_servers_started_and_ready
  wait_control_ping sm-d2-session-a-play-b-ready "$HTTP_SESSION_A" play-b \
    "sm-d2-session-a-play-b-ready" "/channel/control-ping" "spot.service.mesh"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-d2 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d2" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d2.stdout.log" 2>"$LOG_DIR/client-sm-d2.stderr.log"
  cat "$LOG_DIR/client-sm-d2.stdout.log"
  fetch_evidence play-a-sm-d2 "$HTTP_A"
  fetch_evidence play-b-sm-d2 "$HTTP_B"
  fetch_evidence session-a-sm-d2 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-d2-evidence.json" "$LOG_DIR/play-b-sm-d2-evidence.json" "$LOG_DIR/session-a-sm-d2-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
actor = "actor-sm-d2"
spot = "user:play-b:b-sm-d2-remote"

assert any(item["marker"] == "StreamBound"
           and item["actor_id"] == actor
           and item["value"].startswith("play-b:")
           for item in session_a["entries"])
assert any(item["marker"] == "ActorPing"
           and item["actor_id"] == actor
           and item["spot_id"] == spot
           and item["value"] == "remote-relay:1"
           for item in play_b["entries"])
assert any(item["marker"] == "ActorPushedSession"
           and item["actor_id"] == actor
           and item["spot_id"] == spot
           and item["value"] == "push-remote"
           for item in play_b["entries"])
assert not any(item["actor_id"] == actor or item["spot_id"] == spot
               for item in play_a["entries"])
print("scenario SM-D2 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D3" || "$SCENARIO" == "sm-d3" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-d3-session-a-play-a "$HTTP_SESSION_A" play-a "sm-d3-session-a-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-d3 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d3" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d3.stdout.log" 2>"$LOG_DIR/client-sm-d3.stderr.log"
  cat "$LOG_DIR/client-sm-d3.stdout.log"
  fetch_evidence play-a-sm-d3 "$HTTP_A"
  fetch_evidence play-b-sm-d3 "$HTTP_B"
  fetch_evidence session-a-sm-d3 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-d3-evidence.json" "$LOG_DIR/play-b-sm-d3-evidence.json" "$LOG_DIR/session-a-sm-d3-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
entry_actor = "actor-sm-d3-entry"
user_actor = "actor-sm-d3-user"
entry_spot = "play-a"
user_spot = "user:play-a:sm-d3-user"

def has(snapshot, marker, actor, spot=None, value=None):
    return any(item["marker"] == marker
               and item["actor_id"] == actor
               and (spot is None or item["spot_id"] == spot)
               and (value is None or item["value"] == value)
               for item in snapshot["entries"])

assert has(play_a, "ActorEnsured", entry_actor)
assert has(play_a, "EntryActorPing", entry_actor, entry_spot, "entry-relay:1")
assert has(play_a, "EntryActorPushedSession", entry_actor, entry_spot, "entry-push")
assert has(play_a, "ActorJoined", user_actor, user_spot, "sm-d3-user")
assert has(play_a, "ActorPing", user_actor, user_spot, "user-relay:1")
assert has(play_a, "ActorPushedSession", user_actor, user_spot, "user-push")
assert has(session_a, "StreamBound", entry_actor)
assert has(session_a, "StreamBound", user_actor)
assert not any(item["actor_id"] in (entry_actor, user_actor)
               or item["spot_id"] in (entry_spot, user_spot)
               for item in play_b["entries"])
print("scenario SM-D3 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D4" || "$SCENARIO" == "sm-d4" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-d4-session-a-play-a "$HTTP_SESSION_A" play-a "sm-d4-session-a-play-a-ready"
  wait_control_ping sm-d4-session-a-play-b "$HTTP_SESSION_A" play-b "sm-d4-session-a-play-b-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-d4 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d4" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d4.stdout.log" 2>"$LOG_DIR/client-sm-d4.stderr.log"
  cat "$LOG_DIR/client-sm-d4.stdout.log"
  fetch_evidence play-a-sm-d4 "$HTTP_A"
  fetch_evidence play-b-sm-d4 "$HTTP_B"
  fetch_evidence session-a-sm-d4 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-d4-evidence.json" "$LOG_DIR/play-b-sm-d4-evidence.json" "$LOG_DIR/session-a-sm-d4-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
first = "actor-sm-d4-x"
second = "actor-sm-d4-y"
entry = "play-a"

def has(snapshot, marker, actor, value=None):
    return any(item["marker"] == marker
               and item["actor_id"] == actor
               and (value is None or item["value"] == value)
               for item in snapshot["entries"])

assert has(play_a, "EntryActorPing", first, "to-x:1")
assert has(play_a, "EntryActorPing", second, "to-y:1")
assert has(play_a, "EntryActorPushedSession", first, "push-x")
assert has(play_a, "EntryActorPushedSession", second, "push-y")
assert has(session_a, "StreamBound", first)
assert has(session_a, "StreamBound", second)
assert not any(item["actor_id"] in (first, second) or item["spot_id"] == entry
               for item in play_b["entries"])
print("scenario SM-D4 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D5" || "$SCENARIO" == "sm-d5" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-d5-session-a-play-a "$HTTP_SESSION_A" play-a "sm-d5-session-a-play-a-ready"
  wait_control_ping sm-d5-session-a-play-b "$HTTP_SESSION_A" play-b "sm-d5-session-a-play-b-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-d5 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d5" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d5.stdout.log" 2>"$LOG_DIR/client-sm-d5.stderr.log"
  cat "$LOG_DIR/client-sm-d5.stdout.log"
  sleep "$SCENARIO_SETTLE_SECONDS"
  fetch_evidence play-a-sm-d5 "$HTTP_A"
  fetch_evidence play-b-sm-d5 "$HTTP_B"
  fetch_evidence session-a-sm-d5 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-d5-evidence.json" "$LOG_DIR/play-b-sm-d5-evidence.json" "$LOG_DIR/session-a-sm-d5-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
notified = "stream-disconnect-d5-notified"
muted = "stream-disconnect-d5-muted"
single = "stream-disconnect-d5-notified-single"
remote = "stream-disconnect-d5-notified-remote"
notified_spot = "user:play-a:a-stream-disconnect-notified"
muted_spot = "user:play-a:a-stream-disconnect-muted"
single_spot = "user:play-a:a-stream-disconnect-single"
remote_spot = "user:play-b:b-stream-disconnect-remote"

def count(snapshot, marker, actor):
    return sum(1 for item in snapshot["entries"]
               if item["marker"] == marker and item["actor_id"] == actor)

assert count(play_a, "ActorDisconnected", single) == 1
assert count(play_a, "ActorDisconnected", notified) == 1
assert count(play_a, "ActorDisconnected", muted) == 1
assert count(play_b, "ActorDisconnected", remote) == 1
assert count(play_a, "ActorDisconnected", remote) == 0
assert count(play_b, "ActorDisconnected", notified) == 0
assert count(play_b, "ActorDisconnected", muted) == 0
assert count(play_a, "ActorLeft", single) == 0
assert count(play_a, "ActorLeft", notified) == 0
assert count(play_a, "ActorLeft", muted) == 0
assert count(play_b, "ActorLeft", remote) == 0
assert sum(1 for item in session_a["entries"]
           if item["marker"] == "StreamDisconnected") == 3
assert not any(item["actor_id"] in (single, notified, muted)
               or item["spot_id"] in (single_spot, notified_spot, muted_spot)
               for item in play_b["entries"])
assert not any(item["actor_id"] == remote or item["spot_id"] == remote_spot
               for item in play_a["entries"])
print("scenario SM-D5 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D6" || "$SCENARIO" == "sm-d6" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  start_session session-b "$ROUTE_SESSION_B" "$SPOT_SESSION_B" "$PUB_SESSION_B" "$STREAM_B" "$HTTP_SESSION_B"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-d6-session-a-control "$HTTP_SESSION_A" play-a "sm-d6-session-a-ready"
  wait_control_ping sm-d6-session-b-control "$HTTP_SESSION_B" play-b "sm-d6-session-b-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    alternateStreamEndpoint="$STREAM_B" \
    scenarioMode=sm-d6 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d6" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d6.stdout.log" 2>"$LOG_DIR/client-sm-d6.stderr.log"
  cat "$LOG_DIR/client-sm-d6.stdout.log"
  fetch_evidence play-a-sm-d6 "$HTTP_A"
  fetch_evidence play-b-sm-d6 "$HTTP_B"
  fetch_evidence session-a-sm-d6 "$HTTP_SESSION_A"
  fetch_evidence session-b-sm-d6 "$HTTP_SESSION_B"
  python3 - "$LOG_DIR/play-a-sm-d6-evidence.json" "$LOG_DIR/play-b-sm-d6-evidence.json" "$LOG_DIR/session-a-sm-d6-evidence.json" "$LOG_DIR/session-b-sm-d6-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
session_b = json.load(open(sys.argv[4], encoding="utf-8"))
actor = "actor-sm-d6"
shadow = "actor-sm-d6-shadow"

def has(snapshot, marker, actor_id, value=None):
    return any(item["marker"] == marker
               and item["actor_id"] == actor_id
               and (value is None or item["value"] == value)
               for item in snapshot["entries"])

assert has(play_a, "ActorPushedSession", actor, "push-bound-only")
assert not has(play_b, "ActorPushedSession", actor, "push-bound-only")
assert not has(play_b, "ActorPushedSession", shadow, "push-bound-only")
assert has(session_a, "StreamBound", actor)
assert has(session_b, "StreamBound", shadow)
print("scenario SM-D6 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D7" || "$SCENARIO" == "sm-d7" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-g3-session-a-play-a "$HTTP_SESSION_A" play-a "sm-g3-session-a-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-d7 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d7" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d7.stdout.log" 2>"$LOG_DIR/client-sm-d7.stderr.log"
  cat "$LOG_DIR/client-sm-d7.stdout.log"
  fetch_evidence play-a-sm-d7 "$HTTP_A"
  fetch_evidence play-b-sm-d7 "$HTTP_B"
  fetch_evidence session-a-sm-d7 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-d7-evidence.json" "$LOG_DIR/play-b-sm-d7-evidence.json" "$LOG_DIR/session-a-sm-d7-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
actor = "actor-sm-d7"
invalid = "actor-sm-d7-invalid"

def has(snapshot, marker, actor_id, value=None):
    return any(item["marker"] == marker
               and item["actor_id"] == actor_id
               and (value is None or item["value"] == value)
               for item in snapshot["entries"])

assert has(play_a, "ActorEnsured", actor, "SM-D7 Auth")
assert has(play_a, "EntryActorPing", actor, "auth-ok:1")
assert has(session_a, "StreamBound", actor)
assert has(session_a, "StreamAuthFailed", invalid, "play-a")
assert not any(item["actor_id"] in (actor, invalid) for item in play_b["entries"])
print("scenario SM-D7 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D8" || "$SCENARIO" == "sm-d8" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  ensure_servers_started_and_ready
  wait_control_ping sm-d8-session-a-play-a "$HTTP_SESSION_A" play-a "sm-d8-session-a-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-d8 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d8" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d8.stdout.log" 2>"$LOG_DIR/client-sm-d8.stderr.log"
  cat "$LOG_DIR/client-sm-d8.stdout.log"
  fetch_evidence play-a-sm-d8 "$HTTP_A"
  fetch_evidence play-b-sm-d8 "$HTTP_B"
  fetch_evidence session-a-sm-d8 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-d8-evidence.json" "$LOG_DIR/play-b-sm-d8-evidence.json" "$LOG_DIR/session-a-sm-d8-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
actor = "actor-sm-d8-reconnect"

def items(snapshot, marker, actor_id):
    return [item for item in snapshot["entries"]
            if item["marker"] == marker and item["actor_id"] == actor_id]

def has(snapshot, marker, actor_id, value=None):
    return any(item["marker"] == marker
               and item["actor_id"] == actor_id
               and (value is None or item["value"] == value)
               for item in snapshot["entries"])

assert len(items(play_a, "ActorEnsured", actor)) >= 2
assert len(items(play_a, "EntryActorSlowPing", actor)) == 1
assert has(play_a, "EntryActorSlowPing", actor, "before-disconnect:1")
assert has(play_a, "EntryActorPing", actor, "after-reconnect:2")
assert len(items(session_a, "StreamBound", actor)) == 2
assert len(items(session_a, "StreamUnbound", actor)) >= 1
assert not any(item["actor_id"] == actor for item in play_b["entries"])
print("scenario SM-D8 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D9" || "$SCENARIO" == "sm-d9" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-d9-session-a-play-a "$HTTP_SESSION_A" play-a "sm-d9-session-a-play-a-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-d9 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d9" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d9.stdout.log" 2>"$LOG_DIR/client-sm-d9.stderr.log"
  cat "$LOG_DIR/client-sm-d9.stdout.log"
  fetch_evidence play-a-sm-d9 "$HTTP_A"
  fetch_evidence play-b-sm-d9 "$HTTP_B"
  fetch_evidence session-a-sm-d9 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-d9-evidence.json" "$LOG_DIR/play-b-sm-d9-evidence.json" "$LOG_DIR/session-a-sm-d9-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
actor = "actor-sm-d9"

def has(snapshot, marker, actor_id, value=None):
    return any(item["marker"] == marker
               and item["actor_id"] == actor_id
               and (value is None or item["value"] == value)
               for item in snapshot["entries"])

assert has(play_a, "ActorEnsured", actor, "SM-D9 Observer")
assert has(play_a, "EntryActorPing", actor, "observer-1:1")
assert has(play_a, "EntryActorPing", actor, "observer-2:2")
assert has(session_a, "StreamBound", actor)
assert not any(item["actor_id"] == actor for item in play_b["entries"])
print("scenario SM-D9 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D10" || "$SCENARIO" == "sm-d10" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  start_session session-b "$ROUTE_SESSION_B" "$SPOT_SESSION_B" "$PUB_SESSION_B" "$STREAM_B" "$HTTP_SESSION_B"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-d10-session-a-play-a "$HTTP_SESSION_A" play-a "sm-d10-session-a-ready"
  wait_control_ping sm-d10-session-b-play-b "$HTTP_SESSION_B" play-b "sm-d10-session-b-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    alternateStreamEndpoint="$STREAM_B" \
    scenarioMode=sm-d10 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d10" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d10.stdout.log" 2>"$LOG_DIR/client-sm-d10.stderr.log"
  cat "$LOG_DIR/client-sm-d10.stdout.log"
  fetch_evidence play-a-sm-d10 "$HTTP_A"
  fetch_evidence play-b-sm-d10 "$HTTP_B"
  fetch_evidence session-a-sm-d10 "$HTTP_SESSION_A"
  fetch_evidence session-b-sm-d10 "$HTTP_SESSION_B"
  python3 - "$LOG_DIR/play-a-sm-d10-evidence.json" "$LOG_DIR/play-b-sm-d10-evidence.json" "$LOG_DIR/session-a-sm-d10-evidence.json" "$LOG_DIR/session-b-sm-d10-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
session_b = json.load(open(sys.argv[4], encoding="utf-8"))
congested = "actor-sm-d10-congested"
isolated = "actor-sm-d10-isolated"

def has(snapshot, marker, actor_id, value=None):
    return any(item["marker"] == marker
               and item["actor_id"] == actor_id
               and (value is None or item["value"] == value)
               for item in snapshot["entries"])

assert has(session_a, "StreamBound", congested)
assert has(session_b, "StreamBound", isolated)
assert has(play_a, "ActorPing", congested, "after-backpressure:9")
assert has(play_b, "ActorPushedSession", isolated, "isolated-push")
print("scenario SM-D10 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D11" || "$SCENARIO" == "sm-d11" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-d13-session-a-play-a "$HTTP_SESSION_A" play-a "sm-d13-session-a-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    sessionHttpEndpoint="$HTTP_SESSION_A" \
    scenarioMode=sm-d11 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d11" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d11.stdout.log" 2>"$LOG_DIR/client-sm-d11.stderr.log"
  cat "$LOG_DIR/client-sm-d11.stdout.log"
  fetch_evidence play-a-sm-d11 "$HTTP_A"
  fetch_evidence play-b-sm-d11 "$HTTP_B"
  fetch_evidence session-a-sm-d11 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-d11-evidence.json" "$LOG_DIR/play-b-sm-d11-evidence.json" "$LOG_DIR/session-a-sm-d11-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
actor = "actor-sm-d11"

def has(snapshot, marker, actor_id=None, value=None):
    return any(item["marker"] == marker
               and (actor_id is None or item["actor_id"] == actor_id)
               and (value is None or item["value"] == value)
               for item in snapshot["entries"])

assert has(play_a, "ActorEnsured", actor, "SM-D11 Stream Channel")
assert has(play_a, "EntryActorPing", actor, "stream-side:1")
assert has(play_a, "ChannelEcho", value="channel-side")
assert has(session_a, "StreamBound", actor)
assert not any(item["actor_id"] == actor for item in play_b["entries"])
print("scenario SM-D11 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D12" || "$SCENARIO" == "sm-d12" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  start_session session-b "$ROUTE_SESSION_B" "$SPOT_SESSION_B" "$PUB_SESSION_B" "$STREAM_B" "$HTTP_SESSION_B"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-d12-session-a-play-a "$HTTP_SESSION_A" play-a "sm-d12-session-a-ready"
  wait_control_ping sm-d12-session-b-play-a "$HTTP_SESSION_B" play-a "sm-d12-session-b-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    alternateStreamEndpoint="$STREAM_B" \
    scenarioMode=sm-d12 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d12" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d12.stdout.log" 2>"$LOG_DIR/client-sm-d12.stderr.log"
  cat "$LOG_DIR/client-sm-d12.stdout.log"
  fetch_evidence play-a-sm-d12 "$HTTP_A"
  fetch_evidence play-b-sm-d12 "$HTTP_B"
  fetch_evidence session-a-sm-d12 "$HTTP_SESSION_A"
  fetch_evidence session-b-sm-d12 "$HTTP_SESSION_B"
  python3 - "$LOG_DIR/play-a-sm-d12-evidence.json" "$LOG_DIR/play-b-sm-d12-evidence.json" "$LOG_DIR/session-a-sm-d12-evidence.json" "$LOG_DIR/session-b-sm-d12-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
session_b = json.load(open(sys.argv[4], encoding="utf-8"))
actor = "actor-sm-d12-transfer"

def has(snapshot, marker, actor_id=None, value=None):
    return any(item["marker"] == marker
               and (actor_id is None or item["actor_id"] == actor_id)
               and (value is None or item["value"] == value)
               for item in snapshot["entries"])

assert has(play_a, "ActorEnsured", actor, "SM-D12 Transfer")
assert has(play_a, "EntryActorPing", actor, "before-transfer:1")
assert has(play_a, "EntryActorPushedSession", actor, "after-transfer")
assert has(session_a, "StreamBound", actor)
assert has(session_b, "StreamBound", actor)
assert not any(item["actor_id"] == actor for item in play_b["entries"])
print("scenario SM-D12 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D13" || "$SCENARIO" == "sm-d13" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  settle_scenario
  ensure_servers_started_and_ready
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-d13 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d13" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d13.stdout.log" 2>"$LOG_DIR/client-sm-d13.stderr.log"
  cat "$LOG_DIR/client-sm-d13.stdout.log"
  fetch_evidence play-a-sm-d13 "$HTTP_A"
  fetch_evidence play-b-sm-d13 "$HTTP_B"
  fetch_evidence session-a-sm-d13 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-d13-evidence.json" "$LOG_DIR/play-b-sm-d13-evidence.json" "$LOG_DIR/session-a-sm-d13-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))
session_a = json.load(open(sys.argv[3], encoding="utf-8"))
actor = "actor-sm-d13"

def has(snapshot, marker, actor_id=None, value=None):
    return any(item["marker"] == marker
               and (actor_id is None or item["actor_id"] == actor_id)
               and (value is None or item["value"] == value)
               for item in snapshot["entries"])

assert has(play_a, "ActorEnsured", actor, "SM-D13 Heartbeat")
assert has(play_a, "EntryActorPing", actor, "heartbeat:1")
assert has(session_a, "StreamBound", actor)
assert not any(item["actor_id"] == actor for item in play_b["entries"])
print("scenario SM-D13 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D14" || "$SCENARIO" == "sm-d14" ]]; then
  TLS_CERT="$LOG_DIR/sm-d14-server.crt"
  TLS_KEY="$LOG_DIR/sm-d14-server.key"
  generate_tls_cert "$TLS_CERT" "$TLS_KEY"
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "__none__" "$HTTP_SESSION_A" "$STREAM_TLS_A" "$TLS_CERT" "$TLS_KEY"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-d14-session-a-play-a "$HTTP_SESSION_A" play-a "sm-d14-session-a-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    tlsStreamEndpoint="$STREAM_TLS_A" \
    scenarioMode=sm-d14 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-d14" \
    logDir="$LOG_DIR" \
    >"$LOG_DIR/client-sm-d14.stdout.log" 2>"$LOG_DIR/client-sm-d14.stderr.log"
  cat "$LOG_DIR/client-sm-d14.stdout.log"
  fetch_evidence play-a-sm-d14 "$HTTP_A"
  fetch_evidence session-a-sm-d14 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-d14-evidence.json" "$LOG_DIR/session-a-sm-d14-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
session_a = json.load(open(sys.argv[2], encoding="utf-8"))
actor = "actor-sm-d14-tls"

def has(snapshot, marker, actor_id=None, value=None):
    return any(item["marker"] == marker
               and (actor_id is None or item["actor_id"] == actor_id)
               and (value is None or item["value"] == value)
               for item in snapshot["entries"])

assert has(play_a, "ActorEnsured", actor, "SM-D14 TLS")
assert has(play_a, "EntryActorPushedSession", actor, "tls-push")
assert has(session_a, "StreamBound", actor)
print("scenario SM-D14 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-D15" || "$SCENARIO" == "sm-d15" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  start_gateway gateway "$ROUTE_CLIENT" "$SPOT_CLIENT" "$PUB_CLIENT" "$HTTP_GATEWAY"
  ensure_servers_started_and_ready
  wait_control_ping sm-d15-session-a-play-a "$HTTP_SESSION_A" play-a "sm-d15-session-a-play-a-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_STREAM_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-d15 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    gatewayHttpEndpoint="$HTTP_GATEWAY" \
    clientRid="client-sm-d15" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-d15.stdout.log" 2>"$LOG_DIR/client-sm-d15.stderr.log"
  cat "$LOG_DIR/client-sm-d15.stdout.log"
  fetch_evidence play-a-sm-d15 "$HTTP_A"
  fetch_evidence gateway-sm-d15 "$HTTP_GATEWAY"
  python3 - "$LOG_DIR/play-a-sm-d15-evidence.json" "$LOG_DIR/gateway-sm-d15-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
gateway = json.load(open(sys.argv[2], encoding="utf-8"))
actor = "actor-sm-d15-cpp"
marker = "sm-d15-cpp"
assert any(item["marker"] == "EntryActorPushedSession"
           and item["actor_id"] == actor
           and item["value"] == marker
           for item in play_a["entries"])
assert any(item["marker"] == "ActorPushDelivered"
           and item["actor_id"] == actor
           and marker in item["value"]
           for item in gateway["entries"])
print("scenario SM-D15 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-E1" || "$SCENARIO" == "sm-e1" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  ensure_servers_started_and_ready
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    scenarioMode=sm-e1 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-e1" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-e1.stdout.log" 2>"$LOG_DIR/client-sm-e1.stderr.log"
  cat "$LOG_DIR/client-sm-e1.stdout.log"
  fetch_evidence play-a-sm-e1 "$HTTP_A"
  fetch_evidence play-b-sm-e1 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-e1-evidence.json" "$LOG_DIR/play-b-sm-e1-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))

def has(snapshot, marker, spot_id=None):
    return any(item["marker"] == marker
               and (spot_id is None or item["spot_id"] == spot_id)
               for item in snapshot["entries"])

assert has(play_b, "SpotInitialized", "user:play-b:sm-e1-missing")
assert not any(item["spot_id"] == "user:play-b:sm-e1-missing" for item in play_a["entries"])
print("scenario SM-E1 evidence passed")
PY
  grep -q "surface=spot_route.*reason=handler_missing.*action=reply_error.*packet=MissingSpotReq" \
    "$LOG_DIR/play-b-flow.log"
  grep -q "surface=spot_route.*reason=handler_missing.*action=drop.*packet=MissingSpotMsg" \
    "$LOG_DIR/play-b-flow.log"
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-E2" || "$SCENARIO" == "sm-e2" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  settle_scenario
  ensure_servers_started_and_ready
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    scenarioMode=sm-e2 \
    playHttpEndpoint="$HTTP_A" \
    clientRid="client-sm-e2" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-e2.stdout.log" 2>"$LOG_DIR/client-sm-e2.stderr.log"
  cat "$LOG_DIR/client-sm-e2.stdout.log"
  fetch_evidence play-a-sm-e2 "$HTTP_A"
  python3 - "$LOG_DIR/play-a-sm-e2-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
spot = "user:play-a:sm-e2-timer"
assert any(item["marker"] == "SpotInitialized" and item["spot_id"] == spot
           for item in play_a["entries"])
assert any(item["marker"] == "SpotTimerTick"
           and item["spot_id"] == spot
           and item["value"] == "sm-e2-tick:1"
           for item in play_a["entries"])
print("scenario SM-E2 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-E3" || "$SCENARIO" == "sm-e3" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  settle_scenario
  ensure_servers_started_and_ready
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    scenarioMode=sm-e3 \
    playHttpEndpoint="$HTTP_A" \
    clientRid="client-sm-e3" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-e3.stdout.log" 2>"$LOG_DIR/client-sm-e3.stderr.log"
  cat "$LOG_DIR/client-sm-e3.stdout.log"
  fetch_evidence play-a-sm-e3 "$HTTP_A"
  python3 - "$LOG_DIR/play-a-sm-e3-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
spot = "user:play-a:sm-e3-idle"
assert any(item["marker"] == "SpotIdleTimerClosed"
           and item["spot_id"] == spot
           and item["value"] == "sm-e3-idle:closed=true"
           for item in play_a["entries"])
assert any(item["marker"] == "SpotClosing" and item["spot_id"] == spot
           for item in play_a["entries"])
print("scenario SM-E3 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-E4" || "$SCENARIO" == "sm-e4" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  settle_scenario
  ensure_servers_started_and_ready
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    scenarioMode=sm-e4 \
    playHttpEndpoint="$HTTP_A" \
    clientRid="client-sm-e4" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-e4.stdout.log" 2>"$LOG_DIR/client-sm-e4.stderr.log"
  cat "$LOG_DIR/client-sm-e4.stdout.log"
  fetch_evidence play-a-sm-e4 "$HTTP_A"
  python3 - "$LOG_DIR/play-a-sm-e4-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
cases = {
    "user:play-a:sm-e4-skip": "sm-e4-SkipLateTicks",
    "user:play-a:sm-e4-catch": "sm-e4-CatchUpBounded",
    "user:play-a:sm-e4-delay": "sm-e4-DelayNextTick",
}

def field(value, key):
    prefix = key + "="
    for part in value.split("|"):
        if part.startswith(prefix):
            return int(part[len(prefix):])
    raise AssertionError(f"missing {key} in {value}")

for spot, name in cases.items():
    entries = [item for item in play_a["entries"]
               if item["marker"] == "SpotTimerOverrun"
               and item["spot_id"] == spot
               and item["value"].startswith(name + "|")]
    assert len(entries) >= 3, (spot, name, entries)

skip_entries = [item for item in play_a["entries"]
                if item["marker"] == "SpotTimerOverrun"
                and item["spot_id"] == "user:play-a:sm-e4-skip"]
assert any(field(item["value"], "skipped") > 0 for item in skip_entries)

catch_entries = [item for item in play_a["entries"]
                 if item["marker"] == "SpotTimerOverrun"
                 and item["spot_id"] == "user:play-a:sm-e4-catch"]
assert any(field(item["value"], "skipped") > 0 for item in catch_entries)

delay_entries = [item for item in play_a["entries"]
                 if item["marker"] == "SpotTimerOverrun"
                 and item["spot_id"] == "user:play-a:sm-e4-delay"][:3]
assert all(field(item["value"], "skipped") == 0
           and field(item["value"], "delivery") == field(item["value"], "scheduled")
           for item in delay_entries)
print("scenario SM-E4 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-G2" || "$SCENARIO" == "sm-g2" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-g2-play-b-play-a "$HTTP_B" play-a "sm-g2-play-b-play-a-ready"
  wait_control_ping sm-g2-play-a-play-b "$HTTP_A" play-b "sm-g2-play-a-play-b-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    scenarioMode=sm-g2 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-g2" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-g2.stdout.log" 2>"$LOG_DIR/client-sm-g2.stderr.log"
  cat "$LOG_DIR/client-sm-g2.stdout.log"
  fetch_evidence play-a-sm-g2 "$HTTP_A"
  fetch_evidence play-b-sm-g2 "$HTTP_B"
  python3 - "$LOG_DIR/play-a-sm-g2-evidence.json" "$LOG_DIR/play-b-sm-g2-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
play_b = json.load(open(sys.argv[2], encoding="utf-8"))

first_spot = "user:play-a:sm-g2-owner"
remapped_spot = "user:play-b:sm-g2-owner"

def has(snapshot, marker, spot_id, value=None):
    return any(item["marker"] == marker
               and item["spot_id"] == spot_id
               and (value is None or item["value"] == value)
               for item in snapshot["entries"])

assert has(play_a, "SpotInitialized", first_spot)
assert has(play_b, "SpotInitialized", remapped_spot)
assert has(play_a, "SpotToSpotRequest", first_spot, "sm-g2-before-remap")
assert has(play_b, "SpotToSpotRequest", remapped_spot, "sm-g2-after-remap")
assert not any(item["spot_id"] == remapped_spot for item in play_a["entries"])
assert not any(item["spot_id"] == first_spot for item in play_b["entries"])
print("scenario SM-G2 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-G3" || "$SCENARIO" == "sm-g3" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-g3-session-a-play-a "$HTTP_SESSION_A" play-a "sm-g3-session-a-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-g3 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-g3" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-g3.stdout.log" 2>"$LOG_DIR/client-sm-g3.stderr.log"
  cat "$LOG_DIR/client-sm-g3.stdout.log"
  fetch_evidence play-a-sm-g3 "$HTTP_A"
  fetch_evidence session-a-sm-g3 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-g3-evidence.json" "$LOG_DIR/session-a-sm-g3-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
session_a = json.load(open(sys.argv[2], encoding="utf-8"))
spot = "user:play-a:sm-g3-concurrent"
actors = ["actor-sm-g3-0", "actor-sm-g3-1"]

def count(snapshot, marker, actor_id, spot_id=None):
    return sum(1 for item in snapshot["entries"]
               if item["marker"] == marker
               and item["actor_id"] == actor_id
               and (spot_id is None or item["spot_id"] == spot_id))

for actor in actors:
    assert count(play_a, "ActorJoined", actor, spot) == 1
    assert count(play_a, "ActorLeft", actor, spot) == 1
    assert count(session_a, "StreamBound", actor) == 1
print("scenario SM-G3 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

if [[ "$SCENARIO" == "SM-G4" || "$SCENARIO" == "sm-g4" ]]; then
  ensure_location_store
  start_play play-a "$ROUTE_A" "$SPOT_A" "$PUB_A" "$HTTP_A"
  start_play play-b "$ROUTE_B" "$SPOT_B" "$PUB_B" "$HTTP_B"
  start_session session-a "$ROUTE_SESSION_A" "$SPOT_SESSION_A" "$PUB_SESSION_A" "$STREAM_A" "$HTTP_SESSION_A"
  settle_scenario
  ensure_servers_started_and_ready
  wait_control_ping sm-g4-session-a-play-a "$HTTP_SESSION_A" play-a "sm-g4-session-a-ready"
  run_client_from_options \
    routeEndpoint="$ROUTE_CLIENT" \
    routeAEndpoint="$ROUTE_A" \
    routeBEndpoint="$ROUTE_B" \
    spotRouterEndpoint="$SPOT_CLIENT" \
    pubsubEndpoint="$PUB_CLIENT" \
    publisherEndpoint="$PUBLISHER_CLIENT" \
    apiEndpoint="$API_CLIENT" \
    streamEndpoint="$STREAM_A" \
    scenarioMode=sm-g4 \
    playHttpEndpoint="$HTTP_A" \
    playBHttpEndpoint="$HTTP_B" \
    clientRid="client-sm-g4" \
    logDir="$LOG_DIR" \
     >"$LOG_DIR/client-sm-g4.stdout.log" 2>"$LOG_DIR/client-sm-g4.stderr.log"
  cat "$LOG_DIR/client-sm-g4.stdout.log"
  fetch_evidence play-a-sm-g4 "$HTTP_A"
  fetch_evidence session-a-sm-g4 "$HTTP_SESSION_A"
  python3 - "$LOG_DIR/play-a-sm-g4-evidence.json" "$LOG_DIR/session-a-sm-g4-evidence.json" <<'PY'
import json
import sys

play_a = json.load(open(sys.argv[1], encoding="utf-8"))
session_a = json.load(open(sys.argv[2], encoding="utf-8"))
actors = [f"actor-sm-g4-{index}" for index in range(6)]

def count(snapshot, marker, actor_id, value=None):
    return sum(1 for item in snapshot["entries"]
               if item["marker"] == marker
               and item["actor_id"] == actor_id
               and (value is None or item["value"] == value))

for index, actor in enumerate(actors):
    assert count(session_a, "StreamBound", actor) == 1
    assert count(play_a, "EntryActorPushedSession", actor, f"push-{index}") == 1
print("scenario SM-G4 evidence passed")
PY
  echo "spot-service e2e result=passed"
  exit 0
fi

scenario_index=0
for scenario in \
  SM-B1 SM-B2 SM-B3 SM-B5 \
  SM-B6 SM-B8 SM-B9 \
  SM-D1 SM-D2 SM-D6 SM-D3 SM-D4 SM-D5 SM-D7 SM-D8 SM-D9 SM-D11 SM-D13 SM-D10 SM-D12 SM-D14 SM-D15 \
  SM-C1 SM-C2 SM-C3 SM-C5 \
  SM-E4 SM-E1 SM-E2 SM-E3 \
  SM-A7 SM-A8 SM-C4 \
  SM-A3 SM-A6 SM-B4 SM-B7 \
  SM-A5 SM-A1-A2-A4-F1-F2 SM-F3 SM-F4 SM-F5 SM-F6 \
  SM-G2 SM-G3 SM-G4 SM-G1; do
  run_focused_from_all "$scenario" "$scenario_index"
  scenario_index=$((scenario_index + 1))
  sleep "$CHILD_SWEEP_SETTLE_SECONDS"
done

echo "spot-service e2e result=passed"
