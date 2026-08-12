#!/usr/bin/env bash

zlink_dotnet_e2e_acquire_run_lock() {
  local runner="$1"
  shift
  local lock_path="/tmp/zlink-dotnet-framework-e2e.lock"

  if [[ "${ZLINK_DOTNET_E2E_RUN_LOCK_HELD:-}" == "1" ]]; then
    if [[ "${ZLINK_DOTNET_E2E_PORT_REGISTRY_READY:-}" != "1" ]]; then
      : >"${ZLINK_DOTNET_E2E_PORT_REGISTRY}"
      export ZLINK_DOTNET_E2E_PORT_REGISTRY_READY=1
    fi
    return 0
  fi

  if ! command -v flock >/dev/null 2>&1; then
    echo "flock is required to run .NET framework E2Es." >&2
    return 1
  fi
  if [[ "${runner}" != /* ]]; then
    runner="$(realpath "${runner}")"
  fi

  export ZLINK_DOTNET_E2E_RUN_LOCK_HELD=1
  export ZLINK_DOTNET_E2E_PORT_REGISTRY="${lock_path}.ports"
  exec flock --close "${lock_path}" bash "${runner}" "$@"
}

zlink_dotnet_e2e_allocate_ports() {
  local count="$1"
  python3 - "${count}" "${ZLINK_DOTNET_E2E_PORT_REGISTRY}" "$$" <<'PY'
import fcntl
import os
import random
import socket
import sys

count = int(sys.argv[1])
registry_path = sys.argv[2]
owner = sys.argv[3]
minimum_port = 32100
maximum_port = 33999
pool_size = maximum_port - minimum_port + 1
if count < 1 or count > pool_size:
    raise SystemExit(f"application port count must be between 1 and {pool_size}")

with open(registry_path, "a+", encoding="utf-8") as registry:
    fcntl.flock(registry.fileno(), fcntl.LOCK_EX)
    registry.seek(0)
    claimed = {
        int(fields[1])
        for line in registry
        if len(fields := line.split()) == 2 and fields[0] == owner
    }
    start_port = random.SystemRandom().randrange(minimum_port, maximum_port + 1)
    sockets = []
    selected = []
    try:
        for offset in range(pool_size):
            port = minimum_port + ((start_port - minimum_port + offset) % pool_size)
            if port in claimed:
                continue
            current = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                current.bind(("127.0.0.1", port))
            except OSError:
                current.close()
                continue
            sockets.append(current)
            selected.append(port)
            if len(selected) == count:
                break
        if len(selected) != count:
            raise SystemExit(
                f"could not reserve {count} application ports within "
                f"{minimum_port}-{maximum_port}")
        registry.seek(0, os.SEEK_END)
        for port in selected:
            registry.write(f"{owner} {port}\n")
        registry.flush()
        os.fsync(registry.fileno())
        print(" ".join(str(port) for port in selected))
    finally:
        for current in sockets:
            current.close()
PY
}

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
  local image="${2:-redis:7.2-alpine}"
  local log_dir="${3:-.}"
  local docker_timeout_seconds=10
  local redis_min_port=32000
  local redis_max_port=32099
  local redis_pool_size=$((redis_max_port - redis_min_port + 1))
  local start_port=$((redis_min_port + RANDOM % redis_pool_size))
  local run_id="$$"

  local offset port name create_output create_status container_id
  local start_output start_status running host_port failure_details

  printf 'redis_start scope=%s image=%s log_dir=%s started_at=%s\n' \
    "${scope}" "${image}" "${log_dir}" "$(date -Is)" >&2
  for ((offset = 0; offset < redis_pool_size; offset++)); do
    port=$((redis_min_port + (start_port - redis_min_port + offset) % redis_pool_size))
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
        printf 'redis_port_retry port=%s stage=create\n' "${port}" >&2
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
        printf 'redis_port_retry port=%s stage=start\n' "${port}" >&2
        continue
      fi
      printf 'Failed to start Redis container %s (docker status %s)\n%s\n' \
        "${name}" "${start_status}" "${start_output}" >&2
      return 1
    fi

    running="$(timeout -k 2s 5s docker inspect -f '{{.State.Running}}' \
      "${container_id}" 2>/dev/null || true)"
    if [[ "${running}" != "true" ]]; then
      zlink_redis_remove_attempt "${container_id}" "${name}"
      printf 'Redis container %s did not enter the running state.\n' "${name}" >&2
      return 1
    fi

    host_port="$(timeout -k 2s 5s docker inspect \
      -f '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}' \
      "${container_id}" 2>/dev/null || true)"
    if [[ "${host_port}" != "${port}" ]]; then
      zlink_redis_remove_attempt "${container_id}" "${name}"
      printf 'Redis container %s did not publish the selected host port %s.\n' \
        "${name}" "${port}" >&2
      return 1
    fi

    printf 'redis_started name=%s endpoint=127.0.0.1:%s\n' "${name}" "${host_port}" >&2
    printf '%s 127.0.0.1:%s\n' "${container_id}" "${host_port}"
    return 0
  done

  printf 'No Redis port is available within %s-%s.\n' \
    "${redis_min_port}" "${redis_max_port}" >&2
  return 1
}

zlink_redis_start_scoped_assign() {
  local container_var="$1"
  local endpoint_var="$2"
  shift 2

  local output container_id endpoint_value
  output="$(zlink_redis_start_scoped "$@")" || return $?
  read -r container_id endpoint_value <<<"${output}"
  if [[ -z "${container_id}" || -z "${endpoint_value}" ]]; then
    printf 'Redis helper did not return container id and endpoint.\n' >&2
    return 1
  fi

  printf -v "${container_var}" '%s' "${container_id}"
  printf -v "${endpoint_var}" '%s' "${endpoint_value}"
}

zlink_redis_wait_ready() {
  local container_id="$1"
  local timeout_seconds="${2:-60}"
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
