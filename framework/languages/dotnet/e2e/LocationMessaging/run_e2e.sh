#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../redis-common.sh"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/logs/$RUN_ID"
mkdir -p "$LOG_DIR"
CONFIG_DIR="$(mktemp -d)"
if [[ "$#" -eq 0 ]]; then
  SCENARIO="all"
else
  SCENARIO="$*"
  SCENARIO="${SCENARIO// /,}"
fi
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
HTTP_PROBE_TIMEOUT_SECONDS=3
REDIS_READINESS_TIMEOUT_SECONDS=60

PROVIDER_APPLICATION_HWM_BYTES=0
if [[ "$SCENARIO" == "RM-C9" ]]; then
  PROVIDER_APPLICATION_HWM_BYTES=1048576
elif [[ "$SCENARIO" == "all" ]]; then
  PROVIDER_APPLICATION_HWM_BYTES=1048576
fi

PROVIDER_PROJECT="$ROOT_DIR/Server/Provider/LocationMessaging.Provider.csproj"
WORKFLOW_PROJECT="$ROOT_DIR/Server/Workflow/LocationMessaging.Workflow.csproj"
CONSUMER_PROJECT="$ROOT_DIR/Server/Consumer/LocationMessaging.Consumer.csproj"
CLIENT_PROJECT="$ROOT_DIR/Client/LocationMessaging.Client.csproj"

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

wait_tcp() {
  local host="$1"
  local port="$2"
  local name="$3"
  if python3 - "$host" "$port" "$REDIS_READINESS_TIMEOUT_SECONDS" <<'PY'
import socket
import sys
import time

host, port, timeout = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
deadline = time.monotonic() + timeout
while time.monotonic() < deadline:
    try:
        with socket.create_connection((host, port), timeout=1):
            sys.exit(0)
    except OSError:
        time.sleep(0.2)
sys.exit(1)
PY
  then
    return 0
  fi
  echo "Timed out waiting ${REDIS_READINESS_TIMEOUT_SECONDS}s for $name at $host:$port" >&2
  return 1
}

REDIS_CONTAINER=""
REDIS_HOST=""
REDIS_TCP_PORT=""
cleanup_redis() {
  if [[ -n "$REDIS_CONTAINER" ]]; then
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  elif [[ -n "$REDIS_HOST" ]] && command -v redis-cli >/dev/null 2>&1; then
    # External instance: delete only this run's keys under its unique prefix.
    redis-cli -h "$REDIS_HOST" -p "$REDIS_TCP_PORT" --scan --pattern "$REDIS_KEY_PREFIX*" 2>/dev/null \
      | xargs -r redis-cli -h "$REDIS_HOST" -p "$REDIS_TCP_PORT" DEL >/dev/null 2>&1 || true
  fi
}

pids=()
cleanup() {
  local code=$?
  rm -rf "$CONFIG_DIR"
  for pid in "${pids[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true
    fi
  done
  sleep 0.5
  for pid in "${pids[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -KILL -- "-$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null || true
    fi
  done
  wait "${pids[@]:-}" 2>/dev/null || true
  cleanup_redis
  if [[ "$code" -ne 0 ]]; then
    echo "E2E failed. Logs: $LOG_DIR" >&2
  fi
}
trap cleanup EXIT

# --- Redis provisioning (doc §3 실행 모델) ---------------------------------
# Honor an externally provided instance; otherwise start a disposable
# container. Every run is isolated by a run-unique key prefix.
REDIS_KEY_PREFIX="zlink:e2e:cfg1:$(date +%s)-$$"
zlink_redis_start_scoped_assign \
  REDIS_CONTAINER \
  REDIS_ENDPOINT \
  "zlink-redis-dotnet-e2e-location-messaging" \
  "redis:7.2-alpine" \
  "$LOG_DIR"
echo "redis endpoint=$REDIS_ENDPOINT (container $REDIS_CONTAINER)"
REDIS_HOST="${REDIS_ENDPOINT%:*}"
REDIS_TCP_PORT="${REDIS_ENDPOINT##*:}"
if [[ -n "$REDIS_CONTAINER" ]]; then
  zlink_redis_wait_ready "$REDIS_CONTAINER" "$REDIS_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS"
fi
wait_tcp "$REDIS_HOST" "$REDIS_TCP_PORT" redis
echo "redis key prefix=$REDIS_KEY_PREFIX"

PROVIDER_A_HTTP_PORT="$(pick_port)"
PROVIDER_B_HTTP_PORT="$(pick_port)"
WORKFLOW_HTTP_PORT="$(pick_port)"
CONSUMER_HTTP_PORT="$(pick_port)"
SINGLE_CONSUMER_HTTP_PORT="$(pick_port)"
STORE_CONSUMER_HTTP_PORT="$(pick_port)"
BACKPRESSURE_CONSUMER_HTTP_PORT="$(pick_port)"
API_A_PORT="$(pick_port)"
API_B_PORT="$(pick_port)"
WORKFLOW_PORT="$(pick_port)"
ROUTE_A_PORT="$(pick_port)"
ROUTE_B_PORT="$(pick_port)"
CLIENT_ROUTE_PORT="$(pick_port)"

API_A="tcp://127.0.0.1:$API_A_PORT"
API_B="tcp://127.0.0.1:$API_B_PORT"
WORKFLOW="tcp://127.0.0.1:$WORKFLOW_PORT"
ROUTE_A="tcp://127.0.0.1:$ROUTE_A_PORT"
ROUTE_B="tcp://127.0.0.1:$ROUTE_B_PORT"
CLIENT_ROUTE="tcp://127.0.0.1:$CLIENT_ROUTE_PORT"

wait_health() {
  local url="$1"
  local name="$2"
  local deadline_ns
  deadline_ns="$(
    python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" <<PY
import sys
import time

timeout = float(sys.argv[1])
print(time.monotonic_ns() + int(timeout * 1_000_000_000))
PY
  )"
  while true; do
    local probe_timeout
    probe_timeout="$(
      python3 - "$deadline_ns" "$HTTP_PROBE_TIMEOUT_SECONDS" <<PY
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
    python3 - "$deadline_ns" "$LOCAL_READINESS_POLL_SECONDS" <<PY
import sys
import time

deadline_ns = int(sys.argv[1])
poll = float(sys.argv[2])
remaining = (deadline_ns - time.monotonic_ns()) / 1_000_000_000
if remaining > 0:
    time.sleep(min(poll, remaining))
PY
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name at $url" >&2
  return 1
}

wait_route_ready() {
  local url="$1"
  local expected_count="$2"
  local name="$3"
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
      -fsS "$url/topology/ready?count=$expected_count" 2>/dev/null \
      | grep -Fq '"ready":true'; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  curl -fsS "$url/topology/ready?count=$expected_count" >&2 || true
  echo >&2
  curl -fsS "http://127.0.0.1:$PROVIDER_A_HTTP_PORT/locations/peers?mesh=profile" >&2 || true
  echo >&2
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name route readiness" >&2
  return 1
}

start_server() {
  local name="$1"
  local project="$2"
  shift
  shift
  local config="$CONFIG_DIR/$name.json"
  python3 "$ROOT_DIR/../write_role_config.py" "$config" -- "$@"
  setsid dotnet run --no-build --project "$project" -- --config "$config" \
    >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  LAST_STARTED_PID="$!"
  pids+=("$LAST_STARTED_PID")
}

wait_object_client_status() {
  local url="$1"
  local peer_state="$2"
  local ready_count="$3"
  local topology_state="$4"
  local name="$5"
  local deadline=$((SECONDS + 40))
  while (( SECONDS < deadline )); do
    local status
    status="$(curl --connect-timeout 0.2 --max-time 1 -fsS \
      "$url/topology/status" 2>/dev/null || true)"
    if [[ -n "$status" ]] && python3 - "$peer_state" "$ready_count" \
      "$topology_state" "$status" <<'PY'
import json
import sys

peer_state, ready_count, topology_state, encoded = sys.argv[1:]
value = json.loads(encoded)
peers = value["peers"]
if not peers:
    raise SystemExit(1)
if any(peer["state"] != peer_state for peer in peers):
    raise SystemExit(1)
if value["readyPeerCount"] != int(ready_count):
    raise SystemExit(1)
if value["state"] != topology_state:
    raise SystemExit(1)
PY
    then
      printf '%s\n' "$status" >"$LOG_DIR/$name.status.json"
      return 0
    fi
    sleep 0.1
  done
  curl -fsS "$url/topology/status" >&2 || true
  echo >&2
  echo "Timed out waiting for $name peer state $peer_state." >&2
  return 1
}

start_object_probe() {
  local name="$1"
  local mesh_name="$2"
  local object_role="$3"
  local http_port="$4"
  local mesh_port="$5"
  local peer_endpoint="${6:-}"
  local route_channel_role="${7:-Client}"
  local route_channel_weight="${8:-100}"
  local independent_topologies="${9:-false}"
  local arguments=(
    --http-url "http://127.0.0.1:$http_port"
    --redis-endpoint "$REDIS_ENDPOINT"
    --redis-key-prefix "$REDIS_KEY_PREFIX"
    --trace-label "$name"
    --mesh-name "$mesh_name"
    --mesh-endpoint "tcp://127.0.0.1:$mesh_port"
    --object-role "$object_role"
    --route-channel-role "$route_channel_role"
    --route-channel-weight "$route_channel_weight"
    --register-independent-topologies "$independent_topologies"
    --log-dir "$LOG_DIR"
  )
  if [[ -n "$peer_endpoint" ]]; then
    arguments+=(--provider-endpoint "$peer_endpoint")
  fi
  start_server "$name" "$CONSUMER_PROJECT" "${arguments[@]}"
}

run_required_client_pair() {
  local name="$1"
  local topology="$2"
  local weight="$3"
  local a_http b_http a_mesh b_mesh
  a_http="$(pick_port)"; b_http="$(pick_port)"
  a_mesh="$(pick_port)"; b_mesh="$(pick_port)"

  if [[ "$topology" == "automatic" ]]; then
    start_object_probe "$name-a" "$name" Client \
      "$a_http" "$a_mesh" "" Server "$weight"
  else
    start_object_probe "$name-a" "$name" Client \
      "$a_http" "$a_mesh" "tcp://127.0.0.1:$b_mesh" Server "$weight"
  fi
  wait_health "http://127.0.0.1:$a_http" "$name-a"

  if [[ "$topology" == "automatic" ]]; then
    start_object_probe "$name-b" "$name" Client \
      "$b_http" "$b_mesh"
  else
    start_object_probe "$name-b" "$name" Client \
      "$b_http" "$b_mesh" "tcp://127.0.0.1:$a_mesh"
  fi
  wait_health "http://127.0.0.1:$b_http" "$name-b"

  wait_object_client_status "http://127.0.0.1:$a_http" \
    Ready 1 Ready "$name-a"
  wait_object_client_status "http://127.0.0.1:$b_http" \
    Ready 1 Ready "$name-b"
}

run_rm_a3() {
  local auto_a_http auto_b_http auto_a_mesh auto_b_mesh
  local manual_a_http manual_b_http manual_a_mesh manual_b_mesh
  local sc_server_http sc_client_http sc_server_mesh sc_client_mesh
  local ss_a_http ss_b_http ss_a_mesh ss_b_mesh
  local nc_server_http nc_client_http nc_server_mesh nc_client_mesh
  local independent_a_http independent_b_http independent_a_mesh independent_b_mesh
  auto_a_http="$(pick_port)"; auto_b_http="$(pick_port)"
  auto_a_mesh="$(pick_port)"; auto_b_mesh="$(pick_port)"
  manual_a_http="$(pick_port)"; manual_b_http="$(pick_port)"
  manual_a_mesh="$(pick_port)"; manual_b_mesh="$(pick_port)"
  sc_server_http="$(pick_port)"; sc_client_http="$(pick_port)"
  sc_server_mesh="$(pick_port)"; sc_client_mesh="$(pick_port)"
  ss_a_http="$(pick_port)"; ss_b_http="$(pick_port)"
  ss_a_mesh="$(pick_port)"; ss_b_mesh="$(pick_port)"
  nc_server_http="$(pick_port)"; nc_client_http="$(pick_port)"
  nc_server_mesh="$(pick_port)"; nc_client_mesh="$(pick_port)"
  independent_a_http="$(pick_port)"; independent_b_http="$(pick_port)"
  independent_a_mesh="$(pick_port)"; independent_b_mesh="$(pick_port)"

  start_object_probe client-auto-a object-client-auto Client \
    "$auto_a_http" "$auto_a_mesh"
  wait_health "http://127.0.0.1:$auto_a_http" client-auto-a
  start_object_probe client-auto-b object-client-auto Client \
    "$auto_b_http" "$auto_b_mesh"
  wait_health "http://127.0.0.1:$auto_b_http" client-auto-b
  wait_object_client_status "http://127.0.0.1:$auto_a_http" \
    NotRequired 0 Ready client-auto-a
  wait_object_client_status "http://127.0.0.1:$auto_b_http" \
    NotRequired 0 Ready client-auto-b

  local auto_b_rid direct_result
  auto_b_rid="$(python3 - "$LOG_DIR/client-auto-a.status.json" <<'PY'
import json
import pathlib
import sys
print(json.loads(pathlib.Path(sys.argv[1]).read_text())["peers"][0]["rid"])
PY
)"
  direct_result="$(curl --connect-timeout 0.5 --max-time 3 -fsS -X POST \
    "http://127.0.0.1:$auto_a_http/node-direct/$auto_b_rid")"
  printf '%s\n' "$direct_result" >"$LOG_DIR/rm-a3-node-direct.json"
  python3 - "$direct_result" <<'PY'
import json
import sys
value = json.loads(sys.argv[1])
if value["send"] != "NotFound":
    raise SystemExit(f"RM-A3 send outcome mismatch: {value}")
if value["request"] != "NotFound":
    raise SystemExit(f"RM-A3 request outcome mismatch: {value}")
if value["peerCountBefore"] != value["peerCountAfter"]:
    raise SystemExit(f"RM-A3 Node direct created a peer: {value}")
if value["readyPeerCountBefore"] != 0 or value["readyPeerCountAfter"] != 0:
    raise SystemExit(f"RM-A3 Node direct created a Ready route: {value}")
PY

  sleep 20
  wait_object_client_status "http://127.0.0.1:$auto_a_http" \
    NotRequired 0 Ready client-auto-a-after-20s
  wait_object_client_status "http://127.0.0.1:$auto_b_http" \
    NotRequired 0 Ready client-auto-b-after-20s

  start_object_probe client-manual-a object-client-manual Client \
    "$manual_a_http" "$manual_a_mesh" "tcp://127.0.0.1:$manual_b_mesh"
  start_object_probe client-manual-b object-client-manual Client \
    "$manual_b_http" "$manual_b_mesh" "tcp://127.0.0.1:$manual_a_mesh"
  wait_health "http://127.0.0.1:$manual_a_http" client-manual-a
  wait_health "http://127.0.0.1:$manual_b_http" client-manual-b
  wait_object_client_status "http://127.0.0.1:$manual_a_http" \
    NotRequired 0 Ready client-manual-a
  wait_object_client_status "http://127.0.0.1:$manual_b_http" \
    NotRequired 0 Ready client-manual-b
  sleep 20
  wait_object_client_status "http://127.0.0.1:$manual_a_http" \
    NotRequired 0 Ready client-manual-a-after-20s
  wait_object_client_status "http://127.0.0.1:$manual_b_http" \
    NotRequired 0 Ready client-manual-b-after-20s

  # A RouteMesh Channel Server membership requires the Object Client pair
  # connection. Weight 0 excludes new channel selection, not peer admission.
  run_required_client_pair client-channel-server-100-auto automatic 100
  run_required_client_pair client-channel-server-0-auto automatic 0
  run_required_client_pair client-channel-server-100-manual manual 100
  run_required_client_pair client-channel-server-0-manual manual 0

  # ClientServer and classic fanout use independent sockets and discovery.
  # Their presence must not change the RouteMesh Object Client predicate.
  start_object_probe client-independent-a object-client-independent Client \
    "$independent_a_http" "$independent_a_mesh" "" Client 100 true
  wait_health "http://127.0.0.1:$independent_a_http" client-independent-a
  start_object_probe client-independent-b object-client-independent Client \
    "$independent_b_http" "$independent_b_mesh" "" Client 100 true
  wait_health "http://127.0.0.1:$independent_b_http" client-independent-b
  wait_object_client_status "http://127.0.0.1:$independent_a_http" \
    NotRequired 0 Ready client-independent-a
  wait_object_client_status "http://127.0.0.1:$independent_b_http" \
    NotRequired 0 Ready client-independent-b

  start_object_probe control-sc-server object-control-sc Server \
    "$sc_server_http" "$sc_server_mesh"
  local sc_server_pid="$LAST_STARTED_PID"
  wait_health "http://127.0.0.1:$sc_server_http" control-sc-server
  start_object_probe control-sc-client object-control-sc Client \
    "$sc_client_http" "$sc_client_mesh"
  local sc_client_pid="$LAST_STARTED_PID"
  wait_health "http://127.0.0.1:$sc_client_http" control-sc-client
  wait_object_client_status "http://127.0.0.1:$sc_server_http" \
    Ready 1 Ready control-sc-server
  wait_object_client_status "http://127.0.0.1:$sc_client_http" \
    Ready 1 Ready control-sc-client

  start_object_probe control-ss-a object-control-ss Server \
    "$ss_a_http" "$ss_a_mesh"
  wait_health "http://127.0.0.1:$ss_a_http" control-ss-a
  start_object_probe control-ss-b object-control-ss Server \
    "$ss_b_http" "$ss_b_mesh"
  wait_health "http://127.0.0.1:$ss_b_http" control-ss-b
  wait_object_client_status "http://127.0.0.1:$ss_a_http" \
    Ready 1 Ready control-ss-a
  wait_object_client_status "http://127.0.0.1:$ss_b_http" \
    Ready 1 Ready control-ss-b

  start_object_probe control-nc-client object-control-nc Client \
    "$nc_client_http" "$nc_client_mesh"
  local nc_client_pid="$LAST_STARTED_PID"
  wait_health "http://127.0.0.1:$nc_client_http" control-nc-client
  kill -KILL -- "-$nc_client_pid" 2>/dev/null || kill -KILL "$nc_client_pid"
  start_object_probe control-nc-server object-control-nc Server \
    "$nc_server_http" "$nc_server_mesh"
  wait_health "http://127.0.0.1:$nc_server_http" control-nc-server
  wait_object_client_status "http://127.0.0.1:$nc_server_http" \
    NotConnected 0 Degraded control-nc-server-not-connected

  {
    echo "automatic_client_pair_not_required=pass"
    echo "automatic_ready_peer_count=0"
    echo "manual_client_pair_not_required=pass"
    echo "manual_retry_window_seconds=20"
    echo "manual_ready_peer_count=0"
    echo "automatic_channel_server_weight_100_ready=pass"
    echo "automatic_channel_server_weight_0_ready=pass"
    echo "manual_channel_server_weight_100_ready=pass"
    echo "manual_channel_server_weight_0_ready=pass"
    echo "client_server_and_fanout_not_required=pass"
    echo "server_client_ready=pass"
    echo "server_server_ready=pass"
    echo "required_peer_not_connected=pass"
    echo "node_direct_send=NotFound"
    echo "node_direct_request=NotFound"
    echo "node_direct_peer_count_unchanged=pass"
  } >"$LOG_DIR/rm-a3.evidence.log"
  echo "RM-A3 PASS logs=$LOG_DIR"
}

echo "log_dir=$LOG_DIR"

if [[ "$SCENARIO" == "RM-A3" ]]; then
  dotnet build "$CONSUMER_PROJECT" --maxcpucount:1 >/dev/null
  run_rm_a3
  exit 0
fi

dotnet build "$PROVIDER_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$WORKFLOW_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$CONSUMER_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$CLIENT_PROJECT" --maxcpucount:1 >/dev/null

start_server api-a "$PROVIDER_PROJECT" \
  --role provider \
  --rid api-a \
  --http-url "http://127.0.0.1:$PROVIDER_A_HTTP_PORT" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --channel-endpoint "$API_A" \
  --application-hwm-bytes "$PROVIDER_APPLICATION_HWM_BYTES" \
  --route-endpoint "$ROUTE_A" \
  --route-peer "$ROUTE_B" \
  --weight 100 \
  --evidence-file "$LOG_DIR/api-a.evidence.log" \
  --log-dir "$LOG_DIR"
wait_health "http://127.0.0.1:$PROVIDER_A_HTTP_PORT" api-a

start_server api-b "$PROVIDER_PROJECT" \
  --role provider \
  --rid api-b \
  --http-url "http://127.0.0.1:$PROVIDER_B_HTTP_PORT" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --channel-endpoint "$API_B" \
  --application-hwm-bytes "$PROVIDER_APPLICATION_HWM_BYTES" \
  --route-endpoint "$ROUTE_B" \
  --route-peer "$ROUTE_A" \
  --weight 100 \
  --evidence-file "$LOG_DIR/api-b.evidence.log" \
  --log-dir "$LOG_DIR"
wait_health "http://127.0.0.1:$PROVIDER_B_HTTP_PORT" api-b

start_server workflow-a "$WORKFLOW_PROJECT" \
  --role workflow \
  --rid workflow-a \
  --http-url "http://127.0.0.1:$WORKFLOW_HTTP_PORT" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --workflow-endpoint "$WORKFLOW" \
  --weight 100 \
  --evidence-file "$LOG_DIR/workflow-a.evidence.log" \
  --log-dir "$LOG_DIR"
wait_health "http://127.0.0.1:$WORKFLOW_HTTP_PORT" workflow-a

start_server direct-consumer "$CONSUMER_PROJECT" \
  --http-url "http://127.0.0.1:$CONSUMER_HTTP_PORT" \
  --provider-endpoint "$API_A" \
  --provider-endpoint "$API_B" \
  --trace-label direct-consumer \
  --log-dir "$LOG_DIR"
wait_health "http://127.0.0.1:$CONSUMER_HTTP_PORT" direct-consumer

start_server single-consumer "$CONSUMER_PROJECT" \
  --http-url "http://127.0.0.1:$SINGLE_CONSUMER_HTTP_PORT" \
  --provider-endpoint "$API_A" \
  --trace-label single-consumer \
  --log-dir "$LOG_DIR"
wait_health "http://127.0.0.1:$SINGLE_CONSUMER_HTTP_PORT" single-consumer

start_server store-consumer "$CONSUMER_PROJECT" \
  --http-url "http://127.0.0.1:$STORE_CONSUMER_HTTP_PORT" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --trace-label store-consumer \
  --log-dir "$LOG_DIR"
wait_health "http://127.0.0.1:$STORE_CONSUMER_HTTP_PORT" store-consumer

start_server backpressure-consumer "$CONSUMER_PROJECT" \
  --http-url "http://127.0.0.1:$BACKPRESSURE_CONSUMER_HTTP_PORT" \
  --provider-endpoint "$API_A" \
  --trace-label backpressure-consumer \
  --log-dir "$LOG_DIR"
wait_health "http://127.0.0.1:$BACKPRESSURE_CONSUMER_HTTP_PORT" backpressure-consumer

wait_route_ready "http://127.0.0.1:$CONSUMER_HTTP_PORT" 2 direct-consumer
wait_route_ready "http://127.0.0.1:$SINGLE_CONSUMER_HTTP_PORT" 1 single-consumer
wait_route_ready "http://127.0.0.1:$STORE_CONSUMER_HTTP_PORT" 2 store-consumer
wait_route_ready "http://127.0.0.1:$BACKPRESSURE_CONSUMER_HTTP_PORT" 1 backpressure-consumer

set +e
python3 "$ROOT_DIR/../write_role_config.py" "$CONFIG_DIR/client.json" -- \
    --config-dir "$CONFIG_DIR" \
  --provider-a-url "http://127.0.0.1:$PROVIDER_A_HTTP_PORT" \
  --provider-b-url "http://127.0.0.1:$PROVIDER_B_HTTP_PORT" \
  --workflow-url "http://127.0.0.1:$WORKFLOW_HTTP_PORT" \
  --direct-consumer-url "http://127.0.0.1:$CONSUMER_HTTP_PORT" \
  --single-consumer-url "http://127.0.0.1:$SINGLE_CONSUMER_HTTP_PORT" \
  --store-consumer-url "http://127.0.0.1:$STORE_CONSUMER_HTTP_PORT" \
  --backpressure-consumer-url "http://127.0.0.1:$BACKPRESSURE_CONSUMER_HTTP_PORT" \
  --redis-endpoint "$REDIS_ENDPOINT" \
  --redis-key-prefix "$REDIS_KEY_PREFIX" \
  --provider-project "$PROVIDER_PROJECT" \
  --consumer-project "$CONSUMER_PROJECT" \
  --workflow-project "$WORKFLOW_PROJECT" \
  --log-dir "$LOG_DIR" \
  --scenario "$SCENARIO"
dotnet run --no-build --project "$CLIENT_PROJECT" -- --config "$CONFIG_DIR/client.json" \
  > >(tee "$LOG_DIR/client.stdout.log") \
  2> >(tee "$LOG_DIR/client.stderr.log" >&2)
client_status=$?
set -e
exit "$client_status"
