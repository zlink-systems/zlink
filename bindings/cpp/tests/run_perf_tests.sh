#!/usr/bin/env bash
set -euo pipefail

echo "[cpp-tests] run perf regressions"
ctest --test-dir "${1}" --output-on-failure --no-tests=error \
  -L '^perf-regression$' -LE '^perf-measurement$'
