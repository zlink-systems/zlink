#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

client_source="Client/src/main/kotlin/systems/zlink/samples/kotlin/bingo/client/BingoClientScenario.kt"
if rg -n 'receivedCount\([^)]*\)\s*==\s*0' "${client_source}"; then
  echo "Bingo negative push assertions must use expectNone" >&2
  exit 1
fi
for assertion in \
  'client1Card.state.players.all { player -> player.card.size == 9 }' \
  'client2Drawn.state == client1Drawn.state' \
  'reward.drawSeq == drawnNumbers.last().drawSeq'; do
  if ! grep -Fq "${assertion}" "${client_source}"; then
    echo "Bingo client release assertion missing: ${assertion}" >&2
    exit 1
  fi
done

source "../../runner-common.sh"
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)

pids=()
redis_container_id=""
log_dir="build/sample-logs"
flow_log_dir="$(pwd)/logs"
config_dir="build/sample-config"
export ZLINK_JAVA_STREAM_TRACE="${ZLINK_JAVA_STREAM_TRACE:-1}"
mkdir -p "${log_dir}" "${flow_log_dir}" "${config_dir}"
rm -f "${log_dir}"/*.log
rm -f "${flow_log_dir}"/*.log "${config_dir}"/*.properties

print_logs() {
  local status="$1"
  if [[ "${status}" == "0" ]]; then
    return
  fi
  for log in "${log_dir}"/*.log; do
    [[ -f "${log}" ]] || continue
    echo "===== ${log} =====" >&2
    tail -n 200 "${log}" >&2 || true
  done
}

trap cleanup EXIT

reserve_ports() {
  local base=$((48000 + ((RANDOM + $$) % 1000) * 15 % 12000))
  local endpoints=()
  for offset in $(seq 0 12); do
    endpoints+=("127.0.0.1:$((base + offset))")
  done
  echo "${endpoints[*]}"
}

build_framework_jars() {
  (
    cd ../../..
    ./gradlew --no-daemon --no-parallel \
      :zlink-framework-core:jar \
      :zlink-framework-spring-boot-starter:jar \
      :zlink-framework-kotlin:jar \
      :zlink-framework-locations-redis:jar \
      :zlink-framework-codec-protobuf:jar \
      :zlink-stream-connector:jar \
      --quiet
  )
}

read -r api_a_channel api_a_mesh session_a_router play_a_router session_a_stream api_b_channel api_b_mesh session_b_router play_b_router session_b_stream api_a_matchmaking api_b_matchmaking matchmaking_router < <(reserve_ports)
api_a_host="${api_a_channel%:*}"
api_a_port="${api_a_channel##*:}"
api_b_host="${api_b_channel%:*}"
api_b_port="${api_b_channel##*:}"
session_a_router_host="${session_a_router%:*}"
session_a_router_port="${session_a_router##*:}"
session_b_router_host="${session_b_router%:*}"
session_b_router_port="${session_b_router##*:}"
play_a_router_host="${play_a_router%:*}"
play_a_router_port="${play_a_router##*:}"
play_b_router_host="${play_b_router%:*}"
play_b_router_port="${play_b_router##*:}"
stream_a_host="${session_a_stream%:*}"
stream_a_port="${session_a_stream##*:}"
stream_b_host="${session_b_stream%:*}"
stream_b_port="${session_b_stream##*:}"
api_a_matchmaking_host="${api_a_matchmaking%:*}"
api_a_matchmaking_port="${api_a_matchmaking##*:}"
api_b_matchmaking_host="${api_b_matchmaking%:*}"
api_b_matchmaking_port="${api_b_matchmaking##*:}"
matchmaking_router_host="${matchmaking_router%:*}"
matchmaking_router_port="${matchmaking_router##*:}"
bingo_redis_key_prefix="${BINGO_REDIS_KEY_PREFIX:-bingo:kotlin:${RANDOM}:$$:}"
zlink_redis_start_scoped_assign redis_container_id redis_port \
  "zlink-redis-kotlin-sample-bingo" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
BINGO_REDIS_ENDPOINT="127.0.0.1:${redis_port}"
redis_host="${BINGO_REDIS_ENDPOINT%:*}"
redis_port="${BINGO_REDIS_ENDPOINT##*:}"
wait_port "${redis_host}" "${redis_port}"
write_config() {
  local path="$1" role_key="$2" role_value="$3"
  cat >"$path" <<EOF
sample.api-a-channel-endpoint=tcp://${api_a_host}:${api_a_port}
sample.api-b-channel-endpoint=tcp://${api_b_host}:${api_b_port}
sample.api-a-mesh-endpoint=tcp://${api_a_mesh}
sample.api-b-mesh-endpoint=tcp://${api_b_mesh}
sample.session-a-router-endpoint=tcp://${session_a_router_host}:${session_a_router_port}
sample.session-b-router-endpoint=tcp://${session_b_router_host}:${session_b_router_port}
sample.play-a-spot-router-endpoint=tcp://${play_a_router_host}:${play_a_router_port}
sample.play-b-spot-router-endpoint=tcp://${play_b_router_host}:${play_b_router_port}
sample.api-matchmaking-router-endpoint=tcp://$([[ "${role_value}" == "b" ]] && echo "${api_b_matchmaking_host}:${api_b_matchmaking_port}" || echo "${api_a_matchmaking_host}:${api_a_matchmaking_port}")
sample.matchmaking-router-endpoint=tcp://${matchmaking_router_host}:${matchmaking_router_port}
sample.session-a-stream-endpoint=tcp://${stream_a_host}:${stream_a_port}
sample.session-b-stream-endpoint=tcp://${stream_b_host}:${stream_b_port}
sample.redis-endpoint=${BINGO_REDIS_ENDPOINT}
sample.redis-key-prefix=${bingo_redis_key_prefix}
sample.log-directory=${flow_log_dir}
sample.api-node=a
sample.play-node=a
sample.session-node=a
sample.matchmaking-node=matchmaking
sample.client-node=client
sample.${role_key}=${role_value}
EOF
  chmod 0600 "$path"
}
session_a_config="${config_dir}/session-a.properties"
session_b_config="${config_dir}/session-b.properties"
api_a_config="${config_dir}/api-a.properties"
api_b_config="${config_dir}/api-b.properties"
play_a_config="${config_dir}/play-a.properties"
play_b_config="${config_dir}/play-b.properties"
matchmaking_config="${config_dir}/matchmaking.properties"
client_config="${config_dir}/client.properties"
write_config "$session_a_config" session-node a
write_config "$session_b_config" session-node b
write_config "$api_a_config" api-node a
write_config "$api_b_config" api-node b
write_config "$play_a_config" play-node a
write_config "$play_b_config" play-node b
write_config "$matchmaking_config" matchmaking-node matchmaking
write_config "$client_config" client-node client

build_framework_jars
gradle_run \
  :Server:Session:installDist \
  :Server:Api:installDist \
  :Server:Play:installDist \
  :Server:Matchmaking:installDist \
  :Client:installDist
"$(app_bin Server/Session Session)" --config "$session_a_config" >"${log_dir}/session-a.log" 2>&1 &
pids+=("$!")
"$(app_bin Server/Matchmaking Matchmaking)" --config "$matchmaking_config" >"${log_dir}/matchmaking.log" 2>&1 &
pids+=("$!")
"$(app_bin Server/Session Session)" --config "$session_b_config" >"${log_dir}/session-b.log" 2>&1 &
pids+=("$!")
"$(app_bin Server/Api Api)" --config "$api_a_config" >"${log_dir}/api-a.log" 2>&1 &
pids+=("$!")
"$(app_bin Server/Api Api)" --config "$api_b_config" >"${log_dir}/api-b.log" 2>&1 &
pids+=("$!")
"$(app_bin Server/Play Play)" --config "$play_a_config" >"${log_dir}/play-a.log" 2>&1 &
pids+=("$!")
"$(app_bin Server/Play Play)" --config "$play_b_config" >"${log_dir}/play-b.log" 2>&1 &
pids+=("$!")
wait_port "${session_a_router_host}" "${session_a_router_port}"
wait_port "${stream_a_host}" "${stream_a_port}"
wait_port "${session_b_router_host}" "${session_b_router_port}"
wait_port "${stream_b_host}" "${stream_b_port}"
wait_port "${api_a_host}" "${api_a_port}"
wait_port "${api_b_host}" "${api_b_port}"
wait_port "${matchmaking_router_host}" "${matchmaking_router_port}"
wait_port "${play_a_router_host}" "${play_a_router_port}"
wait_port "${play_b_router_host}" "${play_b_router_port}"

wait_framework_ready_logs "${log_dir}" 1
wait_framework_peer_ready_counts "${log_dir}" \
  "session-a.log:2" \
  "session-b.log:2" \
  "api-a.log:3" \
  "api-b.log:3" \
  "play-a.log:5" \
  "play-b.log:5" \
  "matchmaking.log:2"

"$(app_bin Client Client)" --config "$client_config" >"${log_dir}/client.log" 2>&1

grep -q "bingo=completed" "${log_dir}/client.log"
grep -q "stream-inbound sample=Bingo" "${log_dir}/client.log"
grep -Rq "message flow" "${flow_log_dir}"
grep -Eq "zlink metric .*name=zlink\.stream\.connections\.active" "${log_dir}"/session-*.log
grep -Eq "zlink metric .*name=zlink\.spot\.count" "${log_dir}"/play-*.log

echo "bingo full client/server self-check completed"
