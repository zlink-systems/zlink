#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

source "$ROOT_DIR/scripts/run_node_job.sh"

for test_file in dist-tools/tests/*.test.js; do
  printf '[test] %s\n' "$test_file"
  run_node_job 120 node --test "$test_file"
done

if [[ "${ZLINK_BINDING_RAW_TEST_ONLY:-0}" != "1" ]]; then
  "$ROOT_DIR/samples/run_samples.sh"
  "$ROOT_DIR/../javascript/samples/run_samples.sh"
fi
