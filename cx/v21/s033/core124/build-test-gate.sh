#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/core124
SRC="$JOB/src-1.24"
BUILD="$SRC/core/build/core-release-gate-tests"
python3 - "$JOB/test-build-preflight.jsonl" <<'PY'
import json,pathlib,shutil,sys,time
mem=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))*1024
usage=sum(p.stat().st_size for p in pathlib.Path(sys.argv[1]).parent.rglob('*') if p.is_file())
r=dict(stage='candidate-test-build',captured_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),MemAvailable_bytes=mem,job_bytes=usage,job_cap_bytes=25*1024**3,root_free_bytes=shutil.disk_usage('/').free,allowed=mem>=12*1024**3 and usage<25*1024**3)
with open(sys.argv[1],'a') as f:f.write(json.dumps(r)+'\n')
print(json.dumps(r))
if not r['allowed']:raise SystemExit(3)
PY
cmake -S "$SRC/core" -B "$BUILD" -G 'Unix Makefiles' \
  -DCMAKE_MAKE_PROGRAM=/usr/bin/make -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=ON -DCMAKE_C_COMPILER=gcc-13 \
  -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_SHARED=ON -DBUILD_STATIC=ON \
  -DBUILD_TESTS=ON -DZLINK_BUILD_TESTS=ON -DBUILD_BENCHMARKS=OFF \
  -DZLINK_CXX_STANDARD=17 -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DWITH_DOC=OFF \
  > "$JOB/logs/candidate-test-configure.log" 2>&1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$BUILD" --parallel 4 > "$JOB/logs/candidate-test-build.log" 2>&1
