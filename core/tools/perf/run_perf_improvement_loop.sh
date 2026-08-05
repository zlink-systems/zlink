#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
GUIDE_PATH="${SCRIPT_DIR}/core-perf-optimization-execution-guide.ko.md"
MASTER_PLAN_PATH="${GUIDE_PATH}"
RECV_BASELINE_PATH="${SCRIPT_DIR}/perf_linux_recv_20260323_094627.txt"
CALLBACK_BASELINE_PATH="${SCRIPT_DIR}/perf_linux_callback_20260323_082648.txt"
LOGS_DIR="${SCRIPT_DIR}/logs"
MAX_ITERATIONS=100
POLL_SECONDS=30
STRESS_COUNT=1
GATE_LABEL="perf_improvement_gate"
MODEL_ARG=(--model "gpt-5.4")
PERF_LOG_STALE_SECONDS=90
CHECK_EXISTING_SUPERVISOR=0
SUPERVISOR_PID=""

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Run the perf-improvement Ralph loop from core/tools/perf.
The loop authority document is:
  ${GUIDE_PATH}

Options:
  --guide PATH          Override execution guide path
                        (default: ${GUIDE_PATH})
  --master-plan PATH    Legacy compatibility path passed to the supervisor.
                        New runs should keep this equal to --guide.
                        (default: ${MASTER_PLAN_PATH})
  --logs-dir PATH       Override log directory
                        (default: ${LOGS_DIR})
  --max-iterations N    Maximum Codex supervisor iterations
                        (default: ${MAX_ITERATIONS})
  --poll-seconds N      Poll interval while a gate is running
                        (default: ${POLL_SECONDS})
  --stress-count N      Pass-through gate repeat count
                        (default: ${STRESS_COUNT})
  --gate-label NAME     Gate status label prefix
                        (default: ${GATE_LABEL})
  --perf-log-stale-seconds N
                        Recent-log freshness window used to confirm
                        that an existing perf run is truly active
                        (default: ${PERF_LOG_STALE_SECONDS})
  --model MODEL         Override the default codex model
                        (default: gpt-5.4)
  --check-existing-supervisor
                        Refuse to start if the same supervisor is already running
  -h, --help            Show this help text

Examples:
  ./run_perf_improvement_loop.sh
  ./run_perf_improvement_loop.sh --model gpt-5.4 --max-iterations 30
EOF
}

cleanup() {
  local exit_rc="${1:-$?}"

  trap - EXIT INT TERM HUP QUIT TSTP

  if [[ -n "${SUPERVISOR_PID}" ]] && kill -0 "${SUPERVISOR_PID}" 2>/dev/null; then
    terminate_process_tree "${SUPERVISOR_PID}"
    wait "${SUPERVISOR_PID}" 2>/dev/null || true
  fi

  exit "${exit_rc}"
}

terminate_process_tree() {
  local pid="$1"
  local child_pid
  local attempt
  local child_pids=()

  if [[ -z "${pid}" ]] || ! kill -0 "${pid}" 2>/dev/null; then
    return 0
  fi

  mapfile -t child_pids < <(pgrep -P "${pid}" || true)
  for child_pid in "${child_pids[@]}"; do
    terminate_process_tree "${child_pid}"
  done

  kill -CONT "${pid}" 2>/dev/null || true
  kill "${pid}" 2>/dev/null || true
  for attempt in 1 2 3 4 5; do
    if ! kill -0 "${pid}" 2>/dev/null; then
      return 0
    fi
    sleep 0.2
  done
  kill -KILL "${pid}" 2>/dev/null || true
}

handle_signal() {
  local signal_name="$1"
  local signal_rc="$2"

  echo "=== Perf loop interrupted by ${signal_name}; cleaning up ===" >&2
  cleanup "${signal_rc}"
}

matches_current_supervisor() {
  local line="$1"

  [[ "${line}" == *"--guide ${GUIDE_PATH}"* ]] && return 0
  [[ "${line}" == *"--master-plan ${MASTER_PLAN_PATH}"* ]] && return 0
  [[ "${line}" == *"--logs-dir ${LOGS_DIR}"* ]] && return 0
  [[ "${line}" == *"--gate-label ${GATE_LABEL}"* ]] && return 0

  return 1
}

trap cleanup EXIT
trap 'handle_signal INT 130' INT
trap 'handle_signal TERM 143' TERM
trap 'handle_signal HUP 129' HUP
trap 'handle_signal QUIT 131' QUIT
trap 'handle_signal TSTP 148' TSTP

while [[ $# -gt 0 ]]; do
  case "$1" in
    --guide)
      GUIDE_PATH="$2"
      shift 2
      ;;
    --master-plan)
      MASTER_PLAN_PATH="$2"
      shift 2
      ;;
    --logs-dir)
      LOGS_DIR="$2"
      shift 2
      ;;
    --max-iterations)
      MAX_ITERATIONS="$2"
      shift 2
      ;;
    --poll-seconds)
      POLL_SECONDS="$2"
      shift 2
      ;;
    --stress-count)
      STRESS_COUNT="$2"
      shift 2
      ;;
    --gate-label)
      GATE_LABEL="$2"
      shift 2
      ;;
    --perf-log-stale-seconds)
      PERF_LOG_STALE_SECONDS="$2"
      shift 2
      ;;
    --model)
      MODEL_ARG=(--model "$2")
      shift 2
      ;;
    --check-existing-supervisor)
      CHECK_EXISTING_SUPERVISOR=1
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

if [[ ! -f "${GUIDE_PATH}" ]]; then
  echo "Execution guide not found: ${GUIDE_PATH}" >&2
  exit 1
fi

if [[ ! -f "${MASTER_PLAN_PATH}" ]]; then
  echo "Master plan not found: ${MASTER_PLAN_PATH}" >&2
  exit 1
fi

if [[ ! -f "${RECV_BASELINE_PATH}" ]]; then
  echo "Recv baseline not found: ${RECV_BASELINE_PATH}" >&2
  exit 1
fi

if [[ ! -f "${CALLBACK_BASELINE_PATH}" ]]; then
  echo "Callback baseline not found: ${CALLBACK_BASELINE_PATH}" >&2
  exit 1
fi

GUIDE_PATH="$(realpath -m "${GUIDE_PATH}")"
MASTER_PLAN_PATH="$(realpath -m "${MASTER_PLAN_PATH}")"
LOGS_DIR="$(realpath -m "${LOGS_DIR}")"

mkdir -p "${LOGS_DIR}"

find_running_perf_processes() {
  pgrep -af "${ROOT_DIR}/core/perf/run_benchmarks(_multi)?\\.sh|${ROOT_DIR}/core/perf/(single/)?run_comparison\\.py" || true
}

collect_running_perf_pids() {
  local lines=""
  lines="$(find_running_perf_processes)"
  if [[ -z "${lines}" ]]; then
    return 0
  fi

  local line=""
  while IFS= read -r line; do
    [[ -z "${line}" ]] && continue
    awk '{print $1}' <<< "${line}"
  done <<< "${lines}"
}

terminate_stale_perf_processes() {
  local pids=()
  local pid=""
  while IFS= read -r pid; do
    [[ -z "${pid}" ]] && continue
    pids+=("${pid}")
  done < <(collect_running_perf_pids)

  if [[ "${#pids[@]}" -eq 0 ]]; then
    return 0
  fi

  echo "Stopping stale perf processes: ${pids[*]}"
  kill "${pids[@]}" 2>/dev/null || true
  sleep 2

  local alive_pids=()
  for pid in "${pids[@]}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      alive_pids+=("${pid}")
    fi
  done

  if [[ "${#alive_pids[@]}" -gt 0 ]]; then
    echo "Force-killing stale perf processes: ${alive_pids[*]}"
    kill -9 "${alive_pids[@]}" 2>/dev/null || true
  fi
}

find_recent_perf_log() {
  local newest_log=""
  local newest_epoch=0
  local file=""
  shopt -s nullglob
  for file in "${LOGS_DIR}"/*.log; do
    if [[ ! -f "${file}" ]]; then
      continue
    fi
    local modified_epoch
    modified_epoch="$(stat -c '%Y' "${file}" 2>/dev/null || echo 0)"
    if (( modified_epoch > newest_epoch )); then
      newest_epoch="${modified_epoch}"
      newest_log="${file}"
    fi
  done
  shopt -u nullglob

  if [[ -z "${newest_log}" ]]; then
    return 1
  fi

  local now_epoch
  now_epoch="$(date +%s)"
  if (( now_epoch - newest_epoch > PERF_LOG_STALE_SECONDS )); then
    return 1
  fi

  printf '%s\n' "${newest_log}"
}

wait_for_active_perf_logging() {
  local running_perf
  running_perf="$(find_running_perf_processes)"
  if [[ -z "${running_perf}" ]]; then
    return 0
  fi

  local log_path=""
  if ! log_path="$(find_recent_perf_log)"; then
    echo "Perf-related processes exist, but no recent log activity was found under ${LOGS_DIR}."
    echo "Proceeding without blocking because active file output was not confirmed."
    return 0
  fi

  local last_size=0
  local last_mtime=0
  last_size="$(stat -c '%s' "${log_path}" 2>/dev/null || echo 0)"
  last_mtime="$(stat -c '%Y' "${log_path}" 2>/dev/null || echo 0)"

  echo "Active perf run detected; waiting for it to finish."
  echo "Perf processes:"
  printf '%s\n' "${running_perf}"
  echo "Observed log: ${log_path}"

  while true; do
    sleep "${POLL_SECONDS}"

    running_perf="$(find_running_perf_processes)"
    if [[ -z "${running_perf}" ]]; then
      echo "Perf run finished; resuming perf improvement loop."
      return 0
    fi

    local current_size
    local current_mtime
    current_size="$(stat -c '%s' "${log_path}" 2>/dev/null || echo 0)"
    current_mtime="$(stat -c '%Y' "${log_path}" 2>/dev/null || echo 0)"

    if [[ "${current_size}" != "${last_size}" ]] || [[ "${current_mtime}" != "${last_mtime}" ]]; then
      echo "Perf log is still updating ($(date '+%Y-%m-%d %H:%M:%S %z'))."
      tail -n 20 "${log_path}" || true
      last_size="${current_size}"
      last_mtime="${current_mtime}"
      continue
    fi

    echo "Perf processes remain, but ${log_path} is not changing."
    echo "Treating current perf run as stale, terminating it, and continuing."
    terminate_stale_perf_processes
    return 0
  done
}

wait_for_active_perf_logging

if [[ "${CHECK_EXISTING_SUPERVISOR}" -eq 1 ]]; then
  mapfile -t existing_supervisors < <(
    pgrep -af "${ROOT_DIR}/core/tools/ralphloop/run_codex_execution_guide_loop.sh" | \
      while IFS= read -r line; do
        [[ -z "${line}" ]] && continue
        if [[ "${line%% *}" == "$$" ]]; then
          continue
        fi
        if matches_current_supervisor "${line}"; then
          printf '%s\n' "${line}"
        fi
      done
  )

  if [[ "${#existing_supervisors[@]}" -gt 0 ]]; then
    echo "Existing execution supervisor detected. Stop it before starting a new perf loop." >&2
    printf '%s\n' "${existing_supervisors[@]}" >&2
    exit 16
  fi
fi

echo "=== Perf improvement loop start ==="
echo "Root: ${ROOT_DIR}"
echo "Guide: ${GUIDE_PATH}"
echo "Guide mode: single execution guide authority"
echo "Legacy secondary plan: ${MASTER_PLAN_PATH}"
echo "Recv baseline: ${RECV_BASELINE_PATH}"
echo "Callback baseline: ${CALLBACK_BASELINE_PATH}"
echo "Logs dir: ${LOGS_DIR}"
echo "Gate label: ${GATE_LABEL}"
echo "Max iterations: ${MAX_ITERATIONS}"

cd "${ROOT_DIR}"

"${ROOT_DIR}/core/tools/ralphloop/run_codex_execution_guide_loop.sh" \
  --guide "${GUIDE_PATH}" \
  --master-plan "${MASTER_PLAN_PATH}" \
  --logs-dir "${LOGS_DIR}" \
  --max-iterations "${MAX_ITERATIONS}" \
  --poll-seconds "${POLL_SECONDS}" \
  --gate-label "${GATE_LABEL}" \
  --stress-count "${STRESS_COUNT}" \
  "${MODEL_ARG[@]}" &
SUPERVISOR_PID=$!

set +e
wait "${SUPERVISOR_PID}"
supervisor_rc=$?
set -e

SUPERVISOR_PID=""
exit "${supervisor_rc}"
