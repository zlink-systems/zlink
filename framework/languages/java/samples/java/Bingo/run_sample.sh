#!/usr/bin/env bash
set -euo pipefail
set +m

cd "$(dirname "${BASH_SOURCE[0]}")"

source "../../runner-common.sh"
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)

if rg -n 'System\.(getProperty|getenv)' Server Client --glob '*.java'; then
  echo "Bingo application code must use sample config files" >&2
  exit 1
fi
if rg -n -U '\.enableClient\(\s*[^)\s]|\.connect(?:Router|PeerPub)\(' Server --glob '*.java'; then
  echo "Bingo server code must use location-store automatic connections" >&2
  exit 1
fi

pids=()
redis_container_id=""
log_dir="build/sample-logs"
flow_log_dir="$(pwd)/logs"
config_dir="$(mktemp -d)"
chmod 0700 "${config_dir}"
mkdir -p "${log_dir}" "${flow_log_dir}"
rm -f "${log_dir}"/*.log
rm -f "${flow_log_dir}"/*.log

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

cleanup_sample() {
  local status="$?"
  trap - EXIT
  set +e
  set_cleanup_status() { return "$1"; }
  set_cleanup_status "${status}"
  cleanup
  local cleanup_status="$?"
  rm -rf "${config_dir}"
  if [[ "${status}" != "0" ]]; then
    exit "${status}"
  fi
  exit "${cleanup_status}"
}
trap cleanup_sample EXIT

reserve_ports() {
  local base=$((20000 + ((RANDOM + $$) % 1000) * 15 % 9000))
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
bingo_redis_key_prefix="bingo:java:${RANDOM}:$$:"
zlink_redis_start_scoped_assign redis_container_id redis_port \
  "zlink-redis-java-sample-bingo" "redis:7.2-alpine"
redis_endpoint="127.0.0.1:${redis_port}"
redis_host="${redis_endpoint%:*}"
redis_port="${redis_endpoint##*:}"
wait_port "${redis_host}" "${redis_port}"
write_config() {
  local path="$1" role_key="$2" role_value="$3"
  if [[ "${role_key}" == "clientNode" ]]; then
    cat >"$path" <<EOF
sessionAStreamEndpoint=tcp://${stream_a_host}:${stream_a_port}
sessionBStreamEndpoint=tcp://${stream_b_host}:${stream_b_port}
EOF
    chmod 0600 "$path"
    return
  fi
  cat >"$path" <<EOF
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${bingo_redis_key_prefix}
sample.logDirectory=${flow_log_dir}
sample.${role_key}=${role_value}
EOF
  case "${role_key}" in
    apiNode)
      cat >>"$path" <<EOF
sample.apiAChannelEndpoint=tcp://${api_a_host}:${api_a_port}
sample.apiBChannelEndpoint=tcp://${api_b_host}:${api_b_port}
sample.apiAMeshEndpoint=tcp://${api_a_mesh%:*}:${api_a_mesh##*:}
sample.apiBMeshEndpoint=tcp://${api_b_mesh%:*}:${api_b_mesh##*:}
sample.apiMatchmakingRouterEndpoint=tcp://$([[ "${role_value}" == "b" ]] && echo "${api_b_matchmaking_host}:${api_b_matchmaking_port}" || echo "${api_a_matchmaking_host}:${api_a_matchmaking_port}")
EOF
      ;;
    playNode)
      cat >>"$path" <<EOF
sample.playASpotRouterEndpoint=tcp://${play_a_router_host}:${play_a_router_port}
sample.playBSpotRouterEndpoint=tcp://${play_b_router_host}:${play_b_router_port}
EOF
      ;;
    sessionNode)
      cat >>"$path" <<EOF
sample.sessionARouterEndpoint=tcp://${session_a_router_host}:${session_a_router_port}
sample.sessionBRouterEndpoint=tcp://${session_b_router_host}:${session_b_router_port}
sample.sessionAStreamEndpoint=tcp://${stream_a_host}:${stream_a_port}
sample.sessionBStreamEndpoint=tcp://${stream_b_host}:${stream_b_port}
EOF
      ;;
  esac
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
write_config "$session_a_config" sessionNode a
write_config "$session_b_config" sessionNode b
write_config "$api_a_config" apiNode a
write_config "$api_b_config" apiNode b
write_config "$play_a_config" playNode a
write_config "$play_b_config" playNode b
write_config "$matchmaking_config" matchmakingNode matchmaking
cat >>"$matchmaking_config" <<EOF
sample.matchmakingRouterEndpoint=tcp://${matchmaking_router_host}:${matchmaking_router_port}
EOF
write_config "$client_config" clientNode client

build_framework_jars
rm -rf \
  Server/Session/build/install \
  Server/Api/build/install \
  Server/Play/build/install \
  Server/Matchmaking/build/install \
  Client/build/install
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

"$(app_bin Client Client)" --config "$client_config" >"${log_dir}/client.log" 2>&1

grep -q "bingo=completed" "${log_dir}/client.log"
grep -q "stream-inbound sample=Bingo" "${log_dir}/client.log"
grep -Eq "stream-inbound sample=Bingo .* name=.*Notify" "${log_dir}/client.log"
grep -Rq "message flow" "${flow_log_dir}"
grep -Eq "zlink metric .*name=zlink\.stream\.connections\.active" "${log_dir}"/session-*.log
grep -Eq "zlink metric .*name=zlink\.spot\.count" "${log_dir}"/play-*.log

echo "bingo full client/server self-check completed"
