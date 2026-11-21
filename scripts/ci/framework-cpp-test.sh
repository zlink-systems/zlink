#!/usr/bin/env bash
set -e
# No test is excluded. sample_parity and layout_contract were once
# believed to fail on main; they pass, and both read only the source
# tree, so BUILD_SAMPLES=OFF above does not change their result.
parallel_args=()
if [[ "${ZLINK_SINGLE_WORKER}" == "true" ]]; then
  parallel_args=(--parallel 1)
fi
ctest --preset linux-ninja-release "${parallel_args[@]}" \
  -L '^(unit|contract)$'
