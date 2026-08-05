#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
HTTP_PROBE_TIMEOUT_SECONDS=3

SERVER_PROJECT="$ROOT_DIR/Server/Main/RegistrationCodec.Server.csproj"
INVALID_SERVER_PROJECT="$ROOT_DIR/Server/InvalidDuplicate/RegistrationCodec.InvalidDuplicate.csproj"
JSON_ONLY_PEER_PROJECT="$ROOT_DIR/Server/JsonOnlyPeer/RegistrationCodec.JsonOnlyPeer.csproj"
CODEC_REQUESTER_PROJECT="$ROOT_DIR/Server/CodecRequester/RegistrationCodec.CodecRequester.csproj"
CLIENT_PROJECT="$ROOT_DIR/Client/RegistrationCodec.Client.csproj"

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

SERVER_HTTP_PORT="$(pick_port)"
CHANNEL_PORT="$(pick_port)"
JSON_ONLY_HTTP_PORT="$(pick_port)"
JSON_ONLY_CHANNEL_PORT="$(pick_port)"
CODEC_REQUESTER_HTTP_PORT="$(pick_port)"
SCENARIO_REQUESTER_HTTP_PORT="$(pick_port)"
SERVER_URL="http://127.0.0.1:$SERVER_HTTP_PORT"
CHANNEL_ENDPOINT="tcp://127.0.0.1:$CHANNEL_PORT"
JSON_ONLY_URL="http://127.0.0.1:$JSON_ONLY_HTTP_PORT"
JSON_ONLY_CHANNEL_ENDPOINT="tcp://127.0.0.1:$JSON_ONLY_CHANNEL_PORT"
CODEC_REQUESTER_URL="http://127.0.0.1:$CODEC_REQUESTER_HTTP_PORT"
SCENARIO_REQUESTER_URL="http://127.0.0.1:$SCENARIO_REQUESTER_HTTP_PORT"

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
  if [[ "$code" -ne 0 ]]; then
    echo "E2E failed. Logs: $LOG_DIR" >&2
  fi
}
trap cleanup EXIT

start_server() {
  local name="$1" project="$2"
  shift 2
  local config="$CONFIG_DIR/$name.json"
  python3 "$ROOT_DIR/../write_role_config.py" "$config" -- "$@"
  setsid dotnet run --no-build --project "$project" -- --config "$config" \
    >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  pids+=("$!")
}

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
  local name="$2"
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
      -fsS "$url/topology/ready" 2>/dev/null | grep -Fq '"ready":true'; then
      return 0
    fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $name route readiness" >&2
  return 1
}

echo "log_dir=$LOG_DIR"
dotnet build "$SERVER_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$INVALID_SERVER_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$JSON_ONLY_PEER_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$CODEC_REQUESTER_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$CLIENT_PROJECT" --maxcpucount:1 >/dev/null

start_server server "$SERVER_PROJECT" \
  --rid reg-codec-node \
  --http-url "$SERVER_URL" \
  --channel-endpoint "$CHANNEL_ENDPOINT" \
  --evidence-file "$LOG_DIR/server.evidence.log" \
  --codec-mode all \
  --log-dir "$LOG_DIR"
wait_health "$SERVER_URL" server

start_server codec-mismatch-json-only "$JSON_ONLY_PEER_PROJECT" \
  --rid codec-mismatch-json-only \
  --http-url "$JSON_ONLY_URL" \
  --channel-endpoint "$JSON_ONLY_CHANNEL_ENDPOINT" \
  --evidence-file "$LOG_DIR/codec-mismatch-json-only.evidence.log" \
  --codec-mode all \
  --log-dir "$LOG_DIR"
wait_health "$JSON_ONLY_URL" codec-mismatch-json-only

start_server codec-mismatch-requester "$CODEC_REQUESTER_PROJECT" \
  --rid codec-mismatch-requester \
  --http-url "$CODEC_REQUESTER_URL" \
  --channel-endpoint "$JSON_ONLY_CHANNEL_ENDPOINT" \
  --log-dir "$LOG_DIR"
wait_health "$CODEC_REQUESTER_URL" codec-mismatch-requester

start_server scenario-requester "$CODEC_REQUESTER_PROJECT" \
  --rid codec-scenario-requester \
  --http-url "$SCENARIO_REQUESTER_URL" \
  --channel-endpoint "$CHANNEL_ENDPOINT" \
  --log-dir "$LOG_DIR"
wait_health "$SCENARIO_REQUESTER_URL" scenario-requester

wait_route_ready "$CODEC_REQUESTER_URL" codec-mismatch-requester
wait_route_ready "$SCENARIO_REQUESTER_URL" scenario-requester

python3 "$ROOT_DIR/../write_role_config.py" "$CONFIG_DIR/client.json" -- \
    --config-dir "$CONFIG_DIR" \
  --channel-endpoint "$CHANNEL_ENDPOINT" \
  --server-url "$SERVER_URL" \
  --scenario-requester-url "$SCENARIO_REQUESTER_URL" \
  --codec-requester-url "$CODEC_REQUESTER_URL" \
  --invalid-server-project "$INVALID_SERVER_PROJECT" \
  --scenario "$SCENARIO" \
  --log-dir "$LOG_DIR"
dotnet run --no-build --project "$CLIENT_PROJECT" -- --config "$CONFIG_DIR/client.json" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"

cat "$LOG_DIR/client.stdout.log"
