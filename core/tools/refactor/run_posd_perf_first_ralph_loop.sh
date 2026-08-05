#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
GUIDE_PATH="${SCRIPT_DIR}/core-system-posd-performance-first-ralph-guide.ko.md"
MASTER_PLAN_PATH="${GUIDE_PATH}"
LOGS_DIR="${SCRIPT_DIR}/logs"
MAX_ITERATIONS=100
POLL_SECONDS=30
STRESS_COUNT=10
GATE_LABEL="posd_perf_first_gate"
MODEL_ARG=(--model "gpt-5.4")
CHECK_EXISTING_SUPERVISOR=0
SUPERVISOR_PID=""
ARTIFACT_SNAPSHOT_FILE=""
EXTERNAL_ARTIFACTS_DIR=""
ARTIFACT_ARCHIVE_TAG=""

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Run the performance-first POSD Ralph loop from core/tools/refactor.
The default authority document is:
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
  --model MODEL         Override the default codex model
                        (default: gpt-5.4)
  --check-existing-supervisor
                        Refuse to start if the same supervisor is already running
  -h, --help            Show this help text

Examples:
  ./run_posd_perf_first_ralph_loop.sh
  ./run_posd_perf_first_ralph_loop.sh --max-iterations 30
  ./run_posd_perf_first_ralph_loop.sh --model gpt-5.4 --stress-count 20
EOF
}

matches_current_supervisor() {
  local line="$1"

  [[ "${line}" == *"--guide ${GUIDE_PATH}"* ]] && return 0
  [[ "${line}" == *"--master-plan ${MASTER_PLAN_PATH}"* ]] && return 0
  [[ "${line}" == *"--logs-dir ${LOGS_DIR}"* ]] && return 0
  [[ "${line}" == *"--gate-label ${GATE_LABEL}"* ]] && return 0

  return 1
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

collect_external_artifact_candidates() {
  local candidate_path

  find "${ROOT_DIR}" -type f \
    \( \
      -name '*.log' -o \
      -name '*.status' -o \
      -name '*.exitcode' -o \
      -name '*.summary' -o \
      -name '*last_message*.txt' -o \
      -name '*prompt*.txt' -o \
      -name 'hs_err_pid*.log' -o \
      -path '*/Testing/Temporary/*' \
    \) | while IFS= read -r candidate_path; do
      if [[ "${candidate_path}" == "${LOGS_DIR}"/* ]]; then
        continue
      fi
      printf '%s\n' "${candidate_path}"
    done | sort -u
}

prune_empty_parent_dirs() {
  local current_dir="$1"

  while [[ "${current_dir}" == "${ROOT_DIR}"/* ]] && [[ "${current_dir}" != "${ROOT_DIR}" ]]; do
    rmdir "${current_dir}" 2>/dev/null || break
    current_dir="$(dirname "${current_dir}")"
  done
}

capture_external_artifact_snapshot() {
  if [[ -z "${ARTIFACT_ARCHIVE_TAG}" ]]; then
    ARTIFACT_ARCHIVE_TAG="$(date '+%Y%m%d_%H%M%S')"
  fi
  ARTIFACT_SNAPSHOT_FILE="${LOGS_DIR}/.external_artifacts_before.lst"
  EXTERNAL_ARTIFACTS_DIR="${LOGS_DIR}/external-artifacts/${ARTIFACT_ARCHIVE_TAG}"
  collect_external_artifact_candidates > "${ARTIFACT_SNAPSHOT_FILE}"
}

relocate_new_external_artifacts() {
  local artifact_path
  local relative_path
  local destination_path
  local current_artifacts_file

  if [[ -z "${ARTIFACT_SNAPSHOT_FILE}" ]] || [[ ! -f "${ARTIFACT_SNAPSHOT_FILE}" ]]; then
    return 0
  fi

  current_artifacts_file="${LOGS_DIR}/.external_artifacts_after.lst"
  collect_external_artifact_candidates > "${current_artifacts_file}"

  while IFS= read -r artifact_path; do
    if [[ -z "${artifact_path}" ]] || [[ ! -f "${artifact_path}" ]]; then
      continue
    fi

    relative_path="${artifact_path#${ROOT_DIR}/}"
    destination_path="${EXTERNAL_ARTIFACTS_DIR}/${relative_path}"
    mkdir -p "$(dirname "${destination_path}")"
    mv "${artifact_path}" "${destination_path}"
    prune_empty_parent_dirs "$(dirname "${artifact_path}")"
  done < <(comm -13 "${ARTIFACT_SNAPSHOT_FILE}" "${current_artifacts_file}")

  rm -f "${current_artifacts_file}" "${ARTIFACT_SNAPSHOT_FILE}"
}

cleanup() {
  local exit_rc="${1:-$?}"

  trap - EXIT INT TERM HUP QUIT TSTP

  if [[ -n "${SUPERVISOR_PID}" ]] && kill -0 "${SUPERVISOR_PID}" 2>/dev/null; then
    terminate_process_tree "${SUPERVISOR_PID}"
    wait "${SUPERVISOR_PID}" 2>/dev/null || true
  fi

  relocate_new_external_artifacts

  exit "${exit_rc}"
}

handle_signal() {
  local signal_name="$1"
  local signal_rc="$2"

  echo "=== Ralph loop interrupted by ${signal_name}; cleaning up ===" >&2
  cleanup "${signal_rc}"
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

GUIDE_PATH="$(realpath -m "${GUIDE_PATH}")"
MASTER_PLAN_PATH="$(realpath -m "${MASTER_PLAN_PATH}")"
LOGS_DIR="$(realpath -m "${LOGS_DIR}")"

mkdir -p "${LOGS_DIR}"
capture_external_artifact_snapshot

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
    echo "Existing execution supervisor detected. Stop it before starting a new Ralph loop." >&2
    printf '%s\n' "${existing_supervisors[@]}" >&2
    exit 16
  fi
fi

echo "=== POSD performance-first Ralph loop start ==="
echo "Root: ${ROOT_DIR}"
echo "Guide: ${GUIDE_PATH}"
echo "Guide mode: single execution guide authority"
echo "Legacy secondary plan: ${MASTER_PLAN_PATH}"
echo "Logs dir: ${LOGS_DIR}"
echo "Gate label: ${GATE_LABEL}"
echo "Max iterations: ${MAX_ITERATIONS}"
echo "Stress count: ${STRESS_COUNT}"

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
