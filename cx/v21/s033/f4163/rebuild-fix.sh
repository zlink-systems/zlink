#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/f4163
src="$JOB/src"
b="$src/core/build/core-release-gate"
h="$src/bindings/c/build-release-1.23.0"
prefix="$JOB/prefix-fix"
python3 "$JOB/stage-preflight.py" "$JOB"
cmake -S "$src/core" -B "$b" -DCMAKE_INSTALL_PREFIX="$prefix" > "$JOB/logs/fix-configure.log" 2>&1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$b" --parallel 4 > "$JOB/logs/fix-build.log" 2>&1
cmake --install "$b" > "$JOB/logs/fix-install.log" 2>&1
python3 "$JOB/manifest.py" "$prefix" fix "$src"
cmake -S "$src/bindings/c" -B "$h" -DZLINK_C_CORE_BUILD_DIR="$prefix" > "$JOB/logs/fix-harness-configure.log" 2>&1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$h" --parallel 4 > "$JOB/logs/fix-harness-build.log" 2>&1
ldd "$h/perf/perf_router_router" > "$JOB/logs/fix-ldd.log"
