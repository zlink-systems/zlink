#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SHARED_RALPH_DIR="${ROOT_DIR}/tools/ralphloop"
GUIDE_PATH="${SCRIPT_DIR}/bindings-perf-execution-guide.ko.md"
LOGS_DIR="${SCRIPT_DIR}/logs"
BASELINE_DIR="${ROOT_DIR}/perf/baseline"
BASELINE_FILE="${BINDINGS_PERF_BASELINE_FILE:-}"
BASELINE_RECV_FILE="${BINDINGS_PERF_BASELINE_RECV_FILE:-}"
BASELINE_CALLBACK_FILE="${BINDINGS_PERF_BASELINE_CALLBACK_FILE:-}"
DEFAULT_LANGUAGES="cpp,dotnet,java,rust,go,node,python"
DEFAULT_TARGET_CPP="0.95"
DEFAULT_TARGET_DOTNET="0.90"
DEFAULT_TARGET_GO="0.85"
DEFAULT_TARGET_JAVA="0.90"
DEFAULT_TARGET_NODE="0.75"
DEFAULT_TARGET_PYTHON="0.75"
DEFAULT_TARGET_RUST="0.95"

ENV_LANGUAGES="${BINDINGS_PERF_LANGUAGES:-}"
LANGUAGES=""
LANGUAGE_FILTER_SET=0
MAX_ITERATIONS="0"
POLL_SECONDS="30"
STRESS_COUNT="1"
MODEL=""
REASONING_EFFORT=""
INIT_ONLY=0
CHECK_EXISTING_SUPERVISOR=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Run the bindings perf Ralph loop against the shared execution-guide supervisor.

Options:
  --language NAME[,NAME...]
                        Restrict to one or more languages.
                        May be passed multiple times.
                        Preserves the specified order and treats it as the
                        required execution order.
                        Default: BINDINGS_PERF_LANGUAGES if set,
                        otherwise all supported languages
  --logs-dir PATH       Log directory
                        (default: ${LOGS_DIR})
  --baseline-dir PATH   Core baseline report directory
                        (default: ${BASELINE_DIR})
  --baseline-file PATH  Legacy alias for callback baseline report file
  --baseline-recv-file PATH
                        Explicit recv baseline report file
  --baseline-callback-file PATH
                        Explicit callback baseline report file
  --target-ratio LANG=R
                        Override target throughput ratio for one language.
                        May be passed multiple times.
  --max-iterations N    Forwarded to shared Ralph supervisor
                        (default: ${MAX_ITERATIONS})
  --poll-seconds N      Forwarded to shared Ralph supervisor
                        (default: ${POLL_SECONDS})
  --stress-count N      Forwarded to shared Ralph supervisor
                        (default: ${STRESS_COUNT})
  --model MODEL         Forwarded to shared Ralph supervisor
  --reasoning-effort E  Forwarded to shared Ralph supervisor
  --init-only           Initialize session/log directories and exit
  --check-existing-supervisor
                        Terminate a matching existing Ralph supervisor first
  -h, --help            Show this help

Supported languages:
  cpp, dotnet, java, rust, go, node, python
EOF
}

trim() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "${value}"
}

is_supported_language() {
  case "$1" in
    cpp|dotnet|go|java|node|python|rust) return 0 ;;
    *) return 1 ;;
  esac
}

append_languages() {
  local raw="$1"
  local token
  LANGUAGE_FILTER_SET=1
  IFS=',' read -r -a parts <<< "${raw}"
  for token in "${parts[@]}"; do
    token="$(trim "${token}")"
    [[ -z "${token}" ]] && continue
    if ! is_supported_language "${token}"; then
      echo "Unsupported language: ${token}" >&2
      exit 1
    fi
    if [[ -z "${LANGUAGES}" ]]; then
      LANGUAGES="${token}"
    elif [[ ",${LANGUAGES}," != *",${token},"* ]]; then
      LANGUAGES="${LANGUAGES},${token}"
    fi
  done
}

set_target_ratio() {
  local spec="$1"
  local lang="${spec%%=*}"
  local ratio="${spec#*=}"
  lang="$(trim "${lang}")"
  ratio="$(trim "${ratio}")"

  if [[ -z "${lang}" || -z "${ratio}" || "${lang}" == "${ratio}" ]]; then
    echo "Invalid --target-ratio value: ${spec}" >&2
    exit 1
  fi
  if ! is_supported_language "${lang}"; then
    echo "Unsupported language in --target-ratio: ${lang}" >&2
    exit 1
  fi
  if [[ ! "${ratio}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "Invalid ratio for ${lang}: ${ratio}" >&2
    exit 1
  fi

  local env_name="BINDINGS_PERF_TARGET_${lang^^}"
  export "${env_name}=${ratio}"
}

resolve_baseline_file() {
  local dir="$1"
  local explicit="$2"
  local mode="$3"
  local candidate

  if [[ -n "${explicit}" ]]; then
    if [[ ! -f "${explicit}" ]]; then
      echo "Baseline file not found: ${explicit}" >&2
      exit 1
    fi
    printf '%s' "${explicit}"
    return 0
  fi

  if [[ "${mode}" == "recv" ]]; then
    candidate="$(find "${dir}" -maxdepth 1 -type f -name 'perf_*recv*.txt' | sort | tail -n 1)"
    if [[ -n "${candidate}" ]]; then
      printf '%s' "${candidate}"
      return 0
    fi

    candidate="$(find "${dir}" -maxdepth 1 -type f -name 'perf_*.txt' ! -name '*callback*' | sort | tail -n 1)"
    if [[ -n "${candidate}" ]]; then
      printf '%s' "${candidate}"
      return 0
    fi
  else
    candidate="$(find "${dir}" -maxdepth 1 -type f -name 'perf_*callback*.txt' | sort | tail -n 1)"
    if [[ -n "${candidate}" ]]; then
      printf '%s' "${candidate}"
      return 0
    fi
  fi

  echo "No ${mode} baseline report file found in: ${dir}" >&2
  exit 1
}

check_language_runner() {
  local lang="$1"
  local runner="${ROOT_DIR}/../bindings/${lang}/perf/run_benchmarks.sh"

  if [[ ! -f "${runner}" ]]; then
    echo "Missing perf runner for ${lang}: ${runner}" >&2
    return 1
  fi

  if [[ ! -x "${runner}" ]]; then
    echo "Perf runner is not executable for ${lang}: ${runner}" >&2
    return 1
  fi

  return 0
}

run_preflight_checks() {
  local lang
  local failed=0
  local token
  IFS=',' read -r -a langs <<< "${LANGUAGES}"
  for token in "${langs[@]}"; do
    lang="$(trim "${token}")"
    [[ -z "${lang}" ]] && continue
    if ! check_language_runner "${lang}"; then
      failed=1
    fi
  done

  if [[ "${failed}" != "0" ]]; then
    echo "Perf runner preflight failed." >&2
    exit 1
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --language)
      append_languages "$2"
      shift 2
      ;;
    --logs-dir)
      LOGS_DIR="$2"
      shift 2
      ;;
    --baseline-dir)
      BASELINE_DIR="$2"
      shift 2
      ;;
    --baseline-file)
      BASELINE_FILE="$2"
      shift 2
      ;;
    --baseline-recv-file)
      BASELINE_RECV_FILE="$2"
      shift 2
      ;;
    --baseline-callback-file)
      BASELINE_CALLBACK_FILE="$2"
      shift 2
      ;;
    --target-ratio)
      set_target_ratio "$2"
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
    --model)
      MODEL="$2"
      shift 2
      ;;
    --reasoning-effort)
      REASONING_EFFORT="$2"
      shift 2
      ;;
    --init-only)
      INIT_ONLY=1
      shift
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

if [[ "${LANGUAGE_FILTER_SET}" == "0" ]] && [[ -n "${ENV_LANGUAGES}" ]]; then
  LANGUAGES="${ENV_LANGUAGES}"
fi

if [[ -z "${LANGUAGES}" ]]; then
  LANGUAGES="${DEFAULT_LANGUAGES}"
fi

if [[ ! -d "${BASELINE_DIR}" ]]; then
  echo "Baseline directory not found: ${BASELINE_DIR}" >&2
  exit 1
fi

if [[ -n "${BASELINE_FILE}" && -z "${BASELINE_CALLBACK_FILE}" ]]; then
  BASELINE_CALLBACK_FILE="${BASELINE_FILE}"
fi

BASELINE_RECV_FILE="$(resolve_baseline_file "${BASELINE_DIR}" "${BASELINE_RECV_FILE}" recv)"
BASELINE_CALLBACK_FILE="$(resolve_baseline_file "${BASELINE_DIR}" "${BASELINE_CALLBACK_FILE}" callback)"
BASELINE_FILE="${BASELINE_CALLBACK_FILE}"

mkdir -p "${LOGS_DIR}"

if [[ "${INIT_ONLY}" != "1" ]]; then
  run_preflight_checks
fi

export BINDINGS_PERF_LANGUAGES="${LANGUAGES}"
export BINDINGS_PERF_BASELINE_DIR="${BASELINE_DIR}"
export BINDINGS_PERF_BASELINE_FILE="${BASELINE_FILE}"
export BINDINGS_PERF_BASELINE_RECV_FILE="${BASELINE_RECV_FILE}"
export BINDINGS_PERF_BASELINE_CALLBACK_FILE="${BASELINE_CALLBACK_FILE}"
export BINDINGS_PERF_TARGET_CPP="${BINDINGS_PERF_TARGET_CPP:-${DEFAULT_TARGET_CPP}}"
export BINDINGS_PERF_TARGET_DOTNET="${BINDINGS_PERF_TARGET_DOTNET:-${DEFAULT_TARGET_DOTNET}}"
export BINDINGS_PERF_TARGET_GO="${BINDINGS_PERF_TARGET_GO:-${DEFAULT_TARGET_GO}}"
export BINDINGS_PERF_TARGET_JAVA="${BINDINGS_PERF_TARGET_JAVA:-${DEFAULT_TARGET_JAVA}}"
export BINDINGS_PERF_TARGET_NODE="${BINDINGS_PERF_TARGET_NODE:-${DEFAULT_TARGET_NODE}}"
export BINDINGS_PERF_TARGET_PYTHON="${BINDINGS_PERF_TARGET_PYTHON:-${DEFAULT_TARGET_PYTHON}}"
export BINDINGS_PERF_TARGET_RUST="${BINDINGS_PERF_TARGET_RUST:-${DEFAULT_TARGET_RUST}}"

CMD=(
  "${SHARED_RALPH_DIR}/run_codex_execution_guide_loop.sh"
  --guide "${GUIDE_PATH}"
  --master-plan "${GUIDE_PATH}"
  --logs-dir "${LOGS_DIR}"
  --max-iterations "${MAX_ITERATIONS}"
  --poll-seconds "${POLL_SECONDS}"
  --gate-label "bindings_perf"
  --stress-count "${STRESS_COUNT}"
)

if [[ "${INIT_ONLY}" == "1" ]]; then
  CMD+=(--init-only)
fi
if [[ -n "${MODEL}" ]]; then
  CMD+=(--model "${MODEL}")
fi
if [[ -n "${REASONING_EFFORT}" ]]; then
  CMD+=(--reasoning-effort "${REASONING_EFFORT}")
fi
if [[ "${CHECK_EXISTING_SUPERVISOR}" == "1" ]]; then
  CMD+=(--check-existing-supervisor)
fi

echo "=== Bindings perf Ralph loop ==="
echo "Guide: ${GUIDE_PATH}"
echo "Logs: ${LOGS_DIR}"
echo "Baseline: ${BASELINE_DIR}"
echo "Recv baseline file: ${BASELINE_RECV_FILE}"
echo "Callback baseline file: ${BASELINE_CALLBACK_FILE}"
echo "Languages: ${LANGUAGES}"
echo "Targets: cpp=${BINDINGS_PERF_TARGET_CPP}, dotnet=${BINDINGS_PERF_TARGET_DOTNET}, go=${BINDINGS_PERF_TARGET_GO}, java=${BINDINGS_PERF_TARGET_JAVA}, node=${BINDINGS_PERF_TARGET_NODE}, python=${BINDINGS_PERF_TARGET_PYTHON}, rust=${BINDINGS_PERF_TARGET_RUST}"
echo "Priority: 1) policy-compliant core/perf-equivalent measurement, 2) full-surface normal operation across all patterns/sizes, 3) performance improvement"
python3 "${SCRIPT_DIR}/summarize_bindings_perf.py" \
  --languages "${LANGUAGES}" \
  --baseline-dir "${BASELINE_DIR}" \
  --baseline-recv-file "${BASELINE_RECV_FILE}" \
  --baseline-callback-file "${BASELINE_CALLBACK_FILE}"

RALPH_LOOP_DISPLAY_NAME="bindings_perf_ralph_loop" "${CMD[@]}"
