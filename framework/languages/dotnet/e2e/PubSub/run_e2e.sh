#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/logs/$RUN_ID"
if [[ "$#" -eq 0 ]]; then
  SCENARIO="all"
else
  SCENARIO="$*"
  SCENARIO="${SCENARIO// /,}"
fi
mkdir -p "$LOG_DIR"
CONFIG_DIR="$(mktemp -d)"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
HTTP_PROBE_TIMEOUT_SECONDS=3
# The readiness barrier below drives a real connect and first delivery, so it
# needs a longer budget than a local HTTP health probe.
WARMUP_TIMEOUT_SECONDS=30
MAIN_TOPIC=orders

PUBLISHER_PROJECT="$ROOT_DIR/Server/Publisher/PubSub.Publisher.csproj"
SUBSCRIBER_PROJECT="$ROOT_DIR/Server/Subscriber/PubSub.Subscriber.csproj"
CLIENT_PROJECT="$ROOT_DIR/Client/PubSub.Client.csproj"

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

PUB_HTTP_PORT="$(pick_port)"
SUB_1_HTTP_PORT="$(pick_port)"
SUB_2_HTTP_PORT="$(pick_port)"
SUB_3_HTTP_PORT="$(pick_port)"
SUB_LATE_HTTP_PORT="$(pick_port)"
PUB_PORT="$(pick_port)"

PUB_ENDPOINT="tcp://127.0.0.1:$PUB_PORT"
PUB_URL="http://127.0.0.1:$PUB_HTTP_PORT"
SUB_1_URL="http://127.0.0.1:$SUB_1_HTTP_PORT"
SUB_2_URL="http://127.0.0.1:$SUB_2_HTTP_PORT"
SUB_3_URL="http://127.0.0.1:$SUB_3_HTTP_PORT"
SUB_LATE_URL="http://127.0.0.1:$SUB_LATE_HTTP_PORT"

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

start_server() {
  local name="$1"
  local project="$2"
  shift
  shift
  local config="$CONFIG_DIR/$name.json"
  python3 "$ROOT_DIR/../write_role_config.py" "$config" -- "$@"
  setsid dotnet run --no-build --project "$project" -- --config "$config" \
    >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  pids+=("$!")
}

echo "log_dir=$LOG_DIR"
dotnet build "$PUBLISHER_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$SUBSCRIBER_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$CLIENT_PROJECT" --maxcpucount:1 >/dev/null

for sub in 1 2 3; do
  url_var="SUB_${sub}_URL"
  extra_args=()
  if [[ "$sub" == "3" ]]; then
    extra_args+=(--handler-delay-ms 3000)
  fi
  start_server "sub-$sub" "$SUBSCRIBER_PROJECT" \
    --rid "sub-$sub" \
    --http-url "${!url_var}" \
    --publisher-endpoint "$PUB_ENDPOINT" \
    --evidence-file "$LOG_DIR/sub-$sub.evidence.log" \
    --log-dir "$LOG_DIR" \
    "${extra_args[@]}"
  wait_health "${!url_var}" "sub-$sub"
done

# The subscribers connect first so that no scenario record can be published
# into a fanout nobody has subscribed to yet.
start_server pub-a "$PUBLISHER_PROJECT" \
  --rid pub-a \
  --http-url "$PUB_URL" \
  --publisher-endpoint "$PUB_ENDPOINT" \
  --evidence-file "$LOG_DIR/pub-a.evidence.log" \
  --log-dir "$LOG_DIR"
wait_health "$PUB_URL" pub-a

# Classic fanout has no status surface to poll. Spec 24 §2.2 scopes topology
# state to RouteMesh, ClientServer and automatic fanout, and the fanout runtime
# registers only channels whose subscriber uses automatic discovery, so asking
# it about this manual channel is an error rather than a "not ready" answer.
# Spec 29 §170 defines classic fanout readiness as the subscriber receiving its
# first valid application record, which is what this barrier waits for.
#
# The record is republished on an interval instead of sent once. A record
# published before a subscriber's subscription has reached the publisher is
# dropped with no trace, so a single warm-up can vanish and leave the barrier
# waiting on evidence that will never arrive.
warmup_run="warmup-$RUN_ID"
warmup_deadline_ns="$(python3 - "$WARMUP_TIMEOUT_SECONDS" <<'PY'
import sys
import time

print(time.monotonic_ns() + int(float(sys.argv[1]) * 1_000_000_000))
PY
)"
warmup_sequence=0
while true; do
  warmup_pending=0
  for sub in 1 2 3; do
    if ! grep -Fq "run=$warmup_run" "$LOG_DIR/sub-$sub.evidence.log" 2>/dev/null; then
      warmup_pending=1
    fi
  done
  if [[ "$warmup_pending" == "0" ]]; then
    break
  fi
  if [[ "$(python3 - "$warmup_deadline_ns" <<'PY'
import sys
import time

print("yes" if time.monotonic_ns() < int(sys.argv[1]) else "no")
PY
  )" != "yes" ]]; then
    echo "Timed out waiting ${WARMUP_TIMEOUT_SECONDS}s for subscribers to receive the warm-up record" >&2
    exit 1
  fi
  warmup_sequence=$((warmup_sequence + 1))
  curl --max-time "$HTTP_PROBE_TIMEOUT_SECONDS" -fsS -X POST \
    "$PUB_URL/publish/event?topic=$MAIN_TOPIC&runId=$warmup_run&sequence=$warmup_sequence&value=warmup" \
    >/dev/null 2>&1 || true
  sleep "$LOCAL_READINESS_POLL_SECONDS"
done

# The warm-up records are readiness proof, not scenario evidence. Drop them so
# that scenarios reading from index 0 see only what they published.
for sub in 1 2 3; do
  url_var="SUB_${sub}_URL"
  curl --max-time "$HTTP_PROBE_TIMEOUT_SECONDS" -fsS -X POST \
    "${!url_var}/evidence/clear" >/dev/null
done

python3 "$ROOT_DIR/../write_role_config.py" "$CONFIG_DIR/client.json" -- \
    --config-dir "$CONFIG_DIR" \
  --publisher-url "$PUB_URL" \
  --subscriber-url "$SUB_1_URL" \
  --subscriber-url "$SUB_2_URL" \
  --subscriber-url "$SUB_3_URL" \
  --late-subscriber-url "$SUB_LATE_URL" \
  --publisher-endpoint "$PUB_ENDPOINT" \
  --publisher-project "$PUBLISHER_PROJECT" \
  --subscriber-project "$SUBSCRIBER_PROJECT" \
  --log-dir "$LOG_DIR" \
  --scenario "$SCENARIO"
dotnet run --no-build --project "$CLIENT_PROJECT" -- --config "$CONFIG_DIR/client.json" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"

cat "$LOG_DIR/client.stdout.log"
