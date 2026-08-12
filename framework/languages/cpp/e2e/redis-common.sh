#!/usr/bin/env bash

ZLINK_CPP_E2E_REDIS_PORT_MIN=30000
ZLINK_CPP_E2E_REDIS_PORT_MAX=30099
ZLINK_CPP_E2E_APP_PORT_MIN=30100
ZLINK_CPP_E2E_APP_PORT_MAX=31999
ZLINK_CPP_E2E_RUN_LOCK_PATH=/tmp/zlink-framework-cpp-e2e.lock
declare -n zlink_cpp_e2e_redis_port_min=ZLINK_CPP_E2E_REDIS_PORT_MIN
declare -n zlink_cpp_e2e_redis_port_max=ZLINK_CPP_E2E_REDIS_PORT_MAX
declare -n zlink_cpp_e2e_app_port_min=ZLINK_CPP_E2E_APP_PORT_MIN
declare -n zlink_cpp_e2e_app_port_max=ZLINK_CPP_E2E_APP_PORT_MAX
declare -n zlink_cpp_e2e_run_lock_path=ZLINK_CPP_E2E_RUN_LOCK_PATH

if ! declare -p ZLINK_CPP_E2E_OWNED_REDIS_IDS >/dev/null 2>&1; then
  declare -ga ZLINK_CPP_E2E_OWNED_REDIS_IDS=()
fi
declare -n zlink_cpp_e2e_owned_redis_ids=ZLINK_CPP_E2E_OWNED_REDIS_IDS

zlink_cpp_e2e_acquire_run_lock() {
  local runner="$1"
  shift

  if [[ "${zlink_cpp_e2e_run_lock_held:-}" == "${zlink_cpp_e2e_run_lock_path}" ]]; then
    if [[ "${zlink_cpp_e2e_port_registry_ready:-}" != "1" ]]; then
      : >"${zlink_cpp_e2e_port_registry}"
      export zlink_cpp_e2e_port_registry_ready=1
    fi
    return 0
  fi
  if ! command -v flock >/dev/null 2>&1; then
    printf 'flock is required to serialize C++ framework E2E runs.\n' >&2
    return 1
  fi
  if [[ "${runner}" != /* ]]; then
    runner="$(realpath "${runner}")"
  fi

  export zlink_cpp_e2e_run_lock_held="${zlink_cpp_e2e_run_lock_path}"
  export zlink_cpp_e2e_port_registry="${zlink_cpp_e2e_run_lock_path}.ports"
  export zlink_cpp_e2e_run_id="${zlink_cpp_e2e_run_id:-$$-${RANDOM}}"
  exec flock --exclusive --close "${zlink_cpp_e2e_run_lock_path}" \
    bash "${runner}" "$@"
}

zlink_cpp_e2e_cleanup_dispatch() {
  local status="$1"
  local cleanup_status=0
  trap - EXIT
  set +e

  if declare -F cleanup >/dev/null 2>&1; then
    cleanup
    cleanup_status=$?
  else
    local owned_id
    for owned_id in "${zlink_cpp_e2e_owned_redis_ids[@]}"; do
      zlink_redis_remove_by_id "${owned_id}" || cleanup_status=$?
    done
  fi
  if [[ "${status}" == "0" && "${cleanup_status}" != "0" ]]; then
    status="${cleanup_status}"
  fi
  exit "${status}"
}

zlink_cpp_e2e_install_cleanup_trap() {
  trap 'zlink_cpp_e2e_cleanup_dispatch "$?"' EXIT
}

zlink_cpp_e2e_allocate_ports() {
  local count="$1"
  local bind_host="${2:-127.0.0.1}"
  if [[ -z "${zlink_cpp_e2e_port_registry:-}" ]]; then
    printf 'C++ E2E port allocation requires the language-wide run lock.\n' >&2
    return 1
  fi

  python3 - "${count}" "${zlink_cpp_e2e_port_registry}" \
    "${zlink_cpp_e2e_app_port_min}" "${zlink_cpp_e2e_app_port_max}" \
    "${bind_host}" <<'PY'
import fcntl
import os
import random
import socket
import sys

count = int(sys.argv[1])
registry_path = sys.argv[2]
minimum_port = int(sys.argv[3])
maximum_port = int(sys.argv[4])
bind_host = sys.argv[5]
pool_size = maximum_port - minimum_port + 1
if count < 1 or count > pool_size:
    raise SystemExit(f"application port count must be between 1 and {pool_size}")

with open(registry_path, "a+", encoding="utf-8") as registry:
    fcntl.flock(registry.fileno(), fcntl.LOCK_EX)
    registry.seek(0)
    claimed = {
        int(line.strip())
        for line in registry
        if line.strip().isdigit()
    }
    candidates = list(range(minimum_port, maximum_port + 1))
    random.SystemRandom().shuffle(candidates)
    sockets = []
    selected = []
    try:
        for port in candidates:
            if port in claimed:
                continue
            current = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                current.bind((bind_host, port))
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
            registry.write(f"{port}\n")
        registry.flush()
        os.fsync(registry.fileno())
        print(" ".join(str(port) for port in selected))
    finally:
        for current in sockets:
            current.close()
PY
}

zlink_cpp_e2e_allocate_endpoints() {
  zlink_cpp_e2e_allocate_endpoints_for_host 127.0.0.1 "$@"
}

zlink_cpp_e2e_allocate_endpoints_for_host() {
  local bind_host="$1"
  shift
  local -a schemes=("$@")
  local -a ports=()
  local index

  read -r -a ports <<<"$(zlink_cpp_e2e_allocate_ports \
    "${#schemes[@]}" "${bind_host}")"
  if (( ${#ports[@]} != ${#schemes[@]} )); then
    printf 'Expected %s C++ E2E ports but allocated %s.\n' \
      "${#schemes[@]}" "${#ports[@]}" >&2
    return 1
  fi
  for ((index=0; index<${#schemes[@]}; index++)); do
    printf '%s://%s:%s' "${schemes[index]}" "${bind_host}" "${ports[index]}"
    if (( index + 1 < ${#schemes[@]} )); then
      printf ' '
    fi
  done
  printf '\n'
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
  local docker_timeout_seconds=10

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
  local image="${2:-redis:7-alpine}"
  local docker_timeout_seconds="${3:-10}"
  local run_id="${4:-${zlink_cpp_e2e_run_id:-$$}}"
  local pool_size=$((zlink_cpp_e2e_redis_port_max - zlink_cpp_e2e_redis_port_min + 1))
  local start_offset=$((RANDOM % pool_size))
  local attempt port name create_output create_status container_id
  local start_output start_status running host_port failure_details

  for ((attempt=0; attempt<pool_size; attempt++)); do
    port=$((zlink_cpp_e2e_redis_port_min + (start_offset + attempt) % pool_size))
    zlink_redis_port_is_available "${port}" || continue
    name="${scope}-${run_id}-${BASHPID}-${RANDOM}-${port}"

    if create_output="$(timeout -k 2s "${docker_timeout_seconds}s" docker create \
      --name "${name}" --tmpfs /data -p "127.0.0.1:${port}:6379" \
      "${image}" 2>&1)"; then
      create_status=0
    else
      create_status=$?
    fi
    container_id="$(printf '%s\n' "${create_output}" \
      | awk '/^[0-9a-f]{12,64}$/ { print; exit }')"
    if [[ "${create_status}" != "0" || -z "${container_id}" ]]; then
      zlink_redis_remove_attempt "${container_id}" "${name}"
      if zlink_redis_is_bind_conflict "${create_output}"; then
        continue
      fi
      printf 'Failed to create Redis container %s (docker status %s)\n%s\n' \
        "${name}" "${create_status}" "${create_output}" >&2
      return 1
    fi

    if start_output="$(timeout -k 2s "${docker_timeout_seconds}s" \
      docker start "${container_id}" 2>&1)"; then
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
    if [[ "${running}" == "true" && "${host_port}" == "${port}" ]]; then
      printf '%s %s\n' "${container_id}" "${host_port}"
      return 0
    fi

    zlink_redis_remove_attempt "${container_id}" "${name}"
    printf 'Redis container %s did not enter running state on selected port %s.\n' \
      "${name}" "${port}" >&2
    return 1
  done

  printf 'No Redis port is available within %s-%s.\n' \
    "${zlink_cpp_e2e_redis_port_min}" "${zlink_cpp_e2e_redis_port_max}" >&2
  return 1
}

zlink_redis_start_scoped_assign() {
  local container_var="$1"
  local port_var="$2"
  shift 2

  local output container_id host_port
  output="$(zlink_redis_start_scoped "$@")" || return $?
  read -r container_id host_port <<<"${output}"
  if [[ -z "${container_id}" || -z "${host_port}" ]]; then
    printf 'Redis helper did not return container id and host port.\n' >&2
    return 1
  fi

  printf -v "${container_var}" '%s' "${container_id}"
  printf -v "${port_var}" '%s' "${host_port}"
  ZLINK_CPP_E2E_OWNED_REDIS_IDS+=("${container_id}")
}

zlink_redis_wait_ready() {
  local container_id="$1"
  local timeout_seconds="${2:-60}"
  local poll_seconds="${3:-1}"
  local deadline=$((SECONDS + timeout_seconds))

  while (( SECONDS < deadline )); do
    if timeout -k 2s 5s docker exec "${container_id}" redis-cli ping 2>/dev/null \
      | grep -qx PONG; then
      return 0
    fi
    sleep "${poll_seconds}"
  done

  printf 'Timed out waiting %ss for Redis container readiness: %s\n' \
    "${timeout_seconds}" "${container_id}" >&2
  return 1
}
