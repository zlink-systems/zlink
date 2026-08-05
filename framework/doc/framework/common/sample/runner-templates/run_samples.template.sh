#!/usr/bin/env bash
set -euo pipefail

# Aggregate sample runner template.
# It delegates Redis ownership and cleanup to each selected sample runner.
# Expected layout:
#   samples/redis-common.sh
#   samples/run_samples.sh
#   samples/<language>/<Sample>/run_sample.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAX_ATTEMPTS="${ZLINK_SAMPLE_RETRY_ATTEMPTS:-3}"
BIND_RETRY_PATTERN="Address already in use|EADDRINUSE|already bound|errno=98"

SAMPLE_RUNNERS=(
  java/ExampleSampleA/run_sample.sh
  kotlin/ExampleSampleB/run_sample.sh
)

SELECTED_RUNNERS=()

if [[ "$#" -eq 0 ]]; then
  SELECTED_RUNNERS=("${SAMPLE_RUNNERS[@]}")
else
  for selector in "$@"; do
    matched=0
    for runner in "${SAMPLE_RUNNERS[@]}"; do
      sample_dir="$(basename "$(dirname "${runner}")")"
      runner_key="${runner%/run_sample.sh}"
      if [[ "${selector}" == "${runner}" || "${selector}" == "${runner_key}" || "${selector}" == "${sample_dir}" ]]; then
        SELECTED_RUNNERS+=("${runner}")
        matched=1
      fi
    done
    if [[ "${matched}" == "0" ]]; then
      echo "Unknown sample selector '${selector}'." >&2
      exit 2
    fi
  done
fi

run_sample_with_retry() {
  local runner="$1"
  local attempt output status
  output="$(mktemp)"

  for attempt in $(seq 1 "${MAX_ATTEMPTS}"); do
    : >"${output}"
    set +e
    "${SCRIPT_DIR}/${runner}" 2>&1 | tee "${output}"
    status="${PIPESTATUS[0]}"
    set -e

    if [[ "${status}" == "0" ]]; then
      rm -f "${output}"
      return 0
    fi

    if ! grep -Eq "${BIND_RETRY_PATTERN}" "${output}"; then
      rm -f "${output}"
      return "${status}"
    fi

    if [[ "${attempt}" == "${MAX_ATTEMPTS}" ]]; then
      rm -f "${output}"
      return "${status}"
    fi

    echo "sample transient bind failure; retrying ${runner} (${attempt}/${MAX_ATTEMPTS})" >&2
    sleep 1
  done
}

for runner in "${SELECTED_RUNNERS[@]}"; do
  run_sample_with_retry "${runner}"
done

echo "sample all result=passed"
