#!/usr/bin/env bash

wait_redis_ready() {
  local container_id="$1"
  local timeout_seconds="${ZLINK_REDIS_READY_TIMEOUT_SECONDS:-60}"
  local deadline=$((SECONDS + timeout_seconds))

  while (( SECONDS < deadline )); do
    if timeout -k 2s 5s docker exec "${container_id}" redis-cli ping 2>/dev/null | grep -q '^PONG$'; then
      return 0
    fi
    sleep 1
  done

  printf 'Timed out waiting for Redis container %s to answer PING\n' "${container_id}" >&2
  return 1
}

start_redis_container() {
  local name="$1"
  shift
  local create_output create_status start_output start_status candidate running
  set +e
  create_output="$(timeout -k 2s 10s docker create --name "${name}" --tmpfs /data "$@" 2>&1)"
  create_status="$?"
  set -e
  candidate="$(printf '%s\n' "${create_output}" | awk '/^[0-9a-f]{12,64}$/ { print; exit }')"
  if [[ "${create_status}" != "0" || -z "${candidate}" ]]; then
    printf 'Failed to create Redis container %s (docker status %s)\n%s\n' "${name}" "${create_status}" "${create_output}" >&2
    return 1
  fi
  REDIS_CONTAINER_ID="${candidate}"
  set +e
  start_output="$(timeout -k 2s 10s docker start "${candidate}" 2>&1)"
  start_status="$?"
  set -e
  running="$(timeout -k 2s 5s docker inspect -f '{{.State.Running}}' "${candidate}" 2>/dev/null || true)"
  if [[ "${running}" == "true" ]]; then
    if ! wait_redis_ready "${candidate}"; then
      timeout -k 2s 10s docker rm -fv "${candidate}" >/dev/null 2>&1 || true
      REDIS_CONTAINER_ID=""
      return 1
    fi
    return 0
  fi
  timeout -k 2s 10s docker rm -fv "${candidate}" >/dev/null 2>&1 || true
  REDIS_CONTAINER_ID=""
  printf 'Failed to start Redis container %s (docker status %s)\n%s\n' "${name}" "${start_status}" "${start_output}" >&2
  return 1
}

redis_container_endpoint() {
  local container_id="$1"
  local timeout_seconds="${ZLINK_REDIS_READY_TIMEOUT_SECONDS:-60}"
  local deadline=$((SECONDS + timeout_seconds))
  local host_port

  while (( SECONDS < deadline )); do
    host_port="$(timeout -k 2s 5s docker inspect \
      -f '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}' \
      "${container_id}" 2>/dev/null || true)"
    if [[ -n "${host_port}" ]] \
      && timeout 1 bash -c ":</dev/tcp/127.0.0.1/${host_port}" >/dev/null 2>&1; then
      printf '127.0.0.1:%s\n' "${host_port}"
      return 0
    fi
    sleep 0.1
  done

  printf 'Timed out waiting for Redis container %s host port to accept connections\n' "${container_id}" >&2
  return 1
}
