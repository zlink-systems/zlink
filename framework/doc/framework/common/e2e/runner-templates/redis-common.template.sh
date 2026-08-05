#!/usr/bin/env bash

# Copy this helper into each language e2e runner area and keep the behavior
# equivalent across languages.

zlink_redis_start_scoped() {
  local scope="$1"
  local image="${2:-redis:7-alpine}"
  local port_mapping="${3:-127.0.0.1::6379}"
  local docker_timeout_seconds="${ZLINK_REDIS_DOCKER_TIMEOUT_SECONDS:-10}"
  local run_id="${ZLINK_REDIS_RUN_ID:-$$}"
  local name="${scope}-${run_id}-${BASHPID}-${RANDOM}"

  local create_output create_status container_id start_output start_status running host_port

  printf 'redis_start name=%s image=%s started_at=%s\n' "${name}" "${image}" "$(date -Is)" >&2
  set +e
  create_output="$(timeout -k 2s "${docker_timeout_seconds}s" docker create \
    --name "${name}" \
    --tmpfs /data \
    -p "${port_mapping}" \
    "${image}" 2>&1)"
  create_status="$?"
  set -e

  container_id="$(printf '%s\n' "${create_output}" | awk '/^[0-9a-f]{12,64}$/ { print; exit }')"
  if [[ "${create_status}" != "0" || -z "${container_id}" ]]; then
    printf 'Failed to create Redis container %s (docker status %s)\n%s\n' \
      "${name}" "${create_status}" "${create_output}" >&2
    return 1
  fi

  set +e
  start_output="$(timeout -k 2s "${docker_timeout_seconds}s" docker start "${container_id}" 2>&1)"
  start_status="$?"
  set -e

  running="$(timeout -k 2s 5s docker inspect -f '{{.State.Running}}' "${container_id}" 2>/dev/null || true)"
  if [[ "${running}" != "true" ]]; then
    timeout -k 2s 10s docker rm -fv "${container_id}" >/dev/null 2>&1 || true
    printf 'Failed to start Redis container %s (docker status %s)\n%s\n' \
      "${name}" "${start_status}" "${start_output}" >&2
    return 1
  fi

  host_port="$(timeout -k 2s 5s docker inspect \
    -f '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}' \
    "${container_id}" 2>/dev/null || true)"
  if [[ -z "${host_port}" ]]; then
    timeout -k 2s 10s docker rm -fv "${container_id}" >/dev/null 2>&1 || true
    printf 'Failed to inspect Redis host port for %s\n' "${name}" >&2
    return 1
  fi

  printf 'redis_started name=%s endpoint=127.0.0.1:%s\n' "${name}" "${host_port}" >&2
  printf '%s %s\n' "${container_id}" "${host_port}"
}

zlink_redis_start_scoped_assign() {
  local container_var="$1"
  local host_port_var="$2"
  shift 2

  local output container_id host_port
  output="$(zlink_redis_start_scoped "$@")" || return $?
  read -r container_id host_port <<<"${output}"
  if [[ -z "${container_id}" || -z "${host_port}" ]]; then
    printf 'Redis helper did not return container id and host port.\n' >&2
    return 1
  fi

  printf -v "${container_var}" '%s' "${container_id}"
  printf -v "${host_port_var}" '%s' "${host_port}"
}

zlink_redis_wait_ready() {
  local container_id="$1"
  local timeout_seconds="${2:-${ZLINK_REDIS_READY_TIMEOUT_SECONDS:-60}}"
  local poll_seconds="${3:-1}"
  local deadline=$((SECONDS + timeout_seconds))

  while (( SECONDS < deadline )); do
    if timeout -k 2s 5s docker exec "${container_id}" redis-cli ping 2>/dev/null | grep -qx PONG; then
      printf 'redis_ready container=%s ready_at=%s\n' "${container_id}" "$(date -Is)" >&2
      return 0
    fi
    sleep "${poll_seconds}"
  done

  echo "Timed out waiting ${timeout_seconds}s for Redis container readiness: ${container_id}" >&2
  return 1
}
