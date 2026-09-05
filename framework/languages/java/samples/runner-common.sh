#!/usr/bin/env bash

zlink_sample_configure_port_pool() {
  local language="$1"
  case "${language}" in
    java)
      ZLINK_SAMPLE_REDIS_PORT_MIN=24000
      ZLINK_SAMPLE_REDIS_PORT_MAX=24099
      ZLINK_SAMPLE_APP_PORT_MIN=24100
      ZLINK_SAMPLE_APP_PORT_MAX=25999
      ;;
    kotlin)
      ZLINK_SAMPLE_REDIS_PORT_MIN=26000
      ZLINK_SAMPLE_REDIS_PORT_MAX=26099
      ZLINK_SAMPLE_APP_PORT_MIN=26100
      ZLINK_SAMPLE_APP_PORT_MAX=27999
      ;;
    *)
      printf 'Unsupported JVM sample language for port allocation: %s\n' "${language}" >&2
      return 1
      ;;
  esac
}

zlink_sample_require_port_pool() {
  if [[ -z "${ZLINK_SAMPLE_REDIS_PORT_MIN:-}" \
      || -z "${ZLINK_SAMPLE_REDIS_PORT_MAX:-}" \
      || -z "${ZLINK_SAMPLE_APP_PORT_MIN:-}" \
      || -z "${ZLINK_SAMPLE_APP_PORT_MAX:-}" ]]; then
    echo 'JVM sample port pool is not configured.' >&2
    return 1
  fi
}

zlink_sample_reserve_ports_in_range() {
  local count="$1"
  local minimum="$2"
  local maximum="$3"
  python3 - "${count}" "${minimum}" "${maximum}" <<'PY'
import random
import socket
import sys

count = int(sys.argv[1])
minimum = int(sys.argv[2])
maximum = int(sys.argv[3])
if count < 1 or minimum < 1 or maximum > 65535 or minimum > maximum:
    print("invalid JVM sample port allocation request", file=sys.stderr)
    sys.exit(1)
if count > maximum - minimum + 1:
    print("JVM sample port pool is smaller than the requested allocation", file=sys.stderr)
    sys.exit(1)

sockets = []
try:
    candidates = list(range(minimum, maximum + 1))
    random.SystemRandom().shuffle(candidates)
    for port in candidates:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.bind(("127.0.0.1", port))
        except OSError:
            sock.close()
            continue
        sockets.append(sock)
        if len(sockets) == count:
            break
    if len(sockets) != count:
        print(
            f"unable to bind-check {count} ports in {minimum}-{maximum}",
            file=sys.stderr,
        )
        sys.exit(1)
    print(" ".join(str(sock.getsockname()[1]) for sock in sockets))
finally:
    for sock in sockets:
        sock.close()
PY
}

zlink_sample_reserve_ports() {
  local count="$1"
  zlink_sample_require_port_pool || return 1
  zlink_sample_reserve_ports_in_range \
    "${count}" "${ZLINK_SAMPLE_APP_PORT_MIN}" "${ZLINK_SAMPLE_APP_PORT_MAX}"
}

zlink_sample_reserve_endpoints() {
  local count="$1"
  local ports=()
  local port

  read -r -a ports <<<"$(zlink_sample_reserve_ports "${count}")"
  for port in "${ports[@]}"; do
    printf '127.0.0.1:%s ' "${port}"
  done
  printf '\n'
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

zlink_sample_preserve_logs() {
  local log_dir="$1"
  local root="${ZLINK_SAMPLE_FAILURE_LOG_ROOT:-}"
  [[ -n "${root}" && -d "${log_dir}" ]] || return 0
  local prefix="${ZLINK_SAMPLE_FAILURE_LOG_PREFIX:-sample}"
  local preserved_dir="${root}/${prefix}-$(date +%Y%m%d-%H%M%S)-$$"
  mkdir -p "${preserved_dir}"
  cp -a "${log_dir}/." "${preserved_dir}/"
  echo "Sample failure logs: ${preserved_dir}" >&2
}

zlink_sample_verify_framework_termination() {
  local log_dir="$1"
  [[ -n "${log_dir}" ]] || return 0
  local role_log
  local ready_count
  local termination_count
  local stopped_count
  local force_stopped_count
  local failed=0
  local -a role_logs=()
  read -r -a role_logs <<< "${ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS:-}"
  if (( ${#role_logs[@]} == 0 )); then
    echo "Framework role logs are not configured for this sample: ${log_dir}" >&2
    return 1
  fi
  for role_log in "${role_logs[@]}"; do
    local log_file="${log_dir}/${role_log}"
    local first_line=1
    if declare -p ZLINK_SAMPLE_FRAMEWORK_ROLE_LOG_OFFSETS >/dev/null 2>&1; then
      first_line="${ZLINK_SAMPLE_FRAMEWORK_ROLE_LOG_OFFSETS[$role_log]:-1}"
    fi
    if [[ ! -f "${log_file}" ]]; then
      echo "Framework role log is missing: ${log_file}" >&2
      failed=1
      continue
    fi
    ready_count="$(tail -n +"${first_line}" "${log_file}" | grep -c 'ZLINK_FRAMEWORK_READY' || true)"
    termination_count="$(tail -n +"${first_line}" "${log_file}" | grep -c 'ZLINK_FRAMEWORK_TERMINATION outcome=' || true)"
    stopped_count="$(tail -n +"${first_line}" "${log_file}" | grep -c 'ZLINK_FRAMEWORK_TERMINATION outcome=STOPPED reason=NONE' || true)"
    force_stopped_count="$(tail -n +"${first_line}" "${log_file}" | grep -c 'ZLINK_FRAMEWORK_TERMINATION outcome=FORCE_STOPPED' || true)"
    if [[ "${ready_count}" != "1" || "${termination_count}" != "1" \
        || "${stopped_count}" != "1" || "${force_stopped_count}" != "0" ]]; then
      echo "Framework lifecycle evidence is incomplete: ${log_file}" >&2
      echo "READY=${ready_count} TERMINATION=${termination_count} \
STOPPED_NONE=${stopped_count} FORCE_STOPPED=${force_stopped_count}" >&2
      rg 'ZLINK_FRAMEWORK_(READY|TERMINATION)' "${log_file}" || true
      failed=1
    fi
  done
  if [[ "${failed}" != "0" ]]; then
    return 1
  fi
  return 0
}

cleanup() {
  local status="$?"
  local cleanup_status=0
  local force_killed=0
  local zlink_sample_log_dir="${log_dir:-${LOG_DIR:-}}"
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
    # Runtime drain uses the public 30-second deadline and may then finish its
    # bounded owner/resource cleanup. The default 90-second observation window
    # must complete before the runner uses SIGKILL.
    for _ in $(seq 1 "${ZLINK_SAMPLE_CLEANUP_WAIT_ATTEMPTS:-900}"); do
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
  if [[ "${status}" == "0" && "${cleanup_status}" == "0" ]]; then
    if ! zlink_sample_verify_framework_termination "${zlink_sample_log_dir}"; then
      cleanup_status=1
    fi
  fi
  if [[ "${status}" != "0" || "${cleanup_status}" != "0" ]]; then
    zlink_sample_preserve_logs "${zlink_sample_log_dir}"
  fi
  if [[ -n "${redis_container_id:-}" ]]; then
    zlink_redis_remove_by_id "${redis_container_id}" || true
  elif [[ -n "${REDIS_CONTAINER:-}" ]]; then
    zlink_redis_remove_by_id "${REDIS_CONTAINER}" || true
  fi
  if [[ "${status}" != "0" ]]; then
    return "${status}"
  fi
  if [[ "${cleanup_status}" != "0" ]]; then
    exit "${cleanup_status}"
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
  local -a role_logs=()
  read -r -a role_logs <<< "${ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS:-}"
  if (( ${#role_logs[@]} == 0 )); then
    echo "Framework role logs are not configured for this sample: ${log_dir}" >&2
    return 1
  fi
  while (( SECONDS < deadline )); do
    local all_ready=1
    local log_file role_log
    for role_log in "${role_logs[@]}"; do
      log_file="${log_dir}/${role_log}"
      if [[ ! -f "${log_file}" ]]; then
        all_ready=0
        break
      fi
      if ! grep -q 'ZLINK_FRAMEWORK_READY' "${log_file}"; then
        all_ready=0
        break
      fi
    done
    local peer_ready=1
    if [[ "${require_peer_ready}" == "1" ]]; then
      for role_log in "${role_logs[@]}"; do
        log_file="${log_dir}/${role_log}"
        if [[ ! -f "${log_file}" ]]; then
          peer_ready=0
          break
        fi
        if ! grep -q 'ZLINK_FRAMEWORK_PEER_READY' "${log_file}"; then
          peer_ready=0
          break
        fi
      done
    fi
    if [[ "${all_ready}" == "1" \
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

zlink_sample_gradle_locked() {
  local lock_path="/tmp/zlink-framework-java-kotlin-sample-gradle.lock"
  if ! command -v flock >/dev/null 2>&1; then
    echo 'flock is required to serialize Java and Kotlin sample builds.' >&2
    return 1
  fi
  flock --exclusive --close "${lock_path}" "$@"
}

gradle_run() {
  local -a command=(../../gradlew)
  if declare -p ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS >/dev/null 2>&1; then
    command+=("${ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS[@]}")
  else
    command+=(--settings-file standalone.settings.gradle.kts)
  fi
  command+=(--no-daemon --no-parallel --max-workers=1 "$@" --quiet)
  zlink_sample_gradle_locked "${command[@]}"
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
  zlink_sample_require_port_pool || return 1

  local range_size=$((ZLINK_SAMPLE_REDIS_PORT_MAX - ZLINK_SAMPLE_REDIS_PORT_MIN + 1))
  local start_offset=$((RANDOM % range_size))
  local offset host_port name create_output create_status container_id
  local start_output start_status running published_port
  for ((offset=0; offset<range_size; offset++)); do
    host_port=$((ZLINK_SAMPLE_REDIS_PORT_MIN + (start_offset + offset) % range_size))
    if ! zlink_sample_reserve_ports_in_range 1 "${host_port}" "${host_port}" \
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
    container_id="$(printf '%s\n' "${create_output}" | awk '/^[0-9a-f]{12,64}$/ { print; exit }')"
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
    start_output="$(timeout -k 2s "${docker_timeout_seconds}s" docker start "${container_id}" 2>&1)"
    start_status="$?"
    set -e
    running="$(timeout -k 2s 5s docker inspect -f '{{.State.Running}}' "${container_id}" 2>/dev/null || true)"
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
        "${name}" "${start_status}" "${running}" "${published_port}" "${host_port}" \
        "${start_output}" >&2
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
    "${ZLINK_SAMPLE_REDIS_PORT_MIN}" "${ZLINK_SAMPLE_REDIS_PORT_MAX}" "${scope}" >&2
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
