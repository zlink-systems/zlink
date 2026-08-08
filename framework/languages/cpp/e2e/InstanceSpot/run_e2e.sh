#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$CPP_DIR/build"
source "$SCRIPT_DIR/../redis-common.sh"

SCENARIO="${1:-all}"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$RUN_ID"
CONFIG_DIR="$LOG_DIR/config"
mkdir -p "$CONFIG_DIR"

PIDS=()
REDIS_CONTAINER=""
REDIS_CONTAINER_OWNED=0

cleanup() {
  local code=$?
  set +e
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then
      kill -TERM "$pid" >/dev/null 2>&1 || true
    fi
  done
  for pid in "${PIDS[@]:-}"; do
    wait "$pid" >/dev/null 2>&1 || true
  done
  rm -rf -- "$CONFIG_DIR"
  if [[ -n "$REDIS_CONTAINER" && "$REDIS_CONTAINER_OWNED" == "1" ]]; then
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  fi
  if [[ "$code" != "0" ]]; then
    echo "InstanceSpot failed; logs=$LOG_DIR" >&2
    for log in "$LOG_DIR"/*.stderr.log; do
      [[ -f "$log" ]] || continue
      echo "===== $log =====" >&2
      tail -n 120 "$log" >&2 || true
    done
  fi
  exit "$code"
}
trap cleanup EXIT

cmake -S "$CPP_DIR" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" --target \
  zlink_cpp_e2e_instance_spot_role \
  zlink_cpp_e2e_instance_spot_client >/dev/null

zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-cpp-e2e-instance-spot" "redis:7-alpine"
REDIS_CONTAINER_OWNED=1
REDIS_ENDPOINT="127.0.0.1:${redis_port}"

read -r OWNER_HTTP OWNER_B_HTTP CALLER_HTTP OWNER_MESH OWNER_B_MESH CALLER_MESH < <(python3 - <<'PY'
import socket
sockets = []
ports = []
try:
    for _ in range(6):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
finally:
    for sock in sockets:
        sock.close()
print(*(f"http://127.0.0.1:{ports[i]}" if i < 3 else f"tcp://127.0.0.1:{ports[i]}" for i in range(6)))
PY
)

wait_http() {
  local endpoint="$1"
  for _ in $(seq 1 100); do
    if python3 - "$endpoint/health" <<'PY'
import sys
import urllib.request
try:
    with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
        response.read()
except Exception:
    raise SystemExit(1)
PY
    then
      return 0
    fi
    sleep 0.05
  done
  echo "Timed out waiting for $endpoint" >&2
  return 1
}

write_config() {
  local path="$1" role="$2" rid="$3" http_endpoint="$4" mesh_endpoint="$5" peer_rid="$6" peer_endpoint="$7" second_peer_rid="$8" second_peer_endpoint="$9"
  python3 - "$path" "$role" "$rid" "$http_endpoint" "$mesh_endpoint" "$peer_rid" "$peer_endpoint" \
    "$second_peer_rid" "$second_peer_endpoint" "$REDIS_ENDPOINT" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

(path, role, rid, http_endpoint, mesh_endpoint, peer_rid, peer_endpoint,
 second_peer_rid, second_peer_endpoint,
 redis_endpoint, log_dir) = sys.argv[1:]
value = {
    "e2e": {
        "role": role, "rid": rid, "httpEndpoint": http_endpoint,
        "meshEndpoint": mesh_endpoint, "redis": {
            "endpoint": redis_endpoint,
            "keyPrefix": "zlink:e2e:instance-spot"
        }, "logDir": log_dir,
        "evidenceFile": os.path.join(log_dir, rid + ".evidence.jsonl")
    }
}
if peer_rid:
    value["e2e"]["peerRid"] = peer_rid
    value["e2e"]["peerEndpoint"] = peer_endpoint
if second_peer_rid:
    value["e2e"]["secondPeerRid"] = second_peer_rid
    value["e2e"]["secondPeerEndpoint"] = second_peer_endpoint
with open(path, "w", encoding="utf-8") as output:
    json.dump(value, output)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

write_config "$CONFIG_DIR/owner.json" owner instance-owner "$OWNER_HTTP" "$OWNER_MESH" "" "" "" ""
write_config "$CONFIG_DIR/owner-b.json" owner instance-owner-b "$OWNER_B_HTTP" "$OWNER_B_MESH" "" "" "" ""
write_config "$CONFIG_DIR/caller.json" caller instance-caller "$CALLER_HTTP" "$CALLER_MESH" \
  instance-owner "$OWNER_MESH" \
  "$([[ "$SCENARIO" == "IS-E2E-05" ]] && echo instance-owner-b)" \
  "$([[ "$SCENARIO" == "IS-E2E-05" ]] && echo "$OWNER_B_MESH")"
python3 - "$CONFIG_DIR/client.json" "$CALLER_HTTP" "$OWNER_HTTP" <<'PY'
import json
import os
import stat
import sys

path, caller, owner = sys.argv[1:]
with open(path, "w", encoding="utf-8") as output:
    json.dump({"e2e": {"callerUrl": caller, "ownerUrl": owner, "ownerRid": "instance-owner"}}, output)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY

"$BUILD_DIR/zlink_cpp_e2e_instance_spot_role" --config="$CONFIG_DIR/owner.json" \
  >"$LOG_DIR/owner.stdout.log" 2>"$LOG_DIR/owner.stderr.log" &
PIDS+=("$!")
OWNER_PID="${PIDS[-1]}"
if [[ "$SCENARIO" == "IS-E2E-05" ]]; then
  "$BUILD_DIR/zlink_cpp_e2e_instance_spot_role" --config="$CONFIG_DIR/owner-b.json" \
    >"$LOG_DIR/owner-b.stdout.log" 2>"$LOG_DIR/owner-b.stderr.log" &
  PIDS+=("$!")
  OWNER_B_PID="${PIDS[-1]}"
fi
"$BUILD_DIR/zlink_cpp_e2e_instance_spot_role" --config="$CONFIG_DIR/caller.json" \
  >"$LOG_DIR/caller.stdout.log" 2>"$LOG_DIR/caller.stderr.log" &
PIDS+=("$!")
wait_http "$OWNER_HTTP"
if [[ "$SCENARIO" == "IS-E2E-05" ]]; then
  wait_http "$OWNER_B_HTTP"
fi
wait_http "$CALLER_HTTP"

ready=0
for _ in $(seq 1 100); do
  if python3 - "$CALLER_HTTP/ready?targetRid=instance-owner" <<'PY'
import sys
import urllib.request
try:
    with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
        if response.status < 400:
            raise SystemExit(0)
except Exception:
    pass
raise SystemExit(1)
PY
  then
    ready=1
    break
  fi
  sleep 0.05
done
if [[ "$ready" != "1" ]]; then
  echo "Timed out waiting for Instance Spot target readiness" >&2
  exit 1
fi

if [[ "$SCENARIO" == "IS-E2E-05" ]]; then
  for _ in $(seq 1 100); do
    if python3 - "$CALLER_HTTP/ready?targetRid=instance-owner-b" <<'PY'
import sys
import urllib.request
try:
    with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
        raise SystemExit(0 if response.status < 400 else 1)
except Exception:
    raise SystemExit(1)
PY
    then
      break
    fi
    sleep 0.05
  done

  python3 - "$CALLER_HTTP/instance/request" <<'PY'
import json
import sys
import urllib.request
body = json.dumps({"spotId": "IS-E2E-05-spot", "operationId": "IS-E2E-05-prime", "action": "IS-E2E-05"}).encode()
request = urllib.request.Request(sys.argv[1], data=body, headers={"Content-Type": "application/json"})
with urllib.request.urlopen(request, timeout=10) as response:
    if response.status >= 400:
        raise SystemExit(1)
PY

  for _ in $(seq 1 100); do
    if rg -q '"operationId":"IS-E2E-05-prime"' \
      "$LOG_DIR/instance-owner.evidence.jsonl" \
      "$LOG_DIR/instance-owner-b.evidence.jsonl" 2>/dev/null; then
      break
    fi
    sleep 0.05
  done
  if rg -q '"operationId":"IS-E2E-05-prime"' "$LOG_DIR/instance-owner.evidence.jsonl" 2>/dev/null; then
    kill -KILL "$OWNER_PID"
    wait "$OWNER_PID" 2>/dev/null || true
    SURVIVOR_HTTP="$OWNER_B_HTTP"
  elif rg -q '"operationId":"IS-E2E-05-prime"' "$LOG_DIR/instance-owner-b.evidence.jsonl" 2>/dev/null; then
    kill -KILL "$OWNER_B_PID"
    wait "$OWNER_B_PID" 2>/dev/null || true
    SURVIVOR_HTTP="$OWNER_HTTP"
  else
    echo "IS-E2E-05 could not identify the Ready owner" >&2
    exit 1
  fi

  # owner_lease_ttl is five seconds. Wait beyond the public lease boundary
  # before asserting that the surviving owner does not take over.
  sleep 6
fi

"$BUILD_DIR/zlink_cpp_e2e_instance_spot_client" --config="$CONFIG_DIR/client.json" "$SCENARIO" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
cat "$LOG_DIR/client.stdout.log"
if [[ "$SCENARIO" == "IS-E2E-05" ]] && \
   rg -q '"operationId":"IS-E2E-05-after-crash"' \
     "$LOG_DIR/instance-owner.evidence.jsonl" \
     "$LOG_DIR/instance-owner-b.evidence.jsonl" 2>/dev/null; then
  echo "IS-E2E-05 handler entered after the Ready owner crash" >&2
  exit 1
fi
if [[ "$SCENARIO" == "IS-E2E-05" ]]; then
  python3 - "$SURVIVOR_HTTP/lifecycle" <<'PY'
import json
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=2) as response:
    lifecycle = json.load(response)
if lifecycle.get("materializedInstances", -1) != 0:
    raise SystemExit("surviving owner materialized a replacement Instance Spot")
PY
fi
echo "InstanceSpot PASS scenario=$SCENARIO logs=$LOG_DIR"
