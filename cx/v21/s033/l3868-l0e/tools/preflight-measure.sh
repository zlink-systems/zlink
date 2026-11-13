#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/l3868-l0e
REPO=/root/worktree/zlink-v21-s033/l3868
cd "$REPO"
export ZLINK_CPP_LADDER_LEVEL=0
"$JOB/tools/verify-build.sh" >"$JOB/raw/verify-build.txt" 2>&1 &&
  ctest --test-dir build-l3868 --output-on-failure >"$JOB/raw/bench-tests.txt" 2>&1 &&
  python3 "$JOB/tools/provenance.py" --seal >"$JOB/raw/seal.txt" 2>&1 &&
  "$JOB/tools/measure.sh"
