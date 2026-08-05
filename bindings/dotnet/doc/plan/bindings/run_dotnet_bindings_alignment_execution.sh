#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
GUIDE_PATH="${SCRIPT_DIR}/dotnet-bindings-alignment-execution-guide.ko.md"
MASTER_PLAN_PATH="${SCRIPT_DIR}/dotnet-bindings-core-alignment-plan.ko.md"
SUPERVISOR_PATH="${ROOT_DIR}/core/tools/run_codex_execution_guide_loop.sh"
LOGS_DIR="${SCRIPT_DIR}/logs"
MAX_ITERATIONS=100
POLL_SECONDS=30
GATE_LABEL="dotnet_bindings_alignment_gate"
MODEL_ARG=()

require_non_negative_integer() {
  local value="$1"
  local name="$2"

  if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
    echo "${name} must be a non-negative integer: ${value}" >&2
    exit 1
  fi
}

require_non_empty_value() {
  local value="$1"
  local name="$2"

  if [[ -z "${value}" ]]; then
    echo "${name} must not be empty" >&2
    exit 1
  fi
}

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Run the .NET bindings core-alignment execution campaign.
The execution guide authority is:
  ${GUIDE_PATH}

The master plan authority is:
  ${MASTER_PLAN_PATH}

Options:
  --guide PATH          Override execution guide path
                        (default: ${GUIDE_PATH})
  --master-plan PATH    Override master plan path
                        (default: ${MASTER_PLAN_PATH})
  --logs-dir PATH       Override log directory
                        (default: ${LOGS_DIR})
  --max-iterations N    Maximum Codex supervisor iterations
                        (default: ${MAX_ITERATIONS})
  --poll-seconds N      Poll interval while a gate is running
                        (default: ${POLL_SECONDS})
  --gate-label NAME     Gate status label prefix
                        (default: ${GATE_LABEL})
  --model MODEL         Pass --model MODEL to codex exec
  -h, --help            Show this help text

Examples:
  ./run_dotnet_bindings_alignment_execution.sh
  ./run_dotnet_bindings_alignment_execution.sh --max-iterations 0
  ./run_dotnet_bindings_alignment_execution.sh --model gpt-5.4
EOF
}

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
    --gate-label)
      GATE_LABEL="$2"
      shift 2
      ;;
    --model)
      MODEL_ARG=(--model "$2")
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

require_non_negative_integer "${MAX_ITERATIONS}" "--max-iterations"
require_non_negative_integer "${POLL_SECONDS}" "--poll-seconds"
require_non_empty_value "${GUIDE_PATH}" "--guide"
require_non_empty_value "${MASTER_PLAN_PATH}" "--master-plan"
require_non_empty_value "${LOGS_DIR}" "--logs-dir"
require_non_empty_value "${GATE_LABEL}" "--gate-label"

if [[ ! -f "${GUIDE_PATH}" ]]; then
  echo "Execution guide not found: ${GUIDE_PATH}" >&2
  exit 1
fi

if [[ ! -f "${MASTER_PLAN_PATH}" ]]; then
  echo "Master plan not found: ${MASTER_PLAN_PATH}" >&2
  exit 1
fi

if [[ ! -x "${SUPERVISOR_PATH}" ]]; then
  echo "Supervisor script not executable: ${SUPERVISOR_PATH}" >&2
  exit 1
fi

mkdir -p "${LOGS_DIR}"

echo "=== .NET bindings alignment execution start ==="
echo "Root: ${ROOT_DIR}"
echo "Guide: ${GUIDE_PATH}"
echo "Master plan: ${MASTER_PLAN_PATH}"
echo "Logs dir: ${LOGS_DIR}"
echo "Gate label: ${GATE_LABEL}"
echo "Max iterations: ${MAX_ITERATIONS}"
echo "Execution lock: disabled"

cd "${ROOT_DIR}"

set +e
"${SUPERVISOR_PATH}" \
  --guide "${GUIDE_PATH}" \
  --master-plan "${MASTER_PLAN_PATH}" \
  --logs-dir "${LOGS_DIR}" \
  --max-iterations "${MAX_ITERATIONS}" \
  --poll-seconds "${POLL_SECONDS}" \
  --gate-label "${GATE_LABEL}" \
  "${MODEL_ARG[@]}"
rc=$?
set -e

if [[ "${MAX_ITERATIONS}" == "0" ]] && [[ "${rc}" -eq 3 ]]; then
  echo "Smoke check passed: supervisor invocation path is valid."
  exit 0
fi

exit "${rc}"
