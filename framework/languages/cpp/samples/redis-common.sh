#!/usr/bin/env bash

declare -ag zlink_cpp_sample_forced_teardown_roles=()
declare -ag zlink_cpp_sample_teardown_failures=()

zlink_cpp_sample_role_name_for_pid() {
  local pid="$1"
  local stdout_path=""
  stdout_path="$(readlink "/proc/${pid}/fd/1" 2>/dev/null || true)"
  if [[ -n "${stdout_path}" ]]; then
    basename "${stdout_path}" .log
    return 0
  fi

  local argument=""
  while IFS= read -r -d '' argument; do
    if [[ "${argument}" == *.exe ]]; then
      basename "${argument}" .exe
      return 0
    fi
  done <"/proc/${pid}/cmdline" 2>/dev/null
  printf 'pid-%s\n' "${pid}"
}

# A role that has to be SIGKILLed to stop is a sample failure, not a teardown
# detail. This is the only place where C++ sample runners escalate a role from
# SIGTERM to SIGKILL, so it records the role and the common EXIT trap turns that
# fact into the run's verdict.
zlink_cpp_sample_stop_processes() {
  local pids=("$@")
  local -A roles=()
  local -A forced=()
  local pid status any_alive role
  local wait_attempts=300

  for pid in "${pids[@]}"; do
    [[ "${pid}" =~ ^[0-9]+$ ]] || continue
    roles["${pid}"]="$(zlink_cpp_sample_role_name_for_pid "${pid}")"
  done
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    pid="${pids[$i]}"
    [[ "${pid}" =~ ^[0-9]+$ ]] || continue
    if kill -0 "${pid}" >/dev/null 2>&1; then
      kill -TERM "${pid}" >/dev/null 2>&1 || true
    fi
  done
  for ((i=0; i<wait_attempts; i++)); do
    any_alive=0
    for pid in "${pids[@]}"; do
      if kill -0 "${pid}" >/dev/null 2>&1; then
        any_alive=1
        break
      fi
    done
    [[ "${any_alive}" == "0" ]] && break
    sleep 0.1
  done
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    pid="${pids[$i]}"
    if kill -0 "${pid}" >/dev/null 2>&1; then
      role="${roles[${pid}]:-pid-${pid}}"
      if kill -KILL "${pid}" >/dev/null 2>&1; then
        forced["${pid}"]=1
        zlink_cpp_sample_forced_teardown_roles+=(
          "Sample role ${role} (pid ${pid}) required SIGKILL during cleanup.")
      fi
    fi
  done
  for pid in "${pids[@]}"; do
    [[ "${pid}" =~ ^[0-9]+$ ]] || continue
    status=0
    wait "${pid}" >/dev/null 2>&1 || status=$?
    if [[ -n "${forced[${pid}]:-}" ]]; then
      continue
    fi
    if [[ "${status}" != "0" && "${status}" != "127" &&
          "${status}" != "130" && "${status}" != "143" ]]; then
      zlink_cpp_sample_teardown_failures+=(
        "Sample process ${roles[${pid}]:-pid-${pid}} (pid ${pid}) exited during cleanup with status ${status}.")
    fi
  done
}

zlink_cpp_sample_assert_graceful_teardown() {
  local failure=""
  if (( ${#zlink_cpp_sample_forced_teardown_roles[@]} == 0 &&
        ${#zlink_cpp_sample_teardown_failures[@]} == 0 )); then
    return 0
  fi
  for failure in "${zlink_cpp_sample_forced_teardown_roles[@]}" \
                 "${zlink_cpp_sample_teardown_failures[@]}"; do
    printf '%s\n' "${failure}" >&2
  done
  if (( ${#zlink_cpp_sample_forced_teardown_roles[@]} > 0 )); then
    exit 137
  fi
  exit 1
}

zlink_cpp_sample_exit_trap() {
  local status=$?
  cleanup
  zlink_cpp_sample_assert_graceful_teardown
  exit "${status}"
}

ZLINK_CPP_SAMPLE_REDIS_PORT_MIN=20000
ZLINK_CPP_SAMPLE_REDIS_PORT_MAX=20099
ZLINK_CPP_SAMPLE_APP_PORT_MIN=20100
ZLINK_CPP_SAMPLE_APP_PORT_MAX=21999
declare -n zlink_cpp_sample_redis_port_min=ZLINK_CPP_SAMPLE_REDIS_PORT_MIN
declare -n zlink_cpp_sample_redis_port_max=ZLINK_CPP_SAMPLE_REDIS_PORT_MAX
declare -n zlink_cpp_sample_app_port_min=ZLINK_CPP_SAMPLE_APP_PORT_MIN
declare -n zlink_cpp_sample_app_port_max=ZLINK_CPP_SAMPLE_APP_PORT_MAX

# A free port refuses the connect; a port with a listener answers it. Bash's
# /dev/tcp is the whole toolchain this needs, so the runner has no runtime
# beyond the sample's own.
zlink_tcp_port_is_available() {
  ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}

zlink_allocate_tcp_ports() {
  local count="$1"
  local first_port="${2:-${zlink_cpp_sample_app_port_min}}"
  local last_port="${3:-${zlink_cpp_sample_app_port_max}}"
  local paired_offset="${4:-0}"
  local absolute_last="${zlink_cpp_sample_app_port_max}"
  if [[ "$count" -le 0 || "$first_port" -le 0 || "$last_port" -lt "$first_port" \
    || "$paired_offset" -lt 0 ]]; then
    echo "invalid TCP port allocation request" >&2
    return 1
  fi

  local -a candidates selected=()
  local -A used=()
  local candidate port ports taken
  mapfile -t candidates < <(shuf -i "${first_port}-${last_port}")
  for candidate in "${candidates[@]}"; do
    ports=("$candidate")
    if [[ "$paired_offset" -gt 0 ]]; then
      ports+=("$((candidate + paired_offset))")
    fi
    if [[ "${ports[-1]}" -gt "$absolute_last" ]]; then
      continue
    fi
    taken=0
    for port in "${ports[@]}"; do
      if [[ -n "${used[$port]:-}" ]] || ! zlink_tcp_port_is_available "$port"; then
        taken=1
        break
      fi
    done
    if [[ "$taken" == "1" ]]; then
      continue
    fi
    for port in "${ports[@]}"; do
      used[$port]=1
    done
    selected+=("$candidate")
    if [[ "${#selected[@]}" -eq "$count" ]]; then
      break
    fi
  done
  if [[ "${#selected[@]}" -ne "$count" ]]; then
    echo "only ${#selected[@]} of $count requested TCP ports are available in ${first_port}-${last_port}" >&2
    return 1
  fi
  echo "${selected[*]}"
}

zlink_sample_allocate_ports() {
  zlink_allocate_tcp_ports "$1" \
    "${zlink_cpp_sample_app_port_min}" "${zlink_cpp_sample_app_port_max}"
}

zlink_sample_allocate_paired_ports() {
  zlink_allocate_tcp_ports "$1" 20100 20999 1000
}

# Runner-generated role configuration is private to the run: it is written with
# mode 0600 from stdin. Every runner's write_role_config goes through here.
zlink_sample_write_private_file() {
  local path="$1"
  (umask 077 && cat >"$path")
}

# Single owner of every C++ sample runner's run-directory lifetime.
#
# A passing run removes the directory, generated role config and all. A failing
# run keeps it and prints the path, because the role stdout/stderr logs inside it
# are the only evidence of why the run failed and a runner that deletes them on
# the failure path leaves nothing to diagnose.
zlink_sample_close_run_dir() {
  local run_dir="$1"
  local status="$2"
  local label="$3"

  [[ -n "${run_dir}" && -d "${run_dir}" ]] || return 0
  if (( ${#ZLINK_CPP_SAMPLE_FORCED_TEARDOWN_ROLES[@]} > 0 ||
        ${#ZLINK_CPP_SAMPLE_TEARDOWN_FAILURES[@]} > 0 )); then
    status=1
  fi
  if [[ "${status}" -ne 0 ]]; then
    printf '%s run directory preserved: %s\n' "${label}" "${run_dir}" >&2
    return 0
  fi
  rm -rf "${run_dir}"
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
      | grep -E -m 1 '^[0-9a-f]{12,64}$' || true)"
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
