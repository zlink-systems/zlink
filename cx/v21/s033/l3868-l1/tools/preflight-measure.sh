#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/l3868-l1
cd /root/worktree/zlink-v21-s033/l3868
export ZLINK_CPP_LADDER_LEVEL=0 PYTHONDONTWRITEBYTECODE=1
python3 - <<'PYSYNTAX' >"$JOB/raw/tool-syntax.txt"
import ast,pathlib
for path in sorted(pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868-l1/tools').glob('*.py')):
    ast.parse(path.read_text(),filename=str(path))
    print('syntax:',path.name)
PYSYNTAX
python3 "$JOB/tools/verify-control.py" >"$JOB/raw/normal-runtime.txt" 2>&1
python3 "$JOB/tools/reply-parity.py" "$JOB/raw/preflight" >"$JOB/raw/parity.txt" 2>&1
build-l3868/bench_cpp_framework_server --ladder-wire-check 1 >"$JOB/raw/wire-check.txt" 2>&1
ctest --test-dir build-l3868 --output-on-failure >"$JOB/raw/bench-tests.txt" 2>&1
cp build-l3868/CMakeCache.txt "$JOB/raw/CMakeCache.txt"
cp build-l3868/compile_commands.json "$JOB/raw/compile_commands.json"
python3 "$JOB/tools/provenance.py" --seal >"$JOB/raw/seal.txt" 2>&1
exec python3 "$JOB/tools/measure.py"
