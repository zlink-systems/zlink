#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/f4163
src="$JOB/src"
core_build="$src/core/build/tests4163"
prefix="$JOB/prefix-test-unused"
python3 "$JOB/stage-preflight.py" "$JOB"
cmake -S "$src/core" -B "$core_build" -G 'Unix Makefiles' \
  -DCMAKE_MAKE_PROGRAM=/usr/bin/make -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=ON -DCMAKE_C_COMPILER=gcc-13 \
  -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_SHARED=ON -DBUILD_STATIC=ON \
  -DBUILD_TESTS=ON -DZLINK_BUILD_TESTS=ON -DBUILD_BENCHMARKS=OFF \
  -DZLINK_CXX_STANDARD=17 -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DWITH_DOC=OFF \
  -DCMAKE_INSTALL_PREFIX="$prefix" > "$JOB/logs/tests-configure.log" 2>&1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$core_build" --target test_router_complete_record_hwm unittest_router_pipe_contract test_phase3_request_reply_contract --parallel 4 > "$JOB/logs/tests-build.log" 2>&1
ctest --test-dir "$core_build" -R '^(test_router_complete_record_hwm|unittest_router_pipe_contract|test_phase3_request_reply_contract)$' --output-on-failure > "$JOB/logs/tests.log" 2>&1
cat "$JOB/logs/tests.log"
