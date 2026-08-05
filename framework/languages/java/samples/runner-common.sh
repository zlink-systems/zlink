#!/usr/bin/env bash

zlink_sample_reserve_ports() {
  local count="$1"
  python3 - "${count}" <<'PY'
import socket
import sys

sockets = []
try:
    for _ in range(int(sys.argv[1])):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind(("127.0.0.1", 0))
        sockets.append(sock)
    print(" ".join(str(sock.getsockname()[1]) for sock in sockets))
finally:
    for sock in sockets:
        sock.close()
PY
}

zlink_sample_descendants() {
  local pid="$1"
  local child
  (pgrep -P "${pid}" 2>/dev/null || true) | while read -r child; do
    zlink_sample_descendants "${child}"
    echo "${child}"
  done
}

descendants() {
  zlink_sample_descendants "$@"
}

zlink_sample_print_logs() {
  local status="$1"
  if declare -F print_logs >/dev/null 2>&1; then
    print_logs "${status}"
  fi
}

cleanup() {
  local status="$?"
  local cleanup_status=0
  local force_killed=0
  set +e
  zlink_sample_print_logs "${status}"
  local pid_list_name=""
  if declare -p pids >/dev/null 2>&1; then
    pid_list_name="pids"
  elif declare -p PIDS >/dev/null 2>&1; then
    pid_list_name="PIDS"
  fi
  if [[ -n "${pid_list_name}" ]]; then
    local -n zlink_sample_pids="${pid_list_name}"
    for ((i=${#zlink_sample_pids[@]}-1; i>=0; i--)); do
      local pid="${zlink_sample_pids[$i]}"
      for child in $(zlink_sample_descendants "${pid}"); do
        kill "${child}" >/dev/null 2>&1 || true
      done
      kill "${pid}" >/dev/null 2>&1 || true
    done
    local any_alive=1
    # Spring's framework lifecycle allows up to 25 seconds for actor handoff and
    # ownership cleanup. Keep the runner order-neutral and wait for that contract.
    for _ in $(seq 1 "${ZLINK_SAMPLE_CLEANUP_WAIT_ATTEMPTS:-300}"); do
      any_alive=0
      for pid in "${zlink_sample_pids[@]}"; do
        if kill -0 "${pid}" >/dev/null 2>&1; then
          any_alive=1
          break
        fi
        for child in $(zlink_sample_descendants "${pid}"); do
          if kill -0 "${child}" >/dev/null 2>&1; then
            any_alive=1
            break
          fi
        done
      done
      if [[ "${any_alive}" == "0" ]]; then
        break
      fi
      sleep 0.1
    done
    if [[ "${any_alive}" == "1" ]]; then
      force_killed=1
      for ((i=${#zlink_sample_pids[@]}-1; i>=0; i--)); do
        local pid="${zlink_sample_pids[$i]}"
        for child in $(zlink_sample_descendants "${pid}"); do
          kill -9 "${child}" >/dev/null 2>&1 || true
        done
        kill -9 "${pid}" >/dev/null 2>&1 || true
      done
    fi
    set +m
    for pid in "${zlink_sample_pids[@]}"; do
      wait "${pid}" >/dev/null 2>&1
      local wait_status="$?"
      case "${wait_status}" in
        0|143)
          ;;
        *)
          echo "Sample process ${pid} exited during cleanup with status ${wait_status}." >&2
          if [[ "${cleanup_status}" == "0" ]]; then
            cleanup_status="${wait_status}"
          fi
          ;;
      esac
    done
    if [[ "${force_killed}" == "1" && "${cleanup_status}" == "0" ]]; then
      echo "Sample cleanup exceeded the graceful shutdown deadline." >&2
      cleanup_status=1
    fi
  fi
  if [[ -n "${redis_container_id:-}" ]]; then
    zlink_redis_remove_by_id "${redis_container_id}" || true
  elif [[ -n "${REDIS_CONTAINER:-}" ]]; then
    zlink_redis_remove_by_id "${REDIS_CONTAINER}" || true
  fi
  if [[ "${status}" != "0" ]]; then
    return "${status}"
  fi
  return "${cleanup_status}"
}

wait_port() {
  local host="$1"
  local port="${2:-}"
  if [[ "${port}" == tcp://* ]]; then
    local endpoint="${port#tcp://}"
    host="${endpoint%:*}"
    port="${endpoint##*:}"
  elif [[ "${port}" == http://* ]]; then
    local endpoint="${port#http://}"
    host="${endpoint%:*}"
    port="${endpoint##*:}"
  elif [[ "${port}" == redis://* ]]; then
    local endpoint="${port#redis://}"
    host="${endpoint%:*}"
    port="${endpoint##*:}"
  elif [[ "${host}" == tcp://* ]]; then
    local endpoint="${host#tcp://}"
    host="${endpoint%:*}"
    port="${endpoint##*:}"
  elif [[ "${host}" == http://* ]]; then
    local endpoint="${host#http://}"
    host="${endpoint%:*}"
    port="${endpoint##*:}"
  elif [[ "${host}" == redis://* ]]; then
    local endpoint="${host#redis://}"
    host="${endpoint%:*}"
    port="${endpoint##*:}"
  elif [[ "${host}" == *:* && -z "${port}" ]]; then
    port="${host##*:}"
    host="${host%:*}"
  fi
  local deadline=$((SECONDS + ${ZLINK_SAMPLE_WAIT_SECONDS:-60}))
  while (( SECONDS < deadline )); do
    if (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${host}:${port}" >&2
  return 1
}

wait_http() {
  local url="$1"
  local deadline=$((SECONDS + ${ZLINK_SAMPLE_WAIT_SECONDS:-60}))
  while (( SECONDS < deadline )); do
    if curl -fsS "${url}/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for ${url}/health" >&2
  return 1
}

wait_http_health() {
  wait_http "$@"
}

wait_framework_ready_logs() {
  local log_dir="$1"
  local require_peer_ready="${2:-0}"
  local deadline=$((SECONDS + ${ZLINK_SAMPLE_WAIT_SECONDS:-60}))
  while (( SECONDS < deadline )); do
    local found_log=0
    local all_ready=1
    for log_file in "${log_dir}"/*.log; do
      [[ -f "${log_file}" ]] || continue
      case "$(basename "${log_file}")" in
        build.log|client.log|*-client.log|api.log|play.log|flow-*.log)
          continue
          ;;
      esac
      found_log=1
      if ! grep -q 'ZLINK_FRAMEWORK_READY' "${log_file}"; then
        all_ready=0
        break
      fi
    done
    local peer_ready=1
    if [[ "${require_peer_ready}" == "1" ]]; then
      for log_file in "${log_dir}"/*.log; do
        [[ -f "${log_file}" ]] || continue
        case "$(basename "${log_file}")" in
        build.log|client.log|*-client.log|api.log|play.log|flow-*.log)
            continue
            ;;
        esac
        if ! grep -q 'ZLINK_FRAMEWORK_PEER_READY' "${log_file}"; then
          peer_ready=0
          break
        fi
      done
    fi
    if [[ "${found_log}" == "1" && "${all_ready}" == "1" \
        && "${peer_ready}" == "1" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for Framework readiness in ${log_dir}" >&2
  return 1
}

wait_framework_peer_ready_counts() {
  local log_dir="$1"
  shift
  local deadline=$((SECONDS + ${ZLINK_SAMPLE_WAIT_SECONDS:-60}))
  while (( SECONDS < deadline )); do
    local all_ready=1
    local spec log_name expected actual
    for spec in "$@"; do
      log_name="${spec%%:*}"
      expected="${spec##*:}"
      if [[ -z "${log_name}" || -z "${expected}" || ! "${expected}" =~ ^[0-9]+$ ]]; then
        echo "Invalid Framework peer readiness specification: ${spec}" >&2
        return 1
      fi
      if [[ ! -f "${log_dir}/${log_name}" ]]; then
        all_ready=0
        break
      fi
      actual="$({
        rg 'ZLINK_FRAMEWORK_PEER_READY' "${log_dir}/${log_name}" || true
      } | { rg -o 'peer=[^ ]+' || true; } | sort -u | wc -l | tr -d ' ')"
      if (( actual < expected )); then
        all_ready=0
        break
      fi
    done
    if [[ "${all_ready}" == "1" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for Framework peer topology in ${log_dir}" >&2
  return 1
}

gradle_run() {
  if declare -p ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS >/dev/null 2>&1; then
    ../../gradlew "${ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS[@]}" --no-daemon --no-parallel "$@" --quiet
  else
    ../../gradlew --settings-file standalone.settings.gradle.kts --no-daemon --no-parallel "$@" --quiet
  fi
}

app_bin() {
  local project="$1"
  local script="$2"
  echo "${project}/build/install/${script}/bin/${script}"
}

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
  timeout -k 2s "${docker_timeout_seconds}s" docker rm -fv "${container_id}" \
    >/dev/null 2>&1
}

zlink_redis_start_scoped() {
  local scope="$1"
  local image="${2:-${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}}"
  local port_mapping="${3:-127.0.0.1::6379}"
  local docker_timeout_seconds="${ZLINK_REDIS_DOCKER_TIMEOUT_SECONDS:-10}"
  local run_id="${ZLINK_REDIS_RUN_ID:-$$}"
  local name="${scope}-${run_id}-${BASHPID}-${RANDOM}"

  local create_output create_status container_id start_output start_status running
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
    printf 'Failed to create Redis container %s (docker status %s)\n%s\n' "${name}" "${create_status}" "${create_output}" >&2
    return 1
  fi
  set +e
  start_output="$(timeout -k 2s "${docker_timeout_seconds}s" docker start "${container_id}" 2>&1)"
  start_status="$?"
  set -e
  running="$(timeout -k 2s 5s docker inspect -f '{{.State.Running}}' "${container_id}" 2>/dev/null || true)"
  if [[ "${running}" != "true" ]]; then
    zlink_redis_remove_by_id "${container_id}" || true
    printf 'Failed to start Redis container %s (docker status %s)\n%s\n' "${name}" "${start_status}" "${start_output}" >&2
    return 1
  fi
  if ! zlink_redis_wait_ready "${container_id}"; then
    zlink_redis_remove_by_id "${container_id}" || true
    return 1
  fi
  printf '%s' "${container_id}"
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

  printf -v "${container_var}" '%s' "${container_id}"
  printf -v "${host_port_var}" '%s' "${host_port}"
}
