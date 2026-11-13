#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/wire
bash "$job/build_bench.sh" > "$job/bench-verified-build.log" 2>&1
cmake -S /root/worktree/zlink-3867w/framework/languages/cpp -B "$job/build" -DZLINK_FRAMEWORK_CPP_TEST_PROBES=ON -DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=OFF > "$job/test-configure.log" 2>&1
bash "$job/build_tests.sh" > "$job/unit-contract-final-build.log" 2>&1
cmake --build "$job/probe-build" --parallel 3 --target alloc-W -- -k 0 > "$job/probe-W-build.log" 2>&1
