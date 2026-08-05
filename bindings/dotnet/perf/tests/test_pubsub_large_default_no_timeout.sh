#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOTNET_PERF_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
RUNNER="${DOTNET_PERF_DIR}/run_benchmarks_multi.sh"

size="${PERF_DOTNET_PUBSUB_LARGE_SIZE:-262144}"
duration="${PERF_DOTNET_PUBSUB_LARGE_DURATION:-1}"
tag="${PERF_DOTNET_PUBSUB_LARGE_TAG:-pubsub_large_default_no_timeout}"
log_file="$(mktemp -t zlink-dotnet-pubsub-large.XXXXXX.log)"
trap 'rm -f "${log_file}"' EXIT

set +e
PERF_FAIL_FAST=1 \
"${RUNNER}" \
  --pattern MULTI_PUBSUB \
  --transports tcp \
  --msg-sizes "${size}" \
  --duration "${duration}" \
  --results-tag "${tag}" \
  >"${log_file}" 2>&1
status=$?
set -e

if [[ "${status}" -ne 0 ]]; then
  cat "${log_file}" >&2
  exit "${status}"
fi

if ! rg -q "status: complete" "${log_file}"; then
  cat "${log_file}" >&2
  exit 1
fi
