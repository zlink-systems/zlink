#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail

runner="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/run_benchmarks.sh"
unset PERF_MULTI_MONITOR_HWM PERF_MONITOR_HWM PERF_MULTI_MONITOR_HWM_BYTES PERF_MONITOR_HWM_BYTES

check() {
  local expected="$1" output
  shift
  if output="$("$@" 2>&1)" || [[ "${output}" != *"${expected}"* ]]; then
    printf 'Expected runner failure containing %s, got:\n%s\n' "${expected}" "${output}" >&2
    return 1
  fi
}

# An invalid profile stops the runner after numeric option validation.
check 'auto-hwm-profile must be' bash "${runner}" --monitor-hwm-bytes 0 --auto-hwm-profile invalid
check 'unknown option: --monitor-hwm' bash "${runner}" --monitor-hwm 1 --auto-hwm-profile invalid
check 'must be a non-negative integer' bash "${runner}" --monitor-hwm-bytes -1 --auto-hwm-profile invalid
check 'requires a value' bash "${runner}" --auto-hwm-profile invalid --monitor-hwm-bytes
for legacy in PERF_MULTI_MONITOR_HWM PERF_MONITOR_HWM; do
  check "${legacy} is obsolete" env "${legacy}=" bash "${runner}" --monitor-hwm-bytes 1 --auto-hwm-profile invalid
done
check 'export PERF_MULTI_MONITOR_HWM_BYTES=0' env PERF_MULTI_MONITOR_HWM_BYTES=5 \
  bash -x "${runner}" --monitor-hwm-bytes 0 --auto-hwm-profile invalid
check 'export PERF_MULTI_MONITOR_HWM_BYTES=5' env PERF_MULTI_MONITOR_HWM_BYTES=5 PERF_MONITOR_HWM_BYTES=invalid \
  bash -x "${runner}" --auto-hwm-profile invalid
check 'export PERF_MULTI_MONITOR_HWM_BYTES=7' env PERF_MONITOR_HWM_BYTES=7 \
  bash -x "${runner}" --auto-hwm-profile invalid
check 'export PERF_MULTI_MONITOR_HWM_BYTES=4096000' bash -x "${runner}" --auto-hwm-profile invalid
check 'must be a non-negative integer' env PERF_MONITOR_HWM_BYTES=invalid \
  bash "${runner}" --auto-hwm-profile invalid
