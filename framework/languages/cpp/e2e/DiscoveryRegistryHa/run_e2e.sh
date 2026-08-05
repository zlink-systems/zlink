#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
BUILD_DIR="${ZLINK_CPP_BUILD_DIR:-$CPP_DIR/build-redis-vcpkg}"
SCENARIO="${1:-all}"
HEARTBEAT_MS=1000
LEASE_TTL_MS=5000
POLLING_MS=500
GRACE_MS=6000
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
ROUTE_SETTLE_SECONDS=5
PROCESS_STOP_TIMEOUT_SECONDS=15
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
PROCESS_STOP_ATTEMPTS="$(
  python3 - "$PROCESS_STOP_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"

normalize_scenario() {
  case "$1" in
    all) echo all ;;
    sf-a1|SF-A1|a1) echo SF-A1 ;;
    sf-a2|SF-A2|a2) echo SF-A2 ;;
    sf-b1|SF-B1|b1) echo SF-B1 ;;
    sf-b2|SF-B2|b2) echo SF-B2 ;;
    sf-c1|SF-C1|c1) echo SF-C1 ;;
    sf-c2|SF-C2|c2) echo SF-C2 ;;
    sf-d1|SF-D1|d1) echo SF-D1 ;;
    sf-d2|SF-D2|d2) echo SF-D2 ;;
    sf-d3|SF-D3|d3) echo SF-D3 ;;
    sf-e1|SF-E1|e1) echo SF-E1 ;;
    *) echo "Unsupported C++ StoreFailure scenario: $1" >&2; exit 2 ;;
  esac
}

pick_loopback_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

SCENARIO="$(normalize_scenario "$SCENARIO")"

if [[ "$SCENARIO" == "all" ]]; then
  for scenario in SF-A1 SF-A2 SF-B1 SF-B2 SF-C1 SF-C2 SF-D1 SF-D2 SF-D3 SF-E1; do
    "$0" "$scenario"
  done
  echo "store-failure c++ e2e result=passed"
  exit 0
fi

read -r API_A API_B API_B_REPLACEMENT API_C HTTP_A HTTP_B HTTP_B_REPLACEMENT HTTP_C HTTP_CONSUMER <<<"$(python3 - <<'PY'
import socket

sockets = []
ports = []
for _ in range(9):
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    sockets.append(sock)
    ports.append(sock.getsockname()[1])
print(" ".join(f"tcp://127.0.0.1:{p}" for p in ports[:4]), end=" ")
print(" ".join(f"http://127.0.0.1:{p}" for p in ports[4:9]))
for sock in sockets:
    sock.close()
PY
)"

RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$RUN_ID"
mkdir -p "$LOG_DIR"
CONFIG_DIR="$LOG_DIR/config"
mkdir -p "$CONFIG_DIR"
echo "log_dir=$LOG_DIR"

cmake -S "$CPP_DIR" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" --target \
  zlink_cpp_e2e_store_failure_provider \
  zlink_cpp_e2e_store_failure_consumer \
  zlink_cpp_e2e_store_failure_client >/dev/null

PROVIDER_SERVER="$BUILD_DIR/zlink_cpp_e2e_store_failure_provider"
CONSUMER_SERVER="$BUILD_DIR/zlink_cpp_e2e_store_failure_consumer"
CLIENT="$BUILD_DIR/zlink_cpp_e2e_store_failure_client"
REDIS_CONTAINER=""
REDIS_OWNED=0
REDIS_KEY_PREFIX="zlink:cpp:store-failure:${RUN_ID}"
STORE_ENDPOINT=""
REDIS_PROXY_ADMIN_URL=""
PIDS=()
API_A_PID=""
API_B_PID=""
CONSUMER_PID=""
SF_B2_REPLACEMENT_PID=""
SF_A2_PROVIDER_PID=""
SF_A2_PROVIDER_START_FILE="$LOG_DIR/sf-a2-start-provider-c"

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
  for _ in $(seq 1 "$PROCESS_STOP_ATTEMPTS"); do
    state="$(ps -o stat= -p "$pid" 2>/dev/null | awk '{print $1}')"
    if [[ -z "$state" || "$state" == Z* ]]; then
      wait_pid_status "$pid" "$label" 0 130 143
      return $?
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  kill -9 "$pid" >/dev/null 2>&1 || true
  wait_pid_status "$pid" "$label" 0 130 143
}

post_shutdown() {
  local url="$1"
  python3 - "$url" <<'PY' >/dev/null 2>&1 || true
import http.client
import sys
import urllib.parse

url = urllib.parse.urlparse(sys.argv[1])
conn = http.client.HTTPConnection(url.hostname, url.port, timeout=1)
conn.request("POST", url.path or "/")
conn.getresponse().read()
conn.close()
PY
}

cleanup() {
  local code=$?
  local cleanup_failed=0
  if [[ -n "$API_A_PID" ]]; then
    post_shutdown "$HTTP_A/shutdown"
  fi
  if [[ -n "$API_B_PID" ]]; then
    post_shutdown "$HTTP_B/shutdown"
  fi
  post_shutdown "$HTTP_B_REPLACEMENT/shutdown"
  post_shutdown "$HTTP_C/shutdown"
  if [[ -n "$CONSUMER_PID" ]]; then
    post_shutdown "$HTTP_CONSUMER/shutdown"
  fi
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
  elif [[ -n "${REDIS_ENDPOINT:-}" ]] && command -v redis-cli >/dev/null 2>&1; then
    redis-cli -h "${REDIS_ENDPOINT%:*}" -p "${REDIS_ENDPOINT##*:}" --scan --pattern "$REDIS_KEY_PREFIX*" 2>/dev/null \
      | xargs -r redis-cli -h "${REDIS_ENDPOINT%:*}" -p "${REDIS_ENDPOINT##*:}" DEL >/dev/null 2>&1 || true
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
  for _ in $(seq 1 120); do
    if (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for $name at $endpoint" >&2
  return 1
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

redis_port="$(pick_loopback_port)"
zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-cpp-e2e-discoveryregistryha" "redis:7-alpine" \
  "127.0.0.1:${redis_port}:6379"
REDIS_ENDPOINT="127.0.0.1:${redis_port}"
REDIS_OWNED=1
echo "redis endpoint=$REDIS_ENDPOINT (container $REDIS_CONTAINER)"

wait_tcp "${REDIS_ENDPOINT%:*}" "${REDIS_ENDPOINT##*:}" redis
echo "redis key prefix=$REDIS_KEY_PREFIX"

STORE_ENDPOINT="$REDIS_ENDPOINT"
if [[ "$SCENARIO" == "SF-E1" ]]; then
  redis_proxy_port="$(pick_loopback_port)"
  redis_proxy_admin_port="$(pick_loopback_port)"
  python3 "$SCRIPT_DIR/Support/redis_latency_proxy.py" \
    --listen-port "$redis_proxy_port" \
    --admin-port "$redis_proxy_admin_port" \
    --upstream-host "${REDIS_ENDPOINT%:*}" \
    --upstream-port "${REDIS_ENDPOINT##*:}" \
    >"$LOG_DIR/redis-latency-proxy.stdout.log" \
    2>"$LOG_DIR/redis-latency-proxy.stderr.log" &
  REDIS_PROXY_PID="$!"
  PIDS+=("$REDIS_PROXY_PID")
  STORE_ENDPOINT="127.0.0.1:${redis_proxy_port}"
  REDIS_PROXY_ADMIN_URL="http://127.0.0.1:${redis_proxy_admin_port}"
  wait_tcp 127.0.0.1 "$redis_proxy_port" "Redis latency proxy"
  wait_tcp 127.0.0.1 "$redis_proxy_admin_port" "Redis latency proxy admin"
  echo "redis latency proxy endpoint=$STORE_ENDPOINT admin=$REDIS_PROXY_ADMIN_URL"
fi

write_role_config() {
  local path="$1"
  local rid="$2"
  local http="$3"
  local channel="$4"
  local log_name="$5"
  python3 - "$path" "$rid" "$http" "$channel" "$STORE_ENDPOINT" "$REDIS_KEY_PREFIX" \
    "$LOG_DIR" "$log_name" "$HEARTBEAT_MS" "$LEASE_TTL_MS" "$POLLING_MS" "$GRACE_MS" <<'PY'
import json
import os
import stat
import sys

(path, rid, http_endpoint, channel_endpoint, redis_endpoint, redis_key_prefix,
 log_dir, log_name, heartbeat_ms, lease_ttl_ms, polling_ms, grace_ms) = sys.argv[1:]
role = {"rid": rid, "httpEndpoint": http_endpoint,
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "logDir": log_dir, "location": {"heartbeatMs": int(heartbeat_ms),
        "leaseTtlMs": int(lease_ttl_ms), "pollingMs": int(polling_ms),
        "graceMs": int(grace_ms)}}
if channel_endpoint:
    role["channelEndpoint"] = channel_endpoint
    role["logName"] = log_name
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": role}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

write_client_config() {
  local path="$1"
  python3 - "$path" "$SCENARIO" "$HTTP_CONSUMER" "$HTTP_A" "$HTTP_B" "$HTTP_C" \
    "$SF_A2_PROVIDER_START_FILE" "$REDIS_PROXY_ADMIN_URL" "$REDIS_CONTAINER" "$API_A" \
    "$API_B" "$HTTP_B_REPLACEMENT" "$API_B_REPLACEMENT" "$HEARTBEAT_MS" \
    "$LEASE_TTL_MS" "$POLLING_MS" "$GRACE_MS" <<'PY'
import json
import os
import stat
import sys

(path, scenario, consumer_url, provider_a_url, provider_b_url, provider_c_url,
 provider_c_start_file, redis_proxy_admin_url, redis_container, provider_a_endpoint,
 provider_b_endpoint, replacement_provider_url, replacement_provider_endpoint,
 heartbeat_ms, lease_ttl_ms, polling_ms, grace_ms) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"scenario": scenario, "consumerUrl": consumer_url,
        "providerAUrl": provider_a_url, "providerBUrl": provider_b_url,
        "providerCUrl": provider_c_url, "providerCStartFile": provider_c_start_file,
        "redisProxyAdminUrl": redis_proxy_admin_url, "redisContainer": redis_container,
        "providerAEndpoint": provider_a_endpoint, "providerBEndpoint": provider_b_endpoint,
        "replacementProviderUrl": replacement_provider_url,
        "replacementProviderEndpoint": replacement_provider_endpoint,
        "location": {"heartbeatMs": int(heartbeat_ms), "leaseTtlMs": int(lease_ttl_ms),
        "pollingMs": int(polling_ms), "graceMs": int(grace_ms)}}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

start_provider() {
  local rid="$1"
  local channel="$2"
  local http="$3"
  local log_name="${4:-$rid}"
  local config_path="$CONFIG_DIR/$log_name.json"
  write_role_config "$config_path" "$rid" "$http" "$channel" "$log_name"
  "$PROVIDER_SERVER" --config="$config_path" \
    >"$LOG_DIR/$log_name.stdout.log" 2>"$LOG_DIR/$log_name.stderr.log" &
  LAST_PID="$!"
  PIDS+=("$LAST_PID")
  wait_port "$rid-channel" "$channel"
  wait_port "$rid-http" "$http"
}

start_consumer() {
  local rid="$1"
  local http="$2"
  local config_path="$CONFIG_DIR/$rid.json"
  write_role_config "$config_path" "$rid" "$http" "" "$rid"
  "$CONSUMER_SERVER" --config="$config_path" \
    >"$LOG_DIR/$rid.stdout.log" 2>"$LOG_DIR/$rid.stderr.log" &
  LAST_PID="$!"
  PIDS+=("$LAST_PID")
  wait_port "$rid-http" "$http"
}

start_sf_b2_replacement() {
  (
    while [[ "$(docker inspect -f '{{.State.Running}}' "$REDIS_CONTAINER" 2>/dev/null || true)" == "true" ]]; do
      sleep "$LOCAL_READINESS_POLL_SECONDS"
    done
    kill -9 "$API_B_PID" >/dev/null 2>&1 || true
    while (echo >"/dev/tcp/127.0.0.1/$(port_of "$HTTP_B")") >/dev/null 2>&1; do
      sleep "$LOCAL_READINESS_POLL_SECONDS"
    done
    start_provider api-b "$API_B_REPLACEMENT" "$HTTP_B_REPLACEMENT" api-b-replacement
    wait "$LAST_PID"
  ) &
  SF_B2_REPLACEMENT_PID="$!"
  PIDS+=("$SF_B2_REPLACEMENT_PID")
}

start_sf_a2_provider() {
  (
    for _ in $(seq 1 "$PROCESS_STOP_ATTEMPTS"); do
      if [[ -f "$SF_A2_PROVIDER_START_FILE" ]]; then
        start_provider api-c "$API_C" "$HTTP_C"
        wait "$LAST_PID"
        exit $?
      fi
      sleep "$LOCAL_READINESS_POLL_SECONDS"
    done
    echo "Timed out waiting for SF-A2 provider start signal" >&2
    exit 1
  ) &
  SF_A2_PROVIDER_PID="$!"
  PIDS+=("$SF_A2_PROVIDER_PID")
}

start_provider api-a "$API_A" "$HTTP_A"
API_A_PID="$LAST_PID"
start_provider api-b "$API_B" "$HTTP_B"
API_B_PID="$LAST_PID"
start_consumer consumer "$HTTP_CONSUMER"
CONSUMER_PID="$LAST_PID"

if [[ "$SCENARIO" == "SF-B2" ]]; then
  start_sf_b2_replacement
fi
if [[ "$SCENARIO" == "SF-A2" ]]; then
  start_sf_a2_provider
fi

sleep "$ROUTE_SETTLE_SECONDS"

write_client_config "$CONFIG_DIR/client.json"
"$CLIENT" --config="$CONFIG_DIR/client.json" \
  >"$LOG_DIR/client-$SCENARIO.stdout.log" 2>"$LOG_DIR/client-$SCENARIO.stderr.log"
cat "$LOG_DIR/client-$SCENARIO.stdout.log"

if [[ "$SCENARIO" == "SF-A2" ]]; then
  wait_pid_status "$SF_A2_PROVIDER_PID" "added provider api-c" 0 130 143
  forget_pid "$SF_A2_PROVIDER_PID"
  SF_A2_PROVIDER_PID=""
fi

if [[ "$SCENARIO" == "SF-B2" ]]; then
  wait_pid_status "$API_B_PID" "expected replaced provider api-b" 137
  forget_pid "$API_B_PID"
  API_B_PID=""
  post_shutdown "$HTTP_B_REPLACEMENT/shutdown"
  wait_pid_status "$SF_B2_REPLACEMENT_PID" "replacement provider api-b" 0 130 143
  forget_pid "$SF_B2_REPLACEMENT_PID"
  SF_B2_REPLACEMENT_PID=""
fi

if [[ "$SCENARIO" == "SF-C1" || "$SCENARIO" == "SF-D2" ]]; then
  wait_pid_status "$API_B_PID" "expected crashed provider api-b" 134
  forget_pid "$API_B_PID"
fi

echo "store-failure c++ scenario=$SCENARIO result=passed"
