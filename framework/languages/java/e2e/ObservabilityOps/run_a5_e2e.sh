#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../../e2e-redis-common.sh"

run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="${SCRIPT_DIR}/logs/${run_id}-obs-a5"
config_dir="$(mktemp -d)"
build_dir="${HOME}/.cache/zlink/java-e2e/ObservabilityOps-A5"
redis_container=""
pids=()
mkdir -p "${log_dir}"

cleanup() {
  local status="$?"
  set +e
  for ((index=${#pids[@]}-1; index>=0; index--)); do
    kill "${pids[index]}" >/dev/null 2>&1 || true
  done
  wait >/dev/null 2>&1 || true
  if [[ -n "${redis_container}" ]]; then
    docker rm -fv "${redis_container}" >/dev/null 2>&1 || true
  fi
  rm -rf "${config_dir}"
  exit "${status}"
}
trap cleanup EXIT

repo_root="$(cd "${SCRIPT_DIR}/../../../../.." && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi

read -r route_port http_port <<<"$(python3 - <<'PY'
import socket
sockets = []
try:
    for _ in range(2):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
    print(" ".join(str(sock.getsockname()[1]) for sock in sockets))
finally:
    for sock in sockets:
        sock.close()
PY
)"
route_endpoint="tcp://127.0.0.1:${route_port}"
http_endpoint="http://127.0.0.1:${http_port}"
location_key_prefix="zlink:e2e:observability-a5:${run_id}"

zlink_redis_start_scoped_assign redis_container redis_port \
  "zlink-redis-java-e2e-observability-a5" "redis:7.2-alpine"

"${SCRIPT_DIR}/../../gradlew" -p "${SCRIPT_DIR}/A5" \
  -PzlinkE2eBuildDir="${build_dir}" --no-daemon --no-parallel --max-workers=1 \
  :Server:installDist :Client:installDist

server_config="${config_dir}/server.properties"
printf '%s\n' \
  "e2e.node-rid=obs-a5" \
  "e2e.route-endpoint=${route_endpoint}" \
  "e2e.http-endpoint=${http_endpoint}" \
  "e2e.redis-location-endpoint=127.0.0.1:${redis_port}" \
  "e2e.location-key-prefix=${location_key_prefix}" \
  "e2e.log-dir=${log_dir}" >"${server_config}"

server_bin="${build_dir}/Server/install/observability-ops-a5-server/bin/observability-ops-a5-server"
client_bin="${build_dir}/Client/install/observability-ops-a5-client/bin/observability-ops-a5-client"
"${server_bin}" --config "${server_config}" \
  >"${log_dir}/server.stdout.log" 2>"${log_dir}/server.stderr.log" &
pids+=("$!")

for _ in $(seq 1 30); do
  if python3 - "${http_endpoint}/health" 2>/dev/null <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    raise SystemExit(0 if response.status == 200 else 1)
PY
  then
    break
  fi
  sleep 0.1
done

if ! python3 - "${http_endpoint}/health" 2>/dev/null <<'PY'
import sys
import urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    raise SystemExit(0 if response.status == 200 else 1)
PY
then
  echo "OBS-A5 server did not become ready" >&2
  exit 1
fi

timeout -k 5s 90s "${client_bin}" --endpoint "${http_endpoint}" \
  >"${log_dir}/client.stdout.log" 2>"${log_dir}/client.stderr.log"
cat "${log_dir}/client.stdout.log"
grep -q "scenario OBS-A5 passed" "${log_dir}/client.stdout.log"
echo "observability-ops OBS-A5 result=passed log_dir=${log_dir}"
