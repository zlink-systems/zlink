#!/usr/bin/env bash

zlink_sample_copy_evidence() {
  local run_dir="$1"
  local sample_name="$2"
  local evidence_root="${ZLINK_SAMPLE_EVIDENCE_DIR:-}"

  [[ -n "${evidence_root}" ]] || return 0
  mkdir -p "${evidence_root}/${sample_name}"
  cp -a "${run_dir}/." "${evidence_root}/${sample_name}/"
  printf 'evidenceDir=%s\n' "${evidence_root}/${sample_name}"
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
  local docker_timeout_seconds=10
  local redis_min_port=22000
  local redis_max_port=22099
  local redis_pool_size=$((redis_max_port - redis_min_port + 1))
  local start_port=$((redis_min_port + RANDOM % redis_pool_size))
  local run_id="$$"

  local offset port name create_output create_status container_id
  local start_output start_status running host_port failure_details

  printf 'redis_start scope=%s image=%s started_at=%s\n' \
    "${scope}" "${image}" "$(date -Is)" >&2
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

  local output container_id redis_endpoint
  output="$(zlink_redis_start_scoped "$@")" || return $?
  read -r container_id redis_endpoint <<<"${output}"
  if [[ -z "${container_id}" || -z "${redis_endpoint}" ]]; then
    printf 'Redis helper did not return container id and endpoint.\n' >&2
    return 1
  fi

  printf -v "${container_var}" '%s' "${container_id}"
  printf -v "${endpoint_var}" '%s' "${redis_endpoint}"
}
