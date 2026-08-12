#!/usr/bin/env bash

ZLINK_CPP_SAMPLE_REDIS_PORT_MIN=20000
ZLINK_CPP_SAMPLE_REDIS_PORT_MAX=20099
ZLINK_CPP_SAMPLE_APP_PORT_MIN=20100
ZLINK_CPP_SAMPLE_APP_PORT_MAX=21999
declare -n zlink_cpp_sample_redis_port_min=ZLINK_CPP_SAMPLE_REDIS_PORT_MIN
declare -n zlink_cpp_sample_redis_port_max=ZLINK_CPP_SAMPLE_REDIS_PORT_MAX
declare -n zlink_cpp_sample_app_port_min=ZLINK_CPP_SAMPLE_APP_PORT_MIN
declare -n zlink_cpp_sample_app_port_max=ZLINK_CPP_SAMPLE_APP_PORT_MAX

zlink_tcp_port_is_available() {
  python3 - "$1" <<'PY'
import socket
import sys

port = int(sys.argv[1])
listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    listener.bind(("127.0.0.1", port))
except OSError:
    raise SystemExit(1)
finally:
    listener.close()
PY
}

zlink_allocate_tcp_ports() {
  local count="$1"
  local first_port="${2:-${zlink_cpp_sample_app_port_min}}"
  local last_port="${3:-${zlink_cpp_sample_app_port_max}}"
  local paired_offset="${4:-0}"

  python3 - "$count" "$first_port" "$last_port" "$paired_offset" \
    "${zlink_cpp_sample_app_port_max}" <<'PY'
import secrets
import socket
import sys

count, first_port, last_port, paired_offset, absolute_last = map(int, sys.argv[1:])
if count <= 0 or first_port <= 0 or last_port < first_port or paired_offset < 0:
    raise SystemExit("invalid TCP port allocation request")

candidates = list(range(first_port, last_port + 1))
secrets.SystemRandom().shuffle(candidates)
listeners = []
selected = []
used = set()
try:
    for candidate in candidates:
        ports = [candidate]
        if paired_offset:
            ports.append(candidate + paired_offset)
        if ports[-1] > absolute_last or any(port in used for port in ports):
            continue
        current = []
        try:
            for port in ports:
                listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                listener.bind(("127.0.0.1", port))
                current.append(listener)
        except OSError:
            for listener in current:
                listener.close()
            continue
        listeners.extend(current)
        used.update(ports)
        selected.append(candidate)
        if len(selected) == count:
            break
    if len(selected) != count:
        raise SystemExit(
            f"only {len(selected)} of {count} requested TCP ports are available "
            f"in {first_port}-{last_port}")
    print(" ".join(str(port) for port in selected))
finally:
    for listener in listeners:
        listener.close()
PY
}

zlink_sample_allocate_ports() {
  zlink_allocate_tcp_ports "$1" \
    "${zlink_cpp_sample_app_port_min}" "${zlink_cpp_sample_app_port_max}"
}

zlink_sample_allocate_paired_ports() {
  zlink_allocate_tcp_ports "$1" 20100 20999 1000
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
  local run_id="${4:-$$}"
  local range_size=$((zlink_cpp_sample_redis_port_max - zlink_cpp_sample_redis_port_min + 1))
  local start_offset=$(((BASHPID + RANDOM) % range_size))
  local attempt port name create_output create_status container_id
  local start_output start_status running host_port failure_details

  for ((attempt=0; attempt<range_size; attempt++)); do
    port=$((zlink_cpp_sample_redis_port_min + (start_offset + attempt) % range_size))
    zlink_tcp_port_is_available "$port" || continue
    name="${scope}-${run_id}-${BASHPID}-${attempt}-${RANDOM}"

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
    "${zlink_cpp_sample_redis_port_min}" "${zlink_cpp_sample_redis_port_max}" >&2
  return 1
}

zlink_redis_start_scoped_assign() {
  local container_var="$1"
  local port_var="$2"
  shift 2

  local output container_id host_port
  output="$(zlink_redis_start_scoped "$@")" || return $?
  read -r container_id host_port <<<"$output"
  if [[ -z "$container_id" || -z "$host_port" ]]; then
    printf 'Redis helper did not return container id and host port.\n' >&2
    return 1
  fi

  printf -v "$container_var" '%s' "$container_id"
  printf -v "$port_var" '%s' "$host_port"
}
