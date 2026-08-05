#!/usr/bin/env bash
set -euo pipefail
set +m

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../../runner-common.sh"
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)

RUN_DIR="$(mktemp -d)"
LOG_DIR="$RUN_DIR/logs"
BUILD_LOG="$LOG_DIR/build.log"
mkdir -p "$LOG_DIR"

read -r api_a_stream_port api_b_stream_port api_a_http_port api_b_http_port \
  mission_a_channel_port mission_b_channel_port mission_a_http_port mission_b_http_port \
  <<<"$(zlink_sample_reserve_ports 8)"
api_a_stream="tcp://127.0.0.1:${api_a_stream_port}"
api_b_stream="tcp://127.0.0.1:${api_b_stream_port}"
api_a_http="http://127.0.0.1:${api_a_http_port}"
api_b_http="http://127.0.0.1:${api_b_http_port}"
mission_a_channel="tcp://127.0.0.1:${mission_a_channel_port}"
mission_b_channel="tcp://127.0.0.1:${mission_b_channel_port}"
mission_a_http="http://127.0.0.1:${mission_a_http_port}"
mission_b_http="http://127.0.0.1:${mission_b_http_port}"

REDIS_CONTAINER=""
zlink_redis_start_scoped_assign REDIS_CONTAINER REDIS_PORT \
  "zlink-redis-kotlin-sample-gamequest" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
redis_endpoint="127.0.0.1:${REDIS_PORT}"
redis_key_prefix="gamequest:kotlin:$(date +%s):$$:"

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
  local path="$1" instance="$2" endpoint_key="$3" endpoint="$4" http_endpoint="$5"
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
write_role_config "$mission_a_config" mission-a channelEndpoint "$mission_a_channel" "$mission_a_http"
write_role_config "$mission_b_config" mission-b channelEndpoint "$mission_b_channel" "$mission_b_http"
write_role_config "$api_a_config" api-a streamEndpoint "$api_a_stream" "$api_a_http"
write_role_config "$api_b_config" api-b streamEndpoint "$api_b_stream" "$api_b_http"
cat >"$client_config" <<EOF
sample.apiAStreamEndpoint=${api_a_stream}
sample.apiBStreamEndpoint=${api_b_stream}
sample.apiAHttpEndpoint=${api_a_http}
sample.apiBHttpEndpoint=${api_b_http}
sample.missionAHttpEndpoint=${mission_a_http}
sample.missionBHttpEndpoint=${mission_b_http}
EOF
chmod 0600 "$mission_a_config" "$mission_b_config" "$api_a_config" "$api_b_config" "$client_config"

cd "$ROOT_DIR"
(
  cd ../../..
  ./gradlew --no-daemon --no-parallel --max-workers=1 \
    :zlink-framework-core:jar :zlink-framework-kotlin:jar \
    :zlink-framework-spring-boot-starter:jar :zlink-framework-locations-redis:jar \
    :zlink-stream-connector:jar --quiet
)
gradle_run :Server:GameApi:installDist :Server:QuestMission:installDist :Client:installDist >"$BUILD_LOG" 2>&1

start_role() {
  local name="$1" binary="$2" config="$3"
  "$binary" --config "$config" >"$LOG_DIR/$name.log" 2>&1 &
  pids+=("$!")
}

start_role mission-a "$(app_bin Server/QuestMission QuestMission)" "$mission_a_config"
start_role mission-b "$(app_bin Server/QuestMission QuestMission)" "$mission_b_config"
wait_port "$mission_a_channel"
wait_port "$mission_b_channel"
wait_http "$mission_a_http"
wait_http "$mission_b_http"

start_role api-a "$(app_bin Server/GameApi GameApi)" "$api_a_config"
start_role api-b "$(app_bin Server/GameApi GameApi)" "$api_b_config"
wait_port "$api_a_stream"
wait_port "$api_b_stream"
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
echo "gamequest kotlin full client/server self-check completed"
