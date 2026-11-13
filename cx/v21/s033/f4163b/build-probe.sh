#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/f4163b
kind="$1"
src="$JOB/src-$kind"
b="$src/core/build/core-release-gate"
h="$src/bindings/c/build-release-1.23.0"
prefix="$JOB/prefix-probe-$kind"
[[ ! -e "$prefix" ]]
stage() { python3 "$JOB/stage-preflight.py" "$JOB"; }
stage
python3 "$JOB/instrument.py" "$src"
git -C "$src" diff > "$JOB/probe-$kind.diff"
git -C "$src" diff -- core/src/runtime/core/pipe_write.cpp > "$JOB/logs/instrument-$kind.diff"
cmp "$JOB/logs/instrument-$kind.diff" "$JOB/logs/probe-C-original.diff"
stage
cmake -S "$src/core" -B "$b" -DCMAKE_INSTALL_PREFIX="$prefix" > "$JOB/logs/probe-$kind-configure.log" 2>&1
stage
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$b" --parallel 4 > "$JOB/logs/probe-$kind-build.log" 2>&1
stage
cmake --install "$b" > "$JOB/logs/probe-$kind-install.log" 2>&1
python3 "$JOB/manifest.py" "$prefix" "probe-$kind" "$src"
stage
cmake -S "$src/bindings/c" -B "$h" -DZLINK_C_CORE_BUILD_DIR="$prefix" > "$JOB/logs/probe-$kind-harness-configure.log" 2>&1
stage
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$h" --parallel 4 > "$JOB/logs/probe-$kind-harness-build.log" 2>&1
ldd "$h/perf/perf_router_router" > "$JOB/logs/probe-$kind-ldd.log"
