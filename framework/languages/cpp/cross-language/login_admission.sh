#!/usr/bin/env bash
# Sourced from run_cross_language_smoke.sh.

# Login admission runs a raw wire probe per server, then the real connectors.
start_login_host() {
  local language="$1" role="$2" endpoint="$3"
  local name="login-${role}" mode="login-admission-${role}"
  local args=("${mode}" --stream-endpoint "${endpoint}" --event-file "${ZLINK_LOGIN_DIR}/${role}.events")
  case "${language}" in
    cpp) start_cpp "${name}" "${args[@]}" --ready-file "${RUN_DIR}/${name}.ready" ;;
    dotnet)
      local dll="${ZLINK_DOTNET_TEST_HOST_DLL:-${DOTNET_TEST_HOST%/*}/bin/Debug/net8.0/Zlink.Framework.TestHost.dll}"
      if [[ ! -f "${dll}" ]]; then
        echo "Build ${DOTNET_TEST_HOST} or set ZLINK_DOTNET_TEST_HOST_DLL" >&2
        return 1
      fi
      dotnet "${dll}" "${args[@]}" \
        --ready-file "${RUN_DIR}/${name}.ready" --stop-file "${RUN_DIR}/${name}.stop" \
        >"${RUN_DIR}/${name}.log" 2>&1 &
      PIDS+=("$!")
      ;;
    java) start_java "${name}" "${args[@]}" ;;
    node) start_node "${name}" "${args[@]}" ;;
    *) echo "Unknown login admission role language ${language}" >&2; return 1 ;;
  esac
}

run_login_admission_cell() {
  local server="$1" client="$2" port endpoint client_role=connector client_pid
  port="$(free_port)" || return
  endpoint="ws://127.0.0.1:${port}"
  rm -f "${RUN_DIR}/login-server.ready"
  start_login_host "${server}" server "${endpoint}" || return
  if ! "${PYTHON_BIN[@]}" - "${RUN_DIR}/login-server.ready" "${PIDS[-1]}" "${port}" <<'PYTHON'
import os
from pathlib import Path
import socket
import sys
import time

ready, pid, port = Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
deadline = time.monotonic() + 60
while time.monotonic() < deadline:
    os.kill(pid, 0)
    if ready.is_file():
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                sys.exit()
        except OSError:
            pass
    time.sleep(0.02)
sys.exit(f"Timed out waiting for server readiness on port {port}")
PYTHON
  then
    return 1
  fi
  if [[ "${client}" == wire ]]; then
    client_role=wire
    start_login_host node wire "${endpoint}" || return
  else
    start_login_host "${client}" connector "${endpoint}" || return
    client_pid="${PIDS[-1]}"
    local deadline=$((SECONDS + 20))
    while [[ ! -f "${ZLINK_LOGIN_DIR}/waiting" ]]; do
      if ! kill -0 "${client_pid}" 2>/dev/null || (( SECONDS >= deadline )); then
        echo "No admission callback from ${client}" >&2
        return 1
      fi
      sleep 0.01
    done
    touch "${ZLINK_LOGIN_DIR}/release"
  fi
  local client_result=0
  wait "${PIDS[-1]}" || client_result=$?
  unset 'PIDS[-1]'
  if (( client_result != 0 )); then
    return "${client_result}"
  fi
  "${PYTHON_BIN[@]}" - "${client_role}" <<'PYTHON'
import os
from pathlib import Path
import sys

run = Path(os.environ["ZLINK_LOGIN_DIR"])
control, held, contender = (os.environ[f"ZLINK_LOGIN_{name}"]
                            for name in ("CONTROL", "HELD", "CONTENDER"))
dispatches = [line.split("|", 1) for line in (run / "server.events").read_text().splitlines()]
names = [held, control] if sys.argv[1] == "wire" else [control, held, contender]
if [name for name, _ in dispatches] != names:
    sys.exit(f"Expected handler dispatches {names}, got {dispatches}")
if any(not session for _, session in dispatches) or dispatches[0][1] == dispatches[1][1]:
    sys.exit(f"Expected two different sessions, got {dispatches}")
if sys.argv[1] == "connector":
    if dispatches[0][1] != dispatches[2][1]:
        sys.exit(f"The contender changed sessions: {dispatches}")
    packet = os.environ["ZLINK_LOGIN_PACKET"]
    delay = int(os.environ["ZLINK_LOGIN_RETRY_MS"])
    callbacks = (run / "waiting").read_text().splitlines()
    if callbacks != [f"{packet}|{delay}"]:
        sys.exit(f"Unexpected admission callbacks: {callbacks}")
    elapsed = ((run / "server.events").stat().st_mtime_ns - (run / "waiting").stat().st_mtime_ns) / 1e6
    # Filesystem timestamps can round each marker. The connector suites cover
    # the exact retry deadline against the connector's injected clock.
    if elapsed < delay - 5:
        sys.exit(f"Contender dispatched early after {elapsed:.1f} ms, expected {delay} ms")
    print(f"retry dispatched after {elapsed:.1f} ms on session {dispatches[2][1]}")
PYTHON
}

stage_login_admission() {
  export ZLINK_LOGIN_FIXTURE="${ZLINK_LOGIN_FIXTURE:-${REPO_ROOT}/framework/runtime/conformance/login-admission-v1.json}"
  local values
  values="$("${PYTHON_BIN[@]}" - "${ZLINK_LOGIN_FIXTURE}" <<'PYTHON'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    fixture = json.load(source)
cross = fixture["crossLanguage"]
scenario = next(item for item in fixture["serverScenarios"] if item["name"] == cross["serverScenario"])
given = scenario["given"]
print(given["authenticationPacketName"], given["maxConcurrentAuthentications"],
      given["authenticationRetryDelayMs"], cross["control"], cross["held"], cross["contender"],
      cross["timeoutMs"], sep="\n")
PYTHON
  )" || return
  local -a fixture
  mapfile -t fixture <<<"${values}"
  export ZLINK_LOGIN_PACKET="${fixture[0]}" ZLINK_LOGIN_LIMIT="${fixture[1]}" \
    ZLINK_LOGIN_RETRY_MS="${fixture[2]}" ZLINK_LOGIN_CONTROL="${fixture[3]}" \
    ZLINK_LOGIN_HELD="${fixture[4]}" ZLINK_LOGIN_CONTENDER="${fixture[5]}" \
    ZLINK_LOGIN_TIMEOUT_MS="${fixture[6]}"
  local server client role result=0
  local -a servers connectors
  read -ra servers <<<"${ZLINK_LOGIN_SERVERS:-cpp dotnet java node}"
  read -ra connectors <<<"${ZLINK_LOGIN_CONNECTORS:-cpp dotnet java node}"
  for server in "${servers[@]}"; do
    for client in wire "${connectors[@]}"; do
      export ZLINK_LOGIN_DIR="${RUN_DIR}/${server}-${client}"
      mkdir "${ZLINK_LOGIN_DIR}"
      if run_login_admission_cell "${server}" "${client}"; then
        echo "PASS login-admission ${server} ${client} ${ZLINK_LOGIN_DIR}"
      else
        echo "FAIL login-admission ${server} ${client} ${ZLINK_LOGIN_DIR}"
        result=1
      fi
      touch "${ZLINK_LOGIN_DIR}/release"
      stop_all
      for role in server connector wire; do
        if [[ -f "${RUN_DIR}/login-${role}.log" ]]; then
          mv "${RUN_DIR}/login-${role}.log" "${ZLINK_LOGIN_DIR}/${role}.log"
        fi
      done
    done
  done
  return "${result}"
}
