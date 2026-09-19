#!/usr/bin/env bash

# A role that has to be SIGKILLed to stop is a sample failure, not a teardown
# detail. `zlink_sample_stop_processes` is the only place that escalates a role from
# SIGTERM to SIGKILL, so it names the roles it had to force and
# `zlink_sample_assert_graceful_teardown` turns that into the run's verdict;
# `sample_runner.ps1`'s `Stop-SampleProcesses` states the same rule for PowerShell.
declare -ag ZLINK_SAMPLE_FORCED_TEARDOWN_ROLES=()

zlink_sample_role_name_for_pid() {
  local pid="$1"
  local stdout_path=""
  stdout_path="$(readlink "/proc/${pid}/fd/1" 2>/dev/null || true)"
  if [[ -n "${stdout_path}" ]]; then
    basename "${stdout_path}" .log
    return 0
  fi

  local argument=""
  while IFS= read -r -d '' argument; do
    if [[ "${argument}" == *.dll ]]; then
      basename "${argument}" .dll
      return 0
    fi
  done <"/proc/${pid}/cmdline" 2>/dev/null
  printf 'pid-%s\n' "${pid}"
}

zlink_sample_stop_processes() {
  local pids=("$@")
  local i pid any_alive status
  local -A roles=()
  # /proc entries disappear as the roles exit, so name them while they are alive.
  for pid in "${pids[@]}"; do
    [[ "${pid}" =~ ^[0-9]+$ ]] || continue
    roles["${pid}"]="$(zlink_sample_role_name_for_pid "${pid}")"
  done
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    pid="${pids[$i]}"
    if kill -0 "${pid}" 2>/dev/null; then
      # Bash background jobs inherit ignored SIGINT; ConsoleLifetime handles SIGTERM.
      kill -TERM "${pid}" 2>/dev/null || true
    fi
  done
  # 05-host-relocation-flow §17: Shutdown's default deadline is 30 seconds.
  for ((i=0; i<300; i++)); do
    any_alive=0
    for pid in "${pids[@]}"; do
      if kill -0 "${pid}" 2>/dev/null; then
        any_alive=1
        break
      fi
    done
    [[ "${any_alive}" == "0" ]] && break
    sleep 0.1
  done
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    pid="${pids[$i]}"
    if kill -0 "${pid}" 2>/dev/null; then
      kill -KILL "${pid}" 2>/dev/null || true
    fi
  done
  for pid in "${pids[@]}"; do
    status=0
    wait "${pid}" 2>/dev/null || status=$?
    if [[ "${status}" == "137" ]]; then
      ZLINK_SAMPLE_FORCED_TEARDOWN_ROLES+=(
        "Sample role ${roles[${pid}]:-pid-${pid}} (pid ${pid}) exited during cleanup with status 137 (SIGKILL).")
    fi
  done
}

# The run's verdict on teardown. Bash keeps the shell's exit status across an EXIT
# trap unless the trap itself exits, so `cleanup` cannot report a forced kill by
# returning non-zero; the failure has to be an explicit `exit` from here. The
# diagnostic is printed here rather than where the kill happens so that it survives
# a sample that redirects its cleanup output.
zlink_sample_assert_graceful_teardown() {
  local failure=""
  (( ${#ZLINK_SAMPLE_FORCED_TEARDOWN_ROLES[@]} > 0 )) || return 0
  for failure in "${ZLINK_SAMPLE_FORCED_TEARDOWN_ROLES[@]}"; do
    printf '%s\n' "${failure}" >&2
  done
  exit 137
}

# Every sample installs this as its EXIT trap instead of its own `cleanup`, so the
# teardown verdict is stated once for all of them. Samples that tear down early and
# then drop the trap call `zlink_sample_assert_graceful_teardown` themselves.
zlink_sample_exit_trap() {
  local status=$?
  cleanup
  zlink_sample_assert_graceful_teardown
  exit "${status}"
}

remove_owned_pid() {
  local completed_pid="$1"
  local active=()
  local pid
  for pid in "${PIDS[@]:-}"; do
    [[ "$pid" == "$completed_pid" ]] || active+=("$pid")
  done
  PIDS=("${active[@]}")
}

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
  local port="$1"
  # Connect probe: bash cannot bind a socket to hold a reservation, but neither did the
  # bind-test this replaced -- it closed its socket immediately after the check, so both
  # versions leave the same gap up to actual use. A successful connect means something is
  # listening (port taken); a refused connect means free. This misses a port that is bound
  # but not yet listening, or lingering in TIME_WAIT without SO_REUSEADDR -- rarer than the
  # ordinary "something else is using it" case this exists to catch, and the docker/dotnet
  # bind that follows still fails loudly (and the caller moves on) on a real collision.
  if { exec 3<>"/dev/tcp/127.0.0.1/${port}"; } 2>/dev/null; then
    exec 3<&- 3>&- 2>/dev/null
    return 1
  fi
  return 0
}

# Reserves `count` distinct free ports in the samples' shared ephemeral range for one run's
# role endpoints. Same best-effort guarantee as zlink_redis_port_is_available above -- ports
# are checked one at a time and not held, where the former implementation held all of
# a batch's sockets open until every port in it was found. Prints them space-separated.
zlink_sample_pick_ports() {
  local count="$1"
  local min_port=22100
  local max_port=23999
  local -A chosen=()
  local picked=()
  local port attempt
  for ((attempt = 0; attempt < 20000 && ${#picked[@]} < count; attempt++)); do
    port=$((min_port + RANDOM % (max_port - min_port + 1)))
    [[ -n "${chosen[$port]:-}" ]] && continue
    zlink_redis_port_is_available "$port" || continue
    chosen[$port]=1
    picked+=("$port")
  done
  if [[ "${#picked[@]}" -ne "$count" ]]; then
    echo "Could not find ${count} free ports in ${min_port}-${max_port}." >&2
    return 1
  fi
  printf '%s\n' "${picked[*]}"
}

# Reads one scalar field out of a JSON document: quoted strings come back unquoted, and
# null/true/false/numbers come back as their literal token text (a null is the string "null",
# not empty -- callers that need an empty/null fallback do that themselves). This
# is a text search, not a parser: every caller passes a document this same sample's own typed
# JSON serializer produced, for a field name that appears once in it, never third-party or
# adversarial JSON. Returns non-zero and prints nothing if the field is absent.
zlink_json_field() {
  local json="$1" field="$2" match
  match="$(printf '%s' "${json}" | grep -oE "\"${field}\"[[:space:]]*:[[:space:]]*(\"([^\"\\\\]|\\\\.)*\"|null|true|false|-?[0-9]+(\.[0-9]+)?)" | head -1)" || return 1
  [[ -n "${match}" ]] || return 1
  match="${match#*:}"
  while [[ "${match}" == [[:space:]]* ]]; do match="${match# }"; done
  if [[ "${match}" == \"*\" ]]; then
    match="${match:1:-1}"
  fi
  printf '%s' "${match}"
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

    # grep -E, not awk: stock Ubuntu's /usr/bin/awk is mawk, which has no interval
    # expressions ({12,64}) and always fails to match here, so this always came back
    # empty on plain Ubuntu (WSL ships gawk, which does support them, and masked it).
    container_id="$(printf '%s\n' "${create_output}" | grep -E '^[0-9a-f]{12,64}$' | head -n1)"
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
