#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/salloc
/root/jobs/fx/cx/cpp-build-slot.sh bash "$job/solo_build.sh" bash "$job/send-fix-build.sh" > "$job/send-fix-build.log" 2>&1
/root/jobs/fx/cx/measure-shared.sh ctest --test-dir "$job/build" -R '^test_cpp_framework_send_allocations$' \
  --output-on-failure --output-junit "$job/send-fix.xml" > "$job/send-fix.log" 2>&1
/root/jobs/fx/cx/measure-shared.sh ctest --test-dir "$job/debug-build" -R '^test_cpp_framework_send_allocations$' \
  --output-on-failure --output-junit "$job/send-debug.xml" > "$job/send-debug.log" 2>&1
/root/jobs/fx/cx/measure-shared.sh ctest --test-dir "$job/build" -L '^(unit|contract)$' \
  --parallel 3 --output-on-failure --output-junit "$job/unit-contract.xml" > "$job/unit-contract-final.log" 2>&1
cd /root/worktree/zlink-3867a
/root/jobs/fx/cx/measure-shared.sh bash scripts/format/format.sh --check cpp > "$job/format-final.log" 2>&1
