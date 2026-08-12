#!/usr/bin/env bash

if ! declare -F zlink_e2e_require_configuration >/dev/null 2>&1; then
  source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/e2e-runner-common.sh"
fi

zlink_redis_wait_ready() {
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

zlink_redis_remove_by_id() {
  local container_id="$1"
  local docker_timeout_seconds="${ZLINK_REDIS_DOCKER_TIMEOUT_SECONDS:-10}"

  [[ "${container_id}" =~ ^[0-9a-f]{12,64}$ ]] || return 1
  timeout -k 2s "${docker_timeout_seconds}s" docker rm -fv "${container_id}" \
    >/dev/null 2>&1
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
  local image="${2:-${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}}"
  local docker_timeout_seconds="${ZLINK_REDIS_DOCKER_TIMEOUT_SECONDS:-10}"
  local run_id="${ZLINK_REDIS_RUN_ID:-$$}"
  zlink_e2e_require_configuration || return 1
  if (( $# > 2 )); then
    echo 'Redis host-port mappings are selected by the JVM E2E helper.' >&2
    return 1
  fi

  local range_size=$((ZLINK_E2E_REDIS_PORT_MAX - ZLINK_E2E_REDIS_PORT_MIN + 1))
  local start_offset=$((RANDOM % range_size))
  local offset host_port name create_output create_status container_id
  local start_output start_status running published_port
  for ((offset=0; offset<range_size; offset++)); do
    host_port=$((ZLINK_E2E_REDIS_PORT_MIN + (start_offset + offset) % range_size))
    if ! zlink_e2e_reserve_ports_in_range 1 "${host_port}" "${host_port}" \
        >/dev/null 2>&1; then
      continue
    fi
    name="${scope}-${run_id}-${BASHPID}-${RANDOM}-${host_port}"

    set +e
    create_output="$(timeout -k 2s "${docker_timeout_seconds}s" docker create \
      --name "${name}" \
      --tmpfs /data \
      -p "127.0.0.1:${host_port}:6379" \
      "${image}" 2>&1)"
    create_status="$?"
    set -e
    container_id="$(printf '%s\n' "${create_output}" \
      | awk '/^[0-9a-f]{12,64}$/ { print; exit }')"
    if [[ "${create_status}" != "0" || -z "${container_id}" ]]; then
      zlink_redis_remove_attempt "${container_id}" "${name}"
      if grep -Eqi 'address already in use|port is already allocated|failed to bind host port' \
          <<<"${create_output}"; then
        continue
      fi
      printf 'Failed to create Redis container %s (docker status %s)\n%s\n' \
        "${name}" "${create_status}" "${create_output}" >&2
      return 1
    fi

    set +e
    start_output="$(timeout -k 2s "${docker_timeout_seconds}s" \
      docker start "${container_id}" 2>&1)"
    start_status="$?"
    set -e
    running="$(timeout -k 2s 5s docker inspect -f '{{.State.Running}}' \
      "${container_id}" 2>/dev/null || true)"
    published_port="$(timeout -k 2s 5s docker inspect \
      -f '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}' \
      "${container_id}" 2>/dev/null || true)"
    if [[ "${running}" != "true" || "${published_port}" != "${host_port}" ]]; then
      zlink_redis_remove_by_id "${container_id}" || true
      if [[ "${running}" != "true" ]] \
          && grep -Eqi 'address already in use|port is already allocated|failed to bind host port' \
          <<<"${start_output}"; then
        continue
      fi
      printf 'Failed to verify Redis container %s (docker status %s, running=%s, published=%s, expected=%s)\n%s\n' \
        "${name}" "${start_status}" "${running}" "${published_port}" \
        "${host_port}" "${start_output}" >&2
      return 1
    fi
    if ! zlink_redis_wait_ready "${container_id}"; then
      zlink_redis_remove_by_id "${container_id}" || true
      return 1
    fi
    printf '%s' "${container_id}"
    return 0
  done

  printf 'No bindable Redis host port remained in %s-%s for %s.\n' \
    "${ZLINK_E2E_REDIS_PORT_MIN}" "${ZLINK_E2E_REDIS_PORT_MAX}" "${scope}" >&2
  return 1
}

zlink_redis_host_port() {
  local container_id="$1"
  local host_port
  host_port="$(timeout -k 2s 5s docker inspect \
    -f '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}' \
    "${container_id}" 2>/dev/null || true)"
  if [[ -z "${host_port}" ]]; then
    printf 'Failed to inspect Redis host port for container %s\n' "${container_id}" >&2
    return 1
  fi
  printf '%s\n' "${host_port}"
}

zlink_redis_wait_host_ready() {
  local host_port="$1"
  local timeout_seconds="${ZLINK_REDIS_HOST_READY_TIMEOUT_SECONDS:-10}"
  local deadline=$((SECONDS + timeout_seconds))

  while (( SECONDS < deadline )); do
    if python3 - "${host_port}" <<'PY'
import socket
import sys

try:
    with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=1):
        pass
except OSError:
    raise SystemExit(1)
raise SystemExit(0)
PY
    then
      return 0
    fi
    sleep 0.1
  done

  printf 'Timed out waiting for Redis host port %s\n' "${host_port}" >&2
  return 1
}

zlink_redis_endpoint() {
  local container_id="$1"
  local host_port
  host_port="$(zlink_redis_host_port "${container_id}")"
  printf '127.0.0.1:%s\n' "${host_port}"
}

zlink_redis_start_scoped_assign() {
  local container_var="$1"
  local host_port_var="$2"
  shift 2

  local container_id host_port
  if ! container_id="$(zlink_redis_start_scoped "$@")"; then
    return 1
  fi
  if ! host_port="$(zlink_redis_host_port "${container_id}")"; then
    zlink_redis_remove_by_id "${container_id}" || true
    return 1
  fi
  if ! zlink_redis_wait_host_ready "${host_port}"; then
    zlink_redis_remove_by_id "${container_id}" || true
    return 1
  fi

  printf -v "${container_var}" '%s' "${container_id}"
  printf -v "${host_port_var}" '%s' "${host_port}"
}
