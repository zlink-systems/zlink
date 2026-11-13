#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/crecv3
repo=/root/worktree/zlink-3867f
export TMPDIR="$job/tmp"
stage() { printf '%s\n' "$1" > "$job/stage"; date -Is > "$job/stage-time"; }
cd "$repo/framework/languages/cpp"
stage full-build
flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build --preset linux-ninja-release --parallel 3 -- -k 0 > "$job/build-all.log" 2>&1
stage unit-contract
flock -s /root/jobs/fx/cx/measure.lock ctest --preset linux-ninja-release -L '^(unit|contract)$' > "$job/test-unit-contract.log" 2>&1
stage format
cd "$repo"
scripts/format/format.sh --check cpp > "$job/format-final.log" 2>&1
stage measurements
bash "$job/measure.sh" > "$job/measure.log" 2>&1
stage complete
