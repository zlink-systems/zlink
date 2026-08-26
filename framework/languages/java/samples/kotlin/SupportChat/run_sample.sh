#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../../runner-common.sh"
zlink_sample_configure_port_pool kotlin
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)
cd "${SCRIPT_DIR}"

client_source="Client/src/main/kotlin/systems/zlink/samples/kotlin/supportchat/client/SupportChatClientScenario.kt"
if ! rg -q 'expectNone<(TypingChangedNotify|ConversationClosedNotify)>' "${client_source}"; then
  echo "SupportChat negative push checks must use connector expectNone." >&2
  exit 1
fi
if rg -n 'fun ([^ (]+ )?(expectFailure|expectTimeout|expectNoPush|awaitPush)\(' "${client_source}"; then
  echo "SupportChat must not rebuild connector test helpers locally." >&2
  exit 1
fi
if rg -n 'CompletableFuture<JoinConversationRes>|awaitJoin' Server; then
  echo "SupportChat handlers must return after scheduling a deferred actor Spot join." >&2
  exit 1
fi

RUN_DIR="$(mktemp -d)"
RUN_ID="$(basename "${RUN_DIR}")-$$-${RANDOM}"
LOG_DIR="${RUN_DIR}/logs"
ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="support.log api.log session.log"
SAMPLE_LOG_DIR="${RUN_DIR}/sample-logs"
BUILD_LOG="${LOG_DIR}/build.log"
mkdir -p "${LOG_DIR}" "${SAMPLE_LOG_DIR}"

PIDS=()
REDIS_CONTAINER=""
redis_key_prefix="supportchat:kotlin:${RUN_ID}:"

on_exit() {
  local status="$?"
  if [[ "${status}" != "0" ]]; then
    for log in "${BUILD_LOG}" "${LOG_DIR}"/*.log; do
      [[ -f "${log}" ]] || continue
      echo "===== ${log} =====" >&2
      tail -n 200 "${log}" >&2 || true
    done
  fi
  cleanup
  return "${status}"
}

trap on_exit EXIT

read -r -a PORTS <<<"$(zlink_sample_reserve_ports 7)"

api_channel_endpoint="tcp://127.0.0.1:${PORTS[0]}"
api_http_endpoint="http://127.0.0.1:${PORTS[1]}"
api_router_endpoint="tcp://127.0.0.1:${PORTS[2]}"
support_channel_endpoint="tcp://127.0.0.1:${PORTS[3]}"
session_router_endpoint="tcp://127.0.0.1:${PORTS[4]}"
support_router_endpoint="tcp://127.0.0.1:${PORTS[5]}"
stream_endpoint="tcp://127.0.0.1:${PORTS[6]}"

log_count() {
  local evidence="$1"
  shift
  { grep -Fh -- "${evidence}" "$@" 2>/dev/null || true; } | wc -l | tr -d '[:space:]'
}

wait_log_count() {
  local expected="$1"
  local evidence="$2"
  shift 2
  local count
  for _ in $(seq 1 300); do
    count="$(log_count "${evidence}" "$@")"
    if (( count == expected )); then
      return 0
    fi
    if (( count > expected )); then
      echo "Expected ${expected} '${evidence}', found ${count}." >&2
      return 1
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${expected} '${evidence}'." >&2
  return 1
}

wait_log_at_least() {
  local minimum="$1"
  local evidence="$2"
  shift 2
  local count
  for _ in $(seq 1 300); do
    count="$(log_count "${evidence}" "$@")"
    if (( count >= minimum )); then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for at least ${minimum} '${evidence}'." >&2
  return 1
}

start_role() {
  local name="$1"
  local binary="$2"
  local config="$3"
  "${binary}" --config "${config}" >"${LOG_DIR}/${name}.log" 2>&1 &
  PIDS+=("$!")
}

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run the SupportChat sample." >&2
  exit 1
fi

zlink_redis_start_scoped_assign REDIS_CONTAINER REDIS_PORT \
  "zlink-redis-kotlin-sample-supportchat" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
redis_endpoint="127.0.0.1:${REDIS_PORT}"
wait_port redis "tcp://${redis_endpoint}"

api_config="${RUN_DIR}/api.properties"
session_config="${RUN_DIR}/session.properties"
support_config="${RUN_DIR}/support.properties"
cat >"${api_config}" <<EOF
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${redis_key_prefix}
sample.logDirectory=${SAMPLE_LOG_DIR}
sample.apiChannelEndpoint=${api_channel_endpoint}
sample.apiSpotRouterEndpoint=${api_router_endpoint}
sample.apiHttpEndpoint=${api_http_endpoint}
EOF
cat >"${session_config}" <<EOF
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${redis_key_prefix}
sample.logDirectory=${SAMPLE_LOG_DIR}
sample.sessionRouterEndpoint=${session_router_endpoint}
sample.streamEndpoint=${stream_endpoint}
EOF
cat >"${support_config}" <<EOF
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${redis_key_prefix}
sample.logDirectory=${SAMPLE_LOG_DIR}
sample.supportChannelEndpoint=${support_channel_endpoint}
sample.supportSpotRouterEndpoint=${support_router_endpoint}
EOF
chmod 0600 "${api_config}" "${session_config}" "${support_config}"

cd "${SCRIPT_DIR}"
zlink_sample_gradle_locked ../../gradlew --settings-file standalone.settings.gradle.kts --no-daemon --no-parallel --max-workers=1 \
  :Server:Api:installDist \
  :Server:Session:installDist \
  :Server:Support:installDist \
  :Client:installDist >"${BUILD_LOG}" 2>&1

start_role support "${SCRIPT_DIR}/Server/Support/build/install/Support/bin/Support" "${support_config}"
start_role api "${SCRIPT_DIR}/Server/Api/build/install/Api/bin/Api" "${api_config}"
start_role session "${SCRIPT_DIR}/Server/Session/build/install/Session/bin/Session" "${session_config}"

wait_log_count 1 "supportchat-ready kind=public node=api" "${LOG_DIR}/api.log"
wait_log_count 1 "supportchat-ready kind=public node=support" "${LOG_DIR}/support.log"
wait_log_count 1 "supportchat-ready kind=stream node=session" "${LOG_DIR}/session.log"
wait_log_count 1 "supportchat-ready kind=spot-route node=api mesh=supportchat.support.spots" "${LOG_DIR}/api.log"
wait_log_count 1 "supportchat-ready kind=spot-route node=session mesh=supportchat.support.spots" "${LOG_DIR}/session.log"

"${SCRIPT_DIR}/Client/build/install/Client/bin/Client" \
  --stream-endpoint "${stream_endpoint}" >"${LOG_DIR}/client.log" 2>&1

wait_log_count 1 "supportchat=completed" "${LOG_DIR}/client.log"
wait_log_count 1 "supportchat-closed-typing-ignore=verified" "${LOG_DIR}/client.log"

server_logs=("${LOG_DIR}/api.log" "${LOG_DIR}/support.log")
wait_log_at_least 1 "supportchat-conversation created conversation=" "${server_logs[@]}"
wait_log_at_least 1 "supportchat-conversation agent-joined conversation=" "${server_logs[@]}"
wait_log_at_least 1 "supportchat-conversation status=WaitingForAgent conversation=" "${server_logs[@]}"
wait_log_at_least 1 "supportchat-conversation status=Active conversation=" "${server_logs[@]}"
wait_log_at_least 1 "supportchat-conversation status=WaitingForClose conversation=" "${server_logs[@]}"
wait_log_at_least 1 "supportchat-conversation status=Closed conversation=" "${server_logs[@]}"

cleanup
trap - EXIT
rm -rf "${RUN_DIR}"
echo "supportchat-placement=completed"
