#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
BUILD_DIR="$CPP_DIR/build"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
ROUTE_SETTLE_SECONDS=5
# Fanout subscribers connect after their HTTP listener is ready.  The native
# publisher emits its first readiness beacon at five seconds, so application
# publishes must wait past that beacon instead of relying on process readiness.
SCENARIO_SETTLE_SECONDS=6
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

read -r PUBLISHER PUBLISHER_HTTP HTTP_1 HTTP_2 HTTP_3 <<<"$(python3 - <<'PY'
import socket

sockets = []
ports = []
for _ in range(5):
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    sockets.append(s)
    ports.append(s.getsockname()[1])
print(f"tcp://127.0.0.1:{ports[0]}", end=" ")
print(" ".join(f"http://127.0.0.1:{p}" for p in ports[1:5]))
for s in sockets:
    s.close()
PY
)"

RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$RUN_ID"
CONFIG_DIR="$LOG_DIR/config"
mkdir -p "$LOG_DIR"
mkdir -p "$CONFIG_DIR"
echo "log_dir=$LOG_DIR"
SCENARIO="${1:-all}"

cmake -S "$CPP_DIR" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" --target \
  zlink_cpp_e2e_pubsub_publisher \
  zlink_cpp_e2e_pubsub_subscriber \
  zlink_cpp_e2e_pubsub_client >/dev/null

PUBLISHER_SERVER="$BUILD_DIR/zlink_cpp_e2e_pubsub_publisher"
SUBSCRIBER="$BUILD_DIR/zlink_cpp_e2e_pubsub_subscriber"
CLIENT="$BUILD_DIR/zlink_cpp_e2e_pubsub_client"
PIDS=()
LAST_PID=""
PUBLISHER_PID=""
REDIS_CONTAINER=""
REDIS_CONTAINER_OWNED=0

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

REDIS_KEY_PREFIX="zlink:e2e:cfg3:$(date +%s)-$$"
zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-cpp-e2e-pubsub" "redis:7-alpine"
REDIS_CONTAINER_OWNED=1
REDIS_ENDPOINT="127.0.0.1:${redis_port}"
echo "redis endpoint=$REDIS_ENDPOINT (container $REDIS_CONTAINER)"
REDIS_HOST="${REDIS_ENDPOINT%:*}"
REDIS_TCP_PORT="${REDIS_ENDPOINT##*:}"
wait_tcp "$REDIS_HOST" "$REDIS_TCP_PORT" redis
echo "redis key prefix=$REDIS_KEY_PREFIX"

cleanup() {
  local code=$?
  local cleanup_failed=0
  local status
  rm -rf "$CONFIG_DIR"
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
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
  if [[ -n "$REDIS_CONTAINER" && "$REDIS_CONTAINER_OWNED" == "1" ]]; then
    docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  fi
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

wait_port_closed() {
  local name="$1"
  local endpoint="$2"
  local host="127.0.0.1"
  local port
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

check_operational_endpoints() {
  local name="$1"
  local endpoint="$2"
  python3 - "$name" "$endpoint" "$LOG_DIR/$name-operational.log" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import json
import sys
import urllib.request

name = sys.argv[1]
endpoint = sys.argv[2]
log_path = sys.argv[3]
timeout_seconds = float(sys.argv[4])

def get(path):
    with urllib.request.urlopen(endpoint + path, timeout=timeout_seconds) as response:
        return response.read().decode()

def post(path):
    request = urllib.request.Request(endpoint + path, data=b"", method="POST")
    with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
        return response.read().decode()

health = get("/health")
evidence = get("/evidence")
cleared = post("/evidence/clear")
with open(log_path, "a", encoding="utf-8") as log:
    log.write(f"{name} health={health}\n")
    log.write(f"{name} evidence={evidence}\n")
    log.write(f"{name} clear={cleared}\n")
print(f"operational {name} passed")
PY
}

snapshot_operational_evidence() {
  local name="$1"
  local endpoint="$2"
  local output="$3"
  python3 - "$name" "$endpoint" "$output" "$HTTP_PROBE_TIMEOUT_SECONDS" <<'PY'
import sys
import urllib.request

name = sys.argv[1]
endpoint = sys.argv[2]
output = sys.argv[3]
timeout_seconds = float(sys.argv[4])
with urllib.request.urlopen(endpoint + "/evidence", timeout=timeout_seconds) as response:
    body = response.read().decode()
with open(output, "w", encoding="utf-8") as file:
    file.write(body)
    file.write("\n")
print(f"snapshot {name} evidence written")
PY
}

start_publisher() {
  local suffix="${1:-publisher}"
  local config_path="$CONFIG_DIR/$suffix.json"
  python3 - "$config_path" "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$PUBLISHER" \
    "$PUBLISHER_HTTP" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

path, redis_endpoint, redis_key_prefix, publisher, http, log_dir = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"redis": {"endpoint": redis_endpoint,
        "keyPrefix": redis_key_prefix}, "publisherEndpoint": publisher,
        "httpEndpoint": http, "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$PUBLISHER_SERVER" --config="$config_path" \
    >"$LOG_DIR/$suffix.stdout.log" 2>"$LOG_DIR/$suffix.stderr.log" &
  LAST_PID="$!"
  PUBLISHER_PID="$LAST_PID"
  PIDS+=("$LAST_PID")
  wait_port "$suffix-http" "$PUBLISHER_HTTP"
  check_operational_endpoints "$suffix" "$PUBLISHER_HTTP"
}

start_subscriber() {
  local id="$1"
  local topics="$2"
  local http="$3"
  local delay="${4:-0}"
  local accepted_topics="${5:-$topics}"
  local config_path="$CONFIG_DIR/$id.json"
  python3 - "$config_path" "$id" "$topics" "$accepted_topics" "$delay" "$http" \
    "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$LOG_DIR" <<'PY'
import json
import os
import stat
import sys

(path, subscriber_id, topics, accepted_topics, delay, http, redis_endpoint,
 redis_key_prefix, log_dir) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"subscriberId": subscriber_id, "topics": topics,
        "acceptedTopics": accepted_topics, "handlerDelayMs": delay,
        "httpEndpoint": http, "redis": {"endpoint": redis_endpoint,
        "keyPrefix": redis_key_prefix}, "logDir": log_dir}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$SUBSCRIBER" --config="$config_path" \
    >"$LOG_DIR/$id.stdout.log" 2>"$LOG_DIR/$id.stderr.log" &
  LAST_PID="$!"
  PIDS+=("$LAST_PID")
  wait_port "$id-http" "$http"
}

stop_pid() {
  local pid="$1"
  local status
  if kill -0 "$pid" >/dev/null 2>&1; then
    kill "$pid" >/dev/null 2>&1 || true
    set +e
    wait "$pid" >/dev/null 2>&1
    status=$?
    set -e
    if [[ "$status" != "0" && "$status" != "127" && "$status" != "130" && "$status" != "143" ]]; then
      echo "stopped process $pid exited unexpectedly with status $status" >&2
      return 1
    fi
  fi
}

remember_pid_file() {
  local file="$1"
  local array_name="$2"
  if [[ ! -s "$file" ]]; then
    return 0
  fi
  local pid
  pid="$(tail -1 "$file")"
  if [[ "$pid" =~ ^[0-9]+$ ]]; then
    eval "$array_name+=(\"$pid\")"
  fi
}

stop_all_subscribers() {
  local status
  for pid in "${SUB_PIDS[@]:-}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
      set +e
      wait "$pid" >/dev/null 2>&1
      status=$?
      set -e
      if [[ "$status" != "0" && "$status" != "127" && "$status" != "130" && "$status" != "143" ]]; then
        echo "stopped subscriber process $pid exited unexpectedly with status $status" >&2
        return 1
      fi
    fi
  done
  SUB_PIDS=()
}

should_run() {
  [[ "$SCENARIO" == "all" || "$SCENARIO" == "$1" || "$SCENARIO" == "$2" ]]
}

run_client() {
  local scenario="$1"
  local suffix="$2"
  shift 2
  local config_path="$CONFIG_DIR/client-$suffix.json"
  python3 - "$config_path" "$scenario" "$PUBLISHER_HTTP" "$HTTP_1,$HTTP_2,$HTTP_3" \
    "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" "$PUBLISHER" "$LOG_DIR" "$CONFIG_DIR" \
    "$SUBSCRIBER" "$PUBLISHER_SERVER" "$@" <<'PY'
import json
import os
import stat
import sys

(path, scenario, publisher_url, subscriber_urls, redis_endpoint,
 redis_key_prefix, publisher_endpoint, log_dir, config_dir,
 subscriber_executable, publisher_executable, *overrides) = sys.argv[1:]
configuration = {"scenario": scenario, "publisherUrl": publisher_url,
    "subscriberUrls": subscriber_urls, "redisEndpoint": redis_endpoint,
    "redisKeyPrefix": redis_key_prefix, "publisherEndpoint": publisher_endpoint,
    "logDir": log_dir, "configDir": config_dir,
    "subscriberExecutable": subscriber_executable,
    "publisherExecutable": publisher_executable}
allowed = {"startReadyFile", "startContinueFile", "readyFile", "continueFile",
           "reconnectSubscriberUrl", "reconnectSubscriberPidFile",
           "restartedPublisherPidFile"}
for override in overrides:
    key, separator, value = override.partition("=")
    if not separator or key not in allowed:
        raise SystemExit(f"unknown client configuration override: {override}")
    configuration[key] = value
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": configuration}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$CLIENT" --config="$config_path" \
    >"$LOG_DIR/client-$suffix.stdout.log" 2>"$LOG_DIR/client-$suffix.stderr.log"
}

start_client_waiting() {
  local scenario="$1"
  local suffix="$2"
  local ready="$3"
  local continue_file="$4"
  shift 4
  run_client "$scenario" "$suffix" \
    startReadyFile="$ready" \
    startContinueFile="$continue_file" \
    "$@" &
  LAST_PID="$!"
  PIDS+=("$LAST_PID")
}

case "$SCENARIO" in
  all|PS-A1|ps-a1|PS-A2|ps-a2|PS-A3|ps-a3|PS-A4|ps-a4|PS-B1|ps-b1|PS-B2|ps-b2|PS-C1|ps-c1)
    ;;
  *)
    echo "Unknown PubSub scenario: $SCENARIO" >&2
    exit 1
    ;;
esac

start_publisher publisher

SUB_PIDS=()
if should_run PS-A1 ps-a1; then
  START_READY="$LOG_DIR/ps-a1-start-ready"
  START_CONTINUE="$LOG_DIR/ps-a1-start-continue"
  start_client_waiting basic basic "$START_READY" "$START_CONTINUE"
  BASIC_CLIENT_PID="$LAST_PID"
  wait_marker "$START_READY"
  start_subscriber sub-1 fanout "$HTTP_1"; SUB_PIDS+=("$LAST_PID")
  start_subscriber sub-2 fanout "$HTTP_2"; SUB_PIDS+=("$LAST_PID")
  start_subscriber sub-3 fanout "$HTTP_3"; SUB_PIDS+=("$LAST_PID")
  sleep "$SCENARIO_SETTLE_SECONDS"
  touch "$START_CONTINUE"
  wait "$BASIC_CLIENT_PID"
  cat "$LOG_DIR/client-basic.stdout.log"
  stop_all_subscribers
fi

if should_run PS-A2 ps-a2; then
  START_READY="$LOG_DIR/ps-a2-start-ready"
  START_CONTINUE="$LOG_DIR/ps-a2-start-continue"
  start_client_waiting topic topic "$START_READY" "$START_CONTINUE"
  TOPIC_CLIENT_PID="$LAST_PID"
  wait_marker "$START_READY"
  start_subscriber sub-1 alpha,beta "$HTTP_1" 0 alpha; SUB_PIDS+=("$LAST_PID")
  start_subscriber sub-2 alpha,beta "$HTTP_2" 0 beta; SUB_PIDS+=("$LAST_PID")
  start_subscriber sub-3 alpha,beta "$HTTP_3" 0 alpha; SUB_PIDS+=("$LAST_PID")
  sleep "$SCENARIO_SETTLE_SECONDS"
  touch "$START_CONTINUE"
  wait "$TOPIC_CLIENT_PID"
  cat "$LOG_DIR/client-topic.stdout.log"
  stop_all_subscribers
fi

if should_run PS-A3 ps-a3; then
  START_READY="$LOG_DIR/ps-a3-start-ready"
  START_CONTINUE="$LOG_DIR/ps-a3-start-continue"
  READY="$LOG_DIR/ps-a3-ready"
  CONTINUE="$LOG_DIR/ps-a3-continue"
  start_client_waiting late late "$START_READY" "$START_CONTINUE" \
    readyFile="$READY" \
    continueFile="$CONTINUE"
  LATE_CLIENT_PID="$LAST_PID"
  wait_marker "$START_READY"
  start_subscriber sub-1 fanout "$HTTP_1"; SUB_PIDS+=("$LAST_PID")
  start_subscriber sub-2 fanout "$HTTP_2"; SUB_PIDS+=("$LAST_PID")
  sleep "$SCENARIO_SETTLE_SECONDS"
  touch "$START_CONTINUE"
  wait_marker "$READY"
  start_subscriber sub-3 fanout "$HTTP_3"; SUB_PIDS+=("$LAST_PID")
  sleep "$SCENARIO_SETTLE_SECONDS"
  touch "$CONTINUE"
  wait "$LATE_CLIENT_PID"
  cat "$LOG_DIR/client-late.stdout.log"
  stop_all_subscribers
fi

if should_run PS-A4 ps-a4; then
  START_READY="$LOG_DIR/ps-a4-start-ready"
  START_CONTINUE="$LOG_DIR/ps-a4-start-continue"
  RECONNECT_PID_FILE="$LOG_DIR/ps-a4-reconnect-subscriber.pid"
  start_client_waiting reconnect reconnect "$START_READY" "$START_CONTINUE" \
    reconnectSubscriberUrl="$HTTP_3" \
    reconnectSubscriberPidFile="$RECONNECT_PID_FILE"
  RECONNECT_CLIENT_PID="$LAST_PID"
  wait_marker "$START_READY"
  start_subscriber sub-1 fanout "$HTTP_1"; SUB_PIDS+=("$LAST_PID")
  start_subscriber sub-2 fanout "$HTTP_2"; SUB_PIDS+=("$LAST_PID")
  sleep "$SCENARIO_SETTLE_SECONDS"
  touch "$START_CONTINUE"
  wait "$RECONNECT_CLIENT_PID"
  remember_pid_file "$RECONNECT_PID_FILE" SUB_PIDS
  cat "$LOG_DIR/client-reconnect.stdout.log"
  stop_all_subscribers
fi

if should_run PS-B1 ps-b1; then
  START_READY="$LOG_DIR/ps-b1-start-ready"
  START_CONTINUE="$LOG_DIR/ps-b1-start-continue"
  start_client_waiting slow slow "$START_READY" "$START_CONTINUE"
  SLOW_CLIENT_PID="$LAST_PID"
  wait_marker "$START_READY"
  start_subscriber sub-1 fanout "$HTTP_1" 250; SUB_PIDS+=("$LAST_PID")
  start_subscriber sub-2 fanout "$HTTP_2"; SUB_PIDS+=("$LAST_PID")
  start_subscriber sub-3 fanout "$HTTP_3"; SUB_PIDS+=("$LAST_PID")
  sleep "$SCENARIO_SETTLE_SECONDS"
  touch "$START_CONTINUE"
  wait "$SLOW_CLIENT_PID"
  cat "$LOG_DIR/client-slow.stdout.log"
  stop_all_subscribers
fi

if should_run PS-B2 ps-b2; then
  START_READY="$LOG_DIR/ps-b2-start-ready"
  START_CONTINUE="$LOG_DIR/ps-b2-start-continue"
  RESTARTED_PUBLISHER_PID_FILE="$LOG_DIR/ps-b2-restarted-publisher.pid"
  start_subscriber sub-1 fanout "$HTTP_1"; SUB_PIDS+=("$LAST_PID")
  start_subscriber sub-2 fanout "$HTTP_2"; SUB_PIDS+=("$LAST_PID")
  start_subscriber sub-3 fanout "$HTTP_3"; SUB_PIDS+=("$LAST_PID")
  start_client_waiting publisher-restart publisher-before "$START_READY" "$START_CONTINUE" \
    restartedPublisherPidFile="$RESTARTED_PUBLISHER_PID_FILE"
  PUB_RESTART_CLIENT_PID="$LAST_PID"
  wait_marker "$START_READY"
  sleep "$SCENARIO_SETTLE_SECONDS"
  touch "$START_CONTINUE"
  wait "$PUB_RESTART_CLIENT_PID"
  remember_pid_file "$RESTARTED_PUBLISHER_PID_FILE" PIDS
  cat "$LOG_DIR/client-publisher-before.stdout.log"
  stop_all_subscribers
fi

if should_run PS-C1 ps-c1; then
  START_READY="$LOG_DIR/ps-c1-start-ready"
  START_CONTINUE="$LOG_DIR/ps-c1-start-continue"
  start_client_waiting negative negative "$START_READY" "$START_CONTINUE"
  NEGATIVE_CLIENT_PID="$LAST_PID"
  wait_marker "$START_READY"
  start_subscriber sub-1 fanout "$HTTP_1"; SUB_PIDS+=("$LAST_PID")
  start_subscriber sub-2 fanout "$HTTP_2"; SUB_PIDS+=("$LAST_PID")
  start_subscriber sub-3 fanout "$HTTP_3"; SUB_PIDS+=("$LAST_PID")
  sleep "$SCENARIO_SETTLE_SECONDS"
  touch "$START_CONTINUE"
  wait "$NEGATIVE_CLIENT_PID"
  cat "$LOG_DIR/client-negative.stdout.log"
  stop_all_subscribers
fi

snapshot_operational_evidence publisher "$PUBLISHER_HTTP" "$LOG_DIR/publisher-evidence-final.json"
if should_run PS-C1 ps-c1; then
  python3 - "$LOG_DIR/publisher-evidence-final.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as evidence_file:
    entries = json.load(evidence_file).get("entries", [])

dispatch_error_parts = (
    "error|",
    "kind=publish",
    "reason=handlerMissing",
    "action=drop",
)
if any(all(part in entry for part in dispatch_error_parts) for entry in entries):
    raise SystemExit(
        "publisher emitted a dispatch error for submit-only publish"
    )

print("publisher dispatch negative passed")
PY
fi
echo "pubsub e2e result=passed"
