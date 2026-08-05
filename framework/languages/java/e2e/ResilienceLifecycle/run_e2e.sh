#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/e2e-redis-common.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/start-order-common.sh"

cd "$(dirname "${BASH_SOURCE[0]}")"

pids=()
redis_container_name=""
redis_proxy_pid=""
store_pause_command=""
store_resume_command=""
run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="$(pwd)/logs/${run_id}"
config_dir="$(mktemp -d)"
chmod 0700 "${config_dir}"
repo_root="$(cd ../../../../.. && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"
SCENARIO="${1:-all}"
e2e_start_order="$(zlink_e2e_start_order_mode "$@")"
echo "start_order=${e2e_start_order}"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi
readonly e2e_build_dir="${HOME}/.cache/zlink/java-e2e/ResilienceLifecycle"
readonly gradle_cache_dir="${HOME}/.cache/zlink/java-e2e/ResilienceLifecycle-gradle-cache"
location_key_prefix="zlink:e2e:resilience-lifecycle:${run_id}"
redis_location_endpoint=""
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_ATTEMPTS=30
if rg -n 'java\.net\.http\.HttpClient|HttpClient\.new' \
    "$(pwd)/Client/src/main/java" --glob '*.java'; then
  echo "ResilienceLifecycle client must use ZLinkHttpClient" >&2
  exit 1
fi
if rg -n 'runMode\(|/scenario/|class ConsumerScenario' \
    "$(pwd)/Client/src/main/java" "$(pwd)/Server/Consumer/src/main/java" --glob '*.java'; then
  echo "ResilienceLifecycle scenarios must run in Client files" >&2
  exit 1
fi

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

descendants() {
  local pid="$1"
  local child
  (pgrep -P "${pid}" 2>/dev/null || true) | while read -r child; do
    descendants "${child}"
    echo "${child}"
  done
}

cleanup() {
  local status="$?"
  set +e
  print_logs "${status}"
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    local pid="${pids[$i]}"
    for child in $(descendants "${pid}"); do
      kill "${child}" >/dev/null 2>&1 || true
    done
    kill "${pid}" >/dev/null 2>&1 || true
  done
  if [[ -n "${redis_container_name}" ]]; then
    docker rm -fv "${redis_container_name}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${redis_proxy_pid}" ]]; then
    kill -CONT "${redis_proxy_pid}" >/dev/null 2>&1 || true
    kill "${redis_proxy_pid}" >/dev/null 2>&1 || true
    sleep 0.2
    kill -9 "${redis_proxy_pid}" >/dev/null 2>&1 || true
  fi
  wait >/dev/null 2>&1 || true
  rm -rf "${config_dir}"
  exit "${status}"
}
trap cleanup EXIT

reserve_ports() {
  python3 - <<'PY'
import socket
sockets = []
ports = []
try:
    for _ in range(8):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
    print(" ".join(f"tcp://127.0.0.1:{port}" for port in ports[:4]), end=" ")
    print(" ".join(f"http://127.0.0.1:{port}" for port in ports[4:]))
finally:
    for sock in sockets:
        sock.close()
PY
}

reserve_tcp_port() {
  python3 - <<'PY'
import socket
sock = socket.socket()
try:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
finally:
    sock.close()
PY
}

port_of() {
  echo "${1##*:}"
}

wait_port() {
  local name="$1"
  local endpoint="$2"
  local port
  port="$(port_of "${endpoint}")"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if (echo >"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}

gradle_run() {
  ../../gradlew -PzlinkE2eBuildDir="${e2e_build_dir}" \
    --project-cache-dir "${gradle_cache_dir}" --no-daemon "$@" --quiet
}

client_bin() {
  echo "${e2e_build_dir}/Client/install/resilience-lifecycle-client/bin/resilience-lifecycle-client"
}

start_redis_proxy() {
  if [[ -z "${explicit_redis_endpoint}" || !( "${SCENARIO}" == "all" || "${SCENARIO}" == "RL-C4" || "${SCENARIO}" == "rl-c4" ) ]]; then
    return
  fi
  local proxy_port
  proxy_port="$(reserve_tcp_port)"
  python3 - "${explicit_redis_endpoint}" "${proxy_port}" >"${log_dir}/redis-proxy.stdout.log" 2>"${log_dir}/redis-proxy.stderr.log" <<'PY' &
import selectors
import socket
import sys
import threading

target = sys.argv[1]
listen_port = int(sys.argv[2])
if target.startswith("tcp://"):
    target = target[len("tcp://"):]
host, port_text = target.rsplit(":", 1)
target_addr = (host, int(port_text))

def pump(source, destination):
    try:
        while True:
            data = source.recv(65536)
            if not data:
                return
            destination.sendall(data)
    except OSError:
        return
    finally:
        for sock in (source, destination):
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", listen_port))
    server.listen()
    print(f"redis proxy listening on 127.0.0.1:{listen_port} -> {target_addr[0]}:{target_addr[1]}", flush=True)
    while True:
        client, _ = server.accept()
        try:
            upstream = socket.create_connection(target_addr, timeout=5)
        except OSError:
            client.close()
            continue
        threading.Thread(target=pump, args=(client, upstream), daemon=True).start()
        threading.Thread(target=pump, args=(upstream, client), daemon=True).start()
PY
  redis_proxy_pid="$!"
  redis_location_endpoint="127.0.0.1:${proxy_port}"
  wait_port redis-proxy "127.0.0.1:${proxy_port}"
}

create_store_outage_commands() {
  store_pause_command="${log_dir}/store-pause"
  store_resume_command="${log_dir}/store-resume"
  if [[ -n "${redis_container_name}" ]]; then
    cat >"${store_pause_command}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
docker pause "${redis_container_name}" >/dev/null
EOF
    cat >"${store_resume_command}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
docker unpause "${redis_container_name}" >/dev/null
EOF
  elif [[ -n "${redis_proxy_pid}" ]]; then
    cat >"${store_pause_command}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
kill -STOP "${redis_proxy_pid}"
EOF
    cat >"${store_resume_command}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
kill -CONT "${redis_proxy_pid}"
EOF
  else
    cat >"${store_pause_command}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
echo "RL-C4 requires a Redis container or Redis proxy started by this runner." >&2
exit 1
EOF
    cat >"${store_resume_command}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
echo "RL-C4 requires a Redis container or Redis proxy started by this runner." >&2
exit 1
EOF
  fi
  chmod +x "${store_pause_command}" "${store_resume_command}"
}

explicit_redis_endpoint=""
zlink_redis_start_scoped_assign redis_container_name redis_port \
  "zlink-redis-java-e2e" "redis:7.2-alpine" "127.0.0.1::6379"
redis_location_endpoint="127.0.0.1:${redis_port}"
if [[ "${ZLINK_E2E_REDIS_MONITOR:-0}" == "1" ]]; then
  docker exec "${redis_container_name}" redis-cli --csv monitor \
    >"${log_dir}/redis-monitor.log" 2>&1 &
  pids+=("$!")
fi
start_redis_proxy
create_store_outage_commands

read -r API_A API_B API_A_REPLACEMENT API_B_GREEN HTTP_A HTTP_B HTTP_A_REPLACEMENT HTTP_B_GREEN <<<"$(reserve_ports)"

gradle_run installDist
mapfile -t ORDERED_SERVER_ROLES < <(zlink_e2e_order_roles api-a api-b)
START_ORDER_CSV="$(IFS=,; echo "${ORDERED_SERVER_ROLES[*]}")"

client_config="${config_dir}/client.properties"
cat >"${client_config}" <<EOF
redisLocationEndpoint=${redis_location_endpoint}
locationKeyPrefix=${location_key_prefix}
apiAEndpoint=${API_A}
apiBEndpoint=${API_B}
apiAReplacementEndpoint=${API_A_REPLACEMENT}
apiBGreenEndpoint=${API_B_GREEN}
httpAEndpoint=${HTTP_A}
httpBEndpoint=${HTTP_B}
httpAReplacementEndpoint=${HTTP_A_REPLACEMENT}
httpBGreenEndpoint=${HTTP_B_GREEN}
storePauseCommand=${store_pause_command}
storeResumeCommand=${store_resume_command}
buildDir=${e2e_build_dir}
logDir=${log_dir}
controlDir=${log_dir}/control
configDir=${config_dir}
EOF
chmod 0600 "${client_config}"

"$(client_bin)" --config "${client_config}" --scenario "${SCENARIO}" \
  --start-order "${START_ORDER_CSV}" >"${log_dir}/client.stdout.log" 2>"${log_dir}/client.stderr.log"

cat "${log_dir}/client.stdout.log"
if [[ "${SCENARIO}" == "all" ]]; then
  for scenario in \
      RL-A1 RL-A2 RL-A3 RL-A4 RL-A5 \
      RL-B1 RL-B2 RL-B3 RL-B4 RL-B5 RL-B6 \
      RL-C1 RL-C2 RL-C3 RL-C4 \
      RL-D1 RL-D2 RL-D3 RL-D4 RL-D5; do
    grep -q "scenario ${scenario} passed" "${log_dir}/client.stdout.log"
  done
else
  grep -q "scenario ${SCENARIO} passed" "${log_dir}/client.stdout.log"
fi
grep -q "resilience-lifecycle e2e result=passed" "${log_dir}/client.stdout.log"
grep -Rq "message flow" "${log_dir}"/*-flow.log
