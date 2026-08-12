#!/usr/bin/env bash

# Copy this helper into each language sample runner area and keep the behavior
# equivalent across languages.

zlink_redis_port_is_available() {
  python3 - "$1" <<'PY'
import socket
import sys

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    try:
        sock.bind(("127.0.0.1", int(sys.argv[1])))
    except OSError:
        raise SystemExit(1)
PY
}

zlink_redis_is_bind_conflict() {
  local details="${1,,}"
  [[ "${details}" == *"address already in use"* ||
     "${details}" == *"port is already allocated"* ||
     "${details}" == *"failed to bind host port"* ||
     "${details}" == *"bind for"*"failed"* ]]
}

zlink_redis_remove_by_id() {
  local container_id="$1"
  local docker_timeout_seconds="${ZLINK_REDIS_DOCKER_TIMEOUT_SECONDS:-10}"

  [[ "${container_id}" =~ ^[0-9a-f]{12,64}$ ]] || return 1
  timeout -k 2s "${docker_timeout_seconds}s" docker rm -fv \
    "${container_id}" >/dev/null 2>&1
}

zlink_redis_remove_attempt() {
  local container_id="$1"
  local name="$2"

  if [[ ! "${container_id}" =~ ^[0-9a-f]{12,64}$ ]]; then
    container_id="$(timeout -k 2s 5s docker inspect --type container \
      -f '{{.Id}}' "${name}" 2>/dev/null || true)"
  fi
  if [[ "${container_id}" =~ ^[0-9a-f]{12,64}$ ]]; then
    zlink_redis_remove_by_id "${container_id}" || true
  fi
}

zlink_redis_start_scoped() {
  local scope="$1"
  local image="${2:-redis:7-alpine}"
  local redis_min_port="$3"
  local redis_max_port="$4"
  local docker_timeout_seconds="${ZLINK_REDIS_DOCKER_TIMEOUT_SECONDS:-10}"
  local run_id="${ZLINK_REDIS_RUN_ID:-$$}"
  local pool_size=$((redis_max_port - redis_min_port + 1))
  local start_port=$((redis_min_port + RANDOM % pool_size))

  local offset port name create_output create_status container_id
  local start_output start_status running host_port failure_details

  for ((offset = 0; offset < pool_size; offset++)); do
    port=$((redis_min_port + (start_port - redis_min_port + offset) % pool_size))
    if ! zlink_redis_port_is_available "${port}"; then
      continue
    fi

    name="${scope}-${run_id}-${BASHPID}-${RANDOM}-${port}"
    if create_output="$(timeout -k 2s "${docker_timeout_seconds}s" docker create \
      --name "${name}" \
      --tmpfs /data \
      -p "127.0.0.1:${port}:6379" \
      "${image}" 2>&1)"; then
      create_status=0
    else
      create_status=$?
    fi
    container_id="$(printf '%s\n' "${create_output}" | awk '/^[0-9a-f]{12,64}$/ { print; exit }')"
    if [[ "${create_status}" != "0" || -z "${container_id}" ]]; then
      zlink_redis_remove_attempt "${container_id}" "${name}"
      if zlink_redis_is_bind_conflict "${create_output}"; then
        continue
      fi
      printf 'Failed to create Redis container %s (docker status %s)\n%s\n' \
        "${name}" "${create_status}" "${create_output}" >&2
      return 1
    fi

    if start_output="$(timeout -k 2s "${docker_timeout_seconds}s" docker start \
      "${container_id}" 2>&1)"; then
      start_status=0
    else
      start_status=$?
    fi
    if [[ "${start_status}" != "0" ]]; then
      failure_details="${start_output}"
      zlink_redis_remove_attempt "${container_id}" "${name}"
      if zlink_redis_is_bind_conflict "${failure_details}"; then
        continue
      fi
      printf 'Failed to start Redis container %s (docker status %s)\n%s\n' \
        "${name}" "${start_status}" "${start_output}" >&2
      return 1
    fi

    running="$(timeout -k 2s 5s docker inspect -f '{{.State.Running}}' \
      "${container_id}" 2>/dev/null || true)"
    host_port="$(timeout -k 2s 5s docker inspect \
      -f '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}' \
      "${container_id}" 2>/dev/null || true)"
    if [[ "${running}" != "true" || "${host_port}" != "${port}" ]]; then
      zlink_redis_remove_attempt "${container_id}" "${name}"
      printf 'Redis container %s did not bind the selected host port %s.\n' \
        "${name}" "${port}" >&2
      return 1
    fi

    printf 'redis_started name=%s endpoint=127.0.0.1:%s\n' "${name}" "${host_port}" >&2
    printf '%s %s\n' "${container_id}" "${host_port}"
    return 0
  done

  printf 'No Redis port is available within %s-%s.\n' \
    "${redis_min_port}" "${redis_max_port}" >&2
  return 1
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
