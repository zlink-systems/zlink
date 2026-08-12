#!/usr/bin/env bash

zlink_e2e_configure_runner() {
  local requested_language="$1"
  local language="${ZLINK_E2E_RUNNER_LANGUAGE_OVERRIDE:-${requested_language}}"

  case "${language}" in
    java)
      ZLINK_E2E_REDIS_PORT_MIN=34000
      ZLINK_E2E_REDIS_PORT_MAX=34099
      ZLINK_E2E_APP_PORT_MIN=34100
      ZLINK_E2E_APP_PORT_MAX=35999
      ZLINK_E2E_RUN_LOCK_PATH="/tmp/zlink-framework-java-e2e-run.lock"
      ;;
    kotlin)
      ZLINK_E2E_REDIS_PORT_MIN=36000
      ZLINK_E2E_REDIS_PORT_MAX=36099
      ZLINK_E2E_APP_PORT_MIN=36100
      ZLINK_E2E_APP_PORT_MAX=37999
      ZLINK_E2E_RUN_LOCK_PATH="/tmp/zlink-framework-kotlin-e2e-run.lock"
      ;;
    *)
      printf 'Unsupported JVM E2E language for runner isolation: %s\n' \
        "${language}" >&2
      return 1
      ;;
  esac

  ZLINK_E2E_LANGUAGE="${language}"
}

zlink_e2e_require_configuration() {
  if [[ -z "${ZLINK_E2E_LANGUAGE:-}" \
      || -z "${ZLINK_E2E_REDIS_PORT_MIN:-}" \
      || -z "${ZLINK_E2E_REDIS_PORT_MAX:-}" \
      || -z "${ZLINK_E2E_APP_PORT_MIN:-}" \
      || -z "${ZLINK_E2E_APP_PORT_MAX:-}" \
      || -z "${ZLINK_E2E_RUN_LOCK_PATH:-}" ]]; then
    echo 'JVM E2E runner isolation is not configured.' >&2
    return 1
  fi
}

zlink_e2e_acquire_run_lock() {
  local runner="$1"
  shift
  zlink_e2e_require_configuration || return 1
  if ! command -v flock >/dev/null 2>&1; then
    echo 'flock is required to serialize JVM E2E runs within each language.' >&2
    return 1
  fi

  case "${ZLINK_E2E_LANGUAGE}" in
    java)
      if [[ "${ZLINK_JAVA_E2E_RUN_LOCK_HELD:-}" == "${ZLINK_E2E_RUN_LOCK_PATH}" ]]; then
        return 0
      fi
      exec flock --exclusive --close "${ZLINK_E2E_RUN_LOCK_PATH}" \
        env ZLINK_JAVA_E2E_RUN_LOCK_HELD="${ZLINK_E2E_RUN_LOCK_PATH}" \
        bash "${runner}" "$@"
      ;;
    kotlin)
      if [[ "${ZLINK_KOTLIN_E2E_RUN_LOCK_HELD:-}" == "${ZLINK_E2E_RUN_LOCK_PATH}" ]]; then
        return 0
      fi
      exec flock --exclusive --close "${ZLINK_E2E_RUN_LOCK_PATH}" \
        env ZLINK_KOTLIN_E2E_RUN_LOCK_HELD="${ZLINK_E2E_RUN_LOCK_PATH}" \
        bash "${runner}" "$@"
      ;;
  esac
}

zlink_e2e_initialize() {
  local requested_language="$1"
  local runner="$2"
  shift 2
  zlink_e2e_configure_runner "${requested_language}" || return 1
  zlink_e2e_acquire_run_lock "${runner}" "$@"
}

zlink_e2e_reserve_ports_in_range() {
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
    print("invalid JVM E2E port allocation request", file=sys.stderr)
    sys.exit(1)
if count > maximum - minimum + 1:
    print("JVM E2E port pool is smaller than the requested allocation", file=sys.stderr)
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

zlink_e2e_reserve_ports() {
  local count="$1"
  zlink_e2e_require_configuration || return 1
  zlink_e2e_reserve_ports_in_range \
    "${count}" "${ZLINK_E2E_APP_PORT_MIN}" "${ZLINK_E2E_APP_PORT_MAX}"
}

if ! declare -p ZLINK_E2E_ISSUED_APP_PORTS >/dev/null 2>&1; then
  declare -gA ZLINK_E2E_ISSUED_APP_PORTS=()
fi

zlink_e2e_assign_unique_ports() {
  local destination_name="$1"
  local count="$2"
  local attempt port duplicate
  local -a candidates=()
  local -n destination="${destination_name}"

  for ((attempt=1; attempt<=100; attempt++)); do
    read -r -a candidates <<<"$(zlink_e2e_reserve_ports "${count}")"
    if (( ${#candidates[@]} != count )); then
      printf 'Expected %s JVM E2E ports but allocated %s.\n' \
        "${count}" "${#candidates[@]}" >&2
      return 1
    fi
    duplicate=0
    for port in "${candidates[@]}"; do
      if [[ -n "${ZLINK_E2E_ISSUED_APP_PORTS[${port}]:-}" ]]; then
        duplicate=1
        break
      fi
    done
    if [[ "${duplicate}" == "1" ]]; then
      continue
    fi
    destination=("${candidates[@]}")
    for port in "${candidates[@]}"; do
      ZLINK_E2E_ISSUED_APP_PORTS["${port}"]=1
    done
    return 0
  done

  printf 'Unable to allocate %s previously unused JVM E2E ports.\n' "${count}" >&2
  return 1
}

zlink_e2e_reserve_mixed_endpoints() {
  local tcp_count="$1"
  local http_count="$2"
  local total=$((tcp_count + http_count))
  local index
  local -a ports=()

  read -r -a ports <<<"$(zlink_e2e_reserve_ports "${total}")"
  if (( ${#ports[@]} != total )); then
    printf 'Expected %s JVM E2E ports but allocated %s.\n' \
      "${total}" "${#ports[@]}" >&2
    return 1
  fi
  for ((index=0; index<tcp_count; index++)); do
    printf 'tcp://127.0.0.1:%s ' "${ports[$index]}"
  done
  for ((index=tcp_count; index<total; index++)); do
    printf 'http://127.0.0.1:%s ' "${ports[$index]}"
  done
  printf '\n'
}

zlink_e2e_gradle_build_locked() {
  local lock_path="/tmp/zlink-framework-java-kotlin-sample-gradle.lock"
  if ! command -v flock >/dev/null 2>&1; then
    echo 'flock is required to serialize Java and Kotlin Gradle builds.' >&2
    return 1
  fi
  flock --exclusive --close "${lock_path}" "$@"
}
