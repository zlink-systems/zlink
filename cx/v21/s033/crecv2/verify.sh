#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/crecv2
repo=/root/worktree/zlink-3867r
stage() { date -Is > "$job/stage-time"; printf '%s\n' "$1" > "$job/stage"; }
cd "$repo/framework/languages/cpp"
stage full-build
flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build --preset linux-ninja-release --parallel 3 -- -k 0 > "$job/build-all.log" 2>&1
stage unit-contract
flock -s /root/jobs/fx/cx/measure.lock ctest --preset linux-ninja-release -L '^(unit|contract)$' > "$job/test-unit-contract.log" 2>&1
stage format
cd "$repo"
scripts/format/format.sh --check cpp > "$job/format-final.log" 2>&1
stage benchmark-build
flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$job/build" --parallel 3 --target bench_cpp_client bench_cpp_framework_server bench_cpp_zlink_server bench_cpp_grpc_server -- -k 0 > "$job/build-bench-final.log" 2>&1
stage ladder-build
flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh python3 "$job/build-ladder.py" > "$job/build-ladder.log" 2>&1
stage measurements
bash "$job/measure.sh" > "$job/measure.log" 2>&1
stage complete
