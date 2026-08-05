#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
BUILD_DIR="${BUILD_DIR:-$CPP_DIR/build}"
SCENARIO="${*:-all}"
SCENARIO="${SCENARIO// /,}"

should_run() {
  local scenario
  local candidate
  IFS=',' read -ra SELECTED_SCENARIOS <<<"$SCENARIO"
  for scenario in "${SELECTED_SCENARIOS[@]}"; do
    if [[ "$scenario" == "all" ]]; then
      return 0
    fi
    for candidate in "$@"; do
      if [[ "$scenario" == "$candidate" ]]; then
        return 0
      fi
    done
  done
  return 1
}

validate_selector() {
  local scenario
  IFS=',' read -ra SELECTED_SCENARIOS <<<"$SCENARIO"
  for scenario in "${SELECTED_SCENARIOS[@]}"; do
    case "$scenario" in
      all|RL-consumer|rl-consumer|RL-A[1-5]|rl-a[1-5]|RL-B[1-6]|rl-b[1-6]|RL-C[1-4]|rl-c[1-4]|RL-D[1-5]|rl-d[1-5])
        ;;
      *)
        echo "Unsupported ResilienceLifecycle scenario: $scenario" >&2
        exit 2
        ;;
    esac
  done
}

validate_selector
if [[ "$SCENARIO" == "all" ]]; then
  for scenario in RL-consumer RL-A1 RL-A2 RL-A3 RL-A4 RL-A5 RL-B1 RL-B2 RL-B3 RL-B4 \
    RL-B5 RL-B6 RL-C1 RL-C2 RL-C3 RL-C4 RL-D1 RL-D2 RL-D3 RL-D4; do
    "$0" "$scenario"
  done
  echo "resilience-lifecycle e2e result=passed"
  exit 0
fi
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
ROUTE_SETTLE_SECONDS=5
SCENARIO_SETTLE_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
SCENARIO_MARKER_TIMEOUT_SECONDS=30
TOPOLOGY_WAIT_TIMEOUT_MILLISECONDS=30000
TOPOLOGY_WAIT_HTTP_TIMEOUT_SECONDS=35
LOCAL_READINESS_ATTEMPTS="$(
  python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
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

read -r API_A API_B ROUTE_A ROUTE_B API_B_GREEN ROUTE_B_GREEN HTTP_A HTTP_B HTTP_CONSUMER HTTP_B_GREEN CLIENT_ROUTE <<<"$(python3 - <<'PY'
import socket

sockets = []
ports = []
for _ in range(11):
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    sockets.append(s)
    ports.append(s.getsockname()[1])
print(" ".join(f"tcp://127.0.0.1:{p}" for p in ports[:6]), end=" ")
print(" ".join(f"http://127.0.0.1:{p}" for p in ports[6:10]), end=" ")
print(f"tcp://127.0.0.1:{ports[10]}")
for s in sockets:
    s.close()
PY
)"

RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$RUN_ID"
CONFIG_DIR="$LOG_DIR/config"
REDIS_CONTAINER=""
REDIS_OWNED=0
REDIS_KEY_PREFIX="zlink:e2e:cfg5:$RUN_ID:"
mkdir -p "$LOG_DIR"
mkdir -p "$CONFIG_DIR"
echo "log_dir=$LOG_DIR"

cmake -S "$CPP_DIR" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" --target \
  zlink_cpp_e2e_resilience_lifecycle_provider \
  zlink_cpp_e2e_resilience_lifecycle_consumer \
  zlink_cpp_e2e_resilience_lifecycle_client >/dev/null

PROVIDER="$BUILD_DIR/zlink_cpp_e2e_resilience_lifecycle_provider"
CONSUMER="$BUILD_DIR/zlink_cpp_e2e_resilience_lifecycle_consumer"
CLIENT="$BUILD_DIR/zlink_cpp_e2e_resilience_lifecycle_client"
PIDS=()
LAST_PID=""
API_A_PID=""
API_B_PID=""
API_B_GREEN_PID=""
CONSUMER_PID=""

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

forget_pid() {
  local target="$1"
  local remaining=()
  local pid
  for pid in "${PIDS[@]:-}"; do
    if [[ -n "$pid" && "$pid" != "$target" ]]; then
      remaining+=("$pid")
    fi
  done
  PIDS=("${remaining[@]}")
}

terminate_pid() {
  local pid="$1"
  local label="${2:-process $pid}"
  local state
  if [[ -z "$pid" ]]; then
    return 0
  fi
  if ! kill -0 "$pid" >/dev/null 2>&1; then
    wait_pid_status "$pid" "$label" 0 130 143
    return $?
  fi
  kill "$pid" >/dev/null 2>&1 || true
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    state="$(ps -o stat= -p "$pid" 2>/dev/null | awk '{print $1}' || true)"
    if [[ -z "$state" || "$state" == Z* ]]; then
      wait_pid_status "$pid" "$label" 0 130 143
      return $?
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "$label did not stop within ${LOCAL_READINESS_TIMEOUT_SECONDS}s; thread wait states:" >&2
  for task in "/proc/$pid/task"/*; do
    [[ -e "$task" ]] || continue
    echo "  tid=${task##*/} comm=$(cat "$task/comm" 2>/dev/null || true) wchan=$(cat "$task/wchan" 2>/dev/null || true)" >&2
  done
  kill -9 "$pid" >/dev/null 2>&1 || true
  wait_pid_status "$pid" "$label" 0 130 143
}

cleanup() {
  local code=$?
  local cleanup_failed=0
  for pid in "${PIDS[@]:-}"; do
    if [[ -z "$pid" ]]; then
      continue
    fi
    if ! terminate_pid "$pid" "cleanup process $pid"; then
      cleanup_failed=1
    fi
  done
  if [[ -n "$REDIS_CONTAINER" && "$REDIS_OWNED" == "1" ]]; then
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  elif [[ -n "${REDIS_ENDPOINT:-}" ]]; then
    local redis_host redis_port
    redis_host="${REDIS_ENDPOINT%:*}"
    redis_port="${REDIS_ENDPOINT##*:}"
    if command -v redis-cli >/dev/null 2>&1; then
      redis-cli -h "$redis_host" -p "$redis_port" --scan --pattern "$REDIS_KEY_PREFIX*" 2>/dev/null \
        | xargs -r redis-cli -h "$redis_host" -p "$redis_port" DEL >/dev/null 2>&1 || true
    fi
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

wait_tcp() {
  local host="$1"
  local port="$2"
  local name="$3"
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
    if (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name at $host:$port" >&2
  return 1
}

wait_http_health() {
  local name="$1"
  local http="$2"
  python3 - "$name" "$http/health" "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import sys
import time
import urllib.request

name = sys.argv[1]
url = sys.argv[2]
deadline = time.monotonic() + float(sys.argv[3])
poll_seconds = float(sys.argv[4])
probe_timeout = float(sys.argv[5])
last = None
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(url, timeout=probe_timeout) as response:
            if response.status == 200:
                raise SystemExit(0)
            last = f"status {response.status}"
    except Exception as error:
        last = str(error)
    time.sleep(poll_seconds)
sys.stderr.write(f"Timed out waiting for {name} health at {url}: {last}\n")
raise SystemExit(1)
PY
}

wait_marker() {
  local file="$1"
  for _ in $(seq 1 "$SCENARIO_MARKER_ATTEMPTS"); do
    if [[ -f "$file" ]]; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${SCENARIO_MARKER_TIMEOUT_SECONDS}s for marker $file" >&2
  return 1
}

stop_pid() {
  local pid="$1"
  if [[ -z "$pid" ]]; then
    return 0
  fi
  if kill -0 "$pid" >/dev/null 2>&1; then
    terminate_pid "$pid" "stopped process $pid"
    forget_pid "$pid"
  fi
}

kill_pid() {
  local pid="$1"
  if [[ -z "$pid" ]]; then
    return 0
  fi
  if kill -0 "$pid" >/dev/null 2>&1; then
    kill -9 "$pid" >/dev/null 2>&1 || true
    wait_pid_status "$pid" "killed process $pid" 137
    forget_pid "$pid"
  fi
}

crash_provider() {
  local http="$1"
  local pid="$2"
  python3 - "$http" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import sys
import urllib.request

base = sys.argv[1]
probe_timeout = float(sys.argv[2])
request = urllib.request.Request(f"{base}/admin/crash", data=b"", method="POST")
with urllib.request.urlopen(request, timeout=probe_timeout):
    pass
PY
  if python3 - "$http" "$HTTP_PROBE_TIMEOUT_SECONDS" "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import sys
import time
import urllib.request

base = sys.argv[1]
probe_timeout = float(sys.argv[2])
readiness_timeout = float(sys.argv[3])
poll_seconds = float(sys.argv[4])
deadline = time.monotonic() + readiness_timeout
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(f"{base}/health", timeout=probe_timeout) as response:
            if response.status != 200:
                raise SystemExit(0)
    except Exception:
        raise SystemExit(0)
    time.sleep(poll_seconds)
raise SystemExit(1)
PY
  then
    wait_pid_status "$pid" "crashed provider $pid" 134
    forget_pid "$pid"
    return $?
  fi
  echo "Timed out waiting for provider crash at $http" >&2
  return 1
}

set_server_weight() {
  local http="$1"
  local weight="$2"
  local path="/admin/server-weight?weight=$weight"
  if [[ "$weight" == "0" ]]; then
    path="/admin/drain"
  elif [[ "$weight" == "100" ]]; then
    path="/admin/restore"
  fi
  python3 - "$http" "$weight" "$path" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import json
import sys
import urllib.request

base = sys.argv[1]
weight = sys.argv[2]
path = sys.argv[3]
timeout_seconds = float(sys.argv[4])
request = urllib.request.Request(
    f"{base}{path}",
    data=b"",
    method="POST",
)
with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
    if response.status != 200:
        raise SystemExit(f"unexpected status {response.status}")
wait = urllib.request.Request(
    f"{base}/admin/weight/wait",
    data=json.dumps({"expected": int(weight)}).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(wait, timeout=timeout_seconds) as response:
    if response.status != 200:
        raise SystemExit(f"unexpected weight wait status {response.status}")
PY
}

post_consumer_profile() {
  local marker="$1"
  post_consumer_profile_request "/profile/request" "$marker" "" ""
}

wait_consumer_profile_ready() {
  local name="$1"
  local count="$2"
  python3 - "$HTTP_CONSUMER" "$name" "$count" "$SCENARIO_MARKER_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import json
import sys
import time
import urllib.request

base = sys.argv[1]
name = sys.argv[2]
count = int(sys.argv[3])
timeout_seconds = float(sys.argv[4])
poll_seconds = float(sys.argv[5])
deadline = time.monotonic() + timeout_seconds
last = None
for index in range(1, count + 1):
    marker = f"{name}-{index}"
    while time.monotonic() < deadline:
        try:
            request = urllib.request.Request(
                f"{base}/profile/request",
                data=json.dumps({"value": marker}).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(request, timeout=5.0) as response:
                body = response.read().decode("utf-8")
                payload = json.loads(body)
                if response.status == 200 and payload.get("value") == f"profile:{marker}":
                    break
                last = f"unexpected payload {payload}"
        except Exception as error:
            last = str(error)
        time.sleep(poll_seconds)
    else:
        raise SystemExit(
            f"Timed out waiting {timeout_seconds}s for {name} profile readiness: {last}"
        )
PY
}

post_consumer_profile_request() {
  local path="$1"
  local value="$2"
  local marker="$3"
  local expected_provider="${4:-}"
  python3 - "$HTTP_CONSUMER" "$path" "$value" "$marker" "$expected_provider" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import json
import sys
import urllib.request

base = sys.argv[1]
path = sys.argv[2]
value = sys.argv[3]
marker = sys.argv[4]
expected_provider = sys.argv[5]
timeout_seconds = float(sys.argv[6])
payload = {"value": value}
if marker:
    payload["marker"] = marker
request = urllib.request.Request(
    f"{base}{path}",
    data=json.dumps(payload).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
    body = response.read().decode("utf-8")
    if response.status != 200:
        raise SystemExit(f"unexpected status {response.status}: {body}")
    payload = json.loads(body)
    if payload.get("value") != f"profile:{value}":
        raise SystemExit(f"unexpected payload {payload}")
    if marker and payload.get("marker") != marker:
        raise SystemExit(f"unexpected marker payload {payload}")
    if expected_provider and payload.get("provider_rid") != expected_provider:
        raise SystemExit(f"unexpected payload {payload}")
PY
}

post_consumer_profile_burst() {
  local value="$1"
  local marker_prefix="$2"
  local count="$3"
  python3 - "$HTTP_CONSUMER" "$value" "$marker_prefix" "$count" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import concurrent.futures
import json
import sys
import urllib.error
import urllib.request

base = sys.argv[1]
value = sys.argv[2]
marker_prefix = sys.argv[3]
count = int(sys.argv[4])
timeout_seconds = float(sys.argv[5])

def post(index):
    marker = f"{marker_prefix}{index}"
    request = urllib.request.Request(
        f"{base}/profile/request",
        data=json.dumps({"value": value, "marker": marker}).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        response = urllib.request.urlopen(request, timeout=timeout_seconds)
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"unexpected status {error.code} for {marker}: {body}") from error
    with response:
        body = response.read().decode("utf-8")
        if response.status != 200:
            raise RuntimeError(f"unexpected status {response.status} for {marker}: {body}")
        payload = json.loads(body)
        if payload.get("value") != f"profile:{value}":
            raise RuntimeError(f"unexpected payload {payload}")

with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
    list(executor.map(post, range(count)))
PY
}

post_consumer_command_burst() {
  local marker_prefix="$1"
  local count="$2"
  python3 - "$HTTP_CONSUMER" "$marker_prefix" "$count" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import concurrent.futures
import json
import sys
import urllib.request

base = sys.argv[1]
marker_prefix = sys.argv[2]
count = int(sys.argv[3])
timeout_seconds = float(sys.argv[4])

def post(index):
    marker = f"{marker_prefix}{index}"
    request = urllib.request.Request(
        f"{base}/profile/command",
        data=json.dumps({"command_id": marker, "marker": marker}).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
        body = response.read().decode("utf-8")
        if response.status != 200:
            raise RuntimeError(f"unexpected status {response.status}: {body}")
        payload = json.loads(body)
        if payload.get("status") != "sent":
            raise RuntimeError(f"unexpected command payload {payload}")

with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
    list(executor.map(post, range(count)))
PY
}

wait_provider_evidence_value_prefix() {
  local prefix="$1"
  local expected_provider="${2:-}"
  python3 - "$HTTP_A" "$HTTP_B" "$prefix" "$expected_provider" "$SCENARIO_MARKER_TIMEOUT_SECONDS" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import json
import sys
import time
import urllib.request

urls = [sys.argv[1], sys.argv[2]]
prefix = sys.argv[3]
expected_provider = sys.argv[4]
deadline = time.monotonic() + float(sys.argv[5])
probe_timeout = float(sys.argv[6])
while time.monotonic() < deadline:
    for base in urls:
        try:
            with urllib.request.urlopen(f"{base}/evidence", timeout=probe_timeout) as response:
                payload = json.loads(response.read().decode("utf-8"))
        except Exception:
            continue
        for entry in payload.get("entries", []):
            if not str(entry.get("value", "")).startswith(prefix):
                continue
            if expected_provider and entry.get("provider_rid") != expected_provider:
                continue
            raise SystemExit(0)
    time.sleep(0.1)
detail = f" provider {expected_provider}" if expected_provider else ""
raise SystemExit(f"timed out waiting for provider evidence value prefix {prefix}{detail}")
PY
}

wait_location_topology() {
  local routing_id="$1"
  local state="${2:-Ready}"
  local expected_count="${3:-1}"
  local role="${4:-router}"
  python3 - "$HTTP_CONSUMER" "$routing_id" "$state" "$expected_count" "$role" "$TOPOLOGY_WAIT_TIMEOUT_MILLISECONDS" "$TOPOLOGY_WAIT_HTTP_TIMEOUT_SECONDS" <<'PY'
import json
import sys
import urllib.request

base = sys.argv[1]
routing_id = sys.argv[2]
state = sys.argv[3]
expected_count = int(sys.argv[4])
role = sys.argv[5]
topology_wait_timeout_milliseconds = int(sys.argv[6])
http_timeout_seconds = float(sys.argv[7])
request = urllib.request.Request(
    f"{base}/topology/wait",
    data=json.dumps({
        "routing_id": routing_id,
        "state": state,
        "expected_count": expected_count,
        "role": role,
        "timeout_milliseconds": topology_wait_timeout_milliseconds,
    }).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request, timeout=http_timeout_seconds) as response:
    body = response.read().decode("utf-8")
    if response.status != 200:
        raise SystemExit(f"unexpected topology wait status {response.status}: {body}")
    entries = json.loads(body)
    count = sum(1 for entry in entries
                if entry.get("routing_id") == routing_id
                and entry.get("state") == state
                and entry.get("role") == role)
    if count != expected_count:
        raise SystemExit(f"unexpected topology count {count}: {entries}")
PY
}

post_consumer_new_client_profile() {
  local marker="$1"
  post_consumer_profile_request "/profile/request/new-client" "$marker" "" ""
}

post_consumer_new_client_burst() {
  local value="$1"
  local marker_prefix="$2"
  local count="$3"
  local expected_provider="${4:-}"
  for index in $(seq 0 $((count - 1))); do
    post_consumer_profile_request \
      "/profile/request/new-client" \
      "$value" \
      "${marker_prefix}${index}" \
      "$expected_provider"
  done
}

write_provider_config() {
  local path="$1"
  local rid="$2"
  local instance="$3"
  local api="$4"
  local route="$5"
  local http="$6"
  python3 - "$path" "$rid" "$instance" "$api" "$route" "$http" "$REDIS_ENDPOINT" \
    "$REDIS_KEY_PREFIX" "$LOG_DIR/$rid.evidence.log" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

(path, rid, instance, api_endpoint, route_endpoint, http_endpoint, redis_endpoint,
 redis_key_prefix, evidence_file, log_dir) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"rid": rid, "instanceId": instance,
        "apiEndpoint": api_endpoint, "routeEndpoint": route_endpoint,
        "httpEndpoint": http_endpoint,
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "evidenceFile": evidence_file, "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

write_consumer_config() {
  local path="$1"
  python3 - "$path" "$HTTP_CONSUMER" "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" \
    "$API_A,$API_B" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

path, http_endpoint, redis_endpoint, redis_key_prefix, provider_endpoints, log_dir = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"httpEndpoint": http_endpoint,
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "providerEndpoints": provider_endpoints, "logDir": log_dir,
        "traceLabel": "consumer"}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

start_provider() {
  local rid="$1"
  local api="$2"
  local route="$3"
  local http="$4"
  local instance="${5:-$rid}"
  local config_path="$CONFIG_DIR/$rid-$instance.json"
  write_provider_config "$config_path" "$rid" "$instance" "$api" "$route" "$http"
  "$PROVIDER" --config="$config_path" \
    >"$LOG_DIR/$rid-$instance.stdout.log" 2>"$LOG_DIR/$rid-$instance.stderr.log" &
  LAST_PID="$!"
  PIDS+=("$LAST_PID")
  wait_port "$rid-http" "$http"
  wait_http_health "$rid" "$http"
}

start_consumer() {
  write_consumer_config "$CONFIG_DIR/consumer.json"
  "$CONSUMER" --config="$CONFIG_DIR/consumer.json" \
    >"$LOG_DIR/consumer.stdout.log" 2>"$LOG_DIR/consumer.stderr.log" &
  LAST_PID="$!"
  PIDS+=("$LAST_PID")
  wait_port consumer-http "$HTTP_CONSUMER"
  wait_http_health consumer "$HTTP_CONSUMER"
}

run_client() {
  local scenario="$1"
  local suffix="$2"
  shift 2
  local config_path="$CONFIG_DIR/client-$suffix.json"
  python3 - "$config_path" "$scenario" "$LOG_DIR" "$API_A" "$API_B" "$ROUTE_A" \
    "$ROUTE_B" "$HTTP_A" "$HTTP_B" "$HTTP_B_GREEN" \
    "$LOG_DIR/api-a.evidence.log" "$LOG_DIR/api-b.evidence.log" "$HTTP_CONSUMER" \
    "$CLIENT_ROUTE" "$@" <<'PY'
import json
import os
import stat
import sys

(path, scenario, log_dir, api_a, api_b, route_a, route_b, http_a, http_b,
 http_b_green, evidence_a, evidence_b, http_consumer,
 client_route, *overrides) = sys.argv[1:]
configuration = {"scenario": scenario, "logDir": log_dir,
    "apiAEndpoint": api_a, "apiBEndpoint": api_b,
    "routeAEndpoint": route_a, "routeBEndpoint": route_b,
    "httpAEndpoint": http_a, "httpBEndpoint": http_b,
    "httpBGreenEndpoint": http_b_green,
    "apiAEvidenceFile": evidence_a, "apiBEvidenceFile": evidence_b,
    "httpConsumerEndpoint": http_consumer, "clientRouteEndpoint": client_route}
for override in overrides:
    key, separator, value = override.partition("=")
    if not separator or key not in {"readyFile", "continueFile", "drainedFile",
                                    "restoreFile", "secondReadyFile",
                                    "secondContinueFile", "flapPhase", "flapCycle"}:
        raise SystemExit(f"unknown client configuration override: {override}")
    configuration[key] = value
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": configuration}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$CLIENT" --config="$config_path" \
    >"$LOG_DIR/client-$suffix.stdout.log" 2>"$LOG_DIR/client-$suffix.stderr.log"
}

post_consumer_timeout_request() {
  local marker="$1"
  python3 - "$HTTP_CONSUMER" "$marker" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import json
import sys
import urllib.request

base = sys.argv[1]
marker = sys.argv[2]
timeout_seconds = float(sys.argv[3])
request = urllib.request.Request(
    f"{base}/profile/request/timeout/100",
    data=json.dumps({"value": marker}).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
    body = response.read().decode("utf-8")
    if response.status != 200:
        raise SystemExit(f"unexpected status {response.status}: {body}")
    payload = json.loads(body)
    if payload.get("failed") is not True:
        raise SystemExit(f"expected failed timeout payload, got {payload}")
PY
}

post_consumer_missing_request() {
  local marker="$1"
  python3 - "$HTTP_CONSUMER" "$marker" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import json
import sys
import urllib.request

base = sys.argv[1]
marker = sys.argv[2]
timeout_seconds = float(sys.argv[3])
request = urllib.request.Request(
    f"{base}/profile/request/missing",
    data=json.dumps({"value": marker}).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
    body = response.read().decode("utf-8")
    if response.status != 200:
        raise SystemExit(f"unexpected status {response.status}: {body}")
    payload = json.loads(body)
    if payload.get("failed") is not True:
        raise SystemExit(f"expected failed missing-request payload, got {payload}")
PY
}

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run ResilienceLifecycle E2E." >&2
  exit 1
fi
zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-cpp-e2e-resiliencelifecycle" "redis:7-alpine"
REDIS_OWNED=1
REDIS_ENDPOINT="127.0.0.1:${redis_port}"
echo "redis endpoint=$REDIS_ENDPOINT (container $REDIS_CONTAINER)"
REDIS_HOST="${REDIS_ENDPOINT%:*}"
REDIS_TCP_PORT="${REDIS_ENDPOINT##*:}"
wait_tcp "$REDIS_HOST" "$REDIS_TCP_PORT" redis
echo "redis key prefix=$REDIS_KEY_PREFIX"

start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
API_A_PID="$LAST_PID"
start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
API_B_PID="$LAST_PID"
start_consumer
CONSUMER_PID="$LAST_PID"
wait_location_topology api-a Ready 1
wait_location_topology api-b Ready 1
if should_run RL-consumer rl-consumer; then
  post_consumer_profile "rl-consumer-smoke"
  grep -q "message flow" "$LOG_DIR/consumer-flow.log"
  echo "scenario RL-consumer passed"
fi
if should_run RL-C1 rl-c1; then
  post_consumer_new_client_profile "rl-c1-new-client"
  grep -q "message flow" "$LOG_DIR/storm-rl-c1-new-client-flow.log"
  echo "scenario RL-C1 consumer passed"
fi
if should_run RL-B1 rl-b1; then
  post_consumer_timeout_request "slow"
  post_consumer_profile "rl-b1-follow-up"
  echo "scenario RL-B1 passed"
fi
if should_run RL-D3 rl-d3; then
  post_consumer_missing_request "rl-d3-missing"
  grep -Eq "reason=handler_missing.*action=reply_error.*packet=MissingProfileReq" \
    "$LOG_DIR/api-a-flow.log" "$LOG_DIR/api-b-flow.log"
  echo "scenario RL-D3 passed"
fi
if should_run RL-D2 rl-d2; then
  run_client observer-fault rl-d2
  grep -q "scenario RL-D2 passed" "$LOG_DIR/client-rl-d2.stdout.log"
  echo "scenario RL-D2 passed"
fi

if should_run RL-A1 rl-a1; then
  stop_pid "$API_B_PID"
  wait_location_topology api-b Ready 0
  sleep "$ROUTE_SETTLE_SECONDS"
  READY="$LOG_DIR/rl-a1-ready"
  CONTINUE="$LOG_DIR/rl-a1-continue"
  run_client rl-a1 rl-a1 readyFile="$READY" continueFile="$CONTINUE" &
  A1_CLIENT_PID="$!"
  wait_marker "$READY"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B" api-b-v2
  API_B_PID="$LAST_PID"
  wait_location_topology api-b Ready 1
  sleep "$ROUTE_SETTLE_SECONDS"
  touch "$CONTINUE"
  wait "$A1_CLIENT_PID"
  grep -q "scenario RL-A1 client passed" "$LOG_DIR/client-rl-a1.stdout.log"
  echo "scenario RL-A1 passed"
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
fi

if should_run RL-A2 rl-a2; then
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A" api-a-v1
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B_GREEN" "$ROUTE_B_GREEN" "$HTTP_B_GREEN" api-b-remap
  API_B_GREEN_PID="$LAST_PID"
  wait_location_topology api-b Ready 1
  READY="$LOG_DIR/rl-a2-ready"
  CONTINUE="$LOG_DIR/rl-a2-continue"
  run_client rl-a2 rl-a2 readyFile="$READY" continueFile="$CONTINUE" &
  A2_CLIENT_PID="$!"
  wait_marker "$READY"
  stop_pid "$API_B_GREEN_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B" api-b-restored
  API_B_PID="$LAST_PID"
  wait_location_topology api-b Ready 1
  sleep "$ROUTE_SETTLE_SECONDS"
  touch "$CONTINUE"
  wait "$A2_CLIENT_PID"
  grep -q "scenario RL-A2 client passed" "$LOG_DIR/client-rl-a2.stdout.log"
  echo "scenario RL-A2 passed"
fi

if should_run RL-B3 rl-b3; then
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  wait_location_topology api-a Ready 1
  wait_location_topology api-b Ready 1
  wait_consumer_profile_ready rl-b3-channel-ready 2
  READY="$LOG_DIR/rl-b3-ready"
  CONTINUE="$LOG_DIR/rl-b3-continue"
  run_client rl-b3 rl-b3 readyFile="$READY" continueFile="$CONTINUE" &
  B3_CLIENT_PID="$!"
  wait_marker "$READY"
  stop_pid "$API_B_PID"
  wait_location_topology api-b Ready 0
  touch "$CONTINUE"
  wait "$B3_CLIENT_PID"
  grep -q "scenario RL-B3 client passed" "$LOG_DIR/client-rl-b3.stdout.log"
  echo "scenario RL-B3 passed"
  stop_pid "$API_A_PID"
fi

if should_run RL-B2 rl-b2 RL-C2 rl-c2; then
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  wait_location_topology api-a Ready 1
  wait_location_topology api-b Ready 1
  if should_run RL-B2 rl-b2; then
    READY="$LOG_DIR/rl-b2-ready"
    CONTINUE="$LOG_DIR/rl-b2-continue"
    DRAINED="$LOG_DIR/rl-b2-after-crash"
    RESTORE="$LOG_DIR/rl-b2-restart"
    SECOND_READY="$LOG_DIR/rl-b2-second-crash-ready"
    SECOND_CONTINUE="$LOG_DIR/rl-b2-second-crash-continue"
    run_client inflight-crash rl-b2 readyFile="$READY" continueFile="$CONTINUE" \
      drainedFile="$DRAINED" restoreFile="$RESTORE" \
      secondReadyFile="$SECOND_READY" secondContinueFile="$SECOND_CONTINUE" &
    B2_CLIENT_PID="$!"
    wait_marker "$READY"
    kill_pid "$API_B_PID"
    touch "$CONTINUE"
    wait_marker "$DRAINED"
    start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
    API_B_PID="$LAST_PID"
    touch "$RESTORE"
    wait_marker "$SECOND_READY"
    kill_pid "$API_B_PID"
    touch "$SECOND_CONTINUE"
    wait "$B2_CLIENT_PID"
    grep -q "scenario RL-B2 second-crash alternate passed" \
      "$LOG_DIR/client-rl-b2.stdout.log"
    grep -q "scenario RL-B2 passed" "$LOG_DIR/client-rl-b2.stdout.log"
    echo "scenario RL-B2 passed"
    start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
    API_B_PID="$LAST_PID"
    wait_location_topology api-b Ready 1
  fi
  if should_run RL-C2 rl-c2; then
    crash_provider "$HTTP_B" "$API_B_PID"
    wait_location_topology "api-b" "Ready" 0
    post_consumer_new_client_burst "fast" "rl-c2-after-crash-" 8 "api-a"
    wait_provider_evidence_value_prefix "rl-c2-after-crash-" "api-a"
    start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
    API_B_PID="$LAST_PID"
    wait_location_topology "api-b" "Ready" 1
    post_consumer_profile_burst "fast" "rl-c2-restored-" 40
    wait_provider_evidence_value_prefix "rl-c2-restored-" "api-b"
    echo "scenario RL-C2 passed"
  fi
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
fi

if should_run RL-A4 rl-a4; then
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  READY="$LOG_DIR/rl-a4-ready"
  CONTINUE="$LOG_DIR/rl-a4-green-ready"
  DRAINED="$LOG_DIR/rl-a4-green-stopped"
  RESTORE="$LOG_DIR/rl-a4-restored"
  run_client rl-a4 rl-a4 readyFile="$READY" continueFile="$CONTINUE" \
    drainedFile="$DRAINED" restoreFile="$RESTORE" &
  A4_CLIENT_PID="$!"
  wait_marker "$READY"
  start_provider api-b "$API_B_GREEN" "$ROUTE_B_GREEN" "$HTTP_B_GREEN" "api-b-green"
  API_B_GREEN_PID="$LAST_PID"
  sleep "$ROUTE_SETTLE_SECONDS"
  stop_pid "$API_B_PID"
  touch "$CONTINUE"
  wait_marker "$DRAINED"
  stop_pid "$API_B_GREEN_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  touch "$RESTORE"
  wait "$A4_CLIENT_PID"
  grep -q "scenario RL-A4 passed" "$LOG_DIR/client-rl-a4.stdout.log"
  echo "scenario RL-A4 passed"
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
fi

if should_run RL-B4 rl-b4 RL-B5 rl-b5 RL-B6 rl-b6; then
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  if should_run RL-B4 rl-b4; then
    run_client rl-b4 rl-b4
    grep -q "scenario RL-B4 passed" "$LOG_DIR/client-rl-b4.stdout.log"
    echo "scenario RL-B4 passed"
  fi
  if should_run RL-B5 rl-b5; then
    run_client rl-b5 rl-b5
    grep -q "scenario RL-B5 passed" "$LOG_DIR/client-rl-b5.stdout.log"
    echo "scenario RL-B5 passed"
  fi
  if should_run RL-B6 rl-b6; then
    run_client rl-b6 rl-b6
    grep -q "scenario RL-B6 passed" "$LOG_DIR/client-rl-b6.stdout.log"
    echo "scenario RL-B6 passed"
  fi
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
fi

if should_run RL-A3 rl-a3 RL-C1 rl-c1 RL-A5 rl-a5 RL-C3 rl-c3; then
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  wait_location_topology api-b Ready 1
  if should_run RL-A3 rl-a3; then
    run_client rl-a3 rl-a3
    grep -q "scenario RL-A3 passed" "$LOG_DIR/client-rl-a3.stdout.log"
    echo "scenario RL-A3 passed"
  fi
  if should_run RL-C1 rl-c1; then
    run_client rl-c1 rl-c1
    grep -q "scenario RL-C1 passed" "$LOG_DIR/client-rl-c1.stdout.log"
    echo "scenario RL-C1 passed"
  fi
  if should_run RL-A5 rl-a5; then
    for index in 1 2 3; do
      stop_pid "$API_B_PID"
      wait_location_topology api-b Ready 0
      sleep "$ROUTE_SETTLE_SECONDS"
      run_client rl-a5 "rl-a5-down-$index" flapPhase=down flapCycle="$index"
      grep -q "scenario RL-A5 down passed" "$LOG_DIR/client-rl-a5-down-$index.stdout.log"
      start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
      API_B_PID="$LAST_PID"
      wait_location_topology api-b Ready 1
      run_client rl-a5 "rl-a5-up-$index" flapPhase=up flapCycle="$index"
      grep -q "scenario RL-A5 up passed" "$LOG_DIR/client-rl-a5-up-$index.stdout.log"
    done
    echo "scenario RL-A5 passed"
  fi
  if should_run RL-C3 rl-c3; then
    READY="$LOG_DIR/rl-c3-ready"
    CONTINUE="$LOG_DIR/rl-c3-continue"
    run_client rl-c3 rl-c3 readyFile="$READY" continueFile="$CONTINUE" &
    C3_CLIENT_PID="$!"
    wait_marker "$READY"
    wait "$API_B_PID" >/dev/null 2>&1 || true
    start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
    API_B_PID="$LAST_PID"
    touch "$CONTINUE"
    wait "$C3_CLIENT_PID"
    grep -q "scenario RL-C3 passed" "$LOG_DIR/client-rl-c3.stdout.log"
    echo "scenario RL-C3 passed"
  fi
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
fi

if should_run RL-C4 rl-c4; then
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  READY="$LOG_DIR/rl-c4-ready"
  CONTINUE="$LOG_DIR/rl-c4-continue"
  OUTAGE_VERIFIED="$LOG_DIR/rl-c4-outage-verified"
  run_client location-store-outage rl-c4 readyFile="$READY" \
    continueFile="$CONTINUE" drainedFile="$OUTAGE_VERIFIED" &
  C4_CLIENT_PID="$!"
  wait_marker "$READY"
  if [[ "$REDIS_OWNED" != "1" ]]; then
    echo "RL-C4 requires the runner-owned Redis container." >&2
    exit 1
  fi
  docker pause "$REDIS_CONTAINER" >/dev/null
  sleep "$ROUTE_SETTLE_SECONDS"
  touch "$CONTINUE"
  wait_marker "$OUTAGE_VERIFIED"
  docker unpause "$REDIS_CONTAINER" >/dev/null
  wait_tcp "$REDIS_HOST" "$REDIS_TCP_PORT" redis
  sleep "$ROUTE_SETTLE_SECONDS"
  wait "$C4_CLIENT_PID"
  grep -q "scenario RL-C4 passed" "$LOG_DIR/client-rl-c4.stdout.log"
  stop_pid "$API_A_PID"
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  sleep "$ROUTE_SETTLE_SECONDS"
  run_client location-store-recovered rl-c4-recovered
  grep -q "scenario RL-C4 recovery passed" "$LOG_DIR/client-rl-c4-recovered.stdout.log"
  echo "scenario RL-C4 passed"
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
fi

if should_run RL-D1 rl-d1 RL-D4 rl-d4; then
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
  start_provider api-a "$API_A" "$ROUTE_A" "$HTTP_A"
  API_A_PID="$LAST_PID"
  start_provider api-b "$API_B" "$ROUTE_B" "$HTTP_B"
  API_B_PID="$LAST_PID"
  sleep "$ROUTE_SETTLE_SECONDS"
  if should_run RL-D1 rl-d1; then
    post_consumer_profile_burst "fast" "rl-d1-" 120
    wait_provider_evidence_value_prefix "rl-d1-"
    echo "scenario RL-D1 passed"
  fi
  if should_run RL-D4 rl-d4; then
    run_client rl-d4 rl-d4
    grep -q "scenario RL-D4 passed" "$LOG_DIR/client-rl-d4.stdout.log"
    echo "scenario RL-D4 passed"
  fi
  stop_pid "$API_B_PID"
  stop_pid "$API_A_PID"
fi

echo "resilience-lifecycle e2e result=passed"
