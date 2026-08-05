#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ "${ZLINK_CORE_PACKAGE_PREFIX:-}" = /* ]] || {
  echo "ZLINK_CORE_PACKAGE_PREFIX must name the approved Core 11 install prefix" >&2
  exit 2
}
TASKS=(
  ":test"
  ":integrationTest"
  ":zlink-ext-netty:test"
  ":kotlin-samples:runAllKotlinSamples"
)
LOG_DIR="${ROOT_DIR}/build/test-runner-logs"

cd "${ROOT_DIR}"
mkdir -p "${LOG_DIR}"

failures=0
timeout_seconds=420

task_log_name() {
  local name="$1"
  name="${name#:}"
  name="${name//:/_}"
  name="${name//\//_}"
  printf '%s.log' "$name"
}

print_report_hints() {
  local task="$1"
  local report_dir=""
  local results_dir=""

  case "${task}" in
    ":test")
      report_dir="${ROOT_DIR}/build/reports/tests/test"
      results_dir="${ROOT_DIR}/build/test-results/test"
      ;;
    ":integrationTest")
      report_dir="${ROOT_DIR}/build/reports/tests/integrationTest"
      results_dir="${ROOT_DIR}/build/test-results/integrationTest"
      ;;
    ":zlink-ext-netty:test")
      report_dir="${ROOT_DIR}/codec/zlink-ext-netty/build/reports/tests/test"
      results_dir="${ROOT_DIR}/codec/zlink-ext-netty/build/test-results/test"
      ;;
    ":kotlin-samples:runAllKotlinSamples")
      report_dir="${ROOT_DIR}/../kotlin/samples/build/reports"
      results_dir="${ROOT_DIR}/../kotlin/samples/build/test-results"
      ;;
    "samples/run_samples.sh")
      report_dir="${ROOT_DIR}/samples/build/reports"
      results_dir="${ROOT_DIR}/samples/build/test-results"
      ;;
  esac

  if [[ -n "${report_dir}" ]]; then
    printf '[REPORT] %s\n' "${report_dir}"
  fi
  if [[ -n "${results_dir}" ]]; then
    printf '[JUNIT] %s\n' "${results_dir}"
  fi
}

run_task() {
  local task="$1"
  local log_file="$2"
  mkdir -p "$(dirname "${log_file}")"
  if command -v timeout >/dev/null 2>&1; then
    timeout "${timeout_seconds}s" ./gradlew "${task}" --no-daemon 2>&1 | tee "${log_file}"
    return $?
  fi
  ./gradlew "${task}" --no-daemon 2>&1 | tee "${log_file}"
}

for task in "${TASKS[@]}"; do
  log_file="${LOG_DIR}/$(task_log_name "${task}")"
  printf '[RUN] %s\n' "$task"
  if ! run_task "$task" "${log_file}"; then
    printf '[FAIL] %s\n' "$task"
    printf '[LOG] %s\n' "${log_file}"
    print_report_hints "${task}"
    failures=$((failures + 1))
  else
    printf '[PASS] %s\n' "$task"
    printf '[LOG] %s\n' "${log_file}"
  fi
done

sample_log="${LOG_DIR}/samples_run_samples.sh.log"
printf '[RUN] samples/run_samples.sh\n'
mkdir -p "$(dirname "${sample_log}")"
if ! "${ROOT_DIR}/samples/run_samples.sh" 2>&1 | tee "${sample_log}"; then
  printf '[FAIL] samples/run_samples.sh\n'
  printf '[LOG] %s\n' "${sample_log}"
  print_report_hints "samples/run_samples.sh"
  failures=$((failures + 1))
else
  printf '[PASS] samples/run_samples.sh\n'
  printf '[LOG] %s\n' "${sample_log}"
fi

if (( failures > 0 )); then
  printf 'Test summary: %d failed\n' "$failures"
  exit 1
fi

printf 'Test summary: all tasks and sample smoke passed\n'
