#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../redis-common.sh"
E2E_START_ORDER="forward"
ACTOR_A_RID="actor-a"
CALLER_RID="to-actor-caller"
SCENARIO_ARGS=()
while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --start-order)
      E2E_START_ORDER="$2"
      shift 2
      ;;
    --actor-rid)
      ACTOR_A_RID="$2"
      shift 2
      ;;
    --caller-rid)
      CALLER_RID="$2"
      shift 2
      ;;
    --)
      shift
      SCENARIO_ARGS+=("$@")
      break
      ;;
    *)
      SCENARIO_ARGS+=("$1")
      shift
      ;;
  esac
done
if [[ "${#SCENARIO_ARGS[@]}" -eq 0 ]]; then
  SCENARIO="all"
else
  SCENARIO="${SCENARIO_ARGS[*]}"
  SCENARIO="${SCENARIO// /,}"
fi
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/logs/$RUN_ID"
mkdir -p "$LOG_DIR"
CONFIG_DIR="$(mktemp -d)"

ACTOR_PROJECT="$ROOT_DIR/Server/Actor/ToActorMessaging.Actor.csproj"
SESSION_PROJECT="$ROOT_DIR/Server/Session/ToActorMessaging.Session.csproj"
CALLER_PROJECT="$ROOT_DIR/Server/Caller/ToActorMessaging.Caller.csproj"
CLIENT_PROJECT="$ROOT_DIR/Client/ToActorMessaging.Client.csproj"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
REDIS_READINESS_TIMEOUT_SECONDS=60
HTTP_PROBE_TIMEOUT_SECONDS=3
LOCAL_READINESS_ATTEMPTS="$(
  python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys

timeout = float(sys.argv[1])
poll = float(sys.argv[2])
print(max(1, math.ceil(timeout / poll)))
PY
)"

allocate_ports() {
  python3 - "$1" <<'PY'
import socket
import sys
sockets = []
for _ in range(int(sys.argv[1])):
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    sockets.append(sock)
print(" ".join(str(sock.getsockname()[1]) for sock in sockets))
for sock in sockets:
    sock.close()
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
  for _ in $(seq 1 "$LOCAL_READINESS_ATTEMPTS"); do
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

start_actor() {
  local role="$1" rid="$2" url="$3" router_port="$4" pubsub_port="$5" evidence="$6"
  local config="$CONFIG_DIR/$role.json"
  python3 "$ROOT_DIR/../write_role_config.py" "$config" -- \
    --rid "$rid" \
    --http-url "$url" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --router-endpoint "tcp://127.0.0.1:$router_port" \
    --pubsub-endpoint "tcp://127.0.0.1:$pubsub_port" \
    --evidence-file "$evidence" \
    --log-dir "$LOG_DIR"
  setsid dotnet run --no-build --project "$ACTOR_PROJECT" -- --config "$config" \
    >"$LOG_DIR/$role.stdout.log" 2>"$LOG_DIR/$role.stderr.log" &
  pids+=("$!")
}

start_session() {
  local role="$1" rid="$2" url="$3" router_port="$4" pubsub_port="$5" stream_port="$6" evidence="$7"
  local config="$CONFIG_DIR/$role.json"
  python3 "$ROOT_DIR/../write_role_config.py" "$config" -- \
    --rid "$rid" \
    --http-url "$url" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$REDIS_KEY_PREFIX" \
    --router-endpoint "tcp://127.0.0.1:$router_port" \
    --pubsub-endpoint "tcp://127.0.0.1:$pubsub_port" \
    --stream-endpoint "tcp://127.0.0.1:$stream_port" \
    --evidence-file "$evidence" \
    --log-dir "$LOG_DIR"
  setsid dotnet run --no-build --project "$SESSION_PROJECT" -- --config "$config" \
    >"$LOG_DIR/$role.stdout.log" 2>"$LOG_DIR/$role.stderr.log" &
  pids+=("$!")
}

start_caller() {
  local role="$1" rid="$2" url="$3" router_port="$4" pubsub_port="$5" connect_routes="$6" redis_prefix="$7"
  local config="$CONFIG_DIR/$role.json"
  python3 "$ROOT_DIR/../write_role_config.py" "$config" -- \
    --rid "$rid" \
    --http-url "$url" \
    --redis-endpoint "$REDIS_ENDPOINT" \
    --redis-key-prefix "$redis_prefix" \
    --router-endpoint "tcp://127.0.0.1:$router_port" \
    --pubsub-endpoint "tcp://127.0.0.1:$pubsub_port" \
    --actor-rid "$ACTOR_A_RID" \
    --actor-router-endpoint "tcp://127.0.0.1:$ACTOR_A_ROUTER_PORT" \
    --actor-b-rid "$ACTOR_B_RID" \
    --actor-b-router-endpoint "tcp://127.0.0.1:$ACTOR_B_ROUTER_PORT" \
    --connect-actor-routes "$connect_routes" \
    --log-dir "$LOG_DIR"
  setsid dotnet run --no-build --project "$CALLER_PROJECT" -- --config "$config" \
    >"$LOG_DIR/$role.stdout.log" 2>"$LOG_DIR/$role.stderr.log" &
  pids+=("$!")
}

start_role() {
  case "$1" in
    actor-a) start_actor actor-a "$ACTOR_A_RID" "$ACTOR_A_URL" "$ACTOR_A_ROUTER_PORT" "$ACTOR_A_PUBSUB_PORT" "$LOG_DIR/actor-a.evidence.log" ;;
    actor-b) start_actor actor-b "$ACTOR_B_RID" "$ACTOR_B_URL" "$ACTOR_B_ROUTER_PORT" "$ACTOR_B_PUBSUB_PORT" "$LOG_DIR/actor-b.evidence.log" ;;
    session-a) start_session session-a "$SESSION_A_RID" "$SESSION_A_URL" "$SESSION_A_ROUTER_PORT" "$SESSION_A_PUBSUB_PORT" "$SESSION_A_STREAM_PORT" "$LOG_DIR/session-a.evidence.log" ;;
    session-b) start_session session-b "$SESSION_B_RID" "$SESSION_B_URL" "$SESSION_B_ROUTER_PORT" "$SESSION_B_PUBSUB_PORT" "$SESSION_B_STREAM_PORT" "$LOG_DIR/session-b.evidence.log" ;;
    caller) start_caller caller "$CALLER_RID" "$CALLER_URL" "$CALLER_ROUTER_PORT" "$CALLER_PUBSUB_PORT" true "$REDIS_KEY_PREFIX" ;;
    caller-no-route) start_caller caller-no-route "$NO_ROUTE_CALLER_RID" "$NO_ROUTE_CALLER_URL" "$NO_ROUTE_CALLER_ROUTER_PORT" "$NO_ROUTE_CALLER_PUBSUB_PORT" true "$REDIS_KEY_PREFIX" ;;
    *) echo "Unknown server role '$1'" >&2; return 1 ;;
  esac
}

zlink_redis_start_scoped_assign \
  REDIS_CONTAINER \
  REDIS_ENDPOINT \
  "zlink-redis-dotnet-e2e-to-actor-messaging" \
  "redis:7.2-alpine" \
  "$LOG_DIR"
zlink_redis_wait_ready "$REDIS_CONTAINER" "$REDIS_READINESS_TIMEOUT_SECONDS"
REDIS_KEY_PREFIX="zlink:e2e:to-actor:$(date +%s)-$$"
ACTOR_B_RID="actor-b"
SESSION_A_RID="session-a"
SESSION_B_RID="session-b"
NO_ROUTE_CALLER_RID="to-actor-no-route-caller"

read -r \
  ACTOR_A_HTTP_PORT ACTOR_A_ROUTER_PORT ACTOR_A_PUBSUB_PORT \
  ACTOR_B_HTTP_PORT ACTOR_B_ROUTER_PORT ACTOR_B_PUBSUB_PORT \
  SESSION_A_HTTP_PORT SESSION_A_ROUTER_PORT SESSION_A_PUBSUB_PORT SESSION_A_STREAM_PORT \
  SESSION_B_HTTP_PORT SESSION_B_ROUTER_PORT SESSION_B_PUBSUB_PORT SESSION_B_STREAM_PORT \
  CALLER_HTTP_PORT CALLER_ROUTER_PORT CALLER_PUBSUB_PORT \
  NO_ROUTE_CALLER_HTTP_PORT NO_ROUTE_CALLER_ROUTER_PORT NO_ROUTE_CALLER_PUBSUB_PORT \
  <<<"$(allocate_ports 20)"

ACTOR_A_URL="http://127.0.0.1:$ACTOR_A_HTTP_PORT"
ACTOR_B_URL="http://127.0.0.1:$ACTOR_B_HTTP_PORT"
SESSION_A_URL="http://127.0.0.1:$SESSION_A_HTTP_PORT"
SESSION_B_URL="http://127.0.0.1:$SESSION_B_HTTP_PORT"
CALLER_URL="http://127.0.0.1:$CALLER_HTTP_PORT"
NO_ROUTE_CALLER_URL="http://127.0.0.1:$NO_ROUTE_CALLER_HTTP_PORT"

echo "log_dir=$LOG_DIR"
echo "start_order=$E2E_START_ORDER"
dotnet build "$ACTOR_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$SESSION_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$CALLER_PROJECT" --maxcpucount:1 >/dev/null
dotnet build "$CLIENT_PROJECT" --maxcpucount:1 >/dev/null

mapfile -t SERVER_ROLES < <(ordered_roles actor-a actor-b session-a session-b caller caller-no-route)
for role in "${SERVER_ROLES[@]}"; do
  start_role "$role"
done

wait_health "$ACTOR_A_URL" actor-a
wait_health "$ACTOR_B_URL" actor-b
wait_health "$SESSION_A_URL" session-a
wait_health "$SESSION_B_URL" session-b
wait_health "$CALLER_URL" caller
wait_health "$NO_ROUTE_CALLER_URL" caller-no-route

python3 "$ROOT_DIR/../write_role_config.py" "$CONFIG_DIR/client.json" -- \
    --config-dir "$CONFIG_DIR" \
  --actor-url "$ACTOR_A_URL" \
  --actor-b-url "$ACTOR_B_URL" \
  --caller-url "$CALLER_URL" \
  --no-route-caller-url "$NO_ROUTE_CALLER_URL" \
  --session-a-stream-endpoint "tcp://127.0.0.1:$SESSION_A_STREAM_PORT" \
  --session-b-stream-endpoint "tcp://127.0.0.1:$SESSION_B_STREAM_PORT" \
  --session-a-url "$SESSION_A_URL" \
  --session-b-url "$SESSION_B_URL" \
  --scenario "$SCENARIO"
dotnet run --no-build --project "$CLIENT_PROJECT" -- --config "$CONFIG_DIR/client.json" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"

cat "$LOG_DIR/client.stdout.log"
