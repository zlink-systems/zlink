#!/usr/bin/env bash
set -euo pipefail
set +m

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../../runner-common.sh"
zlink_sample_configure_port_pool java
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)

RUN_DIR=""
LOG_DIR=""
REDIS_CONTAINER=""
pids=()

on_exit() {
  local status="$?"
  trap - EXIT
  if [[ "$status" != "0" && -n "${LOG_DIR}" ]]; then
    for log in "$LOG_DIR"/*.log; do
      [[ -f "$log" ]] || continue
      echo "===== $log =====" >&2
      tail -n 200 "$log" >&2 || true
    done
  fi
  cleanup
  [[ -z "${RUN_DIR}" ]] || rm -rf "$RUN_DIR"
  exit "$status"
}
trap on_exit EXIT

cd "$ROOT_DIR"

if grep -R --include='*.java' -n '\.connectRouter(' Server; then
  echo "SupportChat must use location-store-based automatic router connections." >&2
  exit 1
fi

client_source="Client/src/main/java/systems/zlink/samples/supportchat/client/Program.java"
if ! rg -q 'expectNone\(Messages\.(TypingChangedNotify|ConversationClosedNotify)\.class\)' "${client_source}"; then
  echo "SupportChat negative push checks must use connector expectNone." >&2
  exit 1
fi
if rg -n 'private static void expect(Failure|Timeout)\(' "${client_source}"; then
  echo "SupportChat must not rebuild connector assertion helpers locally." >&2
  exit 1
fi
if rg -n 'CompletableFuture<Messages\.JoinConversationRes>|awaitJoin' Server; then
  echo "SupportChat handlers must return after scheduling a deferred actor Spot join." >&2
  exit 1
fi

RUN_DIR="$(mktemp -d)"
chmod 0700 "${RUN_DIR}"
LOG_DIR="$RUN_DIR/logs"
ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="support.log api.log session.log"
BUILD_LOG="$LOG_DIR/build.log"
mkdir -p "$LOG_DIR"

read -r api_channel_port support_channel_port session_stream_port \
  session_router_port support_router_port api_router_port api_http_port support_http_port \
  <<<"$(zlink_sample_reserve_ports 8)"
api_channel_endpoint="tcp://127.0.0.1:${api_channel_port}"
support_channel_endpoint="tcp://127.0.0.1:${support_channel_port}"
session_stream_endpoint="tcp://127.0.0.1:${session_stream_port}"
session_router_endpoint="tcp://127.0.0.1:${session_router_port}"
support_router_endpoint="tcp://127.0.0.1:${support_router_port}"
api_router_endpoint="tcp://127.0.0.1:${api_router_port}"
api_http_endpoint="http://127.0.0.1:${api_http_port}"
support_http_endpoint="http://127.0.0.1:${support_http_port}"
redis_key_prefix="zlink:supportchat:sample:$(date +%s):$$"

zlink_redis_start_scoped_assign REDIS_CONTAINER REDIS_PORT \
  "zlink-redis-java-sample-supportchat" "redis:7.2-alpine"
redis_endpoint="127.0.0.1:$REDIS_PORT"

api_config="$RUN_DIR/api.properties"
session_config="$RUN_DIR/session.properties"
support_config="$RUN_DIR/support.properties"
cat >"$api_config" <<EOF
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${redis_key_prefix}
sample.logDirectory=${LOG_DIR}
sample.apiChannelEndpoint=${api_channel_endpoint}
sample.apiSpotRouterEndpoint=${api_router_endpoint}
sample.apiHttpEndpoint=${api_http_endpoint}
EOF
cat >"$session_config" <<EOF
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${redis_key_prefix}
sample.logDirectory=${LOG_DIR}
sample.sessionStreamEndpoint=${session_stream_endpoint}
sample.sessionSpotRouterEndpoint=${session_router_endpoint}
sample.supportSpotRouterEndpoint=${support_router_endpoint}
EOF
cat >"$support_config" <<EOF
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${redis_key_prefix}
sample.logDirectory=${LOG_DIR}
sample.supportChannelEndpoint=${support_channel_endpoint}
sample.supportSpotRouterEndpoint=${support_router_endpoint}
sample.sessionSpotRouterEndpoint=${session_router_endpoint}
sample.supportHttpEndpoint=${support_http_endpoint}
EOF
chmod 0600 "$api_config" "$session_config" "$support_config"

cd "$ROOT_DIR"
zlink_sample_gradle_locked ../../gradlew --settings-file standalone.settings.gradle.kts \
  --no-daemon --no-parallel --max-workers=1 \
  :Server:Api:installDist \
  :Server:Session:installDist \
  :Server:Support:installDist \
  :Client:installDist >"$BUILD_LOG" 2>&1

start_role() {
  local name="$1"
  local binary="$2"
  local config="$3"
  "$binary" --config "$config" >"$LOG_DIR/$name.log" 2>&1 &
  pids+=("$!")
}

start_role support "$ROOT_DIR/Server/Support/build/install/Support/bin/Support" "$support_config"
start_role api "$ROOT_DIR/Server/Api/build/install/Api/bin/Api" "$api_config"
start_role session "$ROOT_DIR/Server/Session/build/install/Session/bin/Session" "$session_config"
disown "${pids[@]}" 2>/dev/null || true

wait_log_count() {
  local expected="$1"
  local evidence="$2"
  shift 2
  local count
  for _ in $(seq 1 300); do
    count="$(log_count "$evidence" "$@")"
    if (( count == expected )); then
      return 0
    fi
    if (( count > expected )); then
      echo "Expected $expected '$evidence', found $count." >&2
      return 1
    fi
    sleep 0.1
  done
  echo "Timed out waiting for $expected '$evidence'." >&2
  return 1
}

log_count() {
  local evidence="$1"
  shift
  { grep -Fh -- "$evidence" "$@" 2>/dev/null || true; } | wc -l | tr -d '[:space:]'
}

wait_log_at_least() {
  local minimum="$1"
  local evidence="$2"
  shift 2
  local count
  for _ in $(seq 1 300); do
    count="$(log_count "$evidence" "$@")"
    if (( count >= minimum )); then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for at least $minimum '$evidence'." >&2
  return 1
}

wait_log_count 1 "supportchat-ready kind=public node=api" "$LOG_DIR/api.log"
wait_log_count 1 "supportchat-ready kind=public node=support" "$LOG_DIR/support.log"
wait_log_count 1 "supportchat-ready kind=stream node=session" "$LOG_DIR/session.log"
wait_log_count 1 "supportchat-ready kind=spot-route node=api mesh=supportchat-actors" "$LOG_DIR/api.log"
wait_log_count 1 "supportchat-ready kind=spot-route node=session mesh=supportchat-actors" "$LOG_DIR/session.log"

"$ROOT_DIR/Client/build/install/Client/bin/Client" \
  --stream-endpoint "$session_stream_endpoint" >"$LOG_DIR/client.log" 2>&1
cat "$LOG_DIR/client.log"

wait_log_count 1 "supportchat=completed" "$LOG_DIR/client.log"
wait_log_count 1 "supportchat-closed-typing-ignore=verified" "$LOG_DIR/client.log"

server_logs=("$LOG_DIR/api.log" "$LOG_DIR/support.log")
wait_log_at_least 1 "supportchat-conversation created conversation=" "${server_logs[@]}"
wait_log_at_least 1 "supportchat-conversation agent-joined conversation=" "${server_logs[@]}"
wait_log_at_least 1 "supportchat-conversation status=WaitingForAgent conversation=" "${server_logs[@]}"
wait_log_at_least 1 "supportchat-conversation status=Active conversation=" "${server_logs[@]}"
wait_log_at_least 1 "supportchat-conversation status=WaitingForClose conversation=" "${server_logs[@]}"
wait_log_at_least 1 "supportchat-conversation status=Closed conversation=" "${server_logs[@]}"

cleanup
trap - EXIT
rm -rf "$RUN_DIR"
echo "supportchat-placement=completed"
