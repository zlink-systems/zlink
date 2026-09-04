#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="${SCRIPT_DIR}/run_benchmarks_multi.sh"

usage() {
  cat <<'USAGE'
Usage: bindings/c/perf/ci_multi_smoke.sh [options]

Run the bounded multi-socket CI smoke matrix and require a throughput RESULT
for every requested pattern and message size.

Options:
  --ccu N, --clients N        Client count (default: CCU or 8)
  --dur N, --duration N       Active duration in seconds (default: DUR or 2)
  --sizes LIST, --msg-sizes LIST
                              Comma-separated message sizes
                              (default: SIZES or 4096,65536)
  --patterns LIST, --pattern LIST
                              Comma-separated patterns (default: PATTERNS or
                              DEALER_DEALER,DEALER_ROUTER_SENDSEND,PUBSUB)
  --timeout N                 Overall timeout in seconds (default: TIMEOUT or 300)
  -h, --help                  Show this help

The CCU, DUR, SIZES, PATTERNS, and TIMEOUT environment variables provide the
same defaults; command-line options take precedence.
USAGE
}

die() {
  echo "Error: $*" >&2
  exit 2
}

trim() {
  local value="${1:-}"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "${value}"
}

is_positive_decimal() {
  local value="${1:-}"
  local normalized="${value}"
  [[ "${value}" =~ ^[0-9]+$ ]] || return 1
  while [[ "${#normalized}" -gt 1 && "${normalized:0:1}" == "0" ]]; do
    normalized="${normalized:1}"
  done
  [[ "${normalized}" != "0" ]]
}

normalize_decimal() {
  local value="${1:-}"
  while [[ "${#value}" -gt 1 && "${value:0:1}" == "0" ]]; do
    value="${value:1}"
  done
  printf '%s' "${value}"
}

normalize_result_pattern() {
  local pattern
  pattern="$(trim "${1:-}")"
  pattern="$(printf '%s' "${pattern}" | tr '[:lower:]' '[:upper:]')"
  [[ "${pattern}" =~ ^[A-Z0-9_]+$ ]] || return 1
  pattern="${pattern#MULTI_}"
  case "${pattern}" in
    DEALER_ROUTER)
      pattern="DEALER_ROUTER_SENDSEND"
      ;;
    ROUTER_ROUTER)
      pattern="ROUTER_ROUTER_SENDSEND"
      ;;
    STREAMS)
      pattern="STREAM"
      ;;
  esac
  printf 'MULTI_%s' "${pattern}"
}

ccu="${CCU:-8}"
duration="${DUR:-2}"
sizes="${SIZES:-4096,65536}"
patterns="${PATTERNS:-DEALER_DEALER,DEALER_ROUTER_SENDSEND,PUBSUB}"
timeout_seconds="${TIMEOUT:-300}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --ccu|--clients)
      [[ $# -ge 2 ]] || die "$1 requires a value"
      ccu="$2"
      shift 2
      ;;
    --ccu=*|--clients=*)
      ccu="${1#*=}"
      shift
      ;;
    --dur|--duration)
      [[ $# -ge 2 ]] || die "$1 requires a value"
      duration="$2"
      shift 2
      ;;
    --dur=*|--duration=*)
      duration="${1#*=}"
      shift
      ;;
    --sizes|--msg-sizes)
      [[ $# -ge 2 ]] || die "$1 requires a value"
      sizes="$2"
      shift 2
      ;;
    --sizes=*|--msg-sizes=*)
      sizes="${1#*=}"
      shift
      ;;
    --patterns|--pattern)
      [[ $# -ge 2 ]] || die "$1 requires a value"
      patterns="$2"
      shift 2
      ;;
    --patterns=*|--pattern=*)
      patterns="${1#*=}"
      shift
      ;;
    --timeout)
      [[ $# -ge 2 ]] || die "$1 requires a value"
      timeout_seconds="$2"
      shift 2
      ;;
    --timeout=*)
      timeout_seconds="${1#*=}"
      shift
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

is_positive_decimal "${ccu}" || die "CCU must be a positive integer: ${ccu:-<empty>}"
is_positive_decimal "${duration}" || die "DUR must be a positive integer: ${duration:-<empty>}"
is_positive_decimal "${timeout_seconds}" \
  || die "TIMEOUT must be a positive integer: ${timeout_seconds:-<empty>}"
[[ -x "${RUNNER}" ]] || die "multi benchmark runner is not executable: ${RUNNER}"
command -v timeout >/dev/null 2>&1 || die "timeout is required"

IFS=',' read -r -a requested_sizes <<< "${sizes}"
[[ "${#requested_sizes[@]}" -gt 0 ]] || die "SIZES must not be empty"
expected_sizes=()
for raw_size in "${requested_sizes[@]}"; do
  size="$(trim "${raw_size}")"
  is_positive_decimal "${size}" || die "invalid message size: ${raw_size:-<empty>}"
  expected_sizes+=("$(normalize_decimal "${size}")")
done
sizes_csv="$(IFS=,; echo "${expected_sizes[*]}")"

IFS=',' read -r -a requested_patterns <<< "${patterns}"
[[ "${#requested_patterns[@]}" -gt 0 ]] || die "PATTERNS must not be empty"
expected_patterns=()
if [[ "${#requested_patterns[@]}" -eq 1 \
      && "$(printf '%s' "$(trim "${requested_patterns[0]}")" | tr '[:lower:]' '[:upper:]')" == "ALL" ]]; then
  expected_patterns=(
    MULTI_DEALER_DEALER
    MULTI_DEALER_ROUTER_SENDSEND
    MULTI_ROUTER_ROUTER_SENDSEND
    MULTI_DEALER_ROUTER_REQREP
    MULTI_ROUTER_ROUTER_REQREP
    MULTI_PUBSUB
    MULTI_STREAM
  )
else
  for raw_pattern in "${requested_patterns[@]}"; do
    if ! result_pattern="$(normalize_result_pattern "${raw_pattern}")"; then
      die "invalid pattern: ${raw_pattern:-<empty>}"
    fi
    expected_patterns+=("${result_pattern}")
  done
fi

log_file="$(mktemp -t zlink-c-multi-smoke.XXXXXX.log)"
trap 'rm -f "${log_file}"' EXIT

set +e
JOBS="${JOBS:-4}" CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-4}" \
  PERF_CLIENTS="${ccu}" PERF_DURATION_SECONDS="${duration}" \
  timeout "${timeout_seconds}" \
  "${RUNNER}" \
    --pattern "${patterns}" \
    --transports tcp \
    --msg-sizes "${sizes_csv}" \
    --runs 1 \
    2>&1 | tee "${log_file}"
pipeline_status=("${PIPESTATUS[@]}")
set -e

runner_status="${pipeline_status[0]}"
tee_status="${pipeline_status[1]}"
missing_cells=()
for result_pattern in "${expected_patterns[@]}"; do
  for size in "${expected_sizes[@]}"; do
    if ! awk -F',' -v pattern="${result_pattern}" -v size="${size}" '
      NF == 7 && $1 == "RESULT" && $2 == "current" && $3 == pattern &&
        $4 == "tcp" && $5 == size && $6 == "throughput" &&
        $7 ~ /^[-+]?[0-9]+([.][0-9]+)?([eE][-+]?[0-9]+)?$/ { found = 1 }
      END { exit(found ? 0 : 1) }
    ' "${log_file}"; then
      missing_cells+=("pattern=${result_pattern},transport=tcp,size=${size},metric=throughput")
    fi
  done
done

failed=0
if [[ "${runner_status}" -eq 124 ]]; then
  echo "Error: multi smoke timed out after ${timeout_seconds}s" >&2
  failed=1
elif [[ "${runner_status}" -ne 0 ]]; then
  echo "Error: multi benchmark runner exited with status ${runner_status}" >&2
  failed=1
fi
if [[ "${tee_status}" -ne 0 ]]; then
  echo "Error: failed to capture multi smoke output (tee status ${tee_status})" >&2
  failed=1
fi
if [[ "${#missing_cells[@]}" -gt 0 ]]; then
  echo "Missing throughput RESULT cells:" >&2
  printf '  - %s\n' "${missing_cells[@]}" >&2
  failed=1
fi

if [[ "${failed}" -ne 0 ]]; then
  exit 1
fi

echo "CI multi smoke complete: $(( ${#expected_patterns[@]} * ${#expected_sizes[@]} )) throughput cells found."
