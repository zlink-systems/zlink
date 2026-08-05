#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/core/build"
LOGS_DIR="${ROOT_DIR}/core/tools/ralphloop/logs"
STRESS_COUNT=10
POLL_SECONDS=30
LABEL="phase2_thread_safe_stress"
NEXT_SUCCESS_CMD=""
OWNER_PID="$$"
FINAL_STATUS="aborted"
STRESS_PID=""
LOCK_ACQUIRED=0
ENFORCE_LOCK=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Run the thread-safe stress gate and keep the same shell process alive until it
either succeeds or produces a single-test repro log on failure.

Options:
  --build-dir PATH      CTest build directory (default: ${BUILD_DIR})
  --count N             Repeat count for the stress gate (default: ${STRESS_COUNT})
  --logs-dir PATH       Log directory (default: ${LOGS_DIR})
  --label NAME          Log file label prefix (default: ${LABEL})
  --poll-seconds N      Progress summary interval in seconds (default: ${POLL_SECONDS})
  --next-success-cmd CMD
                        Run this command after the gate succeeds
  --enforce-lock        Refuse to start if another gate loop with the same label is active
  -h, --help            Show this help text

Examples:
  $(basename "$0")
  $(basename "$0") --count 20
  $(basename "$0") --next-success-cmd "ctest --test-dir core/build --output-on-failure -R '^(test_gateway_with_handler|test_service_discovery)$'"
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --count)
      STRESS_COUNT="$2"
      shift 2
      ;;
    --logs-dir)
      LOGS_DIR="$2"
      shift 2
      ;;
    --label)
      LABEL="$2"
      shift 2
      ;;
    --poll-seconds)
      POLL_SECONDS="$2"
      shift 2
      ;;
    --next-success-cmd)
      NEXT_SUCCESS_CMD="$2"
      shift 2
      ;;
    --enforce-lock)
      ENFORCE_LOCK=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ ! -f "${BUILD_DIR}/CTestTestfile.cmake" ]]; then
  echo "Build directory does not look configured for CTest: ${BUILD_DIR}" >&2
  exit 1
fi

mkdir -p "${LOGS_DIR}"

timestamp="$(date '+%Y%m%d_%H%M%S')"
started_at="$(date '+%Y-%m-%d %H:%M:%S %z')"
stress_log="${LOGS_DIR}/${LABEL}_${timestamp}.log"
stress_exit="${stress_log}.exitcode"
stress_summary="${stress_log}.summary"
status_file="${LOGS_DIR}/${LABEL}.status"
lock_dir="${LOGS_DIR}/${LABEL}.lock"
lock_owner_file="${lock_dir}/owner"
stress_cmd="./core/tests/run_thread_safe_contract_stress.sh --build-dir ${BUILD_DIR} --count ${STRESS_COUNT}"

write_status() {
  local status_value="$1"
  local stage_value="${2:-stress}"
  {
    printf 'status=%s\n' "${status_value}"
    printf 'stage=%s\n' "${stage_value}"
    printf 'label=%s\n' "${LABEL}"
    printf 'owner_pid=%s\n' "${OWNER_PID}"
    printf 'started_at=%s\n' "${started_at}"
    printf 'updated_at=%s\n' "$(date '+%Y-%m-%d %H:%M:%S %z')"
    printf 'stress_log=%s\n' "${stress_log}"
    printf 'stress_exitcode=%s\n' "${stress_exit}"
    printf 'stress_summary=%s\n' "${stress_summary}"
    printf 'stress_command=%s\n' "${stress_cmd}"
    if [[ -n "${STRESS_PID}" ]]; then
      printf 'stress_pid=%s\n' "${STRESS_PID}"
    fi
  } > "${status_file}"
}

write_lock_owner() {
  {
    printf 'owner_pid=%s\n' "${OWNER_PID}"
    printf 'started_at=%s\n' "${started_at}"
    printf 'status_file=%s\n' "${status_file}"
  } > "${lock_owner_file}"
}

cleanup() {
  local exit_rc=$?
  if [[ "${FINAL_STATUS}" == "running" ]]; then
    if [[ -n "${STRESS_PID}" ]] && kill -0 "${STRESS_PID}" 2>/dev/null; then
      kill "${STRESS_PID}" 2>/dev/null || true
      wait "${STRESS_PID}" 2>/dev/null || true
    fi
    write_status "aborted" "cleanup"
  fi
  if [[ "${LOCK_ACQUIRED}" -eq 1 ]] && [[ -d "${lock_dir}" ]]; then
    rm -rf "${lock_dir}"
  fi
  exit "${exit_rc}"
}

trap cleanup EXIT INT TERM

if [[ "${ENFORCE_LOCK}" -eq 1 ]]; then
  for attempt in 1 2 3 4 5; do
    if mkdir "${lock_dir}" 2>/dev/null; then
      LOCK_ACQUIRED=1
      write_lock_owner
      break
    fi

    running_pid=""
    running_log=""
    running_status=""
    lock_owner_pid=""
    if [[ -f "${status_file}" ]]; then
      running_pid="$(sed -n 's/^owner_pid=//p' "${status_file}" | head -n 1)"
      running_log="$(sed -n 's/^stress_log=//p' "${status_file}" | head -n 1)"
      running_status="$(sed -n 's/^status=//p' "${status_file}" | head -n 1)"
    fi
    if [[ -f "${lock_owner_file}" ]]; then
      lock_owner_pid="$(sed -n 's/^owner_pid=//p' "${lock_owner_file}" | head -n 1)"
    fi

    if [[ "${running_status}" == "running" ]] && [[ -n "${running_pid}" ]] \
      && kill -0 "${running_pid}" 2>/dev/null; then
      echo "Another gate loop is already running (pid=${running_pid})." >&2
      if [[ -n "${running_log}" ]]; then
        echo "Existing log: ${running_log}" >&2
      fi
      exit 16
    fi

    if [[ -n "${lock_owner_pid}" ]] && kill -0 "${lock_owner_pid}" 2>/dev/null; then
      echo "Another gate loop is initializing or running (pid=${lock_owner_pid})." >&2
      exit 16
    fi

    if [[ -n "${lock_owner_pid}" ]] && ! kill -0 "${lock_owner_pid}" 2>/dev/null; then
      rm -rf "${lock_dir}"
      continue
    fi

    if [[ -z "${lock_owner_pid}" ]] && [[ -z "${running_pid}" ]] \
      && [[ "${running_status}" != "running" ]] && [[ "${attempt}" -ge 2 ]]; then
      rm -rf "${lock_dir}"
      continue
    fi

    sleep 1
  done

  if [[ "${LOCK_ACQUIRED}" -ne 1 ]]; then
    echo "Failed to acquire gate loop lock: ${lock_dir}" >&2
    exit 16
  fi
fi

echo "=== Gate loop start ==="
echo "Started: ${started_at}"
echo "Stress command: ${stress_cmd}"
echo "Stress log: ${stress_log}"
echo "Poll interval: ${POLL_SECONDS}s"
echo "Status file: ${status_file}"

bash -lc "${stress_cmd}" 2>&1 | tee "${stress_log}" &
stress_pid=$!
STRESS_PID="${stress_pid}"
FINAL_STATUS="running"
write_status "running" "stress"

last_size=-1
while kill -0 "${stress_pid}" 2>/dev/null; do
  sleep "${POLL_SECONDS}"
  if ! kill -0 "${stress_pid}" 2>/dev/null; then
    break
  fi
  current_size=$(wc -c < "${stress_log}" 2>/dev/null || echo 0)
  if [[ "${current_size}" -ne "${last_size}" ]]; then
    echo "=== Gate loop progress ($(date '+%Y-%m-%d %H:%M:%S %z')) ==="
    tail -n 20 "${stress_log}" || true
    last_size="${current_size}"
  else
    echo "=== Gate loop waiting ($(date '+%Y-%m-%d %H:%M:%S %z')) ==="
  fi
done

set +e
wait "${stress_pid}"
stress_rc=$?
set -e

printf '%s\n' "${stress_rc}" > "${stress_exit}"

if [[ "${stress_rc}" -eq 0 ]]; then
  write_status "running" "next_success"
  echo "=== Gate loop success ==="
  if [[ -n "${NEXT_SUCCESS_CMD}" ]]; then
    next_log="${LOGS_DIR}/${LABEL}_next_${timestamp}.log"
    next_exit="${next_log}.exitcode"
    echo "=== Running next success command ==="
    echo "Command: ${NEXT_SUCCESS_CMD}"
    echo "Log: ${next_log}"
    bash -lc "${NEXT_SUCCESS_CMD}" 2>&1 | tee "${next_log}"
    next_rc=${PIPESTATUS[0]}
    printf '%s\n' "${next_rc}" > "${next_exit}"
    echo "=== Next success command exit: ${next_rc} ==="
    if [[ "${next_rc}" -ne 0 ]]; then
      {
        echo "status=failure"
        echo "started_at=${started_at}"
        echo "completed_at=$(date '+%Y-%m-%d %H:%M:%S %z')"
        echo "stress_log=${stress_log}"
        echo "stress_exitcode=${stress_exit}"
        echo "next_log=${next_log}"
        echo "next_exitcode=${next_exit}"
      } > "${stress_summary}"
      FINAL_STATUS="failure"
      write_status "failure" "next_success"
      exit "${next_rc}"
    fi
  fi
  {
    echo "status=success"
    echo "started_at=${started_at}"
    echo "completed_at=$(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "stress_log=${stress_log}"
    echo "stress_exitcode=${stress_exit}"
  } > "${stress_summary}"
  echo "=== Final gate summary ==="
  cat "${stress_summary}"
  FINAL_STATUS="success"
  write_status "success" "done"
  exit 0
fi

write_status "running" "repro"
mapfile -t failed_tests < <(
  sed -n 's/^[[:space:]]*[0-9]\+[[:space:]]*-[[:space:]]\([^[:space:]]\+\)[[:space:]]*(.*)$/\1/p' \
    "${stress_log}" | awk '!seen[$0]++'
)

if [[ "${#failed_tests[@]}" -eq 0 ]]; then
  mapfile -t failed_tests < <(
    sed -n 's#^.*/core/tests/.*:FAIL: .*#unknown#p' "${stress_log}" | tail -n 1
  )
fi

{
  echo "status=failure"
  echo "started_at=${started_at}"
  echo "completed_at=$(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "stress_log=${stress_log}"
  echo "stress_exitcode=${stress_exit}"
  if [[ "${#failed_tests[@]}" -gt 0 ]]; then
    printf 'failed_test=%s\n' "${failed_tests[@]}"
  fi
} > "${stress_summary}"

echo "=== Gate loop failure ==="
cat "${stress_summary}"
echo "=== Stress failure tail ==="
tail -n 60 "${stress_log}" || true

if [[ "${#failed_tests[@]}" -ne 1 ]]; then
  echo "Skipping automatic single-test repro because failed test count is ${#failed_tests[@]}." >&2
  FINAL_STATUS="failure"
  write_status "failure" "done"
  exit "${stress_rc}"
fi

failed_test="${failed_tests[0]}"
if [[ "${failed_test}" = "unknown" ]]; then
  echo "Skipping automatic single-test repro because the failed test name could not be extracted." >&2
  FINAL_STATUS="failure"
  write_status "failure" "done"
  exit "${stress_rc}"
fi

repro_log="${LOGS_DIR}/${LABEL}_repro_${failed_test}_${timestamp}.log"
repro_exit="${repro_log}.exitcode"
repro_cmd="ctest --test-dir ${BUILD_DIR} --output-on-failure -R '^${failed_test}$'"

echo "=== Automatic single-test repro start ==="
echo "Repro command: ${repro_cmd}"
echo "Repro log: ${repro_log}"
bash -lc "${repro_cmd}" 2>&1 | tee "${repro_log}"
repro_rc=${PIPESTATUS[0]}
printf '%s\n' "${repro_rc}" > "${repro_exit}"
echo "=== Automatic single-test repro exit: ${repro_rc} ==="

{
  echo "repro_log=${repro_log}"
  echo "repro_exitcode=${repro_exit}"
  echo "repro_command=${repro_cmd}"
} >> "${stress_summary}"

FINAL_STATUS="failure"
write_status "failure" "done"
exit "${stress_rc}"
