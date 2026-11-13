#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/core124b
SRC="$JOB/src-1.24"; PREFIX="/root/jobs/fx/cx/v21/s033/core124/prefix-1.24"
BUILD="$SRC/bindings/c/build-release-1.23.0"
python3 - "$JOB/build-preflight.jsonl" <<'PY'
import json,pathlib,shutil,sys,time
mem=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))*1024
usage=sum(p.stat().st_size for p in pathlib.Path(sys.argv[1]).parent.rglob('*') if p.is_file())
r=dict(stage='harness-1.24',captured_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),MemAvailable_bytes=mem,job_bytes=usage,job_cap_bytes=25*1024**3,root_free_bytes=shutil.disk_usage('/').free,allowed=mem>=12*1024**3 and usage<25*1024**3)
with open(sys.argv[1],'a') as f:f.write(json.dumps(r)+'\n')
print(json.dumps(r))
if not r['allowed']:raise SystemExit(3)
PY
cmake -S "$SRC/bindings/c" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 \
  -DENABLE_LTO=OFF -DZLINK_CORE_DIR="$SRC/core" -DZLINK_C_CORE_BUILD_DIR="$PREFIX" \
  -DZLINK_C_BUILD_BENCHMARKS=ON -DZLINK_C_BUILD_SAMPLES=OFF -DZLINK_C_BUILD_TESTS=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > "$JOB/logs/harness-1.24-configure.log" 2>&1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$BUILD" --target \
  perf_pair perf_pubsub perf_dealer_dealer perf_dealer_router perf_dealer_router_reqrep \
  perf_router_router perf_router_router_reqrep comp_src_dealer_dealer_server comp_src_dealer_dealer_client \
  comp_src_dealer_router_sendsend_server comp_src_dealer_router_sendsend_client \
  comp_src_router_router_sendsend_server comp_src_router_router_sendsend_client \
  comp_src_dealer_router_reqrep_server comp_src_dealer_router_reqrep_client \
  comp_src_router_router_reqrep_server comp_src_router_router_reqrep_client \
  comp_src_pubsub_server comp_src_pubsub_client comp_src_stream_server perf_stream_client \
  --parallel 4 --verbose > "$JOB/logs/harness-1.24-build.log" 2>&1
