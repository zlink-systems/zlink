#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/salloc
/root/jobs/fx/cx/cpp-build-slot.sh \
  bash "$job/solo_build.sh" bash "$job/final-compile.sh" > "$job/final-recompile.log" 2>&1
/root/jobs/fx/cx/measure-shared.sh ctest --test-dir "$job/build" \
  -R '^test_cpp_framework_(request_call_allocations|request_native_counts|request_encoder_allocations|async_task|raw_route_port_contract|service_wire_codec)$' \
  --parallel 1 --output-on-failure --output-junit "$job/focused.xml" > "$job/focused-final.log" 2>&1
/root/jobs/fx/cx/measure-shared.sh ctest --test-dir "$job/build" -L '^(unit|contract)$' \
  --parallel 3 --output-on-failure --output-junit "$job/unit-contract.xml" > "$job/unit-contract-tests.log" 2>&1
cd /root/worktree/zlink-3867a
/root/jobs/fx/cx/measure-shared.sh bash scripts/format/format.sh --check cpp > "$job/format-final.log" 2>&1
