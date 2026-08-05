#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
C_PERF_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
RUNNER="${C_PERF_DIR}/run_benchmarks_multi.sh"

tag="${PERF_C_DEALER_DEALER_WS_TAG:-dealer_dealer_ws_all_sizes_no_server_exit}"
log_file="$(mktemp -t zlink-c-dealer-dealer-ws.XXXXXX.log)"
trap 'rm -f "${log_file}"' EXIT

set +e
PERF_FAIL_FAST=1 \
"${RUNNER}" \
  --pattern MULTI_DEALER_DEALER \
  --transports ws \
  --msg-sizes 64,256,1024,4096,65536,131072 \
  --duration 5 \
  --results-tag "${tag}" \
  >"${log_file}" 2>&1
status=$?
set -e

if rg -n "FAIL|server_non_zero_exit|: timeout|did not exit|Segmentation fault|core dumped|Fatal error|Aborted|exited with status" "${log_file}" >&2; then
  cat "${log_file}" >&2
  exit 1
fi

if [[ "${status}" -ne 0 ]]; then
  cat "${log_file}" >&2
  exit "${status}"
fi

if ! rg -q "status: complete" "${log_file}"; then
  cat "${log_file}" >&2
  exit 1
fi
