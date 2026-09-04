#!/usr/bin/env bash
set -euo pipefail
set +m

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../../runner-common.sh"
zlink_sample_configure_port_pool kotlin
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)

RUN_DIR=""
LOG_DIR=""
REDIS_CONTAINER=""
pids=()
declare -A role_pids
readonly WAIT_ATTEMPTS=300
readonly WAIT_INTERVAL_SECONDS=0.1

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
  if [[ -n "${RUN_DIR}" ]]; then
    if [[ "${ZLINK_SAMPLE_KEEP_RUN_DIR:-0}" == "1" ]]; then
      echo "runDir=$RUN_DIR"
    else
      rm -rf "$RUN_DIR"
    fi
  fi
  exit "$status"
}
trap on_exit EXIT

RUN_DIR="$(mktemp -d)"
LOG_DIR="$RUN_DIR/logs"
ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="mission-a.log mission-b.log api-a.log api-b.log"
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

zlink_redis_start_scoped_assign REDIS_CONTAINER REDIS_PORT \
  "zlink-redis-kotlin-sample-gamequest" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
redis_endpoint="127.0.0.1:${REDIS_PORT}"
redis_key_prefix="gamequest:kotlin:$(date +%s):$$:"

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
control_dir="$RUN_DIR/control"
mkdir -p "$control_dir"
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
sample.controlDirectory=${control_dir}
EOF
chmod 0600 "$mission_a_config" "$mission_b_config" "$api_a_config" "$api_b_config" "$client_config"

cd "$ROOT_DIR"
(
  cd ../../..
  zlink_sample_gradle_locked ./gradlew --no-daemon --no-parallel --max-workers=1 \
    :zlink-framework-core:jar :zlink-framework-kotlin:jar \
    :zlink-framework-spring-boot-starter:jar :zlink-framework-locations-redis:jar \
    :zlink-stream-connector:jar --quiet
)
gradle_run :Server:GameApi:installDist :Server:QuestMission:installDist :Client:installDist >"$BUILD_LOG" 2>&1

start_role() {
  local name="$1" binary="$2" config="$3"
  "$binary" --config "$config" >"$LOG_DIR/$name.log" 2>&1 &
  pids+=("$!")
  role_pids["$name"]="$!"
}

wait_for_line() {
  local log="$1" line="$2"
  local attempt
  for ((attempt = 1; attempt <= WAIT_ATTEMPTS; attempt++)); do
    if grep -qF -- "$line" "$log"; then
      return
    fi
    sleep "$WAIT_INTERVAL_SECONDS"
  done
  echo "Timed out waiting for sample evidence '$line' in $log" >&2
  return 1
}

wait_for_min_count() {
  local log="$1" line="$2" minimum="$3"
  local attempt count
  for ((attempt = 1; attempt <= WAIT_ATTEMPTS; attempt++)); do
    count=$(grep -cF -- "$line" "$log" || true)
    if (( count >= minimum )); then
      return
    fi
    sleep "$WAIT_INTERVAL_SECONDS"
  done
  echo "Timed out waiting for $minimum sample evidence rows '$line' in $log" >&2
  return 1
}

wait_for_min_total() {
  local minimum="$1" line="$2"
  shift 2
  local attempt file count matches
  for ((attempt = 1; attempt <= WAIT_ATTEMPTS; attempt++)); do
    count=0
    for file in "$@"; do
      matches=$(grep -cF -- "$line" "$file" || true)
      ((count += matches)) || true
    done
    if (( count >= minimum )); then
      return
    fi
    sleep "$WAIT_INTERVAL_SECONDS"
  done
  echo "Timed out waiting for at least $minimum sample evidence rows '$line'" >&2
  return 1
}

wait_for_exact_total() {
  local expected="$1" line="$2"
  shift 2
  local attempt file count matches
  for ((attempt = 1; attempt <= WAIT_ATTEMPTS; attempt++)); do
    count=0
    for file in "$@"; do
      matches=$(grep -cF -- "$line" "$file" || true)
      ((count += matches)) || true
    done
    if (( count == expected )); then
      return
    fi
    if (( count > expected )); then
      echo "Found $count sample evidence rows '$line'; expected $expected" >&2
      return 1
    fi
    sleep "$WAIT_INTERVAL_SECONDS"
  done
  echo "Timed out waiting for exactly $expected sample evidence rows '$line'" >&2
  return 1
}

wait_for_replayed_owner() {
  local attempt
  for ((attempt = 1; attempt <= WAIT_ATTEMPTS; attempt++)); do
    if grep -qF -- "gamequest-mission replayed player=player-alice generation=" "$LOG_DIR/mission-a.log"; then
      printf '%s' mission-a
      return
    fi
    if grep -qF -- "gamequest-mission replayed player=player-alice generation=" "$LOG_DIR/mission-b.log"; then
      printf '%s' mission-b
      return
    fi
    sleep "$WAIT_INTERVAL_SECONDS"
  done
  echo "Timed out waiting for the replayed player-alice owner" >&2
  return 1
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

wait_for_line "$LOG_DIR/mission-a.log" "gamequest-ready kind=instance-factory node=mission-a"
wait_for_line "$LOG_DIR/mission-b.log" "gamequest-ready kind=instance-factory node=mission-b"
wait_for_line "$LOG_DIR/api-a.log" "gamequest-ready kind=stream node=api-a"
wait_for_line "$LOG_DIR/api-b.log" "gamequest-ready kind=stream node=api-b"
wait_for_line "$LOG_DIR/api-a.log" "gamequest-ready kind=spot-route node=api-a mesh=gamequest.player-quests"
wait_for_line "$LOG_DIR/api-b.log" "gamequest-ready kind=spot-route node=api-b mesh=gamequest.player-quests"

echo "topology=ready"
"$(app_bin Client Client)" --config "$client_config" >"$LOG_DIR/client.log" 2>&1 &
client_pid="$!"
pids+=("$client_pid")
wait_for_line "$LOG_DIR/client.log" "gamequest-owner-termination-ready player=player-alice"
owner_role="$(wait_for_replayed_owner)"
owner_pid="${role_pids[$owner_role]}"
kill -KILL "$owner_pid"
wait "$owner_pid" || true
remaining_pids=()
for pid in "${pids[@]}"; do
  [[ "$pid" == "$owner_pid" ]] || remaining_pids+=("$pid")
done
pids=("${remaining_pids[@]}")
if [[ "$owner_role" == mission-a ]]; then
  ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="mission-b.log api-a.log api-b.log"
else
  ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="mission-a.log api-a.log api-b.log"
fi
touch "$control_dir/owner-terminated"
wait "$client_pid"
cat "$LOG_DIR/client.log"
wait_for_line "$LOG_DIR/client.log" "gamequest=completed"
wait_for_line "$LOG_DIR/client.log" "gamequest-server-evidence=completed"
wait_for_min_total 4 "gamequest-api event-routed player=" \
  "$LOG_DIR/api-a.log" "$LOG_DIR/api-b.log"
wait_for_min_total 4 "gamequest-mission processed player=" \
  "$LOG_DIR/mission-a.log" "$LOG_DIR/mission-b.log"
wait_for_exact_total 1 "gamequest-mission reconciled player=player-alice quest=first-hunt" \
  "$LOG_DIR/mission-a.log" "$LOG_DIR/mission-b.log"
wait_for_exact_total 1 "gamequest-mission replayed player=player-alice generation=" \
  "$LOG_DIR/mission-a.log" "$LOG_DIR/mission-b.log"
wait_for_exact_total 1 "gamequest-owner unavailable player=player-alice" \
  "$LOG_DIR/api-a.log" "$LOG_DIR/api-b.log"
wait_for_exact_total 0 "gamequest-owner replacement-handler-invoked player=player-alice" \
  "$LOG_DIR/mission-a.log" "$LOG_DIR/mission-b.log"
echo "gamequest-placement=completed"
