#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
GUIDE_PATH="${ROOT_DIR}/core/tools/refactor/core-system-posd-performance-first-ralph-guide.ko.md"
MASTER_PLAN_PATH="${GUIDE_PATH}"
LOGS_DIR="${ROOT_DIR}/core/tools/refactor/logs"
MAX_ITERATIONS=0
POLL_SECONDS=30
GATE_LABEL="phase2_thread_safe_stress"
STRESS_COUNT=1
INIT_ONLY=0
CHECK_EXISTING_SUPERVISOR=0
MODEL_ARG=()
REASONING_EFFORT=""
CURRENT_JOB_PID=""
DISPLAY_NAME="${RALPH_LOOP_DISPLAY_NAME:-$(basename "$0")}"
SUPERVISOR_LOCK_DIR=""
SESSION_SCOPE_ID=""
CODEX_ARGS=(
  exec
  --dangerously-bypass-approvals-and-sandbox
  -C "${ROOT_DIR}"
)

usage() {
  cat <<EOF
Usage: ${DISPLAY_NAME} [options]

Run Codex repeatedly against an execution guide until the guide is fully applied
or Codex reports that user input is required.

Options:
  --guide PATH          Execution guide path
                        (default: ${GUIDE_PATH})
  --master-plan PATH    Legacy compatibility path. New runs should keep a
                        single execution guide and leave this equal to --guide.
                        (default: ${MASTER_PLAN_PATH})
  --logs-dir PATH       Log directory
                        (default: ${LOGS_DIR})
  --max-iterations N    Maximum Codex iterations. Use 0 for unlimited.
                        (default: ${MAX_ITERATIONS})
  --init-only           Initialize session/log directories and exit without
                        starting Codex
  --poll-seconds N      Waiting interval while a long-running gate is active
                        (default: ${POLL_SECONDS})
  --gate-label NAME     Gate status label to watch
                        (default: ${GATE_LABEL})
  --stress-count N      Default repeat count to pass to run_execution_gate_loop.sh
                        (default: ${STRESS_COUNT})
  --check-existing-supervisor
                        Terminate matching existing supervisor/child runs and
                        acquire the shared supervisor lock before starting.
  --model MODEL         Pass --model MODEL to codex exec
  --reasoning-effort E  Pass -c model_reasoning_effort=E to codex exec
  -h, --help            Show this help text

Termination contract:
  - If the Codex final message is exactly '미적용 사항이 없습니다.' the loop exits 0.
  - If the Codex final message starts with '사용자 입력 필요:' the loop exits 2.
  - If the Codex final message is exactly '계속 진행 필요' the loop continues.
  - When --max-iterations is 0, the loop runs until one of the termination
    conditions above is met.
EOF
}

is_nonnegative_integer() {
  [[ "${1}" =~ ^[0-9]+$ ]]
}

trim_whitespace() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "${value}"
}

sanitize_scope_token() {
  local value="$1"
  value="$(printf '%s' "${value}" | tr '[:upper:]' '[:lower:]')"
  value="$(printf '%s' "${value}" | tr -cs 'a-z0-9._-' '_')"
  value="${value##_}"
  value="${value%%_}"
  if [[ -z "${value}" ]]; then
    value="default"
  fi
  printf '%s' "${value}"
}

load_supervisor_lock_field() {
  local file_path="$1"
  local key="$2"
  sed -n "s/^${key}=//p" "${file_path}" | head -n 1
}

release_supervisor_lock() {
  local owner_file
  local lock_pid

  if [[ -z "${SUPERVISOR_LOCK_DIR}" ]] || [[ ! -d "${SUPERVISOR_LOCK_DIR}" ]]; then
    return 0
  fi

  owner_file="${SUPERVISOR_LOCK_DIR}/owner"
  if [[ -f "${owner_file}" ]]; then
    lock_pid="$(load_supervisor_lock_field "${owner_file}" pid)"
    if [[ -n "${lock_pid}" ]] && [[ "${lock_pid}" != "$$" ]]; then
      return 0
    fi
  fi

  rm -rf "${SUPERVISOR_LOCK_DIR}"
}

acquire_supervisor_lock() {
  local owner_file
  local lock_pid
  local lock_guide
  local lock_started_at

  SUPERVISOR_LOCK_DIR="${LOGS_DIR}/.${GATE_LABEL}.supervisor.lock"
  owner_file="${SUPERVISOR_LOCK_DIR}/owner"

  while true; do
    if mkdir "${SUPERVISOR_LOCK_DIR}" 2>/dev/null; then
      cat > "${owner_file}" <<EOF
pid=$$
display_name=${DISPLAY_NAME}
guide=${GUIDE_PATH}
started_at=$(date '+%Y-%m-%d %H:%M:%S %z')
EOF
      return 0
    fi

    if [[ ! -f "${owner_file}" ]]; then
      echo "Supervisor lock exists without owner metadata: ${SUPERVISOR_LOCK_DIR}" >&2
      echo "Remove the stale lock directory or wait for the active loop to exit." >&2
      exit 1
    fi

    lock_pid="$(load_supervisor_lock_field "${owner_file}" pid)"
    lock_guide="$(load_supervisor_lock_field "${owner_file}" guide)"
    lock_started_at="$(load_supervisor_lock_field "${owner_file}" started_at)"

    if [[ -n "${lock_pid}" ]] && kill -0 "${lock_pid}" 2>/dev/null; then
      echo "=== Existing supervisor lock owner detected; terminating it first ==="
      echo "Lock: ${SUPERVISOR_LOCK_DIR}"
      echo "Owner pid: ${lock_pid}"
      if [[ -n "${lock_started_at}" ]]; then
        echo "Started at: ${lock_started_at}"
      fi
      if [[ -n "${lock_guide}" ]]; then
        echo "Guide: ${lock_guide}"
      fi
      terminate_supervisor_run "${lock_pid}"
      rm -rf "${SUPERVISOR_LOCK_DIR}"
      continue
    fi

    echo "=== Detected stale supervisor lock; releasing it ==="
    rm -rf "${SUPERVISOR_LOCK_DIR}"
  done
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

terminate_process_group_if_safe() {
  local pid="$1"
  local pgid

  if [[ -z "${pid}" ]] || ! kill -0 "${pid}" 2>/dev/null; then
    return 0
  fi

  pgid="$(ps -o pgid= -p "${pid}" 2>/dev/null | head -n 1 || true)"
  pgid="$(trim_whitespace "${pgid}")"
  if [[ -z "${pgid}" ]]; then
    return 0
  fi

  kill -CONT -- "-${pgid}" 2>/dev/null || true
  kill -- "-${pgid}" 2>/dev/null || true
  sleep 0.2
  kill -KILL -- "-${pgid}" 2>/dev/null || true
}

terminate_supervisor_run() {
  local pid="$1"

  if [[ -z "${pid}" ]]; then
    return 0
  fi

  terminate_process_group_if_safe "${pid}"
  terminate_process_tree "${pid}"
}

terminate_existing_supervisors() {
  local ps_line
  local pid
  local cmd

  while IFS= read -r ps_line; do
    [[ -z "${ps_line}" ]] && continue
    ps_line="$(trim_whitespace "${ps_line}")"
    pid="${ps_line%% *}"
    cmd="${ps_line#* }"

    if [[ -z "${pid}" ]] || [[ "${pid}" == "$$" ]]; then
      continue
    fi
    if [[ "${cmd}" != *"run_codex_execution_guide_loop.sh"* ]]; then
      continue
    fi
    if [[ "${cmd}" != *"--guide ${GUIDE_PATH}"* ]]; then
      continue
    fi
    if [[ "${cmd}" != *"--logs-dir ${LOGS_DIR}"* ]]; then
      continue
    fi
    if [[ "${cmd}" != *"--gate-label ${GATE_LABEL}"* ]]; then
      continue
    fi

    echo "=== Existing Ralph loop supervisor detected; terminating it first ==="
    echo "PID: ${pid}"
    echo "CMD: ${cmd}"
    terminate_supervisor_run "${pid}"
  done < <(ps -eo pid=,args=)
}

terminate_existing_codex_children() {
  local ps_line
  local pid
  local cmd

  while IFS= read -r ps_line; do
    [[ -z "${ps_line}" ]] && continue
    ps_line="$(trim_whitespace "${ps_line}")"
    pid="${ps_line%% *}"
    cmd="${ps_line#* }"

    if [[ -z "${pid}" ]] || [[ "${pid}" == "$$" ]]; then
      continue
    fi
    if [[ "${cmd}" != *"codex exec"* ]]; then
      continue
    fi
    if [[ "${cmd}" != *"${LOGS_DIR}/codex_execution_guide_loop_${SESSION_SCOPE_ID}_"* ]]; then
      continue
    fi

    echo "=== Existing loop child process detected; terminating it first ==="
    echo "PID: ${pid}"
    echo "CMD: ${cmd}"
    terminate_supervisor_run "${pid}"
  done < <(ps -eo pid=,args=)
}

cleanup() {
  local exit_rc="${1:-$?}"

  trap - EXIT INT TERM HUP QUIT TSTP

  if [[ -n "${CURRENT_JOB_PID}" ]] && kill -0 "${CURRENT_JOB_PID}" 2>/dev/null; then
    terminate_supervisor_run "${CURRENT_JOB_PID}"
    wait "${CURRENT_JOB_PID}" 2>/dev/null || true
  fi

  release_supervisor_lock

  exit "${exit_rc}"
}

handle_signal() {
  local signal_name="$1"
  local signal_rc="$2"

  echo "=== Codex execution guide loop interrupted by ${signal_name}; cleaning up ===" >&2
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
    --init-only)
      INIT_ONLY=1
      shift
      ;;
    --poll-seconds)
      POLL_SECONDS="$2"
      shift 2
      ;;
    --gate-label)
      GATE_LABEL="$2"
      shift 2
      ;;
    --stress-count)
      STRESS_COUNT="$2"
      shift 2
      ;;
    --check-existing-supervisor)
      CHECK_EXISTING_SUPERVISOR=1
      shift
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
if ! is_nonnegative_integer "${MAX_ITERATIONS}"; then
  echo "--max-iterations must be a non-negative integer: ${MAX_ITERATIONS}" >&2
  exit 1
fi

REASONING_ARG=()
if [[ -n "${REASONING_EFFORT}" ]]; then
  REASONING_ARG=(-c "model_reasoning_effort=\"${REASONING_EFFORT}\"")
fi

SESSION_SCOPE_ID="$(sanitize_scope_token "${DISPLAY_NAME}_${GATE_LABEL}")"

find_previous_session_dir() {
  local logs_dir="$1"
  local scope_id="$2"
  local current_dir="$3"
  find "${logs_dir}" -maxdepth 1 -mindepth 1 -type d \
    -name "codex_execution_guide_loop_${scope_id}_*" ! -path "${current_dir}" \
    | sort | tail -n 1
}

initialize_session_artifacts() {
  local session_dir="$1"
  local previous_session_dir="$2"
  local run_state_file="${session_dir}/00_run_state.md"
  local checklist_file="${session_dir}/00_checklist.md"
  local notes_file="${session_dir}/00_notes.md"
  local handoff_file="${session_dir}/00_handoff.md"

  cat > "${run_state_file}" <<EOF
# Run State

- session_dir: ${session_dir}
- previous_session_dir: ${previous_session_dir:-none}
- status: in_progress
- active_language: <cpp|dotnet|java|rust|go|node|python>
- active_mode: <single|multi recv|multi callback|normal-operation repair>
- current_focus: <current highest-priority work>
- current_issue: <current failure, divergence, or bottleneck>
- worst_ratio: <ratio and key, or pending if full comparable not ready>
- comparable_report: <latest comparable report path or pending>
- current_action: <what is being changed or verified now>
- next_action: <immediate next step>
- blocker: <blocked reason or none>
- latest_verification: <latest command and outcome>
- changed_files: <high-signal files only or none>
EOF

  cat > "${checklist_file}" <<'EOF'
# Checklist

- [ ] previous session unresolved items reviewed
- [ ] doc/perf forbidden handshake/start-gate wording removed from session files and current interpretation
- [ ] policy-compliant measurement method confirmed against core/perf
- [ ] full-surface normal operation verified across all patterns and sizes
- [ ] required metrics populated (throughput, latency, CPU, memory, queue where required)
- [ ] current focus selected
- [ ] code or document change applied
- [ ] verification run completed
- [ ] results logged
- [ ] perf results saved under bindings/<lang>/perf/results
- [ ] remaining work / blocker updated
EOF

  cat > "${notes_file}" <<EOF
# Notes

- Keep high-signal findings, commands, report paths, and decisions here.
- Use this file for run-local notes instead of writing transient progress into the execution guide.
EOF

  cat > "${handoff_file}" <<EOF
# Handoff

- previous_session_dir: ${previous_session_dir:-none}
- instruction:
  - Review the previous session's run state, checklist, notes, prompt, and run log first.
  - If unresolved work remains, carry it forward here before starting new work.
EOF
}

mkdir -p "${LOGS_DIR}"

if [[ "${INIT_ONLY}" == "1" ]]; then
  timestamp="$(date '+%Y%m%d_%H%M%S')"
  session_dir="${LOGS_DIR}/codex_execution_guide_loop_${SESSION_SCOPE_ID}_${timestamp}"
  previous_session_dir="$(find_previous_session_dir "${LOGS_DIR}" "${SESSION_SCOPE_ID}" "${session_dir}")"
  gate_dir="${session_dir}/gate"
  gate_status_file="${gate_dir}/${GATE_LABEL}.status"
  mkdir -p "${session_dir}"
  mkdir -p "${gate_dir}"
  initialize_session_artifacts "${session_dir}" "${previous_session_dir}"

  cat <<EOF
=== Codex execution guide loop start ===
Guide: ${GUIDE_PATH}
Legacy secondary plan: ${MASTER_PLAN_PATH}
Session dir: ${session_dir}
Previous session dir: ${previous_session_dir:-none}
Gate dir: ${gate_dir}
Supervisor lock: init-only skipped
Max iterations: 0
Gate status file: ${gate_status_file}
Stress count: ${STRESS_COUNT}
=== Init-only requested; exiting without starting Codex ===
EOF
  exit 0
fi

if [[ "${CHECK_EXISTING_SUPERVISOR}" == "1" ]]; then
  terminate_existing_supervisors
  terminate_existing_codex_children
  acquire_supervisor_lock
fi

timestamp="$(date '+%Y%m%d_%H%M%S')"
session_dir="${LOGS_DIR}/codex_execution_guide_loop_${SESSION_SCOPE_ID}_${timestamp}"
previous_session_dir="$(find_previous_session_dir "${LOGS_DIR}" "${SESSION_SCOPE_ID}" "${session_dir}")"
gate_dir="${session_dir}/gate"
gate_status_file="${gate_dir}/${GATE_LABEL}.status"
mkdir -p "${session_dir}"
mkdir -p "${gate_dir}"
initialize_session_artifacts "${session_dir}" "${previous_session_dir}"
run_state_file="${session_dir}/00_run_state.md"
checklist_file="${session_dir}/00_checklist.md"
notes_file="${session_dir}/00_notes.md"
handoff_file="${session_dir}/00_handoff.md"

max_iterations_display="${MAX_ITERATIONS}"
if [[ "${MAX_ITERATIONS}" == "0" ]]; then
  max_iterations_display="unlimited"
fi

cat <<EOF
=== Codex execution guide loop start ===
Guide: ${GUIDE_PATH}
Legacy secondary plan: ${MASTER_PLAN_PATH}
Session dir: ${session_dir}
Previous session dir: ${previous_session_dir:-none}
Gate dir: ${gate_dir}
Supervisor lock: ${SUPERVISOR_LOCK_DIR:-disabled}
Max iterations: ${max_iterations_display}
Gate status file: ${gate_status_file}
Stress count: ${STRESS_COUNT}
Run state file: ${run_state_file}
Checklist file: ${checklist_file}
Notes file: ${notes_file}
Handoff file: ${handoff_file}
EOF

load_gate_field() {
  local file_path="$1"
  local key="$2"
  sed -n "s/^${key}=//p" "${file_path}" | head -n 1
}

wait_for_running_gate() {
  local gate_status
  local gate_stage
  local gate_owner_pid
  local gate_log

  gate_status="$(load_gate_field "${gate_status_file}" status)"
  gate_stage="$(load_gate_field "${gate_status_file}" stage)"
  gate_owner_pid="$(load_gate_field "${gate_status_file}" owner_pid)"
  gate_log="$(load_gate_field "${gate_status_file}" stress_log)"

  if [[ "${gate_status}" != "running" ]]; then
    return 1
  fi

  if [[ -z "${gate_owner_pid}" ]] || ! kill -0 "${gate_owner_pid}" 2>/dev/null; then
    echo "=== Detected stale running gate status; releasing it ==="
    {
      printf 'status=stale\n'
      printf 'label=%s\n' "${GATE_LABEL}"
      printf 'updated_at=%s\n' "$(date '+%Y-%m-%d %H:%M:%S %z')"
      if [[ -n "${gate_log}" ]]; then
        printf 'stress_log=%s\n' "${gate_log}"
      fi
    } > "${gate_status_file}"
    rm -rf "${gate_dir}/${GATE_LABEL}.lock"
    return 1
  fi

  echo "=== Long-running gate active; skipping new Codex iteration ==="
  echo "Owner pid: ${gate_owner_pid}"
  if [[ -n "${gate_stage}" ]]; then
    echo "Gate stage: ${gate_stage}"
  fi
  if [[ -n "${gate_log}" ]]; then
    echo "Gate log: ${gate_log}"
  fi

  while kill -0 "${gate_owner_pid}" 2>/dev/null; do
    sleep "${POLL_SECONDS}"
    if ! kill -0 "${gate_owner_pid}" 2>/dev/null; then
      break
    fi
    echo "=== Waiting for gate owner pid ${gate_owner_pid} ($(date '+%Y-%m-%d %H:%M:%S %z')) ==="
    if [[ -n "${gate_log}" ]] && [[ -f "${gate_log}" ]]; then
      tail -n 20 "${gate_log}" || true
    fi
  done

  echo "=== Long-running gate finished; Codex iterations may resume ==="
  return 0
}

iteration=1
while true; do
  if [[ "${MAX_ITERATIONS}" != "0" ]] && (( iteration > MAX_ITERATIONS )); then
    break
  fi

  if [[ -f "${gate_status_file}" ]]; then
    if wait_for_running_gate; then
      continue
    fi
  fi

  iter_prefix="$(printf '%02d' "${iteration}")"
  prompt_file="${session_dir}/${iter_prefix}_prompt.txt"
  run_log="${session_dir}/${iter_prefix}_codex.log"
  last_message="${session_dir}/${iter_prefix}_last_message.txt"

  legacy_plan_rules=""
  if [[ "${MASTER_PLAN_PATH}" != "${GUIDE_PATH}" ]]; then
    legacy_plan_rules=$(cat <<EOF
- 별도 보조 계획 문서가 입력으로 주어져도 현재 실행의 유일한 기준 문서는 실행 가이드다.
- ${MASTER_PLAN_PATH} 는 legacy 참고 입력으로만 취급한다. 필요한 구현 intent가 남아 있으면 먼저 실행 가이드에 흡수한 뒤, 이후에는 실행 가이드만 기준으로 진행한다.
EOF
)
  fi

  cat > "${prompt_file}" <<'EOF'
/home/hep7/project/kairos/zlink/AGENTS.md 지침과 저장소 규칙을 따른다.

작업 목표:
- __GUIDE_PATH__
  이 문서의 남은 작업과 실제 구현 내용을 중단 없이 끝까지 진행한다.

작업 규칙:
- 현재 실행의 유일한 기준 문서는 실행 가이드다.
- 이 실행은 단일 문서 체계로 운영한다. 명시 요청이 없으면 main/master/gap/spec/residual/보조 계획 문서를 새로 만들지 않는다.
- 실행 가이드는 정책과 완료 기준의 source of truth다.
- 현재 session dir은 __SESSION_DIR__ 이다.
- session dir 아래 실행 상태 파일을 기본 산출물로 사용한다.
  - run state: __RUN_STATE_FILE__
  - checklist: __CHECKLIST_FILE__
  - notes: __NOTES_FILE__
  - handoff: __HANDOFF_FILE__
- 이 session dir 산출물은 실행 로그/상태 기록용이며, 별도 계획 문서 체계로 취급하지 않는다.
- 이전 session dir은 __PREVIOUS_SESSION_DIR__ 이다.
- substantive work를 시작하기 전에 이전 session dir의 미완료 사항과 handoff를 먼저 확인하고, unresolved work가 있으면 현재 session dir 파일에 이어받는다.
- 실행 가이드와 별도 계획 문서로 분산돼 있던 내용이 보이면 실행 가이드로 먼저 합친다.
- 별도 보조 계획서를 유지하거나 참조 체인을 늘리는 방식으로 작업을 진행하지 않는다.
__LEGACY_PLAN_RULES__
- 각 작업 묶음을 끝낼 때마다 실행 가이드 전체를 다시 훑고 아직 코드에 반영되지 않은 구현 항목이 남아 있는지 확인한다.
- 각 작업 묶음을 시작할 때와 끝낼 때마다 관련 상위 정책 문서(예: doc/perf, spec policy)를 다시 확인하고 현재 변경이 그 규칙을 계속 준수하는지 검토한다.
- 매 턴 시작 시와 종료 직전에도 현재 변경이 상위 정책 문서를 계속 준수하는지 다시 확인한다.
- perf 수정 중 정책 위반을 발견하면 성능 최적화보다 먼저 규칙 준수 상태로 수정한다.
- perf 작업에서는 먼저 대상 surface가 정책 준수 상태로 전체 패턴/전체 사이즈에서 정상 동작하는지 확보하고, 그 다음에만 성능 개선을 진행한다.
- perf 작업의 iteration 목표는 먼저 "정책을 만족하며 정상 동작하는 perf" 를 만드는 것이고, 성능 개선은 그 목표를 만족한 뒤에만 진행한다.
- 정상 동작은 throughput만 찍히는 상태가 아니라, 해당 surface에서 기대되는 latency, CPU, memory, queue 지표가 전체 패턴/전체 사이즈에서 빠짐없이 채워지는 상태를 뜻한다.
- `core/perf` 와 동일한 측정 방식을 유지해야 하며, 방식이 다르면 그 숫자와 판단은 무효로 취급하고, 먼저 perf를 `core/perf` 와 동일한 방식으로 수정한다.
- 정책 위반 수정, `core/perf` 방식 정합성 수정, benchmark 자체 버그 수정이 아니면 perf 를 수정하지 않는다.
- `doc/perf` 가 금지하는 handshake/start gate/ready 판정 방식이 session 상태 파일, handoff, notes, 구현, 로그 해석에 보이면 그 표현과 구현을 먼저 바로잡는다.
- 예를 들어 `READY,...`, `CLIENT_READY,...`, `START,...` 같은 orchestration 문자열을 delivery-ready 근거나 benchmark start gate로 서술하거나 사용하면 정책 위반으로 보고 즉시 수정한다.
- 이전 session 파일이나 현재 session 파일에 그런 정책 위반 표현이 남아 있으면, 그 표현을 먼저 정리한 뒤에만 다음 실험이나 구현을 진행한다.
- 이런 정책 위반이 남아 있는 동안에는 다음 실험, 다음 패턴, 다음 언어로 진행하지 않는다.
- perf 측정은 항상 해당 바인딩의 `perf/results` 아래 report가 남도록 실행하고, 결과 파일 경로를 session 상태 파일에 기록한다.
- 매 턴마다 `00_run_state.md` 의 핵심 필드(active language, mode, current issue, worst ratio, current action, next action, latest verification)를 최신 상태로 갱신한다.
- 정책 문서가 금지하는 의미 변경, I/O model 변경, workaround가 보이면 진행하지 말고 blocker나 bug 후보로 승격한다.
- 특정 binding/perf surface에서 보이는 실패만으로 core bug라고 단정하지 않는다.
- 먼저 binding 코드, FFI/integration layer, perf runner semantics를 충분히 확인하고, 같은 실패를 `core/tests/` 저장소 재현으로 옮긴 뒤에만 core bug로 분류한다.
- core bug 후보로 막히면 멈추기 전에 한글 bug report를 먼저 작성하고, 그 절대 경로와 파일명을 session 상태 파일과 최종 blocker 메시지에 남긴다.
- session checklist가 green이어도 실제 구현 내용이 아직 덜 반영됐으면 완료로 처리하지 않는다.
- 실행 가이드와 현재 코드가 어긋나면 실행 가이드를 먼저 고치고 그 다음 코드를 진행한다.
- 실행 가이드의 현재 파일 내용이 source of truth다. 명시적으로 요청받지 않은 한 실행 가이드 자체의 git diff나 삭제된 legacy 문서 diff를 근거로 판단하지 않는다.
- git status 에서 보이는 삭제된 legacy 문서나 과거 계획 문서는 현재 실행 범위의 근거가 아니다. unrelated 변경처럼 취급하고 현재 가이드와 현재 코드만 본다.
- 현재 작업 우선순위는 session handoff, run state, checklist의 미완료 항목을 먼저 따른다.
- session 상태 파일에 명시 우선순위가 없으면 그때 실행 가이드의 첫 미완료 정책 항목부터 진행한다.
- core 버그 수정 요청 범위는 core/ 와 core/tests/ 로 제한한다.
- core/build/ 만 사용한다.
- 장시간 gate가 필요하면 ./core/tools/ralphloop/run_execution_gate_loop.sh --logs-dir __GATE_DIR__ --label __GATE_LABEL__ --count __STRESS_COUNT__ 를 최소 기준으로 사용해 같은 셸 프로세스에서 끝까지 추적한다.
- flake 재현, 신뢰도 보강, 추가 확인이 필요하다고 판단하면 thread-safe stress count를 __STRESS_COUNT__보다 더 크게 올릴 수 있다.
- 장시간 gate 실패 시 문서 규칙대로 단일 재현, core 수정, 재빌드, 원래 gate 재실행까지 처리한다.
- 실행 상태표, 체크리스트, 실행 메모는 session dir 파일에 갱신한다.
- 실행 가이드는 정책, 범위, 완료 기준이 바뀔 때만 수정한다.
- 가이드에 단계별 commit / push 규칙이 있으면 그대로 따른다.
- unrelated 변경은 commit/push에 섞지 않는다. 현재 단계 범위만 안전하게 commit할 수 없으면 완료로 닫지 않는다.
- push한 commit hash를 문서의 검증 증거 또는 진행 메모에 남길 수 있으면 남긴다.
- routine한 판단은 사용자에게 묻지 말고 스스로 진행한다.
- 정말 필요한 사용자 결정이 아니면 멈추지 않는다.
- stress/lane/perf/functional regression은 구현 완료를 증명하는 보조 수단이지 구현 내용 자체를 대체하지 않는다.
- 테스트 통과만으로 실행 가이드의 실제 구현 내용이 반영됐다고 추정하지 않는다.

종료 판정 규칙:
- 실행 가이드 기준으로 더 이상 미적용 사항이 없고 다음에 할 작업이 전혀 없을 때만 정확히 아래 한 줄만 출력한다.
미적용 사항이 없습니다.

- 사용자 결정 없이는 더 진행할 수 없는 blocker가 있을 때만 정확히 아래 형식 한 줄만 출력한다.
사용자 입력 필요: <한 줄 이유>

- 그 외에는 이번 iteration 안에서 할 수 있는 작업을 최대한 수행한 뒤 정확히 아래 한 줄만 출력한다.
계속 진행 필요
EOF
  GUIDE_PATH_ENV="${GUIDE_PATH}" \
  SESSION_DIR_ENV="${session_dir}" \
  RUN_STATE_FILE_ENV="${run_state_file}" \
  CHECKLIST_FILE_ENV="${checklist_file}" \
  NOTES_FILE_ENV="${notes_file}" \
  HANDOFF_FILE_ENV="${handoff_file}" \
  PREVIOUS_SESSION_DIR_ENV="${previous_session_dir:-none}" \
  LEGACY_PLAN_RULES_ENV="${legacy_plan_rules}" \
  GATE_DIR_ENV="${gate_dir}" \
  GATE_LABEL_ENV="${GATE_LABEL}" \
  STRESS_COUNT_ENV="${STRESS_COUNT}" \
  python3 - "${prompt_file}" <<'PY'
import os
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
replacements = {
    "__GUIDE_PATH__": os.environ["GUIDE_PATH_ENV"],
    "__SESSION_DIR__": os.environ["SESSION_DIR_ENV"],
    "__RUN_STATE_FILE__": os.environ["RUN_STATE_FILE_ENV"],
    "__CHECKLIST_FILE__": os.environ["CHECKLIST_FILE_ENV"],
    "__NOTES_FILE__": os.environ["NOTES_FILE_ENV"],
    "__HANDOFF_FILE__": os.environ["HANDOFF_FILE_ENV"],
    "__PREVIOUS_SESSION_DIR__": os.environ["PREVIOUS_SESSION_DIR_ENV"],
    "__LEGACY_PLAN_RULES__": os.environ["LEGACY_PLAN_RULES_ENV"],
    "__GATE_DIR__": os.environ["GATE_DIR_ENV"],
    "__GATE_LABEL__": os.environ["GATE_LABEL_ENV"],
    "__STRESS_COUNT__": os.environ["STRESS_COUNT_ENV"],
}
for key, value in replacements.items():
    text = text.replace(key, value)
path.write_text(text, encoding="utf-8")
PY
  if [[ "${MAX_ITERATIONS}" == "0" ]]; then
    iteration_label="${iteration}/unlimited"
  else
    iteration_label="${iteration}/${MAX_ITERATIONS}"
  fi

  echo "=== Codex iteration ${iteration_label} start ($(date '+%Y-%m-%d %H:%M:%S %z')) ==="
  set +e
  codex "${CODEX_ARGS[@]}" "${MODEL_ARG[@]}" "${REASONING_ARG[@]}" \
    -o "${last_message}" \
    - < "${prompt_file}" 2>&1 | tee "${run_log}" &
  CURRENT_JOB_PID=$!
  wait "${CURRENT_JOB_PID}"
  codex_rc=$?
  CURRENT_JOB_PID=""
  set -e

  if [[ "${codex_rc}" -ne 0 ]]; then
    echo "Codex exec failed on iteration ${iteration} with exit code ${codex_rc}." >&2
    echo "Run log: ${run_log}" >&2
    exit "${codex_rc}"
  fi

  if [[ ! -f "${last_message}" ]]; then
    echo "Codex did not write the final message file: ${last_message}" >&2
    exit 1
  fi

  final_message="$(tr -d '\r' < "${last_message}")"
  echo "=== Codex iteration ${iteration} final message ==="
  printf '%s\n' "${final_message}"

  if [[ "${final_message}" == "미적용 사항이 없습니다." ]]; then
    echo "=== Codex execution guide loop complete ==="
    exit 0
  fi

  if [[ "${final_message}" == 사용자\ 입력\ 필요:* ]]; then
    echo "=== Codex execution guide loop blocked ==="
    exit 2
  fi

  if [[ "${final_message}" != "계속 진행 필요" ]]; then
    echo "Unexpected Codex final message on iteration ${iteration}." >&2
    echo "Expected one of: 미적용 사항이 없습니다. / 계속 진행 필요 / 사용자 입력 필요: ..." >&2
    echo "Run log: ${run_log}" >&2
    exit 1
  fi

  iteration=$((iteration + 1))
done

echo "Reached max iterations without completion: ${MAX_ITERATIONS}" >&2
exit 3
