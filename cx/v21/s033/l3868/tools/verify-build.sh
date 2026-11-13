#!/usr/bin/env bash
set -euo pipefail
REPO=/root/worktree/zlink-v21-s033/l3868
JOB=/root/jobs/fx/cx/v21/s033/l3868
cd "$REPO"
for exe in bench_cpp_client bench_cpp_framework_server bench_cpp_zlink_server bench_cpp_grpc_server; do
  test -x "build-l3868/$exe"
  sha256sum "build-l3868/$exe"
  ldd "build-l3868/$exe" | rg 'zlink|not found'
done
rg 'CMAKE_BUILD_TYPE:|ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE:|ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX:' build-l3868/CMakeCache.txt
cmake --build build-l3868 --target bench_cpp_client bench_cpp_framework_server bench_cpp_zlink_server bench_cpp_grpc_server -- -n | tee "$JOB/raw/build-dry-run.txt"
rg -q 'no work to do' "$JOB/raw/build-dry-run.txt"
cp build-l3868/CMakeCache.txt "$JOB/raw/CMakeCache.txt"
cp build-l3868/compile_commands.json "$JOB/raw/compile_commands.json"
build-l3868/bench_cpp_framework_server --ladder-wire-check 1
python3 "$JOB/tools/verify-control.py"
python3 - <<'PY'
import hashlib,json,pathlib
root=pathlib.Path('.')
paths=[]
for base in ['framework/bench/grpc/cpp','framework/languages/cpp/framework/src','framework/languages/cpp/framework/include','bindings/cpp/src','bindings/cpp/include']:
    paths.extend(p for p in (root/base).rglob('*') if p.suffix in ['.cpp','.hpp','.inc','.h'] and p.is_file())
sums={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(paths)}
pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868/raw/built-sources.json').write_text(json.dumps(sums,indent=2)+'\n')
PY
