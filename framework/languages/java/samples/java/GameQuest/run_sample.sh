#!/usr/bin/env bash
set -euo pipefail
set +m

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../../runner-common.sh"
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)

RUN_DIR="$(mktemp -d)"
chmod 0700 "${RUN_DIR}"
LOG_DIR="$RUN_DIR/logs"
BUILD_LOG="$LOG_DIR/build.log"
mkdir -p "$LOG_DIR"

read -r api_a_stream_port api_b_stream_port api_a_http_port api_b_http_port \
  mission_a_channel_port mission_b_channel_port mission_a_http_port mission_b_http_port \
  mission_a_router_port mission_b_router_port <<<"$(zlink_sample_reserve_ports 10)"
api_a_stream="tcp://127.0.0.1:${api_a_stream_port}"
api_b_stream="tcp://127.0.0.1:${api_b_stream_port}"
api_a_http="http://127.0.0.1:${api_a_http_port}"
api_b_http="http://127.0.0.1:${api_b_http_port}"
mission_a_channel="tcp://127.0.0.1:${mission_a_channel_port}"
mission_b_channel="tcp://127.0.0.1:${mission_b_channel_port}"
mission_a_http="http://127.0.0.1:${mission_a_http_port}"
mission_b_http="http://127.0.0.1:${mission_b_http_port}"
mission_a_router="tcp://127.0.0.1:${mission_a_router_port}"
mission_b_router="tcp://127.0.0.1:${mission_b_router_port}"

REDIS_CONTAINER=""
zlink_redis_start_scoped_assign REDIS_CONTAINER REDIS_PORT \
  "zlink-redis-java-sample-gamequest" "redis:7.2-alpine"
redis_endpoint="127.0.0.1:${REDIS_PORT}"
redis_key_prefix="gamequest:java:$(date +%s):$$:"

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
  if [[ "${ZLINK_SAMPLE_KEEP_RUN_DIR:-0}" == "1" ]]; then
    echo "runDir=$RUN_DIR"
  else
    rm -rf "$RUN_DIR"
  fi
  exit "$status"
}
trap on_exit EXIT

write_role_config() {
  local path="$1"
  local instance="$2"
  local endpoint_key="$3"
  local endpoint="$4"
  local http_endpoint="$5"
  cat >"$path" <<EOF
sample.instanceName=${instance}
sample.logDirectory=${LOG_DIR}
sample.${endpoint_key}=${endpoint}
sample.httpEndpoint=${http_endpoint}
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${redis_key_prefix}
EOF
}

mission_a_config="$RUN_DIR/mission-a.properties"
mission_b_config="$RUN_DIR/mission-b.properties"
api_a_config="$RUN_DIR/api-a.properties"
api_b_config="$RUN_DIR/api-b.properties"
client_config="$RUN_DIR/client.properties"
rehydrate_client_config="$RUN_DIR/rehydrate-client.properties"
write_role_config "$mission_a_config" mission-a channelEndpoint "$mission_a_channel" "$mission_a_http"
write_role_config "$mission_b_config" mission-b channelEndpoint "$mission_b_channel" "$mission_b_http"
cat >>"$mission_a_config" <<EOF
sample.spotRouterEndpoint=${mission_a_router}
EOF
cat >>"$mission_b_config" <<EOF
sample.spotRouterEndpoint=${mission_b_router}
EOF
write_role_config "$api_a_config" api-a streamEndpoint "$api_a_stream" "$api_a_http"
write_role_config "$api_b_config" api-b streamEndpoint "$api_b_stream" "$api_b_http"
cat >>"$api_a_config" <<EOF
sample.spotRouterEndpoint=${mission_a_channel}
EOF
cat >>"$api_b_config" <<EOF
sample.spotRouterEndpoint=${mission_b_channel}
EOF
write_client_config() {
  local path="$1"
  local scenario="$2"
  cat >"$path" <<EOF
sample.apiAStreamEndpoint=${api_a_stream}
sample.apiBStreamEndpoint=${api_b_stream}
sample.apiAHttpEndpoint=${api_a_http}
sample.apiBHttpEndpoint=${api_b_http}
sample.scenario=${scenario}
EOF
}
write_client_config "$client_config" full
write_client_config "$rehydrate_client_config" rehydrate
chmod 0600 "$mission_a_config" "$mission_b_config" "$api_a_config" "$api_b_config" \
  "$client_config" "$rehydrate_client_config"

cd "$ROOT_DIR"
if rg -n 'markRehydrated|recordRehydrated|owner-rehydrates' Server; then
  echo "fake rehydrate evidence must not remain in GameQuest server code" >&2
  exit 1
fi
grep -q 'addRouteMesh' Server/QuestMission/src/main/java/systems/zlink/samples/gamequest/server/questmission/Program.java
grep -q 'addInstanceSpotFactory' Server/QuestMission/src/main/java/systems/zlink/samples/gamequest/server/questmission/Program.java
rg -q 'class PlayerQuestSpot' Server/QuestMission/src/main/java
if rg -n 'public synchronized' \
  Server/QuestMission/src/main/java/systems/zlink/samples/gamequest/server/questmission/store/QuestStore.java; then
  echo "player owners must not share one QuestStore monitor" >&2
  exit 1
fi
if rg -n '\.enableClient\([^)]' Server; then
  echo "GameQuest channels must use location-store auto discovery" >&2
  exit 1
fi
if rg -n '^기준:.*dotnet' sample-porting-inventory.ko.md; then
  echo "GameQuest inventory must use the common sample contract as its authority" >&2
  exit 1
fi
grep -q 'gamequest-scale-out=completed' \
  Client/src/main/java/systems/zlink/samples/gamequest/client/GameQuestClientScenario.java
(
  cd ../../..
  ./gradlew --no-daemon \
    :zlink-framework-core:jar \
    :zlink-framework-spring-boot-starter:jar \
    :zlink-framework-locations-redis:jar \
    :zlink-stream-connector:jar --quiet
)
gradle_run :Server:GameApi:installDist :Server:QuestMission:installDist :Client:installDist >"$BUILD_LOG" 2>&1

start_role() {
  local name="$1"
  local binary="$2"
  local config="$3"
  "$binary" --config "$config" >"$LOG_DIR/$name.log" 2>&1 &
  pids+=("$!")
}

start_role mission-a "$(app_bin Server/QuestMission QuestMission)" "$mission_a_config"
start_role mission-b "$(app_bin Server/QuestMission QuestMission)" "$mission_b_config"
wait_port "$mission_a_router"
wait_port "$mission_b_router"
wait_http "$mission_a_http"
wait_http "$mission_b_http"

start_role api-a "$(app_bin Server/GameApi GameApi)" "$api_a_config"
start_role api-b "$(app_bin Server/GameApi GameApi)" "$api_b_config"
wait_port "$api_a_stream"
wait_port "$api_b_stream"
wait_port "$mission_a_channel"
wait_port "$mission_b_channel"
wait_http "$api_a_http"
wait_http "$api_b_http"
wait_framework_ready_logs "$LOG_DIR" 1
wait_framework_peer_ready_counts \
  "$LOG_DIR" \
  mission-a.log:3 \
  mission-b.log:3 \
  api-a.log:3 \
  api-b.log:3

echo "topology=ready"
"$(app_bin Client Client)" --config "$client_config" >"$LOG_DIR/client.log" 2>&1
cat "$LOG_DIR/client.log"

grep -q "gamequest-server-evidence=completed" "$LOG_DIR/client.log"
grep -q "gamequest=completed" "$LOG_DIR/client.log"
grep -q "gamequest-scale-out=completed" "$LOG_DIR/client.log"
grep -h -q 'surface=SPOT_ROUTE kind=SEND.*packet=GameplayMsg' "$LOG_DIR"/flow-mission-*.log
grep -h -q 'surface=SPOT_ROUTE kind=SEND.*packet=GameplayMsg' "$LOG_DIR"/flow-mission-*.log
grep -h -q 'packet=GameplayMsg.*spot=player-scale-a' "$LOG_DIR"/flow-mission-*.log
grep -h -q 'packet=GameplayMsg.*spot=player-scale-b' "$LOG_DIR"/flow-mission-*.log
grep -h -q 'packet=QuestProcessingMsg' "$LOG_DIR"/flow-api-*.log
echo "gamequest player owner Spot routing completed"

curl --fail --silent --request POST \
  "$mission_a_http/self-check/owner/player-alice/close" \
  | grep -q '"closed":true'
"$(app_bin Client Client)" --config "$rehydrate_client_config" >"$LOG_DIR/rehydrate-client.log" 2>&1
cat "$LOG_DIR/rehydrate-client.log"
grep -q "gamequest-rehydrate=completed" "$LOG_DIR/rehydrate-client.log"
alice_events="$(curl --fail --silent "$mission_a_http/self-check/events")"
grep -q '"questId":"first-hunt"' <<<"$alice_events"
grep -q '"eventType":"QuestProgressReconciledEvent"' <<<"$alice_events"
grep -q '"currentCount":5' <<<"$alice_events"
echo "gamequest owner replay restored player-alice"
echo "gamequest full client/server self-check completed"
