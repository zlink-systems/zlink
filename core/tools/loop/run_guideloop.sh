#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
LOG_FILTER="${SCRIPT_DIR}/filter_guideloop_log.awk"
LOGS_DIR=""
MODEL_ARG=()
REASONING_EFFORT=""
CURRENT_CODEX_PID=""

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] /abs/path/to/guide.manager.md

Run the guide loop repeatedly against an existing manager guide until the guide
reports completion or a blocked state.

Options:
  --logs-dir PATH       Directory for per-iteration logs.
                        (default: sibling 'logs/<stem>.manager/' next to the guide)
  --model MODEL         Pass --model MODEL to codex exec.
  --reasoning-effort E  Pass -c model_reasoning_effort=E to codex exec.
  -h, --help            Show this help text.

Termination:
  - exit 0 when the guide says:
      status: complete
      remaining_tasks: 0
      completion_verified: true
  - exit 2 when the guide says:
      status: blocked
      blocked_reason: <non-empty>
    or the final message starts with:
      사용자 입력 필요:
  - exit non-zero on wrapper or codex execution errors.
EOF
}

trim_whitespace() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "${value}"
}

require_file() {
  local path="$1"
  if [[ ! -f "${path}" ]]; then
    echo "Missing file: ${path}" >&2
    exit 1
  fi
}

list_descendants() {
  local root_pid="$1"
  local frontier children child
  frontier="${root_pid}"

  while [[ -n "${frontier}" ]]; do
    children=""
    while read -r child; do
      [[ -n "${child}" ]] || continue
      printf '%s\n' "${child}"
      children+="${child}"$'\n'
    done < <(
      ps -eo pid=,ppid= |
        awk -v parents="${frontier}" '
          BEGIN {
            split(parents, parent_lines, "\n")
            for (i in parent_lines) {
              if (parent_lines[i] != "") {
                want[parent_lines[i]] = 1
              }
            }
          }
          {
            pid = $1
            ppid = $2
            if (want[ppid]) {
              print pid
            }
          }'
    )
    frontier="${children}"
  done
}

kill_process_tree() {
  local root_pid="$1"
  local descendants descendant

  mapfile -t descendants < <(list_descendants "${root_pid}" | tac)
  for descendant in "${descendants[@]}"; do
    kill "${descendant}" 2>/dev/null || true
  done
  kill "${root_pid}" 2>/dev/null || true
}

cleanup_current_run() {
  if [[ -n "${CURRENT_CODEX_PID}" ]]; then
    kill_process_tree "${CURRENT_CODEX_PID}"
    CURRENT_CODEX_PID=""
  fi

  local child
  while read -r child; do
    [[ -n "${child}" ]] || continue
    kill_process_tree "${child}"
  done < <(ps -eo pid=,ppid= | awk -v parent="$$" '$2 == parent { print $1 }')
}

cleanup_stale_runs() {
  local guide_path="$1"
  local stale_pid
  local script_name
  script_name="$(basename "$0")"

  while read -r stale_pid; do
    [[ -n "${stale_pid}" ]] || continue
    if [[ "${stale_pid}" == "$$" ]]; then
      continue
    fi
    echo "Cleaning stale guide loop for guide: pid=${stale_pid}"
    kill_process_tree "${stale_pid}"
  done < <(
    ps -eo pid=,args= |
      awk -v self="$$" -v script_name="${script_name}" -v guide="${guide_path}" '
        index($0, script_name) && index($0, guide) {
          if ($1 != self) {
            print $1
          }
        }'
  )
}

read_guide_field() {
  local guide_path="$1"
  local field_name="$2"
  sed -n \
    -e "s/^- ${field_name}: //p" \
    -e "s/^${field_name}: //p" \
    "${guide_path}" | head -n 1
}

guide_stem_path() {
  local guide_path="$1"
  local no_md leaf dir
  if [[ "${guide_path}" != *.md ]]; then
    printf '%s' "${guide_path}"
    return 0
  fi

  no_md="${guide_path%.md}"
  leaf="$(basename "${no_md}")"
  dir="$(dirname "${guide_path}")"

  if [[ "${leaf}" == *.manager ]]; then
    leaf="${leaf%.manager}"
  fi

  printf '%s/%s' "${dir}" "${leaf}"
}

progress_path_for_guide() {
  local guide_path="$1"
  printf '%s.progress.md' "${guide_path%.manager.md}"
}

read_source_spec_path() {
  local guide_path="$1"
  sed -n \
    -e 's/^- source_spec_path: //p' \
    -e 's/^source_spec_path: //p' \
    "${guide_path}" | head -n 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --logs-dir)
      LOGS_DIR="$2"
      shift 2
      ;;
    --model)
      MODEL_ARG=(--model "$2")
      shift 2
      ;;
    --reasoning-effort)
      REASONING_EFFORT="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      break
      ;;
  esac
done

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 1
fi

GUIDE_PATH="$1"
if [[ "${GUIDE_PATH}" != /* ]]; then
  GUIDE_PATH="$(cd "$(dirname "${GUIDE_PATH}")" && pwd)/$(basename "${GUIDE_PATH}")"
fi

require_file "${GUIDE_PATH}"
require_file "${LOG_FILTER}"
PROGRESS_PATH="$(progress_path_for_guide "${GUIDE_PATH}")"
SOURCE_SPEC_PATH="$(trim_whitespace "$(read_source_spec_path "${GUIDE_PATH}")")"

trap cleanup_current_run EXIT INT TERM
cleanup_stale_runs "${GUIDE_PATH}"

if [[ -z "${LOGS_DIR}" ]]; then
  LOGS_DIR="$(dirname "${GUIDE_PATH}")/$(basename "$(guide_stem_path "${GUIDE_PATH}")").manager"
fi

mkdir -p "${LOGS_DIR}"

iteration=1

while true; do
  timestamp="$(date '+%Y%m%d_%H%M%S')"
  log_path="${LOGS_DIR}/guideloop_$(basename "$(guide_stem_path "${GUIDE_PATH}")")_${timestamp}_iter${iteration}.log"
  progress_note="${PROGRESS_PATH}"
  prompt_progress_clause="If ${PROGRESS_PATH} exists, use it as the live progress board. If it does not exist yet, continue from ${GUIDE_PATH} and create or refresh ${PROGRESS_PATH} only when needed."
  if [[ ! -f "${PROGRESS_PATH}" ]]; then
    progress_note="${PROGRESS_PATH} (missing)"
  fi

  echo "=== guideloop iteration ${iteration} ==="
  echo "guide: ${GUIDE_PATH}"
  echo "progress: ${progress_note}"
  echo "source spec: ${SOURCE_SPEC_PATH:-<missing>}"
  echo "log: ${log_path}"

  codex_args=(
    exec
    --dangerously-bypass-approvals-and-sandbox
    -C "${ROOT_DIR}"
  )

  if [[ ${#MODEL_ARG[@]} -gt 0 ]]; then
    codex_args+=("${MODEL_ARG[@]}")
  fi
  if [[ -n "${REASONING_EFFORT}" ]]; then
    codex_args+=(-c "model_reasoning_effort=${REASONING_EFFORT}")
  fi

  set +e
  codex "${codex_args[@]}" "Use ${GUIDE_PATH} as the primary execution control document. ${prompt_progress_clause} Resume from the current guide state instead of creating a new guide. Treat ${SOURCE_SPEC_PATH:-the source spec referenced by the guide} as reference authority only when the guide needs revalidation. Execute the next bounded manager action, update the guide/progress documents, and continue until the guide reports complete or blocked." \
    > >(awk -f "${LOG_FILTER}" | tee "${log_path}") \
    2>&1 &
  CURRENT_CODEX_PID=$!
  wait "${CURRENT_CODEX_PID}"
  codex_rc=$?
  CURRENT_CODEX_PID=""
  set -e

  if [[ "${codex_rc}" -ne 0 ]]; then
    echo "codex exec failed with exit code ${codex_rc}" >&2
    exit "${codex_rc}"
  fi

  if [[ ! -f "${GUIDE_PATH}" ]]; then
    echo "Guide document is missing: ${GUIDE_PATH}" >&2
    exit 1
  fi

  status="$(trim_whitespace "$(read_guide_field "${GUIDE_PATH}" "status")")"
  remaining_tasks="$(trim_whitespace "$(read_guide_field "${GUIDE_PATH}" "remaining_tasks")")"
  completion_verified="$(trim_whitespace "$(read_guide_field "${GUIDE_PATH}" "completion_verified")")"
  blocked_reason="$(trim_whitespace "$(read_guide_field "${GUIDE_PATH}" "blocked_reason")")"

  echo "guide status: ${status:-<missing>}"
  echo "guide remaining_tasks: ${remaining_tasks:-<missing>}"
  echo "guide completion_verified: ${completion_verified:-<missing>}"
  echo "guide blocked_reason: ${blocked_reason:-<empty>}"

  if [[ "${status}" == "complete" ]] \
    && [[ "${remaining_tasks}" == "0" ]] \
    && [[ "${completion_verified}" == "true" ]]; then
    echo "Guide loop completed."
    exit 0
  fi

  if [[ "${status}" == "blocked" ]]; then
    if [[ -n "${blocked_reason}" ]]; then
      echo "Guide loop blocked: ${blocked_reason}" >&2
    else
      echo "Guide loop blocked." >&2
    fi
    exit 2
  fi

  if grep -q '^사용자 입력 필요:' "${log_path}"; then
    echo "Guide loop requested user input." >&2
    exit 2
  fi

  iteration=$((iteration + 1))
done
