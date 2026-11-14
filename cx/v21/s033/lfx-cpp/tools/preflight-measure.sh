#!/usr/bin/env bash
# All preparation and tests run outside an exclusive measurement hold.
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lfx-cpp
cd /root/worktree/zlink-v21-s033/cfx
export ZLINK_CPP_LADDER_LEVEL=0 PYTHONDONTWRITEBYTECODE=1
python3 "$JOB/tools/verify-control.py" >"$JOB/raw/normal-runtime.txt" 2>&1
python3 "$JOB/tools/reply-parity.py" "$JOB/raw/preflight" >"$JOB/raw/parity.txt" 2>&1
source /root/jobs/fx/cx/measure-gate.sh
measure_gate_pass
flock -s /root/jobs/fx/cx/measure.lock bash -c '
 set -euo pipefail
 build-cfx/bench_cpp_framework_server --ladder-wire-check 1 > /root/jobs/fx/cx/v21/s033/lfx-cpp/raw/wire-check.txt 2>&1
 ctest --test-dir build-cfx --output-on-failure > /root/jobs/fx/cx/v21/s033/lfx-cpp/raw/bench-tests.txt 2>&1
'
cp build-cfx/CMakeCache.txt "$JOB/raw/CMakeCache.txt"
cp build-cfx/compile_commands.json "$JOB/raw/compile_commands.json"
python3 "$JOB/tools/provenance.py" --seal >"$JOB/raw/seal.txt" 2>&1
