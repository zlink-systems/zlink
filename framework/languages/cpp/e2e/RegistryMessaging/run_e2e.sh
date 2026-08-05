#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
BUILD_DIR="$CPP_DIR/build"
SCENARIO="all"
E2E_START_ORDER="forward"
INTERNAL_REDIS_ENDPOINT=""
INTERNAL_REDIS_CONTAINER=""
if (($# > 0)) && [[ "$1" != --* ]]; then
  SCENARIO="$1"
  shift
fi
while (($# > 0)); do
  case "$1" in
    --start-order=*) E2E_START_ORDER="${1#*=}" ;;
    --redis-endpoint=*) INTERNAL_REDIS_ENDPOINT="${1#*=}" ;;
    --redis-container=*) INTERNAL_REDIS_CONTAINER="${1#*=}" ;;
    *) echo "Unknown RegistryMessaging runner option: $1" >&2; exit 2 ;;
  esac
  shift
done
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
PROCESS_SHUTDOWN_TIMEOUT_SECONDS=15
SCENARIO_SETTLE_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
REDIS_READINESS_TIMEOUT_SECONDS=30
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

read -r API_A API_B ROUTE_A ROUTE_B WORKFLOW_A HTTP_A HTTP_B HTTP_WORKFLOW HTTP_DIRECT_CONSUMER HTTP_SINGLE_CONSUMER HTTP_STORE_CONSUMER HTTP_BACKPRESSURE_CONSUMER CLIENT_ROUTE API_A2 ROUTE_A2 HTTP_A2 RM_A3_ROUTE_A RM_A3_ROUTE_B RM_A3_HTTP_A RM_A3_HTTP_B RM_A3_PROXY_A RM_A3_PROXY_B <<<"$(python3 - <<'PY'
import socket

sockets = []
ports = []
for _ in range(22):
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    sockets.append(s)
    ports.append(s.getsockname()[1])
print(" ".join(f"tcp://127.0.0.1:{p}" for p in ports[:5]), end=" ")
print(" ".join(f"http://127.0.0.1:{p}" for p in ports[5:12]), end=" ")
print(f"tcp://127.0.0.1:{ports[12]}", end=" ")
print(" ".join(f"tcp://127.0.0.1:{p}" for p in ports[13:15]), end=" ")
print(f"http://127.0.0.1:{ports[15]}", end=" ")
print(" ".join(f"tcp://127.0.0.1:{p}" for p in ports[16:18]), end=" ")
print(" ".join(f"http://127.0.0.1:{p}" for p in ports[18:20]), end=" ")
print(" ".join(f"tcp://127.0.0.1:{p}" for p in ports[20:22]))
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
REDIS_CONTAINER_OWNED=0
REDIS_KEY_PREFIX="zlink:e2e:cfg1:$(date +%s)-$$"
if [[ -n "$INTERNAL_REDIS_CONTAINER" && -n "$INTERNAL_REDIS_ENDPOINT" ]]; then
  REDIS_ENDPOINT="$INTERNAL_REDIS_ENDPOINT"
  REDIS_CONTAINER="$INTERNAL_REDIS_CONTAINER"
  echo "redis endpoint=$REDIS_ENDPOINT (existing owned container $REDIS_CONTAINER)"
else
  zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
    "zlink-redis-cpp-e2e-registrymessaging" "redis:7-alpine"
  REDIS_CONTAINER_OWNED=1
  REDIS_ENDPOINT="127.0.0.1:${redis_port}"
  echo "redis endpoint=$REDIS_ENDPOINT (container $REDIS_CONTAINER)"
fi
REDIS_HOST="${REDIS_ENDPOINT%:*}"
REDIS_TCP_PORT="${REDIS_ENDPOINT##*:}"
wait_tcp "$REDIS_HOST" "$REDIS_TCP_PORT" redis
echo "redis key prefix=$REDIS_KEY_PREFIX"

cmake -S "$CPP_DIR" -B "$BUILD_DIR" >/dev/null
if [[ "$SCENARIO" == "RM-A3" || "$SCENARIO" == "rm-a3" ]]; then
  cmake --build "$BUILD_DIR" --target \
    zlink_cpp_e2e_registry_messaging_object_client >/dev/null
else
  cmake --build "$BUILD_DIR" --target \
    zlink_cpp_e2e_registry_messaging_provider \
    zlink_cpp_e2e_registry_messaging_workflow \
    zlink_cpp_e2e_registry_messaging_consumer \
    zlink_cpp_e2e_registry_messaging_client >/dev/null
fi

PROVIDER_SERVER="$BUILD_DIR/zlink_cpp_e2e_registry_messaging_provider"
WORKFLOW_SERVER="$BUILD_DIR/zlink_cpp_e2e_registry_messaging_workflow"
CONSUMER_SERVER="$BUILD_DIR/zlink_cpp_e2e_registry_messaging_consumer"
OBJECT_CLIENT_SERVER="$BUILD_DIR/zlink_cpp_e2e_registry_messaging_object_client"
CLIENT="$BUILD_DIR/zlink_cpp_e2e_registry_messaging_client"
PIDS=()
LAST_PID=""

wait_pid_status() {
  local pid="$1"
  local label="$2"
  local status
  set +e
  wait "$pid"
  status=$?
  set -e
  if [[ "$status" -eq 127 ]]; then
    return 0
  fi
  if [[ "$status" -eq 0 || "$status" -eq 130 || "$status" -eq 143 ]]; then
    return 0
  fi
  echo "$label exited unexpectedly with status $status" >&2
  return 1
}

terminate_pid() {
  local pid="$1"
  local label="${2:-process $pid}"
  local state
  if [[ -z "$pid" ]]; then
    return 0
  fi
  if ! kill -0 "$pid" >/dev/null 2>&1; then
    wait_pid_status "$pid" "$label"
    return $?
  fi
  kill "$pid" >/dev/null 2>&1 || true
  for _ in $(seq 1 "$PROCESS_SHUTDOWN_ATTEMPTS"); do
    state="$(ps -o stat= -p "$pid" 2>/dev/null | awk '{print $1}')"
    if [[ -z "$state" || "$state" == Z* ]]; then
      wait_pid_status "$pid" "$label"
      return $?
    fi
    sleep 0.1
  done
  kill -9 "$pid" >/dev/null 2>&1 || true
  wait_pid_status "$pid" "$label"
}

cleanup() {
  local code=$?
  local cleanup_failed=0
  for pid in "${PIDS[@]:-}"; do
    if [[ -z "$pid" ]]; then
      continue
    fi
    kill -CONT "$pid" >/dev/null 2>&1 || true
    if ! terminate_pid "$pid" "cleanup process $pid"; then
      cleanup_failed=1
    fi
  done
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
  local endpoint="$1"
  echo "${endpoint##*:}"
}

wait_port() {
  local name="$1"
  local endpoint="$2"
  local host="127.0.0.1"
  local port
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

start_provider() {
  local rid="$1"
  local api="$2"
  local route="$3"
  local http="$4"
  local instance="${5:-$rid}"
  if (($# >= 5)); then
    shift 5
  else
    shift 4
  fi
  local config_path="$CONFIG_DIR/provider-$rid-$(date +%s%N).json"
  python3 - "$config_path" "$rid" "$instance" "$api" "$route" "$http" \
    "$ROUTE_A,$ROUTE_B" "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$LOG_DIR" "$@" <<'PY'
import json
import os
import stat
import sys

(path, rid, instance, api, route, http, route_peers, redis_endpoint,
 redis_key_prefix, log_dir, *overrides) = sys.argv[1:]
config = {"rid": rid, "instanceId": instance, "apiEndpoint": api,
    "routeEndpoint": route, "httpEndpoint": http, "routePeers": route_peers,
    "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
    "logDir": log_dir}
allowed = {"serverWeight"}
for override in overrides:
    key, separator, value = override.partition("=")
    if not separator or key not in allowed:
        raise SystemExit(f"unknown provider configuration override: {override}")
    config[key] = value
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": config}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$PROVIDER_SERVER" --config="$config_path" \
    >"$LOG_DIR/$rid.stdout.log" 2>"$LOG_DIR/$rid.stderr.log" &
  LAST_PID="$!"
  PIDS+=("$LAST_PID")
  wait_port "$rid-api" "$api"
  wait_port "$rid-route" "$route"
  wait_port "$rid-http" "$http"
}

start_standard_provider_pair() {
  local role
  mapfile -t ordered_server_roles < <(ordered_roles api-a api-b)
  for role in "${ordered_server_roles[@]}"; do
    echo "starting_role=$role"
    case "$role" in
      api-a)
        start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
        API_A_PID="$LAST_PID"
        ;;
      api-b)
        start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
        API_B_PID="$LAST_PID"
        ;;
    esac
  done
}

start_workflow_provider() {
  local rid="$1"
  local workflow="$2"
  local config_path="$CONFIG_DIR/workflow-$rid.json"
  python3 - "$config_path" "$rid" "$workflow" "$HTTP_WORKFLOW" \
    "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

path, rid, workflow, http, redis_endpoint, redis_key_prefix, log_dir = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"rid": rid, "instanceId": rid,
        "workflowEndpoint": workflow, "httpEndpoint": http,
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$WORKFLOW_SERVER" --config="$config_path" \
    >"$LOG_DIR/$rid.stdout.log" 2>"$LOG_DIR/$rid.stderr.log" &
  LAST_PID="$!"
  PIDS+=("$LAST_PID")
  wait_port "$rid-workflow" "$workflow"
  wait_port "$rid-http" "$HTTP_WORKFLOW"
}

start_consumer() {
  local name="$1"
  local http="$2"
  local endpoints="$3"
  local redis_endpoint="$4"
  shift 4
  local config_path="$CONFIG_DIR/consumer-$name.json"
  python3 - "$config_path" "$name" "$http" "$endpoints" "$redis_endpoint" \
    "$REDIS_KEY_PREFIX" "$LOG_DIR" "$@" <<'PY'
import json
import os
import stat
import sys

(path, name, http, endpoints, redis_endpoint, redis_key_prefix, log_dir,
 *overrides) = sys.argv[1:]
config = {"httpEndpoint": http, "providerEndpoints": endpoints,
    "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
    "traceLabel": name, "logDir": log_dir}
if overrides:
    raise SystemExit(
        "consumer configuration overrides are not defined by the current exact public interface: "
        + ", ".join(overrides))
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": config}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$CONSUMER_SERVER" --config="$config_path" \
    >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  LAST_PID="$!"
  PIDS+=("$LAST_PID")
  wait_port "$name-http" "$http"
}

launch_rm_a3_object_client() {
  local rid="$1"
  local route="$2"
  local http="$3"
  local peers="$4"
  local server_weight="${5:-}"
  local config_path="$CONFIG_DIR/object-client-$rid.json"
  python3 - "$config_path" "$rid" "$route" "$http" "$peers" \
    "$server_weight" "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

(path, rid, route, http, peers, server_weight, redis_endpoint,
 redis_key_prefix, log_dir) = sys.argv[1:]
config = {
    "rid": rid,
    "routeEndpoint": route,
    "httpEndpoint": http,
    "peerEndpoints": peers,
    "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
    "logDir": log_dir,
}
if server_weight != "":
    config["serverWeight"] = server_weight
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": config}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$OBJECT_CLIENT_SERVER" --config="$config_path" \
    >"$LOG_DIR/$rid.stdout.log" 2>"$LOG_DIR/$rid.stderr.log" &
  LAST_PID="$!"
  PIDS+=("$LAST_PID")
}

wait_rm_a3_peer_state() {
  local base_url="$1"
  local peer_rid="$2"
  local expected_state="$3"
  local expected_ready="$4"
  local timeout_seconds="${5:-10}"
  python3 - "$base_url" "$peer_rid" "$expected_state" \
    "$expected_ready" "$timeout_seconds" <<'PY'
import json
import sys
import time
import urllib.request

base_url, peer_rid, expected_state, expected_ready, timeout_text = sys.argv[1:]
expected_ready = expected_ready == "true"
deadline = time.monotonic() + float(timeout_text)
last = None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(
                base_url + "/rm-a3/status", timeout=1) as response:
            last = json.load(response)
        matching = [
            peer for peer in last["peers"]
            if peer["nodeRid"] == peer_rid
        ]
        if (len(matching) == 1
                and matching[0]["state"] == expected_state
                and (matching[0]["state"] == "ready") is expected_ready):
            print(json.dumps(last, sort_keys=True))
            raise SystemExit(0)
    except (OSError, ValueError, KeyError):
        pass
    time.sleep(0.1)
raise SystemExit(
    f"timed out waiting for {peer_rid}={expected_state}/"
    f"{expected_ready}; last={last!r}")
PY
}

wait_rm_a3_location_ready() {
  local base_url="$1"
  python3 - "$base_url" <<'PY'
import json
import sys
import time
import urllib.request

base_url = sys.argv[1]
deadline = time.monotonic() + 10
last = None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(
                base_url + "/rm-a3/status", timeout=1) as response:
            last = json.load(response)
        if last.get("isReady") is True:
            print(json.dumps(last, sort_keys=True))
            raise SystemExit(0)
    except (OSError, ValueError):
        pass
    time.sleep(0.1)
raise SystemExit(f"timed out waiting for Location ready: {last!r}")
PY
}

wait_client_server_ready_targets() {
  local base_url="$1"
  local channel="$2"
  local expected_count="$3"
  local timeout_seconds="${4:-30}"
  python3 - "$base_url" "$channel" "$expected_count" "$timeout_seconds" <<'PY'
import json
import sys
import time
import urllib.parse
import urllib.request

base_url, channel, expected_text, timeout_text = sys.argv[1:]
expected = int(expected_text)
deadline = time.monotonic() + float(timeout_text)
last = None
url = base_url.rstrip("/") + "/client-server/status?channel=" + urllib.parse.quote(channel)
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(url, timeout=1) as response:
            last = json.load(response)
        if (last.get("selectable") is True
                and int(last.get("readyServerCount", 0)) >= expected):
            print(json.dumps(last, sort_keys=True))
            raise SystemExit(0)
    except (OSError, ValueError, KeyError, TypeError):
        pass
    time.sleep(0.1)
raise SystemExit(
    f"timed out waiting for ClientServer ready targets channel={channel} "
    f"count={expected}; last={last!r}")
PY
}

verify_rm_a3_not_required_stays_terminal() {
  local base_url="$1"
  local peer_rid="$2"
  local duration_seconds="${3:-20}"
  python3 - "$base_url" "$peer_rid" "$duration_seconds" <<'PY'
import json
import sys
import time
import urllib.request

base_url, peer_rid, duration_text = sys.argv[1:]
deadline = time.monotonic() + float(duration_text)
observations = 0
while time.monotonic() < deadline:
    with urllib.request.urlopen(
            base_url + "/rm-a3/status", timeout=1) as response:
        status = json.load(response)
    matching = [
        peer for peer in status["peers"]
        if peer["nodeRid"] == peer_rid
    ]
    if len(matching) != 1:
        raise SystemExit(
            f"expected one monitored peer {peer_rid}: {status!r}")
    peer = matching[0]
    if (peer["state"] != "not_required"
            or peer["unavailableReason"] != ""
            or status["readyPeerCount"] != 0):
        raise SystemExit(
            f"NotRequired peer entered reconnect/liveness accounting: "
            f"{status!r}")
    observations += 1
    time.sleep(0.25)
print(f"rm-a3 stable-not-required observations={observations}")
PY
}

verify_rm_a3_node_direct_not_found() {
  local base_url="$1"
  local target_rid="$2"
  python3 - "$base_url" "$target_rid" <<'PY'
import json
import sys
import urllib.request

base_url, target_rid = sys.argv[1:]
payload = json.dumps({"targetRid": target_rid}).encode()
request = urllib.request.Request(
    base_url + "/rm-a3/node-direct",
    data=payload,
    headers={"Content-Type": "application/json"},
    method="POST")
with urllib.request.urlopen(request, timeout=3) as response:
    result = json.load(response)
if result.get("terminal") != "NotFound":
    raise SystemExit(f"Object Client Node direct was not NotFound: {result!r}")
if result.get("errorKind") != "request_target_not_found":
    raise SystemExit(f"Object Client Node direct lost typed error: {result!r}")
print("rm-a3 node-direct terminal=NotFound kind=request_target_not_found")
PY
}

launch_rm_a3_proxy() {
  local name="$1"
  local listen_endpoint="$2"
  local upstream_endpoint="$3"
  local evidence="$LOG_DIR/$name-connections.log"
  local ready="$LOG_DIR/$name-ready"
  python3 "$SCRIPT_DIR/Support/tcp_connection_proxy.py" \
    --listen-port "$(port_of "$listen_endpoint")" \
    --upstream-port "$(port_of "$upstream_endpoint")" \
    --evidence "$evidence" \
    --ready "$ready" \
    >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  LAST_PID="$!"
  PIDS+=("$LAST_PID")
  wait_marker "$ready"
}

verify_rm_a3_single_manual_attempt() {
  local evidence="$1"
  local count
  # One public ROUTER connect owns one Application/Completion transport pair.
  # The proxy observes both physical lanes, while the public intent remains one.
  local expected_transport_lanes=2
  count="$(wc -l <"$evidence")"
  if [[ "$count" != "$expected_transport_lanes" ]]; then
    echo "RM-A3 manual intent opened an unexpected number of physical transport lanes: $count; expected $expected_transport_lanes for one logical connect intent: $evidence" >&2
    return 1
  fi
  echo "rm-a3 manual-connect-intents=1 transport-lanes=$expected_transport_lanes evidence=$evidence"
}

stop_pid() {
  local pid="$1"
  if [[ -z "$pid" ]]; then
    return 0
  fi
  if kill -0 "$pid" >/dev/null 2>&1; then
    kill "$pid" >/dev/null 2>&1 || true
    wait_pid_status "$pid" "stopped process $pid"
  fi
}

wait_marker() {
  local file="$1"
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    if [[ -f "$file" ]]; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for marker $file" >&2
  return 1
}

run_client() {
  local scenario="$1"
  local suffix="$2"
  shift 2
  local config_path="$CONFIG_DIR/client-$suffix.json"
  python3 - "$config_path" "$scenario" "$API_A2" "$HTTP_A" "$HTTP_B" "$HTTP_A2" \
    "$HTTP_WORKFLOW" "$HTTP_DIRECT_CONSUMER" "$HTTP_SINGLE_CONSUMER" \
    "$HTTP_STORE_CONSUMER" "$HTTP_BACKPRESSURE_CONSUMER" "$@" <<'PY'
import json
import os
import stat
import sys

(path, scenario, api_a2, http_a, http_b, http_a2, http_workflow,
 direct_consumer, single_consumer, store_consumer, backpressure_consumer,
 *overrides) = sys.argv[1:]
config = {"scenario": scenario, "apiA2Endpoint": api_a2,
    "httpAEndpoint": http_a, "httpBEndpoint": http_b,
    "httpA2Endpoint": http_a2, "httpWorkflowEndpoint": http_workflow,
    "directConsumerUrl": direct_consumer, "singleConsumerUrl": single_consumer,
    "storeConsumerUrl": store_consumer,
    "backpressureConsumerUrl": backpressure_consumer}
allowed = {"readyFile", "continueFile", "singleConsumerUrl"}
for override in overrides:
    key, separator, value = override.partition("=")
    if not separator or key not in allowed:
        raise SystemExit(f"unknown client configuration override: {override}")
    config[key] = value
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": config}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$CLIENT" --config="$config_path" \
    >"$LOG_DIR/client-$suffix.stdout.log" 2>"$LOG_DIR/client-$suffix.stderr.log"
}

if [[ "$SCENARIO" == "all" ]]; then
  CHILD_LOG_MANIFEST="$LOG_DIR/child-runs.log"
  for scenario in RM-A1 RM-A2 RM-A3 RM-A4 RM-A6 RM-B1 RM-B2 RM-C1 RM-C2 RM-C3 RM-C4 RM-C5 RM-C7 RM-C8 RM-C9; do
    echo "running $scenario"
    child_output="$LOG_DIR/child-$scenario.output.log"
    "$0" "$scenario" --redis-endpoint="$REDIS_ENDPOINT" \
      --redis-container="$REDIS_CONTAINER" | tee "$child_output"
    child_log_dir="$(sed -n 's/^log_dir=//p' "$child_output" | tail -1)"
    if [[ -z "$child_log_dir" || ! -d "$child_log_dir" ]]; then
      echo "missing child log directory for $scenario" >&2
      exit 1
    fi
    echo "$scenario $child_log_dir" >>"$CHILD_LOG_MANIFEST"
  done
  echo "registry-messaging e2e result=passed"
  exit 0
fi

case "$SCENARIO" in
  RM-A1|rm-a1|RM-A2|rm-a2|RM-A3|rm-a3|RM-A4|rm-a4|RM-A6|rm-a6|RM-B1|rm-b1|RM-B2|rm-b2|RM-C1|rm-c1|RM-C2|rm-c2|RM-C3|rm-c3|RM-C4|rm-c4|RM-C5|rm-c5|RM-C7|rm-c7|RM-C8|rm-c8|RM-C9|rm-c9)
    ;;
  *)
    echo "Unknown RegistryMessaging scenario: $SCENARIO" >&2
    exit 1
    ;;
esac

if [[ "$SCENARIO" == "RM-A2" || "$SCENARIO" == "rm-a2" ]]; then
  if ! rg -q 'std::async' "$SCRIPT_DIR/Client/Scenarios/rm_a2_manual_endpoint_scenario.hpp" \
    || ! rg -q 'options.single_consumer_url' \
      "$SCRIPT_DIR/Client/Scenarios/rm_a2_manual_endpoint_scenario.hpp"; then
    echo "RM-A2 contract gate failed: manual and auto endpoints do not share one channel" >&2
    exit 1
  fi
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_consumer manual-consumer "$HTTP_SINGLE_CONSUMER" "$API_A" "$REDIS_ENDPOINT"
  SINGLE_CONSUMER_PID="$LAST_PID"
  READY="$LOG_DIR/rm-a2-ready"
  CONTINUE="$LOG_DIR/rm-a2-continue"
  run_client rm-a2 rm-a2 \
    readyFile="$READY" \
    continueFile="$CONTINUE" &
  A2_CLIENT_PID="$!"
  wait_marker "$READY"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  touch "$CONTINUE"
  wait "$A2_CLIENT_PID"
  cat "$LOG_DIR/client-rm-a2.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-A1" || "$SCENARIO" == "rm-a1" ]]; then
  start_standard_provider_pair
  start_consumer store-consumer "$HTTP_STORE_CONSUMER" "" "$REDIS_ENDPOINT"
  STORE_CONSUMER_PID="$LAST_PID"
  run_client rm-a1 rm-a1
  cat "$LOG_DIR/client-rm-a1.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-A3" || "$SCENARIO" == "rm-a3" ]]; then
  echo "rm-a3 phase=automatic-not-required"
  launch_rm_a3_object_client auto-client-a \
    "$RM_A3_ROUTE_A" "$RM_A3_HTTP_A" ""
  RM_A3_A_PID="$LAST_PID"
  launch_rm_a3_object_client auto-client-b \
    "$RM_A3_ROUTE_B" "$RM_A3_HTTP_B" ""
  RM_A3_B_PID="$LAST_PID"
  wait_port "auto-client-a-route" "$RM_A3_ROUTE_A"
  wait_port "auto-client-b-route" "$RM_A3_ROUTE_B"
  wait_port "auto-client-a-http" "$RM_A3_HTTP_A"
  wait_port "auto-client-b-http" "$RM_A3_HTTP_B"
  wait_rm_a3_peer_state "$RM_A3_HTTP_A" auto-client-b not_required false
  wait_rm_a3_peer_state "$RM_A3_HTTP_B" auto-client-a not_required false
  verify_rm_a3_node_direct_not_found \
    "$RM_A3_HTTP_A" auto-client-b
  verify_rm_a3_not_required_stays_terminal \
    "$RM_A3_HTTP_A" auto-client-b 20
  stop_pid "$RM_A3_A_PID"
  stop_pid "$RM_A3_B_PID"

  echo "rm-a3 phase=manual-not-required"
  launch_rm_a3_proxy manual-proxy-a \
    "$RM_A3_PROXY_A" "$RM_A3_ROUTE_A"
  RM_A3_PROXY_A_PID="$LAST_PID"
  launch_rm_a3_proxy manual-proxy-b \
    "$RM_A3_PROXY_B" "$RM_A3_ROUTE_B"
  RM_A3_PROXY_B_PID="$LAST_PID"
  launch_rm_a3_object_client manual-client-a \
    "$RM_A3_ROUTE_A" "$RM_A3_HTTP_A" "$RM_A3_PROXY_B"
  RM_A3_A_PID="$LAST_PID"
  launch_rm_a3_object_client manual-client-b \
    "$RM_A3_ROUTE_B" "$RM_A3_HTTP_B" "$RM_A3_PROXY_A"
  RM_A3_B_PID="$LAST_PID"
  wait_port "manual-client-a-route" "$RM_A3_ROUTE_A"
  wait_port "manual-client-b-route" "$RM_A3_ROUTE_B"
  wait_port "manual-client-a-http" "$RM_A3_HTTP_A"
  wait_port "manual-client-b-http" "$RM_A3_HTTP_B"
  wait_rm_a3_peer_state "$RM_A3_HTTP_A" manual-client-b not_required false
  wait_rm_a3_peer_state "$RM_A3_HTTP_B" manual-client-a not_required false
  verify_rm_a3_not_required_stays_terminal \
    "$RM_A3_HTTP_A" manual-client-b 20
  verify_rm_a3_single_manual_attempt \
    "$LOG_DIR/manual-proxy-a-connections.log"
  verify_rm_a3_single_manual_attempt \
    "$LOG_DIR/manual-proxy-b-connections.log"
  stop_pid "$RM_A3_A_PID"
  stop_pid "$RM_A3_B_PID"
  stop_pid "$RM_A3_PROXY_A_PID"
  stop_pid "$RM_A3_PROXY_B_PID"

  echo "rm-a3 phase=weight-zero-server-required"
  launch_rm_a3_object_client required-client-b \
    "$RM_A3_ROUTE_B" "$RM_A3_HTTP_B" "" 0
  RM_A3_B_PID="$LAST_PID"
  wait_port "required-client-b-route" "$RM_A3_ROUTE_B"
  wait_port "required-client-b-http" "$RM_A3_HTTP_B"
  wait_rm_a3_location_ready "$RM_A3_HTTP_B"

  # Freeze the published weight-0 Server before the other Object Client
  # starts. Its live Location descriptor requires a connection, but no
  # handshake can finish until the process resumes.
  kill -STOP "$RM_A3_B_PID"
  launch_rm_a3_object_client required-client-a \
    "$RM_A3_ROUTE_A" "$RM_A3_HTTP_A" ""
  RM_A3_A_PID="$LAST_PID"
  wait_port "required-client-a-route" "$RM_A3_ROUTE_A"
  wait_port "required-client-a-http" "$RM_A3_HTTP_A"
  wait_rm_a3_peer_state \
    "$RM_A3_HTTP_A" required-client-b not_connected false 10

  kill -CONT "$RM_A3_B_PID"
  wait_rm_a3_peer_state "$RM_A3_HTTP_A" required-client-b ready true
  wait_rm_a3_peer_state "$RM_A3_HTTP_B" required-client-a ready true

  stop_pid "$RM_A3_A_PID"
  stop_pid "$RM_A3_B_PID"
  echo "scenario RM-A3 passed"
  exit 0
fi

if [[ "$SCENARIO" == "RM-A4" || "$SCENARIO" == "rm-a4" ]]; then
  if ! rg -q 'options.store_consumer_url' \
      "$SCRIPT_DIR/Client/Scenarios/rm_a4_same_rid_failover_scenario.hpp" \
    || rg -q 'locations/peers' \
      "$SCRIPT_DIR/Client/Scenarios/rm_a4_same_rid_failover_scenario.hpp"; then
    echo "RM-A4 contract gate failed: persistent consumer public messaging assertions are invalid" >&2
    exit 1
  fi
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A" api-a-v1
  API_A_PID="$LAST_PID"
  start_consumer store-consumer "$HTTP_STORE_CONSUMER" "" "$REDIS_ENDPOINT"
  STORE_CONSUMER_PID="$LAST_PID"
  READY="$LOG_DIR/rm-a4-ready"
  CONTINUE="$LOG_DIR/rm-a4-continue"
  run_client rm-a4 rm-a4 \
    readyFile="$READY" \
    continueFile="$CONTINUE" &
  A4_CLIENT_PID="$!"
  wait_marker "$READY"
  stop_pid "$API_A_PID"
  start_provider api-a "$API_A2" "$ROUTE_A2" "$HTTP_A2" api-a-v2
  API_A_PID="$LAST_PID"
  touch "$CONTINUE"
  wait "$A4_CLIENT_PID"
  cat "$LOG_DIR/client-rm-a4.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-A6" || "$SCENARIO" == "rm-a6" ]]; then
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  start_workflow_provider workflow-a "$WORKFLOW_A"
  WORKFLOW_A_PID="$LAST_PID"
  start_consumer store-consumer "$HTTP_STORE_CONSUMER" "" "$REDIS_ENDPOINT"
  STORE_CONSUMER_PID="$LAST_PID"
  READY="$LOG_DIR/rm-a6-ready"
  CONTINUE="$LOG_DIR/rm-a6-continue"
  run_client rm-a6 rm-a6 \
    readyFile="$READY" \
    continueFile="$CONTINUE" &
  A6_CLIENT_PID="$!"
  wait_marker "$READY"
  stop_pid "$API_B_PID"
  touch "$CONTINUE"
  wait_marker "$READY.workflow"
  stop_pid "$WORKFLOW_A_PID"
  touch "$CONTINUE.workflow"
  wait "$A6_CLIENT_PID"
  cat "$LOG_DIR/client-rm-a6.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-B1" || "$SCENARIO" == "rm-b1" ]]; then
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  READY="$LOG_DIR/rm-b1-ready"
  CONTINUE="$LOG_DIR/rm-b1-continue"
  run_client rm-b1 rm-b1 \
    readyFile="$READY" \
    continueFile="$CONTINUE" &
  B1_CLIENT_PID="$!"
  wait_marker "$READY"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  wait_client_server_ready_targets "$HTTP_A" registry.messaging.api 2 30
  touch "$CONTINUE"
  wait "$B1_CLIENT_PID"
  cat "$LOG_DIR/client-rm-b1.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-B2" || "$SCENARIO" == "rm-b2" ]]; then
  if ! rg -q 'std::async' "$SCRIPT_DIR/Client/Scenarios/rm_b2_scale_in_scenario.hpp" \
    || rg -q 'catch \(\.\.\.\)' \
      "$SCRIPT_DIR/Client/Scenarios/rm_b2_scale_in_scenario.hpp"; then
    echo "RM-B2 contract gate failed: scale-in traffic is not continuously classified" >&2
    exit 1
  fi
  start_standard_provider_pair
  start_consumer store-consumer "$HTTP_STORE_CONSUMER" "" "$REDIS_ENDPOINT"
  STORE_CONSUMER_PID="$LAST_PID"
  READY="$LOG_DIR/rm-b2-ready"
  CONTINUE="$LOG_DIR/rm-b2-continue"
  run_client rm-b2 rm-b2 \
    readyFile="$READY" \
    continueFile="$CONTINUE" &
  B2_CLIENT_PID="$!"
  wait_marker "$READY"
  stop_pid "$API_B_PID"
  touch "$CONTINUE"
  wait "$B2_CLIENT_PID"
  cat "$LOG_DIR/client-rm-b2.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-C1" || "$SCENARIO" == "rm-c1" ]]; then
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  run_client rm-c1 rm-c1
  cat "$LOG_DIR/client-rm-c1.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-C2" || "$SCENARIO" == "rm-c2" ]]; then
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  run_client rm-c2 rm-c2
  cat "$LOG_DIR/client-rm-c2.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-C3" || "$SCENARIO" == "rm-c3" ]]; then
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  start_consumer direct-consumer "$HTTP_DIRECT_CONSUMER" "$API_A,$API_B" ""
  DIRECT_CONSUMER_PID="$LAST_PID"
  run_client rm-c3 rm-c3
  cat "$LOG_DIR/client-rm-c3.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-C4" || "$SCENARIO" == "rm-c4" ]]; then
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  start_consumer store-consumer "$HTTP_STORE_CONSUMER" "" "$REDIS_ENDPOINT"
  STORE_CONSUMER_PID="$LAST_PID"
  wait_client_server_ready_targets "$HTTP_STORE_CONSUMER" registry.messaging.api 2 30
  run_client rm-c4 rm-c4
  cat "$LOG_DIR/client-rm-c4.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-C5" || "$SCENARIO" == "rm-c5" ]]; then
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  start_consumer store-consumer "$HTTP_STORE_CONSUMER" "" "$REDIS_ENDPOINT"
  STORE_CONSUMER_PID="$LAST_PID"
  run_client rm-c5 rm-c5
  cat "$LOG_DIR/client-rm-c5.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-C7" || "$SCENARIO" == "rm-c7" ]]; then
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A" api-a serverWeight=75
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B" api-b serverWeight=25
  API_B_PID="$LAST_PID"
  start_consumer weighted-consumer "$HTTP_SINGLE_CONSUMER" "" "$REDIS_ENDPOINT"
  WEIGHTED_CONSUMER_PID="$LAST_PID"
  run_client rm-c7 rm-c7
  cat "$LOG_DIR/client-rm-c7.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-C8" || "$SCENARIO" == "rm-c8" ]]; then
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  start_consumer single-consumer "$HTTP_SINGLE_CONSUMER" "" "$REDIS_ENDPOINT"
  SINGLE_CONSUMER_PID="$LAST_PID"
  run_client rm-c8 rm-c8
  cat "$LOG_DIR/client-rm-c8.stdout.log"
  exit 0
fi

if [[ "$SCENARIO" == "RM-C9" || "$SCENARIO" == "rm-c9" ]]; then
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_consumer backpressure-consumer "$HTTP_BACKPRESSURE_CONSUMER" "$API_A" ""
  BACKPRESSURE_CONSUMER_PID="$LAST_PID"
  run_client rm-c9 rm-c9
  cat "$LOG_DIR/client-rm-c9.stdout.log"
  exit 0
fi

echo "Unknown RegistryMessaging scenario: $SCENARIO" >&2
exit 1
