#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/f4163
kind="$1"
src="$2"
prefix="$JOB/prefix-$kind"
core_build="$src/core/build/core-release-gate"
harness_build="$src/bindings/c/build-release-1.23.0"
[[ ! -e "$prefix" ]] || { echo "Refusing to overwrite $prefix"; exit 2; }
[[ ! -e "$core_build" ]] || { echo "Refusing to overwrite $core_build"; exit 2; }
[[ ! -e "$harness_build" ]] || { echo "Refusing to overwrite $harness_build"; exit 2; }
preflight() {
  python3 - "$JOB/build-preflight.jsonl" "$1" <<'PY'
import json,pathlib,shutil,sys,time
mem=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))*1024
usage=sum(p.stat().st_size for p in pathlib.Path(sys.argv[1]).parent.rglob('*') if p.is_file())
r=dict(stage=sys.argv[2],captured_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),MemAvailable_bytes=mem,job_bytes=usage,job_cap_bytes=25*1024**3,root_free_bytes=shutil.disk_usage('/').free,allowed=mem>=12*1024**3 and usage<25*1024**3)
with open(sys.argv[1],'a') as f:f.write(json.dumps(r)+'\n')
print(json.dumps(r))
if not r['allowed']:raise SystemExit(3)
PY
}
mkdir -p "$JOB/logs"
preflight core-configure
cmake -S "$src/core" -B "$core_build" -G 'Unix Makefiles' \
  -DCMAKE_MAKE_PROGRAM=/usr/bin/make -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=ON -DCMAKE_C_COMPILER=gcc-13 \
  -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_SHARED=ON -DBUILD_STATIC=ON \
  -DBUILD_TESTS=OFF -DZLINK_BUILD_TESTS=OFF -DBUILD_BENCHMARKS=OFF \
  -DZLINK_CXX_STANDARD=17 -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DWITH_DOC=OFF \
  -DCMAKE_INSTALL_PREFIX="$prefix" > "$JOB/logs/core-$kind-configure.log" 2>&1
preflight core-build
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$core_build" --parallel 4 > "$JOB/logs/core-$kind-build.log" 2>&1
preflight core-install
cmake --install "$core_build" > "$JOB/logs/core-$kind-install.log" 2>&1
python3 "$JOB/manifest.py" "$prefix" "$kind" "$src"
preflight harness-configure
cmake -S "$src/bindings/c" -B "$harness_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 \
  -DENABLE_LTO=OFF -DZLINK_CORE_DIR="$src/core" -DZLINK_C_CORE_BUILD_DIR="$prefix" \
  -DZLINK_C_BUILD_BENCHMARKS=ON -DZLINK_C_BUILD_SAMPLES=OFF -DZLINK_C_BUILD_TESTS=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > "$JOB/logs/harness-$kind-configure.log" 2>&1
preflight harness-build
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$harness_build" --target \
  perf_pair perf_pubsub perf_dealer_dealer perf_dealer_router perf_dealer_router_reqrep \
  perf_router_router perf_router_router_reqrep comp_src_dealer_dealer_server comp_src_dealer_dealer_client \
  comp_src_dealer_router_sendsend_server comp_src_dealer_router_sendsend_client \
  comp_src_router_router_sendsend_server comp_src_router_router_sendsend_client \
  comp_src_dealer_router_reqrep_server comp_src_dealer_router_reqrep_client \
  comp_src_router_router_reqrep_server comp_src_router_router_reqrep_client \
  comp_src_pubsub_server comp_src_pubsub_client comp_src_stream_server perf_stream_client \
  --parallel 4 --verbose > "$JOB/logs/harness-$kind-build.log" 2>&1
