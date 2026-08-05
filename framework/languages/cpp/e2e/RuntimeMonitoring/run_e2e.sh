#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRAMEWORK_DIR="$(cd "$ROOT_DIR/../.." && pwd)"
source "$ROOT_DIR/../redis-common.sh"
BUILD_DIR="${ZLINK_CPP_E2E_BUILD_DIR:-${ZLINK_CPP_BUILD_DIR:-$FRAMEWORK_DIR/build-redis-vcpkg}}"
SCENARIO="${1:-all}"
SCENARIO_LOWER="$(printf '%s' "$SCENARIO" | tr '[:upper:]' '[:lower:]')"
case "$SCENARIO_LOWER" in
  all|mon-a[1-5]|mon-b[1-2]|mon-c1|mon-d1) ;;
  *)
    echo "Unsupported RuntimeMonitoring scenario: $SCENARIO" >&2
    exit 2
    ;;
esac
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
ROUTE_SETTLE_SECONDS=5
LOCAL_READINESS_ATTEMPTS="$(
  python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/logs/$RUN_ID"
mkdir -p "$LOG_DIR"
CONFIG_DIR="$LOG_DIR/config"
mkdir -p "$CONFIG_DIR"

echo "log_dir=$LOG_DIR"

read -r CHANNEL CHANNEL_FILTERED CHANNEL_THROW SPOT_ROUTER_SERVICE SPOT_ROUTER_FILTERED SPOT_ROUTER_THROW SPOT_PUB_SERVICE SPOT_PUB_FILTERED SPOT_PUB_THROW CHANNEL_REMAP SPOT_ROUTER_REMAP SPOT_PUB_REMAP MESH_SERVICE MESH_FILTERED MESH_THROW HTTP_SERVICE HTTP_FILTERED HTTP_THROW HTTP_TRIGGER HTTP_SERVICE_REMAP <<<"$(python3 - <<'PY'
import socket
sockets = []
ports = []
for _ in range(20):
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    sockets.append(s)
    ports.append(s.getsockname()[1])
print(" ".join(f"tcp://127.0.0.1:{p}" for p in ports[:15]), end=" ")
print(" ".join(f"http://127.0.0.1:{p}" for p in ports[15:20]))
for s in sockets:
    s.close()
PY
)"

cmake -S "$FRAMEWORK_DIR" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" --target \
  zlink_cpp_e2e_runtime_monitoring_service \
  zlink_cpp_e2e_runtime_monitoring_filtered_service \
  zlink_cpp_e2e_runtime_monitoring_throwing_service \
  zlink_cpp_e2e_runtime_monitoring_trigger \
  zlink_cpp_e2e_runtime_monitoring_client

SERVICE="$BUILD_DIR/zlink_cpp_e2e_runtime_monitoring_service"
FILTERED_SERVICE="$BUILD_DIR/zlink_cpp_e2e_runtime_monitoring_filtered_service"
THROWING_SERVICE="$BUILD_DIR/zlink_cpp_e2e_runtime_monitoring_throwing_service"
TRIGGER="$BUILD_DIR/zlink_cpp_e2e_runtime_monitoring_trigger"
CLIENT="$BUILD_DIR/zlink_cpp_e2e_runtime_monitoring_client"
zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-cpp-e2e-runtimemonitoring" "redis:7-alpine"
REDIS_ENDPOINT="127.0.0.1:${redis_port}"
REDIS_KEY_PREFIX="zlink:cpp:runtime-monitoring:${RUN_ID}"
PIDS=()

cleanup() {
  local code=$?
  local cleanup_failed=0
  local status
  for pid in "${PIDS[@]}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
  done
  for pid in "${PIDS[@]}"; do
    set +e
    wait "$pid" >/dev/null 2>&1
    status=$?
    set -e
    if [[ "$status" != "0" && "$status" != "127" && "$status" != "130" && "$status" != "137" && "$status" != "143" ]]; then
      echo "cleanup process $pid exited unexpectedly with status $status" >&2
      cleanup_failed=1
    fi
  done
  docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  rm -rf "$CONFIG_DIR"
  if [[ $code -ne 0 ]]; then
    echo "E2E failed. Logs: $LOG_DIR" >&2
  elif [[ "$cleanup_failed" -ne 0 ]]; then
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
  local port
  port="$(port_of "$endpoint")"
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    if (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name at $endpoint" >&2
  return 1
}

wait_port redis "$REDIS_ENDPOINT"

write_service_config() {
  local path="$1"
  local rid="$2"
  local http_endpoint="$3"
  local channel_endpoint="$4"
  local router_endpoint="$5"
  local pub_endpoint="$6"
  local evidence_file="$7"
  local monitor_profile="$8"
  local mesh_endpoint="$9"
  local mesh_peer_endpoints="${10}"
  python3 - "$path" "$rid" "$http_endpoint" "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" \
    "$channel_endpoint" "$router_endpoint" "$pub_endpoint" "$evidence_file" \
    "$monitor_profile" "$LOG_DIR" "$mesh_endpoint" "$mesh_peer_endpoints" <<'PY'
import json
import os
import stat
import sys

(path, rid, http_endpoint, redis_endpoint, redis_key_prefix, channel_endpoint,
 router_endpoint, pub_endpoint, evidence_file, monitor_profile, log_dir,
 mesh_endpoint, mesh_peer_endpoints) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"rid": rid, "httpEndpoint": http_endpoint,
                       "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
                       "channelEndpoint": channel_endpoint,
                       "spotRouterEndpoint": router_endpoint,
                       "spotPubEndpoint": pub_endpoint,
                       "evidenceFile": evidence_file,
                       "monitorProfile": monitor_profile,
                       "logDir": log_dir,
                       "meshEndpoint": mesh_endpoint,
                       "meshPeerEndpoints": mesh_peer_endpoints}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

write_trigger_config() {
  local path="$1"
  python3 - "$path" "$HTTP_TRIGGER" "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$CHANNEL" \
    "$CHANNEL_FILTERED" "$CHANNEL_THROW" "$LOG_DIR/trigger.evidence.log" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

(path, http_endpoint, redis_endpoint, redis_key_prefix, service_channel,
 service_b_channel, throw_channel, evidence_file, log_dir) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"rid": "trigger", "httpEndpoint": http_endpoint,
                       "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
                       "serviceChannelEndpoint": service_channel,
                       "serviceBChannelEndpoint": service_b_channel,
                       "throwChannelEndpoint": throw_channel,
                       "evidenceFile": evidence_file, "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

wait_port_closed() {
  local name="$1"
  local endpoint="$2"
  local port
  port="$(port_of "$endpoint")"
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    if ! (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name to stop at $endpoint" >&2
  return 1
}

crash_service() {
  local pid="$1"
  local name="$2"
  local endpoint="$3"
  kill -KILL "$pid"
  wait "$pid" >/dev/null 2>&1 || true
  wait_port_closed "$name" "$endpoint"
}

stop_service() {
  local pid="$1"
  local name="$2"
  local endpoint="$3"
  python3 - "$endpoint" <<'PY'
import sys
import urllib.request

base = sys.argv[1]
request = urllib.request.Request(f"{base}/shutdown", data=b"", method="POST")
with urllib.request.urlopen(request, timeout=5) as response:
    if response.status >= 400:
        raise RuntimeError(f"shutdown failed with status {response.status}")
PY
  wait "$pid" >/dev/null 2>&1 || true
  wait_port_closed "$name" "$endpoint"
}

request_profile() {
  local marker="$1"
  python3 - "$HTTP_TRIGGER" "$marker" <<'PY'
import json
import sys
import urllib.request

base, marker = sys.argv[1:]
body = json.dumps({"value": "availability", "marker": marker}).encode()
request = urllib.request.Request(
    f"{base}/profile/request",
    data=body,
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request, timeout=5) as response:
    if response.status >= 400:
        raise RuntimeError(f"profile request failed with status {response.status}")
PY
}

wait_trigger_route_state() {
  local rid="$1"
  local endpoint="$2"
  python3 - "$HTTP_TRIGGER" "$rid" "$endpoint" <<'PY'
import json
import sys
import time
import urllib.request

base, rid, endpoint = sys.argv[1:]
deadline = time.monotonic() + 25
while time.monotonic() < deadline:
    with urllib.request.urlopen(f"{base}/evidence", timeout=2) as response:
        entries = json.load(response)
    topology = next(
        (entry for entry in reversed(entries)
         if "monitor-location|" in entry and "kind=TopologyChanged|" in entry),
        "",
    )
    present = f"{rid}@" in topology
    if endpoint == "absent" and not present:
        break
    if endpoint != "absent" and f"{rid}@{endpoint}" in topology:
        break
    time.sleep(0.1)
else:
    raise RuntimeError(f"latest trigger route did not reach {rid}@{endpoint}")
PY
}

wait_mesh_peer_ready() {
  local rid="$1"
  local expected="$2"
  python3 - "$HTTP_SERVICE" "$rid" "$expected" <<'PY'
import json
import sys
import time
import urllib.request

base, rid, expected = sys.argv[1:]
wanted = expected == "true"
deadline = time.monotonic() + 25
while time.monotonic() < deadline:
    with urllib.request.urlopen(f"{base}/runtime/snapshot", timeout=2) as response:
        snapshot = json.load(response)
    peers = [peer for peer in snapshot["peers"] if peer["rid"] == rid]
    ready = any(peer["state"] == "ready" for peer in peers)
    if ready == wanted:
        break
    time.sleep(0.1)
else:
    raise RuntimeError(
        f"RouteMesh peer {rid} readiness did not become {expected}")
PY
}

start_service_a() {
  local channel_endpoint="$1"
  local router_endpoint="$2"
  local pub_endpoint="$3"
  local http_endpoint="$4"
  local label="$5"
  local config_path="$CONFIG_DIR/$label.json"
  # Config 7 uses Location Store discovery. Manual peer endpoints would keep
  # the route outside the discovery owner and hide removal/replacement state.
  local mesh_peers=""
  write_service_config "$config_path" svc-a "$http_endpoint" "$channel_endpoint" \
    "$router_endpoint" "$pub_endpoint" "$LOG_DIR/$label.evidence.log" all \
    "$MESH_SERVICE" "$mesh_peers"
  "$SERVICE" --config="$config_path" \
    >"$LOG_DIR/$label.stdout.log" 2>"$LOG_DIR/$label.stderr.log" &
  SERVICE_PID="$!"
  PIDS+=("$SERVICE_PID")
  wait_port "$label" "$http_endpoint"
}

start_service_b() {
  local label="$1"
  local config_path="$CONFIG_DIR/$label.json"
  local mesh_peers=""
  write_service_config "$config_path" svc-b "$HTTP_FILTERED" "$CHANNEL_FILTERED" \
    "$SPOT_ROUTER_FILTERED" "$SPOT_PUB_FILTERED" "$LOG_DIR/$label.evidence.log" \
    socket-filter "$MESH_FILTERED" "$mesh_peers"
  "$FILTERED_SERVICE" --config="$config_path" \
    >"$LOG_DIR/$label.stdout.log" 2>"$LOG_DIR/$label.stderr.log" &
  FILTERED_PID="$!"
  PIDS+=("$FILTERED_PID")
  wait_port "$label" "$HTTP_FILTERED"
}

start_service_a "$CHANNEL" "$SPOT_ROUTER_SERVICE" "$SPOT_PUB_SERVICE" \
  "$HTTP_SERVICE" service
start_service_b filtered

write_service_config "$CONFIG_DIR/throw.json" svc-throw "$HTTP_THROW" "$CHANNEL_THROW" \
  "$SPOT_ROUTER_THROW" "$SPOT_PUB_THROW" "$LOG_DIR/throw.evidence.log" throwing \
  "$MESH_THROW" ""
"$THROWING_SERVICE" --config="$CONFIG_DIR/throw.json" \
  >"$LOG_DIR/throw.stdout.log" 2>"$LOG_DIR/throw.stderr.log" &
PIDS+=("$!")
wait_port throwing-service "$HTTP_THROW"

write_trigger_config "$CONFIG_DIR/trigger.json"
"$TRIGGER" --config="$CONFIG_DIR/trigger.json" \
  >"$LOG_DIR/trigger.stdout.log" 2>"$LOG_DIR/trigger.stderr.log" &
PIDS+=("$!")
wait_port trigger "$HTTP_TRIGGER"

sleep "$ROUTE_SETTLE_SECONDS"

write_client_config() {
  local scenario="$1"
  local old_channel="${2-}"
  local new_channel="${3-}"
  python3 - "$CONFIG_DIR/client.json" "$scenario" "$HTTP_SERVICE" "$HTTP_FILTERED" \
    "$HTTP_THROW" "$HTTP_TRIGGER" "$LOG_DIR" "$old_channel" "$new_channel" <<'PY'
import json
import os
import stat
import sys

(path, scenario, service_url, filtered_service_url, throw_service_url,
 trigger_url, log_dir, old_channel, new_channel) = sys.argv[1:]
document = {"e2e": {
    "scenario": scenario,
    "serviceUrl": service_url,
    "filteredServiceUrl": filtered_service_url,
    "throwServiceUrl": throw_service_url,
    "triggerUrl": trigger_url,
    "logDir": log_dir,
}}
if old_channel:
    document["e2e"]["oldServiceChannelEndpoint"] = old_channel
if new_channel:
    document["e2e"]["newServiceChannelEndpoint"] = new_channel
with open(path, "w", encoding="utf-8") as file:
    json.dump(document, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

if [[ "$SCENARIO_LOWER" == "mon-a5" ]]; then
  python3 - "$HTTP_SERVICE" <<'PY'
import json
import sys
import time
import urllib.request

base = sys.argv[1]
request = urllib.request.Request(f"{base}/runtime/observe", data=b"", method="POST")
with urllib.request.urlopen(request, timeout=5) as response:
    if response.status >= 400:
        raise RuntimeError("failed to start public RouteMesh observer")
deadline = time.monotonic() + 15
while time.monotonic() < deadline:
    with urllib.request.urlopen(f"{base}/runtime/snapshot", timeout=2) as response:
        snapshot = json.load(response)
    if snapshot["location"]["state"] == "ready":
        break
    time.sleep(0.1)
else:
    raise RuntimeError("location runtime did not become ready before Redis pause")
PY
  docker pause "$REDIS_CONTAINER" >/dev/null
  python3 - "$HTTP_SERVICE" <<'PY'
import json
import sys
import time
import urllib.request

base = sys.argv[1]
deadline = time.monotonic() + 30
while time.monotonic() < deadline:
    with urllib.request.urlopen(f"{base}/runtime/snapshot", timeout=2) as response:
        snapshot = json.load(response)
    if snapshot["location"]["state"] == "degraded":
        if sum(1 for peer in snapshot["peers"] if peer["ready"]) < 2:
            raise RuntimeError("admitted peers were lost during store-only failure")
        break
    time.sleep(0.2)
else:
    raise RuntimeError("location runtime did not become degraded during Redis pause")
PY
  request_profile mon-a5-store-degraded
  docker unpause "$REDIS_CONTAINER" >/dev/null
  python3 - "$HTTP_SERVICE" <<'PY'
import json
import sys
import time
import urllib.request

base = sys.argv[1]
deadline = time.monotonic() + 30
while time.monotonic() < deadline:
    with urllib.request.urlopen(f"{base}/runtime/snapshot", timeout=2) as response:
        snapshot = json.load(response)
    if snapshot["location"]["state"] == "ready":
        break
    time.sleep(0.2)
else:
    raise RuntimeError("location runtime did not recover after Redis restart")
PY
fi

if [[ "$SCENARIO_LOWER" != "mon-a4" && "$SCENARIO_LOWER" != "mon-d1" ]]; then
  write_client_config "$SCENARIO_LOWER"
  "$CLIENT" --config="$CONFIG_DIR/client.json" \
    >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"

  cat "$LOG_DIR/client.stdout.log"
  grep -q "runtime-monitoring client result=passed" "$LOG_DIR/client.stdout.log"
  if [[ "$SCENARIO_LOWER" == "all" || "$SCENARIO_LOWER" == "mon-c1" ]]; then
    grep -q "mesh-request-completed|target=svc-throw|marker=mon-c1-infrastructure" \
      "$LOG_DIR/filtered.evidence.log"
    grep -q "claim-domain=application|reason=active" \
      "$LOG_DIR/filtered.evidence.log"
    grep -q "claim-domain=application|reason=released" \
      "$LOG_DIR/filtered.evidence.log"
  fi
fi

if [[ "$SCENARIO_LOWER" == "all" || "$SCENARIO_LOWER" == "mon-a4" ]]; then
  python3 - "$HTTP_SERVICE" <<'PY'
import sys
import urllib.request

base = sys.argv[1]
request = urllib.request.Request(f"{base}/runtime/observe", data=b"", method="POST")
with urllib.request.urlopen(request, timeout=5) as response:
    if response.status >= 400:
        raise RuntimeError("failed to start public RouteMesh observer")
PY
  wait_mesh_peer_ready svc-b true
  stop_service "$FILTERED_PID" filtered-service-normal "$HTTP_FILTERED"
  wait_mesh_peer_ready svc-b false
  start_service_b filtered-normal-restart
  wait_mesh_peer_ready svc-b true
  python3 - "$HTTP_SERVICE" <<'PY'
import json
import sys
import urllib.request

base = sys.argv[1]
body = json.dumps({"value": "normal-restart", "marker": "mon-a4-normal"}).encode()
request = urllib.request.Request(
    f"{base}/mesh/profile/request?targetRid=svc-b", data=body,
    headers={"Content-Type": "application/json"}, method="POST")
with urllib.request.urlopen(request, timeout=5) as response:
    if json.load(response)["provider_rid"] != "svc-b":
        raise RuntimeError("normal replacement request reached the wrong peer")
PY
  crash_service "$FILTERED_PID" filtered-service-crash "$HTTP_FILTERED"
  wait_mesh_peer_ready svc-b false
  python3 - "$HTTP_SERVICE" <<'PY'
import json
import sys
import urllib.error
import urllib.request

base = sys.argv[1]
body = json.dumps({"value": "crash-window", "marker": "mon-a4-crash"}).encode()
request = urllib.request.Request(
    f"{base}/mesh/profile/request?targetRid=svc-b", data=body,
    headers={"Content-Type": "application/json"}, method="POST")
try:
    urllib.request.urlopen(request, timeout=5)
except urllib.error.HTTPError:
    pass
else:
    raise RuntimeError("crashed peer remained a successful ready route")
PY
  start_service_b filtered-crash-restart
  wait_mesh_peer_ready svc-b true
  python3 - "$HTTP_SERVICE" <<'PY'
import json
import sys
import urllib.request

base = sys.argv[1]
body = json.dumps({"value": "crash-restart", "marker": "mon-a4-recovered"}).encode()
request = urllib.request.Request(
    f"{base}/mesh/profile/request?targetRid=svc-b", data=body,
    headers={"Content-Type": "application/json"}, method="POST")
with urllib.request.urlopen(request, timeout=5) as response:
    if json.load(response)["provider_rid"] != "svc-b":
        raise RuntimeError("crash recovery request reached the wrong peer")
PY

  write_client_config mon-a4
  "$CLIENT" --config="$CONFIG_DIR/client.json" \
    >"$LOG_DIR/client-a4.stdout.log" 2>"$LOG_DIR/client-a4.stderr.log"

  cat "$LOG_DIR/client-a4.stdout.log"
  grep -q "scenario MON-A4 passed" "$LOG_DIR/client-a4.stdout.log"
fi

if [[ "$SCENARIO_LOWER" == "all" || "$SCENARIO_LOWER" == "mon-d1" ]]; then
  python3 - "$HTTP_SERVICE" <<'PY'
import sys
import urllib.request

base = sys.argv[1]
request = urllib.request.Request(f"{base}/runtime/observe", data=b"", method="POST")
with urllib.request.urlopen(request, timeout=5) as response:
    if response.status >= 400:
        raise RuntimeError("failed to start public RouteMesh observer")
PY
  MON_D1_CYCLES=3
  for cycle in $(seq 1 "$MON_D1_CYCLES"); do
    crash_service "$FILTERED_PID" "filtered-service-cycle-$cycle" "$HTTP_FILTERED"
    wait_mesh_peer_ready svc-b false
    start_service_b "filtered-restart-$cycle"
    wait_mesh_peer_ready svc-b true
  done

  write_client_config mon-d1
  "$CLIENT" --config="$CONFIG_DIR/client.json" \
    >"$LOG_DIR/client-d1.stdout.log" 2>"$LOG_DIR/client-d1.stderr.log"

  cat "$LOG_DIR/client-d1.stdout.log"
  grep -q "scenario MON-D1 passed" "$LOG_DIR/client-d1.stdout.log"
fi

echo "runtime-monitoring e2e result=passed"
