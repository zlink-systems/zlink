#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail

runner="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/run_benchmarks.sh"
unset PERF_MULTI_LATENCY_DURATION_SECONDS

check() {
  local expected="$1" output
  shift
  if output="$("$@" 2>&1)" || [[ "${output}" != *"${expected}"* ]]; then
    printf 'Expected runner failure containing %s, got:\n%s\n' "${expected}" "${output}" >&2
    return 1
  fi
}

# Trace the export and stop at profile validation before any build or measurement.
check 'export PERF_MULTI_LATENCY_DURATION_SECONDS=3' env PERF_MULTI_LATENCY_DURATION_SECONDS=0 \
  bash -x "${runner}" --duration 7 --latency-duration 3 --auto-hwm-profile invalid
check 'export PERF_MULTI_LATENCY_DURATION_SECONDS=4' env PERF_MULTI_LATENCY_DURATION_SECONDS=4 \
  bash -x "${runner}" --duration 7 --auto-hwm-profile invalid
check 'export PERF_MULTI_LATENCY_DURATION_SECONDS=7' \
  bash -x "${runner}" --duration 7 --auto-hwm-profile invalid
check 'export PERF_MULTI_LATENCY_DURATION_SECONDS=009' \
  bash -x "${runner}" --latency-duration=009 --auto-hwm-profile invalid
for invalid in 0 -1 1.5 1e2 ''; do
  check 'must be a positive integer' bash "${runner}" "--latency-duration=${invalid}" --auto-hwm-profile invalid
done
check 'requires a value' bash "${runner}" --auto-hwm-profile invalid --latency-duration
check 'must be a positive integer' env PERF_MULTI_LATENCY_DURATION_SECONDS=0 \
  bash "${runner}" --auto-hwm-profile invalid
