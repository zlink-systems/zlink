#!/usr/bin/env bash
set -euo pipefail
set +m

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../../runner-common.sh"
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)
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
BUILD_LOG="$LOG_DIR/build.log"
mkdir -p "$LOG_DIR"

read -r api_channel_port support_channel_port session_stream_port \
  session_router_port support_router_port api_http_port support_http_port \
  <<<"$(zlink_sample_reserve_ports 7)"
api_channel_endpoint="tcp://127.0.0.1:${api_channel_port}"
support_channel_endpoint="tcp://127.0.0.1:${support_channel_port}"
session_stream_endpoint="tcp://127.0.0.1:${session_stream_port}"
session_router_endpoint="tcp://127.0.0.1:${session_router_port}"
support_router_endpoint="tcp://127.0.0.1:${support_router_port}"
api_http_endpoint="http://127.0.0.1:${api_http_port}"
support_http_endpoint="http://127.0.0.1:${support_http_port}"
redis_key_prefix="zlink:supportchat:sample:$(date +%s):$$"

REDIS_CONTAINER=""
zlink_redis_start_scoped_assign REDIS_CONTAINER REDIS_PORT \
  "zlink-redis-java-sample-supportchat" "redis:7.2-alpine"
redis_endpoint="127.0.0.1:$REDIS_PORT"

pids=()
on_exit() {
  local status="$?"
  trap - EXIT
  if [[ "$status" != "0" ]]; then
    for log in "$LOG_DIR"/*.log; do
      [[ -f "$log" ]] || continue
      echo "===== $log =====" >&2
      tail -n 200 "$log" >&2 || true
    done
  fi
  cleanup
  rm -rf "$RUN_DIR"
  exit "$status"
}
trap on_exit EXIT

api_config="$RUN_DIR/api.properties"
session_config="$RUN_DIR/session.properties"
support_config="$RUN_DIR/support.properties"
cat >"$api_config" <<EOF
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${redis_key_prefix}
sample.logDirectory=${LOG_DIR}
sample.apiChannelEndpoint=${api_channel_endpoint}
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
../../gradlew --settings-file standalone.settings.gradle.kts --no-daemon \
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

wait_http "$support_http_endpoint"
wait_http "$api_http_endpoint"
wait_port "$support_channel_endpoint"
wait_port "$support_router_endpoint"
wait_port "$api_channel_endpoint"
wait_port "$session_router_endpoint"
wait_port "$session_stream_endpoint"
wait_framework_ready_logs "$LOG_DIR"
wait_framework_peer_ready_counts "$LOG_DIR" \
  "session.log:1" \
  "support.log:1"
echo "topology=ready"

"$ROOT_DIR/Client/build/install/Client/bin/Client" \
  --stream-endpoint "$session_stream_endpoint" >"$LOG_DIR/client.log" 2>&1
cat "$LOG_DIR/client.log"

conversation_id="$(sed -n 's/^supportchat-conversation=//p' "$LOG_DIR/client.log" | tail -1)"
if [[ -z "$conversation_id" ]]; then
  echo "missing supportchat conversation marker" >&2
  exit 1
fi

grep -q "support conversation: created" "$LOG_DIR/support.log"
grep -q "status=Active" "$LOG_DIR/support.log"
grep -q "status=WaitingForClose" "$LOG_DIR/support.log"
grep -q "status=Closed" "$LOG_DIR/support.log"
grep -q "supportchat-closed-typing-ignore=verified" "$LOG_DIR/client.log"

for file in "$LOG_DIR"/flow-*.log; do
  if [[ ! -s "$file" ]]; then
    echo "missing message flow log: $file" >&2
    exit 1
  fi
done

echo "supportchat-server-evidence=completed"
echo "supportchat full client/server self-check completed"
