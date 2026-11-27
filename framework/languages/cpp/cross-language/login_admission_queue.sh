#!/usr/bin/env bash
# Sourced from run_cross_language_smoke.sh.

wait_login_queue_role() {
  local file="$1" pid="$2" deadline=$((SECONDS + 60))
  while [[ ! -f "${file}" ]]; do
    if ! kill -0 "${pid}" 2>/dev/null || (( SECONDS >= deadline )); then
      echo "No marker ${file} from role pid ${pid}" >&2
      return 1
    fi
    sleep 0.05
  done
}

check_login_queue_dispatches() {
  "${PYTHON_BIN[@]}" - "${ZLINK_LOGIN_QUEUE_DIR}/handlers.events" <<'PYTHON'
from pathlib import Path
import sys

expected = {"held": "object-server", "second": "object-client",
            "third": "object-client", "ticket-owner": "object-client"}
active = set()
finished = set()
peak = 0
for line in Path(sys.argv[1]).read_text().splitlines():
    event, node, login = line.split("|")
    if node != expected.get(login):
        sys.exit(f"Unexpected handler dispatch: {line}")
    if event == "enter" and login not in active | finished:
        active.add(login)
        peak = max(peak, len(active))
        if peak > 1:
            sys.exit(f"Authentication handlers overlapped: {sorted(active)}")
    elif event == "leave" and login in active:
        active.remove(login)
        finished.add(login)
    else:
        sys.exit(f"Unexpected handler lifecycle: {line}")
if active or finished != expected.keys() or peak != 1:
    sys.exit(f"Incomplete handlers: active={active}, finished={finished}, peak={peak}")
print("Handler peak=1; all three connectors and the ticket owner entered; replay did not enter")
PYTHON
}

run_login_queue_cell() {
  local spot="$1" stream="$2" connector="$3" redis="$4"
  local redis_prefix="cross-login-queue-${RUN_DIR##*/}-${spot}-${stream}-${connector}"
  local RUN_DIR="${ZLINK_LOGIN_QUEUE_DIR}"
  local mesh_a mesh_b stream_a stream_b pid_a pid_b connector_pid
  read -r mesh_a mesh_b stream_a stream_b <<<"$(free_port 4)"
  [[ -n "${stream_b}" ]] || return 1
  local endpoint_a="ws://127.0.0.1:${stream_a}" endpoint_b="ws://127.0.0.1:${stream_b}"
  local common=(--mesh-name cross.login-admission-queue --redis-endpoint "${redis}"
    --redis-key-prefix "${redis_prefix}")
  local launch
  for launch in "${spot}:object-server:${mesh_a}:${stream_a}" "${stream}:object-client:${mesh_b}:${stream_b}"; do
    local language role mesh_port stream_port
    IFS=: read -r language role mesh_port stream_port <<<"${launch}"
    ZLINK_LOGIN_QUEUE_ROLE="${role}" "start_${language}" "${role}" login-admission-queue-server "${common[@]}" \
      --bind-endpoint "tcp://127.0.0.1:${mesh_port}" \
      --stream-endpoint "ws://127.0.0.1:${stream_port}" \
      --event-file "${ZLINK_LOGIN_QUEUE_DIR}/${role}.events" || return
  done
  pid_a="${PIDS[-2]}"
  pid_b="${PIDS[-1]}"
  wait_login_queue_role "${RUN_DIR}/object-server.ready" "${pid_a}" || return
  wait_login_queue_role "${RUN_DIR}/object-client.ready" "${pid_b}" || return
  "start_${connector}" queue-connector login-admission-queue-connector \
    --stream-endpoint "${endpoint_a}" --peer-stream-endpoint "${endpoint_b}" || return
  connector_pid="${PIDS[-1]}"
  wait_login_queue_role "${ZLINK_LOGIN_QUEUE_DIR}/second.position" "${connector_pid}" || return
  wait_login_queue_role "${ZLINK_LOGIN_QUEUE_DIR}/third.position" "${connector_pid}" || return
  touch "${ZLINK_LOGIN_QUEUE_DIR}/release" || return
  local result=0
  wait "${connector_pid}" || result=$?
  unset 'PIDS[-1]'
  if (( result != 0 )); then return "${result}"; fi

  start_node queue-ticket-probe login-admission-queue-wire --stream-endpoint "${endpoint_b}" || return
  wait "${PIDS[-1]}" || result=$?
  unset 'PIDS[-1]'
  if (( result != 0 )); then return "${result}"; fi
  wait_for_line "${ZLINK_LOGIN_QUEUE_DIR}/handlers.events" 'leave|object-client|ticket-owner' || return
  check_login_queue_dispatches
}

stage_login_admission_queue() {
  export ZLINK_LOGIN_QUEUE_PACKET=Login
  export ZLINK_LOGIN_QUEUE_KEY=AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8
  export ZLINK_LOGIN_QUEUE_LOCAL_LIMIT=8 ZLINK_LOGIN_QUEUE_GLOBAL_LIMIT=1
  local redis="${ZLINK_REDIS_TEST_ENDPOINT:-}" result=0 spot stream connector server label
  if [[ -z "${redis}" ]]; then
    local redis_port
    redis_port="$(free_port)" || return
    start_redis login-queue-redis "${redis_port}" || return
    redis="127.0.0.1:${redis_port}"
  fi
  while read -r spot stream connector; do
    server="${spot}"
    if [[ "${spot}" != "${stream}" ]]; then server="${spot}+${stream}"; fi
    export ZLINK_LOGIN_QUEUE_DIR="${RUN_DIR}/${server}-${connector}"
    mkdir "${ZLINK_LOGIN_QUEUE_DIR}" || return
    if run_login_queue_cell "${spot}" "${stream}" "${connector}" "${redis}"; then
      label=PASS
    else
      label=FAIL
      result=1
      ZLINK_CPP_CROSS_KEEP_RUN_DIR=1
    fi
    echo "${label} login-admission-queue ${server} ${connector} ${ZLINK_LOGIN_QUEUE_DIR}"
    touch "${ZLINK_LOGIN_QUEUE_DIR}/release"       "${ZLINK_LOGIN_QUEUE_DIR}/object-server.stop" "${ZLINK_LOGIN_QUEUE_DIR}/object-client.stop"
    stop_roles
  done <<'MATRIX'
java java java
java java node
node node java
node node node
java node node
node java java
dotnet dotnet dotnet
dotnet dotnet java
dotnet dotnet node
java java dotnet
node node dotnet
dotnet java java
java dotnet dotnet
dotnet node node
node dotnet dotnet
cpp cpp cpp
cpp cpp dotnet
cpp cpp java
cpp cpp node
dotnet dotnet cpp
java java cpp
node node cpp
cpp dotnet dotnet
cpp java java
cpp node node
dotnet cpp cpp
java cpp cpp
node cpp cpp
MATRIX
  return "${result}"
}
