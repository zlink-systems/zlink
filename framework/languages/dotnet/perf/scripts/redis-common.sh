#!/usr/bin/env bash
# The run owns one exact container ID. Phase 1's manual baselines do not start Redis.
PERF_REDIS_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${PERF_REDIS_SCRIPT_DIR}/../../../../doc/framework/common/sample/runner-templates/redis-common.template.sh"

perf_start_run_redis() {
  local run_id="$1" output_file="$2"
  local perf_container_id perf_redis_port
  local pinned_image='redis:7-alpine'
  zlink_redis_start_scoped_assign perf_container_id perf_redis_port \
    "zlink-redis-dotnet-perf-${run_id}" "${pinned_image}" 22000 22099
  if ! zlink_redis_wait_ready "${perf_container_id}" 60; then
    zlink_redis_remove_by_id "${perf_container_id}"
    return 1
  fi
  if ! timeout -k 2s 10s docker inspect --format '{{json .}}' "${perf_container_id}" > "${output_file}"; then
    zlink_redis_remove_by_id "${perf_container_id}"
    return 1
  fi
  printf '%s %s\n' "${perf_container_id}" "${perf_redis_port}"
}
