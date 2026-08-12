#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAVA_DIR="$(cd "${SCRIPT_DIR}/../../" && pwd)"
source "${JAVA_DIR}/e2e-redis-common.sh"
zlink_e2e_initialize kotlin "$0" "$@"
source "${JAVA_DIR}/e2e-kotlin-config.sh"

run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="${SCRIPT_DIR}/logs/${run_id}-a5"
repo_root="$(cd "${SCRIPT_DIR}/../../../../.." && pwd)"
build_dir="${ZLINK_KOTLIN_OBSERVABILITY_A5_BUILD_DIR:-${HOME}/.cache/zlink/kotlin-e2e/ObservabilityOps-a5}"
gradle_cache="${ZLINK_KOTLIN_OBSERVABILITY_A5_GRADLE_CACHE:-${HOME}/.cache/zlink/kotlin-e2e/ObservabilityOps-a5-gradle-cache}"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
pids=()
REDIS_CONTAINER=""
mkdir -p "${log_dir}"
echo "log_dir=${log_dir}"

if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi

cleanup() {
  local status="$?"
  set +e
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    kill "${pids[$i]}" >/dev/null 2>&1 || true
  done
  sleep 0.5
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    kill -9 "${pids[$i]}" >/dev/null 2>&1 || true
  done
  [[ -z "${REDIS_CONTAINER}" ]] || zlink_redis_remove_by_id "${REDIS_CONTAINER}" || true
  wait >/dev/null 2>&1 || true
  if [[ "${status}" != 0 ]]; then
    for log in "${log_dir}"/*.log; do
      [[ -f "${log}" ]] || continue
      echo "===== ${log} =====" >&2
      tail -n 160 "${log}" >&2 || true
    done
  fi
  exit "${status}"
}
trap cleanup EXIT

reserve_ports() {
  zlink_e2e_reserve_ports 2
}

wait_http() {
  local endpoint="$1"
  for _ in $(seq 1 300); do
    python3 - "${endpoint}/health" <<'PY' >/dev/null 2>&1 && return 0
import sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=1) as response:
    response.read()
PY
    sleep 0.1
  done
  echo "Timed out waiting for ${endpoint}" >&2
  return 1
}

read -r ROUTE_PORT HTTP_PORT <<<"$(reserve_ports)"
ROUTE_ENDPOINT="tcp://127.0.0.1:${ROUTE_PORT}"
HTTP_ENDPOINT="http://127.0.0.1:${HTTP_PORT}"

zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-kotlin-e2e-observability-a5" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"

export ZLINK_KOTLIN_E2E_BUILD_DIR="${build_dir}"
zlink_e2e_gradle_build_locked bash "${SCRIPT_DIR}/gradlew" \
  --project-cache-dir "${gradle_cache}" \
  --no-daemon --no-parallel --max-workers=1 --quiet \
  :A5:Server:installDist :A5:Client:installDist

server_bin="${build_dir}/A5-Server/install/observability-ops-kotlin-a5-server/bin/observability-ops-kotlin-a5-server"
client_bin="${build_dir}/A5-Client/install/observability-ops-kotlin-a5-client/bin/observability-ops-kotlin-a5-client"

ZLINK_KOTLIN_E2E_NODE_RID="a5-server" \
ZLINK_KOTLIN_E2E_ROUTE_ENDPOINT="${ROUTE_ENDPOINT}" \
ZLINK_KOTLIN_E2E_HTTP_ENDPOINT="${HTTP_ENDPOINT}" \
ZLINK_KOTLIN_E2E_REDIS_LOCATION_ENDPOINT="127.0.0.1:${redis_port}" \
ZLINK_KOTLIN_E2E_LOCATION_KEY_PREFIX="zlink:e2e:kotlin-observability-a5:${run_id}" \
ZLINK_KOTLIN_E2E_LOG_DIR="${log_dir}" \
  zlink_kotlin_e2e_run "${server_bin}" >"${log_dir}/server.stdout.log" 2>"${log_dir}/server.stderr.log" &
pids+=("$!")
wait_http "${HTTP_ENDPOINT}"

timeout -k 5s 90s "${client_bin}" --endpoint "${HTTP_ENDPOINT}" \
  >"${log_dir}/client.stdout.log" 2>"${log_dir}/client.stderr.log"
cat "${log_dir}/client.stdout.log"
grep -q "scenario OBS-A5 passed" "${log_dir}/client.stdout.log"
