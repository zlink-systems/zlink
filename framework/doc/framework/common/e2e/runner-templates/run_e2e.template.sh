#!/usr/bin/env bash
set -euo pipefail

# Individual e2e config runner template.
#
# Configuration policy (sample-e2e-configuration-policy.ko.md) is normative here:
#   - Role servers receive ONE config file path. Nothing else.
#   - The standalone client receives explicit CLI options.
#   - No application setting is passed through environment variables.
#     Exporting a variable for a child process to read is the same violation.
#   - Runner-owned choices (docker image, readiness budget) are runner settings,
#     not an environment-variable interface for the application.
#
# It must not delete same-prefix Redis containers at startup.
# Expected layout:
#   e2e/redis-common.sh
#   e2e/<Config>/run_e2e.sh
#   e2e/<Config>/config/<role>.json      <- checked-in defaults
#
# Runner order (policy section 7):
#   1. prepare run directory and ports
#   2. start Redis, resolve the real endpoint
#   3. generate one config file per role
#   4. pass the config file path to each role executable
#   5. wait for readiness, then run the client scenario with CLI options
#   6. clean up processes, Redis and generated config files

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../redis-common.sh"

# --- runner settings (not application configuration) -------------------------
LANGUAGE="java"
CONFIG_NAME="ExampleConfig"
REDIS_IMAGE="redis:7-alpine"
REDIS_READINESS_TIMEOUT_SECONDS=60

if [[ "$#" -eq 0 ]]; then
  SCENARIO="all"
else
  SCENARIO="$*"
  SCENARIO="${SCENARIO// /,}"
fi

REDIS_SCOPE="zlink-redis-${LANGUAGE}-e2e"
REDIS_KEY_PREFIX="${CONFIG_NAME}:${LANGUAGE}:$$:${RANDOM}:"
RUN_DIR="${SCRIPT_DIR}/run/$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="${RUN_DIR}/logs"
CONF_DIR="${RUN_DIR}/config"
REDIS_CONTAINER_ID=""
REDIS_ENDPOINT=""

cleanup() {
  local status="$?"
  set +e
  # Stop only server/client processes started by this script.
  # kill "${SERVER_PID}" >/dev/null 2>&1 || true
  # Stop only the Redis container started by this script.
  if [[ -n "${REDIS_CONTAINER_ID}" ]]; then
    docker rm -fv "${REDIS_CONTAINER_ID}" >/dev/null 2>&1 || true
  fi
  # Remove generated role config files (they may carry secrets).
  rm -rf "${CONF_DIR}"
  return "${status}"
}
trap cleanup EXIT

# 1. run directory and ports
mkdir -p "${LOG_DIR}"
mkdir -p "${CONF_DIR}"
chmod 700 "${CONF_DIR}"
echo "log_dir=${LOG_DIR}"

# Reserve per-run ports here (language-specific helper).
# PROVIDER_PORT="$(reserve_free_port)"

# 2. Redis
zlink_redis_start_scoped_assign \
  REDIS_CONTAINER_ID \
  redis_host_port \
  "${REDIS_SCOPE}" \
  "${REDIS_IMAGE}" \
  "127.0.0.1::6379"
REDIS_ENDPOINT="127.0.0.1:${redis_host_port}"
zlink_redis_wait_ready "${REDIS_CONTAINER_ID}" "${REDIS_READINESS_TIMEOUT_SECONDS}"

# 3. one config file per role.
#    The runner does NOT author the config schema. It copies the role's
#    checked-in config file and substitutes only the values it owns for this run
#    (ports, the real Redis endpoint, the per-run key prefix, the run directory).
#    Defaults and the meaning of each setting stay in the checked-in file and in
#    the application's Configuration boundary -- never re-implemented in shell.
#
#    The checked-in file carries placeholders for exactly those values, e.g.
#      config/provider.json:
#        { "endpoints": { "provider": "@PROVIDER_ENDPOINT@" },
#          "redis": { "endpoint": "@REDIS_ENDPOINT@", "keyPrefix": "@REDIS_KEY_PREFIX@" },
#          "logging": { "directory": "@LOG_DIR@" } }
write_role_config() {
  local role="$1"
  local source="${SCRIPT_DIR}/config/${role}.json"
  local path="${CONF_DIR}/${role}.json"

  [[ -f "${source}" ]] || { echo "missing role config: ${source}" >&2; return 1; }

  sed -e "s|@PROVIDER_ENDPOINT@|tcp://127.0.0.1:${PROVIDER_PORT}|g" \
      -e "s|@REDIS_ENDPOINT@|${REDIS_ENDPOINT}|g" \
      -e "s|@REDIS_KEY_PREFIX@|${REDIS_KEY_PREFIX}|g" \
      -e "s|@LOG_DIR@|${LOG_DIR}|g" \
      "${source}" >"${path}"
  chmod 600 "${path}"

  # Fail fast if any placeholder survived -- a missing substitution must not
  # reach the application as a literal token.
  if grep -q '@[A-Z_]*@' "${path}"; then
    echo "unsubstituted placeholder in ${path}" >&2
    return 1
  fi
  echo "${path}"
}

# PROVIDER_CONFIG="$(write_role_config provider)"
# CONSUMER_CONFIG="$(write_role_config consumer)"

# 4-5. start role servers with a config file path, then run the client scenario.
#      Each server role has its own entry point -- do not switch roles with
#      --role/--mode on one executable. The client is not a framework host, so
#      it takes explicit CLI options instead of a config file.
#
#      The client's exit status is the verdict. Do not print a pass line unless
#      the client scenario actually succeeded (`set -e` already aborts on a
#      non-zero exit, so reaching the echo means every step above passed).
#
# build_e2e >"${LOG_DIR}/build.log" 2>&1
# start_provider --config "${PROVIDER_CONFIG}" >"${LOG_DIR}/provider.log" 2>&1 &
# SERVER_PID="$!"
# wait_for_readiness
# run_client_scenario \
#   --endpoint "tcp://127.0.0.1:${PROVIDER_PORT}" \
#   --scenario "${SCENARIO}" \
#   >"${LOG_DIR}/client.log" 2>&1
#
# echo "${CONFIG_NAME} e2e result=passed"

echo "template: fill in the commented steps above before using this runner" >&2
exit 1
