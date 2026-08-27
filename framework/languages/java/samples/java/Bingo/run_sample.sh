#!/usr/bin/env bash
set -euo pipefail
set +m

cd "$(dirname "${BASH_SOURCE[0]}")"

source "../../runner-common.sh"
zlink_sample_configure_port_pool java
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
ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="session-a.log session-b.log matchmaking.log api-a.log api-b.log play-a.log play-b.log"
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

log_count() {
  local evidence="$1"
  shift
  { grep -Fh -- "${evidence}" "$@" 2>/dev/null || true; } | wc -l | tr -d '[:space:]'
}

wait_log_count() {
  local expected="$1" evidence="$2"
  shift 2
  local actual
  for _ in $(seq 1 300); do
    actual="$(log_count "${evidence}" "$@")"
    if [[ "${actual}" == "${expected}" ]]; then
      return 0
    fi
    if (( actual > expected )); then
      echo "Expected ${expected} matches for '${evidence}' in $*, found ${actual}." >&2
      return 1
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${expected} matches for '${evidence}' in $*." >&2
  return 1
}

require_log_count() {
  local expected="$1" evidence="$2"
  shift 2
  local actual
  actual="$(log_count "${evidence}" "$@")"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "Expected ${expected} matches for '${evidence}' in $*, found ${actual}." >&2
    return 1
  fi
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

build_framework_jars() {
  (
    cd ../../..
    zlink_sample_gradle_locked ./gradlew --no-daemon --no-parallel --max-workers=1 \
      :zlink-framework-core:jar \
      :zlink-framework-spring-boot-starter:jar \
      :zlink-framework-locations-redis:jar \
      :zlink-framework-codec-protobuf:jar \
      :zlink-stream-connector:jar \
      --quiet
  )
}

read -r api_a_channel api_a_mesh session_a_router play_a_router session_a_stream api_b_channel api_b_mesh session_b_router play_b_router session_b_stream api_a_matchmaking api_b_matchmaking matchmaking_router \
  <<<"$(zlink_sample_reserve_endpoints 13)"
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

wait_log_count 1 "bingo-ready kind=peer-route node=play-a peer=play-b" "${log_dir}/play-a.log"
wait_log_count 1 "bingo-ready kind=peer-route node=play-b peer=play-a" "${log_dir}/play-b.log"
wait_log_count 1 "bingo-ready kind=mesh-route node=api-a mesh=matchmaking" "${log_dir}/api-a.log"
wait_log_count 1 "bingo-ready kind=mesh-route node=api-a mesh=room" "${log_dir}/api-a.log"
wait_log_count 1 "bingo-ready kind=mesh-route node=api-b mesh=matchmaking" "${log_dir}/api-b.log"
wait_log_count 1 "bingo-ready kind=mesh-route node=api-b mesh=room" "${log_dir}/api-b.log"
wait_log_count 1 "bingo-ready kind=mesh-route node=session-a mesh=room" "${log_dir}/session-a.log"
wait_log_count 1 "bingo-ready kind=mesh-route node=session-b mesh=room" "${log_dir}/session-b.log"

"$(app_bin Client Client)" --config "$client_config" >"${log_dir}/client.log" 2>&1

grep -q "bingo=completed" "${log_dir}/client.log"
grep -q "stream-inbound sample=Bingo" "${log_dir}/client.log"
grep -Eq "stream-inbound sample=Bingo .* name=.*Notify" "${log_dir}/client.log"
grep -Eq "zlink flow: event_id=zlink\.message_flow" "${log_dir}"/{session,api,play}-*.log
grep -Eq "zlink metric .*name=zlink\.stream\.connections\.active" "${log_dir}"/session-*.log
grep -Eq "zlink metric .*name=zlink\.spot\.count" "${log_dir}"/play-*.log

play_logs=("${log_dir}/play-a.log" "${log_dir}/play-b.log")
session_logs=("${log_dir}/session-a.log" "${log_dir}/session-b.log")
business_evidence=(
  "bingo-record fetched actor=player-1 wins=0 losses=0"
  "bingo-record fetched actor=player-2 wins=0 losses=0"
  "bingo-record reported actor=player-1 wins=1 losses=0"
  "bingo-record reported actor=player-2 wins=0 losses=1"
  "bingo-lifecycle room-leave actor=player-1"
  "bingo-lifecycle room-leave actor=player-2"
  "bingo-lifecycle room-leave actor=observer"
  "bingo-lifecycle entry-leave actor=player-1"
  "bingo-lifecycle entry-leave actor=player-2"
  "bingo-lifecycle entry-leave actor=observer"
  "bingo-lifecycle entry-destroy-complete actor=player-1"
  "bingo-lifecycle entry-destroy-complete actor=player-2"
)
for evidence in "${business_evidence[@]}"; do
  wait_log_count 1 "${evidence}" "${play_logs[@]}"
done
wait_log_count 1 \
  "bingo-lifecycle session-disconnect actor=player-1 destroy=false" \
  "${session_logs[@]}"
wait_log_count 1 \
  "bingo-lifecycle session-disconnect actor=player-2 destroy=false" \
  "${session_logs[@]}"
wait_log_count 0 "bingo-record reported actor=observer" "${play_logs[@]}"
wait_log_count 0 "bingo-lifecycle entry-destroy-complete actor=observer" "${play_logs[@]}"

for evidence in "${business_evidence[@]}"; do
  require_log_count 1 "${evidence}" "${play_logs[@]}"
done
require_log_count 0 "bingo-record reported actor=observer" "${play_logs[@]}"
require_log_count 0 "bingo-lifecycle entry-destroy-complete actor=observer" "${play_logs[@]}"
require_log_count 1 \
  "bingo-lifecycle session-disconnect actor=player-1 destroy=false" \
  "${session_logs[@]}"
require_log_count 1 \
  "bingo-lifecycle session-disconnect actor=player-2 destroy=false" \
  "${session_logs[@]}"

echo "bingo-placement=completed"
