#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../redis-common.sh"

# Some ST-F handoff markers are runtime diagnostics behind this gate. Message
# Follow completion uses public terminal and application-handler evidence
# instead of runtime log markers.
# Builds and process fixtures share output directories. Serialize complete
# runner invocations so concurrent scenarios cannot corrupt or invalidate
# each other's evidence.
RUNNER_LOCK="${TMPDIR:-/tmp}/zlink-spot-actor-transfer-e2e.lock"
exec 9>"$RUNNER_LOCK"
flock -w 600 9 || {
  echo "timed out waiting for the SpotActorTransfer runner lock" >&2
  exit 1
}

if [[ "$#" -eq 0 ]]; then
  SCENARIO="all"
else
  SCENARIO="$*"
  SCENARIO="${SCENARIO// /,}"
fi

RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/logs/$RUN_ID"
mkdir -p "$LOG_DIR"
CONFIG_DIR="$(mktemp -d)"

SERVER_PROJECT="$ROOT_DIR/Server/ActorNode/SpotActorTransfer.ActorNode.csproj"
SERVER_BINARY="$ROOT_DIR/Server/ActorNode/bin/Debug/net8.0/SpotActorTransfer.ActorNode"
SESSION_GATEWAY_PROJECT="$ROOT_DIR/Server/SessionGateway/SpotActorTransfer.SessionGateway.csproj"
CLIENT_PROJECT="$ROOT_DIR/Client/SpotActorTransfer.Client.csproj"
LOCAL_READINESS_TIMEOUT_SECONDS=3
PROCESS_EXIT_TIMEOUT_SECONDS=30
LOCAL_READINESS_POLL_SECONDS=0.1
REDIS_READINESS_TIMEOUT_SECONDS=60
HTTP_PROBE_TIMEOUT_SECONDS=3
PLACEMENT_READY_TIMEOUT_SECONDS=20
OWNER_LEASE_TTL_SECONDS=10
REPLACEMENT_READY_TIMEOUT_SECONDS=$((OWNER_LEASE_TTL_SECONDS + 20))

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

pids=()
REDIS_CONTAINER=""

cleanup() {
  local code=$?
  rm -rf "$CONFIG_DIR"
  for pid in "${pids[@]:-}"; do
    kill -- "-$pid" >/dev/null 2>&1 || kill "$pid" >/dev/null 2>&1 || true
  done
  sleep 0.5
  for pid in "${pids[@]:-}"; do
    kill -KILL -- "-$pid" >/dev/null 2>&1 || kill -KILL "$pid" >/dev/null 2>&1 || true
  done
  wait "${pids[@]:-}" >/dev/null 2>&1 || true
  if [[ -n "$REDIS_CONTAINER" ]]; then
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  fi
  if [[ "$code" -ne 0 ]]; then
    echo "E2E failed. Logs: $LOG_DIR" >&2
  fi
}
trap cleanup EXIT

wait_health() {
  local url="$1"
  local name="$2"
  local attempts
  attempts="$(
    python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys
print(max(1, math.ceil(float(sys.argv[1]) / float(sys.argv[2]))))
PY
  )"
  for _ in $(seq 1 "$attempts"); do
    if curl --max-time "$HTTP_PROBE_TIMEOUT_SECONDS" \
      --connect-timeout "$HTTP_PROBE_TIMEOUT_SECONDS" \
      -fsS "$url/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name at $url" >&2
  return 1
}

wait_mesh_ready() {
  local url="$1"
  local name="$2"
  local expected_csv="$3"
  local attempts=$((15 * 10))
  for _ in $(seq 1 "$attempts"); do
    snapshot="$(curl --max-time 2 --connect-timeout 2 -fsS "$url/mesh/ready" 2>/dev/null || true)"
    if python3 - "$snapshot" "$expected_csv" <<'PY'
import json
import sys

try:
    snapshot = json.loads(sys.argv[1])
except json.JSONDecodeError:
    raise SystemExit(1)
expected = [item for item in sys.argv[2].split(",") if item]
peers = snapshot.get("readyPeerRids", [])
if all(any(peer == rid or peer.startswith(rid + "-") for peer in peers) for rid in expected):
    raise SystemExit(0)
raise SystemExit(1)
PY
    then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting for $name RouteMesh readiness at $url snapshot=$snapshot" >&2
  return 1
}

wait_placement_ready() {
  local url="$1"
  local name="$2"
  local deadline=$((SECONDS + PLACEMENT_READY_TIMEOUT_SECONDS))
  while (( SECONDS < deadline )); do
    if curl --max-time 6 --connect-timeout 2 -fsS \
      -H 'Content-Type: application/json' \
      -X POST "$url/placement-weight" \
      --data '{"weight":100}' >/dev/null 2>&1; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  local status
  status="$(curl --max-time 2 --connect-timeout 1 -fsS "$url/mesh/status" 2>/dev/null || true)"
  echo "Timed out waiting ${PLACEMENT_READY_TIMEOUT_SECONDS}s for $name placement readiness at $url status=$status" >&2
  return 1
}

wait_all_mesh_ready() {
  wait_mesh_ready "$NODE_A_URL" actor-a "actor-b,actor-c,actor-d,session-a,session-b"
  wait_mesh_ready "$NODE_B_URL" actor-b "actor-a,actor-c,actor-d,session-a,session-b"
  wait_mesh_ready "$NODE_C_URL" actor-c "actor-a,actor-b,actor-d,session-a,session-b"
  wait_mesh_ready "$NODE_D_URL" actor-d "actor-a,actor-b,actor-c"
}

wait_all_placement_ready() {
  wait_placement_ready "$NODE_A_URL" actor-a
  wait_placement_ready "$NODE_B_URL" actor-b
  wait_placement_ready "$NODE_C_URL" actor-c
  wait_placement_ready "$NODE_D_URL" actor-d
}

wait_tcp_endpoint() {
  local endpoint="$1"
  local name="$2"
  if python3 - "$endpoint" "$name" <<'PY'
import socket
import sys
import time
from urllib.parse import urlparse

endpoint, name = sys.argv[1:]
parsed = urlparse(endpoint)
if parsed.scheme != "tcp" or parsed.hostname is None or parsed.port is None:
    raise SystemExit(f"invalid TCP endpoint for {name}: {endpoint}")
deadline = time.monotonic() + 15
while time.monotonic() < deadline:
    try:
        with socket.create_connection((parsed.hostname, parsed.port), timeout=1):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.1)
raise SystemExit(f"Timed out waiting for {name} TCP endpoint {endpoint}")
PY
  then
    return 0
  fi
  return 1
}

wait_process_exit() {
  local pid="$1"
  local name="$2"
  local attempts=$((PROCESS_EXIT_TIMEOUT_SECONDS * 10))
  for _ in $(seq 1 "$attempts"); do
    if ! kill -0 "$pid" >/dev/null 2>&1; then
      wait "$pid" >/dev/null 2>&1 || true
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${PROCESS_EXIT_TIMEOUT_SECONDS}s for $name process $pid to exit" >&2
  return 1
}

wait_replacement_lease_expiry() {
  # A SIGKILL cannot remove the owner rows. Replacement must wait for the
  # exact old owner lease to expire before the new lifecycle is admitted. The
  # surviving observer and retired owner are scenario-specific: the normal
  # replacement flow uses actor-b to observe retired actor-a, while ST-B5
  # uses actor-c to observe retired actor-b because actor-a is the source of
  # the failed planned relocation and may be draining.
  local retired_rid="${1:-actor-a}"
  local observer_url="${2:-$NODE_B_URL}"
  local observer_name="${3:-actor-b}"
  local deadline=$((SECONDS + REPLACEMENT_READY_TIMEOUT_SECONDS))
  local status
  while (( SECONDS < deadline )); do
    status="$(curl --max-time 2 --connect-timeout 1 -fsS \
      "$observer_url/mesh/status" 2>/dev/null || true)"
    if python3 - "$status" "$retired_rid" <<'PY'
import json
import sys

try:
    status = json.loads(sys.argv[1])
except json.JSONDecodeError:
    raise SystemExit(1)

if status.get("state") != "Ready" or status.get("isReady") is not True:
    raise SystemExit(1)
retired = sys.argv[2]
if any(
    peer.get("rid", "") == retired
    or peer.get("rid", "").startswith(retired + "-")
    for peer in status.get("peers", [])
):
    raise SystemExit(1)
raise SystemExit(0)
PY
    then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${REPLACEMENT_READY_TIMEOUT_SECONDS}s for the old ${retired_rid} owner lease to expire at ${observer_name} ($observer_url) status=$status" >&2
  return 1
}

start_node() {
  local rid="$1"
  local url="$2"
  local router="$3"
  local advertise_host="$4"
  local caller_only="${5:-false}"
  local crash_at_complete_gate="${6:-false}"
  local config="$CONFIG_DIR/$rid.json"
  python3 "$ROOT_DIR/../write_role_config.py" "$config" -- \
    --rid "$rid" \
    --http-url "$url" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --router-endpoint "$router" \
    --router-advertise-host "$advertise_host" \
    --caller-only "$caller_only" \
    --crash-at-target-complete-gate "$crash_at_complete_gate" \
    --request-timeout-milliseconds 3000 \
    --evidence-file "$LOG_DIR/${rid}.evidence.log" \
    --log-dir "$LOG_DIR"
  setsid "$SERVER_BINARY" --config "$config" \
    9>&- \
    >>"$LOG_DIR/${rid}.stdout.log" 2>>"$LOG_DIR/${rid}.stderr.log" &
  pids+=("$!")
}

start_transport_proxy() {
  local name="$1"
  local listen_port="$2"
  local upstream_host="$3"
  local upstream_port="$4"
  local admin_port="$5"
  setsid python3 "$ROOT_DIR/Support/stream_marker_proxy.py" \
    --listen-port "$listen_port" \
    --upstream-host "$upstream_host" \
    --upstream-port "$upstream_port" \
    --admin-port "$admin_port" \
    9>&- \
    >>"$LOG_DIR/$name.stdout.log" 2>>"$LOG_DIR/$name.stderr.log" &
  pids+=("$!")
}

start_session_gateway() {
  local rid="$1" url="$2" router="$3" stream="$4"
  local config="$CONFIG_DIR/$rid.json"
  python3 "$ROOT_DIR/../write_role_config.py" "$config" -- \
    --rid "$rid" \
    --http-url "$url" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --router-endpoint "$router" \
    --stream-endpoint "$stream" \
    --evidence-file "$LOG_DIR/${rid}.evidence.log"
  setsid dotnet run --no-build --project "$SESSION_GATEWAY_PROJECT" -- --config "$config" \
    9>&- \
    >>"$LOG_DIR/${rid}.stdout.log" 2>>"$LOG_DIR/${rid}.stderr.log" &
  pids+=("$!")
}

run_client() {
  local scenario="$1"
  local config="$CONFIG_DIR/client-$scenario.json"
  python3 "$ROOT_DIR/../write_role_config.py" "$config" -- \
    --config-dir "$CONFIG_DIR" \
    --node-a-url "$NODE_A_URL" \
    --node-a-pid "$NODE_A_PID" \
    --node-b-url "$NODE_B_URL" \
    --node-c-url "$NODE_C_URL" \
    --node-d-url "$NODE_D_URL" \
    --transport-proxy-admin "$NODE_A_PROXY_ADMIN" \
    --transport-proxy-admin "$NODE_B_PROXY_ADMIN" \
    --transport-proxy-admin "$NODE_C_PROXY_ADMIN" \
    --transport-proxy-admin "$NODE_D_PROXY_ADMIN" \
    --node-a-stream-endpoint "$SESSION_A_STREAM" \
    --node-b-stream-endpoint "$SESSION_B_STREAM" \
    --scenario "$scenario"
  echo "client_config=$config node-a-stream=$SESSION_A_STREAM node-b-stream=$SESSION_B_STREAM" >&2
  cp "$config" "$LOG_DIR/client-$scenario.config.json"
  dotnet run --no-build --project "$CLIENT_PROJECT" -- --config "$config" \
    9>&- \
    >>"$LOG_DIR/client.stdout.log" 2>>"$LOG_DIR/client.stderr.log"
}

zlink_redis_start_scoped_assign \
  REDIS_CONTAINER \
  REDIS_ENDPOINT \
  "zlink-redis-dotnet-e2e-spot-actor-transfer" \
  "redis:7.2-alpine" \
  "$LOG_DIR"
zlink_redis_wait_ready "$REDIS_CONTAINER" "$REDIS_READINESS_TIMEOUT_SECONDS"
REDIS_KEY_PREFIX="zlink:e2e:spot-actor-transfer:$(date +%s)-$$"

NODE_A_HTTP_PORT="$(pick_port)"
NODE_B_HTTP_PORT="$(pick_port)"
NODE_C_HTTP_PORT="$(pick_port)"
NODE_D_HTTP_PORT="$(pick_port)"
NODE_A_ROUTER_PORT="$(pick_port)"
NODE_B_ROUTER_PORT="$(pick_port)"
NODE_C_ROUTER_PORT="$(pick_port)"
NODE_D_ROUTER_PORT="$(pick_port)"
NODE_A_PROXY_ADMIN_PORT="$(pick_port)"
NODE_B_PROXY_ADMIN_PORT="$(pick_port)"
NODE_C_PROXY_ADMIN_PORT="$(pick_port)"
NODE_D_PROXY_ADMIN_PORT="$(pick_port)"
SESSION_A_HTTP_PORT="$(pick_port)"
SESSION_B_HTTP_PORT="$(pick_port)"
SESSION_A_ROUTER_PORT="$(pick_port)"
SESSION_B_ROUTER_PORT="$(pick_port)"
SESSION_A_STREAM_PORT="$(pick_port)"
SESSION_B_STREAM_PORT="$(pick_port)"
NODE_A_URL="http://127.0.0.1:$NODE_A_HTTP_PORT"
NODE_B_URL="http://127.0.0.1:$NODE_B_HTTP_PORT"
NODE_C_URL="http://127.0.0.1:$NODE_C_HTTP_PORT"
NODE_D_URL="http://127.0.0.1:$NODE_D_HTTP_PORT"
NODE_A_ROUTER="tcp://127.0.0.2:$NODE_A_ROUTER_PORT"
NODE_B_ROUTER="tcp://127.0.0.3:$NODE_B_ROUTER_PORT"
NODE_C_ROUTER="tcp://127.0.0.4:$NODE_C_ROUTER_PORT"
NODE_D_ROUTER="tcp://127.0.0.5:$NODE_D_ROUTER_PORT"
NODE_A_PROXY_ADMIN="http://127.0.0.1:$NODE_A_PROXY_ADMIN_PORT"
NODE_B_PROXY_ADMIN="http://127.0.0.1:$NODE_B_PROXY_ADMIN_PORT"
NODE_C_PROXY_ADMIN="http://127.0.0.1:$NODE_C_PROXY_ADMIN_PORT"
NODE_D_PROXY_ADMIN="http://127.0.0.1:$NODE_D_PROXY_ADMIN_PORT"
SESSION_A_URL="http://127.0.0.1:$SESSION_A_HTTP_PORT"
SESSION_B_URL="http://127.0.0.1:$SESSION_B_HTTP_PORT"
SESSION_A_ROUTER="tcp://127.0.0.1:$SESSION_A_ROUTER_PORT"
SESSION_B_ROUTER="tcp://127.0.0.1:$SESSION_B_ROUTER_PORT"
SESSION_A_STREAM="tcp://127.0.0.1:$SESSION_A_STREAM_PORT"
SESSION_B_STREAM="tcp://127.0.0.1:$SESSION_B_STREAM_PORT"

echo "log_dir=$LOG_DIR"
dotnet build "$SERVER_PROJECT" --maxcpucount:1 9>&- >/dev/null
test -x "$SERVER_BINARY"
dotnet build "$SESSION_GATEWAY_PROJECT" --maxcpucount:1 9>&- >/dev/null
dotnet build "$CLIENT_PROJECT" --maxcpucount:1 9>&- >/dev/null

start_transport_proxy actor-a-proxy "$NODE_A_ROUTER_PORT" \
  127.0.0.2 "$NODE_A_ROUTER_PORT" "$NODE_A_PROXY_ADMIN_PORT"
start_transport_proxy actor-b-proxy "$NODE_B_ROUTER_PORT" \
  127.0.0.3 "$NODE_B_ROUTER_PORT" "$NODE_B_PROXY_ADMIN_PORT"
start_transport_proxy actor-c-proxy "$NODE_C_ROUTER_PORT" \
  127.0.0.4 "$NODE_C_ROUTER_PORT" "$NODE_C_PROXY_ADMIN_PORT"
start_transport_proxy actor-d-proxy "$NODE_D_ROUTER_PORT" \
  127.0.0.5 "$NODE_D_ROUTER_PORT" "$NODE_D_PROXY_ADMIN_PORT"
wait_health "$NODE_A_PROXY_ADMIN" actor-a-proxy
wait_health "$NODE_B_PROXY_ADMIN" actor-b-proxy
wait_health "$NODE_C_PROXY_ADMIN" actor-c-proxy
wait_health "$NODE_D_PROXY_ADMIN" actor-d-proxy

start_node actor-a "$NODE_A_URL" "$NODE_A_ROUTER" 127.0.0.1
NODE_A_PID="${pids[${#pids[@]}-1]}"
TARGET_CRASH_AT_COMPLETE_GATE=false
if [[ "$SCENARIO" == "ST-B5" ]]; then
  TARGET_CRASH_AT_COMPLETE_GATE=true
fi
start_node actor-b "$NODE_B_URL" "$NODE_B_ROUTER" 127.0.0.1 \
  false "$TARGET_CRASH_AT_COMPLETE_GATE"
NODE_B_PID="${pids[${#pids[@]}-1]}"
start_node actor-c "$NODE_C_URL" "$NODE_C_ROUTER" 127.0.0.1
start_node actor-d "$NODE_D_URL" "$NODE_D_ROUTER" 127.0.0.1 true

wait_health "$NODE_A_URL" actor-a
wait_health "$NODE_B_URL" actor-b
wait_health "$NODE_C_URL" actor-c
wait_health "$NODE_D_URL" actor-d
start_session_gateway session-a "$SESSION_A_URL" "$SESSION_A_ROUTER" "$SESSION_A_STREAM"
wait_health "$SESSION_A_URL" session-a
start_session_gateway session-b "$SESSION_B_URL" "$SESSION_B_ROUTER" "$SESSION_B_STREAM"
wait_health "$SESSION_B_URL" session-b
wait_tcp_endpoint "$SESSION_A_STREAM" session-a-stream
wait_tcp_endpoint "$SESSION_B_STREAM" session-b-stream
wait_mesh_ready "$SESSION_A_URL" session-a "actor-a,actor-b,actor-c"
wait_mesh_ready "$SESSION_B_URL" session-b "actor-a,actor-b,actor-c"

: >"$LOG_DIR/client.stdout.log"
: >"$LOG_DIR/client.stderr.log"

if [[ "$SCENARIO" == "ST-B5" ]]; then
  python3 "$ROOT_DIR/Support/kill_on_file_marker.py" \
    --pid "$NODE_B_PID" \
    --path "$LOG_DIR/actor-b.stderr.log" \
    --marker "aggregate_target_complete_gate" \
    --timeout 60 &
  TARGET_CRASH_WATCHER_PID="$!"
  run_client "ST-B5" &
  TARGET_CRASH_CLIENT_PID="$!"
  wait "$TARGET_CRASH_WATCHER_PID"
  wait_process_exit "$NODE_B_PID" actor-b
  # Replacement is allowed only after the exact old owner lease expires.
  wait_replacement_lease_expiry actor-b "$NODE_C_URL" actor-c
  start_node actor-b "$NODE_B_URL" "$NODE_B_ROUTER" 127.0.0.1
  NODE_B_PID="${pids[${#pids[@]}-1]}"
  wait_health "$NODE_B_URL" actor-b
  wait_mesh_ready "$NODE_B_URL" actor-b "actor-c,actor-d,session-a,session-b"
  wait_placement_ready "$NODE_B_URL" actor-b
  wait "$TARGET_CRASH_CLIENT_PID"
elif [[ "$SCENARIO" == "all" ]]; then
  run_client "ST-A1,ST-A2,ST-A3,ST-B1,ST-B3,ST-B4,ST-D1,ST-C3,ST-D2,ST-E1,ST-E1A,ST-E2,ST-F1,ST-F2,ST-F3,ST-F6"
  run_client "ST-B2"
  wait_process_exit "$NODE_A_PID" actor-a
  wait_replacement_lease_expiry actor-a "$NODE_B_URL" actor-b
  NODE_A_HTTP_PORT="$(pick_port)"
  NODE_A_URL="http://127.0.0.1:$NODE_A_HTTP_PORT"
  start_node actor-a "$NODE_A_URL" "$NODE_A_ROUTER" 127.0.0.1
  NODE_A_PID="${pids[${#pids[@]}-1]}"
  wait_health "$NODE_A_URL" actor-a
  wait_mesh_ready "$NODE_A_URL" actor-a "actor-b,actor-c,actor-d,session-a,session-b"
  wait_all_mesh_ready
  wait_all_placement_ready
  run_client "ST-C2"
  wait_process_exit "$NODE_A_PID" actor-a
  wait_replacement_lease_expiry actor-a "$NODE_B_URL" actor-b
  NODE_A_HTTP_PORT="$(pick_port)"
  NODE_A_URL="http://127.0.0.1:$NODE_A_HTTP_PORT"
  start_node actor-a "$NODE_A_URL" "$NODE_A_ROUTER" 127.0.0.1
  NODE_A_PID="${pids[${#pids[@]}-1]}"
  wait_health "$NODE_A_URL" actor-a
  wait_mesh_ready "$NODE_A_URL" actor-a "actor-b,actor-c,actor-d,session-a,session-b"
  wait_all_mesh_ready
  wait_all_placement_ready
  run_client "ST-C1"
else
  run_client "$SCENARIO"
fi

require_runtime_marker() {
  local marker="$1"
  if ! grep -h -q "$marker" "$LOG_DIR"/actor-*.*.log; then
    echo "Missing runtime marker '$marker'. Logs: $LOG_DIR" >&2
    return 1
  fi
}

require_marker_order() {
  local actor_prefix="$1"
  local first="$2"
  local second="$3"
  python3 - "$LOG_DIR" "$actor_prefix" "$first" "$second" <<'PY'
import pathlib
import re
import sys

log_dir, actor_prefix, first, second = sys.argv[1:]
lines = []
for path in pathlib.Path(log_dir).glob("actor-*.*.log"):
    lines.extend(path.read_text().splitlines())
actor_pattern = re.compile(rf"actor=({re.escape(actor_prefix)}[^ ]+)")
actors = {m.group(1) for line in lines if (m := actor_pattern.search(line))}
if len(actors) != 1:
    raise SystemExit(f"Expected one actor for prefix {actor_prefix}, got {sorted(actors)}")
actor = next(iter(actors))
first_index = next((i for i, line in enumerate(lines) if first in line and f"actor={actor}" in line), None)
second_index = next((i for i, line in enumerate(lines) if second in line and f"actor={actor}" in line), None)
if first_index is None or second_index is None or first_index >= second_index:
    raise SystemExit(f"Marker order failed for {actor}: {first}={first_index}, {second}={second_index}")
PY
}

if [[ "$SCENARIO" == "all" || "$SCENARIO" == *"ST-C1"* ]]; then
  require_runtime_marker pending_admission_expired
fi

if [[ "$SCENARIO" == "all" || "$SCENARIO" == *"ST-F1"* ]]; then
  require_runtime_marker handoff_backlog
  require_runtime_marker backlog_enqueued
fi
if [[ "$SCENARIO" == "all" || "$SCENARIO" == *"ST-F2"* ]]; then
  # ST-F2 sends B1/B2 after the target authority is committed. They are
  # target-ingress backlog records, so the runtime records handoff_backlog
  # before replay publishes backlog_enqueued. The public scenario separately
  # asserts B1,B2,D1 handler order.
  require_marker_order actor-inflight-overtake- handoff_backlog backlog_enqueued
fi
if [[ "$SCENARIO" == "all" || "$SCENARIO" == *"ST-F6"* ]]; then
  grep -h -E -q 'handoff_backlog actor=actor-inflight-req-.* kind=Request request_id=[1-9][0-9]* flags=[1-9][0-9]*' "$LOG_DIR"/actor-b.*.log
  grep -h -E -q 'backlog_enqueued actor=actor-inflight-req-.* request_id=[1-9][0-9]* flags=[1-9][0-9]*' "$LOG_DIR"/actor-b.*.log
  grep -h -E -q 'request_reply_direct actor=actor-inflight-req-' "$LOG_DIR"/actor-b.*.log
fi

cat "$LOG_DIR/client.stdout.log"
