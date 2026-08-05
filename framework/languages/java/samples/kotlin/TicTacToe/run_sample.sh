#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

client_source="Client/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/client/TicTacToeClientScenario.kt"
if rg -n 'receivedCount\([^)]*\)\s*==\s*0' "${client_source}"; then
  echo "TicTacToe negative push assertions must use expectNone" >&2
  exit 1
fi
for assertion in \
  'game.playEndpoints.distinct().size == game.playEndpoints.size' \
  'game.playNodes.map { it.streamEndpoint }.toSet() == game.playEndpoints.toSet()' \
  'guestJoinNotify.displayName == oAuthentication.player.displayName' \
  'milestone.roomId == game.roomId'; do
  if ! grep -Fq "${assertion}" "${client_source}"; then
    echo "TicTacToe client release assertion missing: ${assertion}" >&2
    exit 1
  fi
done

source "../../runner-common.sh"
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)

pids=()
redis_container_id=""
redis_key_prefix="zlink:tictactoe-kotlin:${RANDOM}:$$:room:"
run_dir="$(mktemp -d)"
log_dir="${run_dir}/logs"
mkdir -p "${log_dir}"

cleanup_sample() {
  local status="$?"
  trap - EXIT
  set +e
  cleanup
  local cleanup_status="$?"
  if [[ "${status}" != "0" && -n "${ZLINK_SAMPLE_FAILURE_LOG_ROOT:-}" ]]; then
    local preserved_dir="${ZLINK_SAMPLE_FAILURE_LOG_ROOT}/tictactoe-kotlin-$(date +%Y%m%d-%H%M%S)-$$"
    mkdir -p "${preserved_dir}"
    cp -a "${log_dir}/." "${preserved_dir}/"
    echo "TicTacToe Kotlin failure logs: ${preserved_dir}" >&2
  fi
  rm -rf "${run_dir}"
  if [[ "${status}" != "0" ]]; then
    exit "${status}"
  fi
  exit "${cleanup_status}"
}

print_logs() {
  local status="$1"
  if [[ "${status}" == "0" ]]; then
    return
  fi
  for log in "${log_dir}"/*.log; do
    [[ -f "${log}" ]] || continue
    echo "===== ${log} =====" >&2
    tail -n 240 "${log}" >&2 || true
  done
}

trap cleanup_sample EXIT

wait_endpoint() {
  local name="$1"
  local endpoint="$2"
  wait_port "${endpoint}" || {
    echo "Timed out waiting for ${name} at ${endpoint}" >&2
    return 1
  }
}

wait_log_contains() {
  local log_file="$1"
  local pattern="$2"
  local deadline=$((SECONDS + 60))
  while (( SECONDS < deadline )); do
    if [[ -f "${log_file}" ]] && grep -Eq "${pattern}" "${log_file}"; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for log pattern '${pattern}' in ${log_file}" >&2
  return 1
}

wait_grep() {
  local name="$1"
  local pattern="$2"
  shift 2
  local deadline=$((SECONDS + 20))
  while (( SECONDS < deadline )); do
    if grep -Eq "${pattern}" "$@" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${name}" >&2
  return 1
}

reserve_ports() {
  python3 - <<'PY'
import random
import socket
reserved = []
ports = []
try:
    chosen = set()
    while len(reserved) < 15:
        port = random.randint(48000, 60999)
        if port in chosen:
            continue
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.bind(("127.0.0.1", port))
        except OSError:
            sock.close()
            continue
        chosen.add(port)
        reserved.append(sock)
        ports.append(str(port))
    print(" ".join(ports))
finally:
    for sock in reserved:
        sock.close()
PY
}

read -r api_a_http_port api_b_http_port api_a_channel_port api_b_channel_port play_a_channel_port play_b_channel_port play_a_stream_port play_b_stream_port play_a_spot_port play_b_spot_port play_a_pub_port play_b_pub_port unused_port1 unused_port2 unused_port3 < <(reserve_ports)

zlink_redis_start_scoped_assign redis_container_id redis_port \
  "zlink-redis-kotlin-sample-tictactoe" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
redis_endpoint="127.0.0.1:${redis_port}"

wait_endpoint redis "${redis_endpoint}"

common_play_channels="tcp://127.0.0.1:${play_a_channel_port},tcp://127.0.0.1:${play_b_channel_port}"
common_play_streams="tcp://127.0.0.1:${play_a_stream_port},tcp://127.0.0.1:${play_b_stream_port}"
common_spots="tcp://127.0.0.1:${play_a_spot_port},tcp://127.0.0.1:${play_b_spot_port}"
common_pubs="tcp://127.0.0.1:${play_a_pub_port},tcp://127.0.0.1:${play_b_pub_port}"

api_a_config="${run_dir}/api-a.properties"
api_b_config="${run_dir}/api-b.properties"
play_a_config="${run_dir}/play-a.properties"
play_b_config="${run_dir}/play-b.properties"

cat >"${api_a_config}" <<EOF
sample.apiBindUrl=http://127.0.0.1:${api_a_http_port}
sample.apiPublicUrl=http://127.0.0.1:${api_a_http_port}
sample.apiChannelEndpoint=tcp://127.0.0.1:${api_a_channel_port}
sample.playEndpoint=tcp://127.0.0.1:${play_a_stream_port}
sample.playEndpoints=${common_play_streams}
sample.spotEndpoint=tcp://127.0.0.1:${play_a_spot_port}
sample.routeEndpoint=tcp://127.0.0.1:${unused_port1}
sample.spotEndpoints=${common_spots}
sample.spotPubSubEndpoint=tcp://127.0.0.1:${play_a_pub_port}
sample.spotPubSubEndpoints=${common_pubs}
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${redis_key_prefix}
sample.peerSpotEndpoint=tcp://127.0.0.1:${play_b_spot_port}
sample.peerSpotPubSubEndpoint=tcp://127.0.0.1:${play_b_pub_port}
sample.logDirectory=${log_dir}
EOF

cp "${api_a_config}" "${api_b_config}"
sed -i \
  -e "s#sample.apiBindUrl=.*#sample.apiBindUrl=http://127.0.0.1:${api_b_http_port}#" \
  -e "s#sample.apiPublicUrl=.*#sample.apiPublicUrl=http://127.0.0.1:${api_b_http_port}#" \
  -e "s#sample.apiChannelEndpoint=.*#sample.apiChannelEndpoint=tcp://127.0.0.1:${api_b_channel_port}#" \
  -e "s#sample.routeEndpoint=.*#sample.routeEndpoint=tcp://127.0.0.1:${unused_port2}#" \
  "${api_b_config}"

cp "${api_a_config}" "${play_a_config}"
cp "${api_a_config}" "${play_b_config}"
sed -i \
  -e "s#sample.routeEndpoint=.*#sample.routeEndpoint=tcp://127.0.0.1:${play_a_spot_port}#" \
  "${play_a_config}"
sed -i \
  -e "s#sample.playEndpoint=.*#sample.playEndpoint=tcp://127.0.0.1:${play_b_stream_port}#" \
  -e "s#sample.spotEndpoint=.*#sample.spotEndpoint=tcp://127.0.0.1:${play_b_spot_port}#" \
  -e "s#sample.routeEndpoint=.*#sample.routeEndpoint=tcp://127.0.0.1:${play_b_spot_port}#" \
  -e "s#sample.spotPubSubEndpoint=.*#sample.spotPubSubEndpoint=tcp://127.0.0.1:${play_b_pub_port}#" \
  -e "s#sample.peerSpotEndpoint=.*#sample.peerSpotEndpoint=tcp://127.0.0.1:${play_a_spot_port}#" \
  -e "s#sample.peerSpotPubSubEndpoint=.*#sample.peerSpotPubSubEndpoint=tcp://127.0.0.1:${play_a_pub_port}#" \
  "${play_b_config}"

chmod 0600 "${api_a_config}" "${api_b_config}" "${play_a_config}" "${play_b_config}"

gradle_run :Server:installDist :Client:installDist

Server/build/install/Server/bin/tictactoe-play --config "${play_b_config}" >"${log_dir}/play-b.log" 2>&1 &
pids+=("$!")
wait_log_contains "${log_dir}/play-b.log" "Started PlayProgram"
wait_endpoint play-b-stream "tcp://127.0.0.1:${play_b_stream_port}"
wait_endpoint play-b-spot "tcp://127.0.0.1:${play_b_spot_port}"

Server/build/install/Server/bin/tictactoe-play --config "${play_a_config}" >"${log_dir}/play-a.log" 2>&1 &
pids+=("$!")
wait_log_contains "${log_dir}/play-a.log" "Started PlayProgram"
wait_endpoint play-a-stream "tcp://127.0.0.1:${play_a_stream_port}"
wait_endpoint play-a-spot "tcp://127.0.0.1:${play_a_spot_port}"

"$(app_bin Server Server)" --config "${api_a_config}" >"${log_dir}/api-a.log" 2>&1 &
pids+=("$!")
wait_log_contains "${log_dir}/api-a.log" "Started ApiProgram"
wait_endpoint api-a-http "http://127.0.0.1:${api_a_http_port}"

"$(app_bin Server Server)" --config "${api_b_config}" >"${log_dir}/api-b.log" 2>&1 &
pids+=("$!")
wait_log_contains "${log_dir}/api-b.log" "Started ApiProgram"
wait_endpoint api-b-http "http://127.0.0.1:${api_b_http_port}"

# The Play ports can accept traffic before the peer Route Mesh connection has
# converged. Let the process topology settle before the first actor request.
topology_settle_seconds="${ZLINK_SAMPLE_TOPOLOGY_SETTLE_SECONDS:-10}"
sleep "${topology_settle_seconds}"

"$(app_bin Client Client)" --api-url "http://127.0.0.1:${api_a_http_port}" >"${log_dir}/client.log" 2>&1

grep -Eq "observer-connected endpoint=tcp://127.0.0.1:${play_b_stream_port}" "${log_dir}/client.log"
grep -Eq "observer-subscription=verified subscribed=true" "${log_dir}/client.log"
grep -Eq "observer-win-milestone=verified actor=player-x wins=100" "${log_dir}/client.log"
grep -Eq "tictactoe completed" "${log_dir}/client.log"
for actor_id in player-x player-o; do
  actor_destroyed=0
  for _ in $(seq 1 100); do
    if grep -q "tictactoe actor destroy completed actor=${actor_id}" \
        "${log_dir}"/play-*.log; then
      actor_destroyed=1
      break
    fi
    sleep 0.1
  done
  if [[ "${actor_destroyed}" != "1" ]]; then
    echo "Timed out waiting for ${actor_id} destroy on any Play node" >&2
    exit 1
  fi
done
grep -Rq "message flow" "${log_dir}"
echo "PASS TicTacToe.Kotlin"
