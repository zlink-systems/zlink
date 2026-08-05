#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../../runner-common.sh"
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

read -r -a PORTS <<<"$(zlink_sample_reserve_ports 6)"

api_channel_endpoint="tcp://127.0.0.1:${PORTS[0]}"
api_http_endpoint="http://127.0.0.1:${PORTS[1]}"
support_channel_endpoint="tcp://127.0.0.1:${PORTS[2]}"
session_router_endpoint="tcp://127.0.0.1:${PORTS[3]}"
support_router_endpoint="tcp://127.0.0.1:${PORTS[4]}"
stream_endpoint="tcp://127.0.0.1:${PORTS[5]}"

wait_log() {
  local pattern="$1"
  local file="$2"
  for _ in $(seq 1 60); do
    if grep -Eq "${pattern}" "${file}"; then
      return 0
    fi
    sleep 0.2
  done
  echo "Timed out waiting for '${pattern}' in ${file}" >&2
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
../../gradlew --settings-file standalone.settings.gradle.kts --no-daemon --no-parallel --max-workers=1 \
  :Server:Api:installDist \
  :Server:Session:installDist \
  :Server:Support:installDist \
  :Client:installDist >"${BUILD_LOG}" 2>&1

start_role support "${SCRIPT_DIR}/Server/Support/build/install/Support/bin/Support" "${support_config}"
wait_port support-channel "${support_channel_endpoint}"
wait_port support-router "${support_router_endpoint}"

start_role api "${SCRIPT_DIR}/Server/Api/build/install/Api/bin/Api" "${api_config}"
wait_port api-channel "${api_channel_endpoint}"

start_role session "${SCRIPT_DIR}/Server/Session/build/install/Session/bin/Session" "${session_config}"
wait_port session-router "${session_router_endpoint}"
wait_port session-stream "${stream_endpoint}"

"${SCRIPT_DIR}/Client/build/install/Client/bin/Client" \
  --stream-endpoint "${stream_endpoint}" >"${LOG_DIR}/client.log" 2>&1

grep -q "supportchat=completed" "${LOG_DIR}/client.log"
grep -q "supportchat-closed-typing-ignore=verified" "${LOG_DIR}/client.log"
wait_log "support conversation: created" "${LOG_DIR}/support.log"
wait_log "support conversation: actor joined" "${LOG_DIR}/support.log"
wait_log "status=WaitingForAgent" "${LOG_DIR}/api.log"
wait_log "status=Active" "${LOG_DIR}/support.log"
wait_log "status=WaitingForClose" "${LOG_DIR}/support.log"
wait_log "status=Closed" "${LOG_DIR}/support.log"
grep -Rq "message flow" "${SAMPLE_LOG_DIR}"
echo "supportchat-server-evidence=completed"
