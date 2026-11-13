#!/bin/bash
set -euo pipefail
export TMPDIR=/root/jobs/fx/cx/v21/s033/cpoll/tmp
job=/root/jobs/fx/cx/v21/s033/cpoll
cd /root/worktree/zlink-3867c/framework/languages/cpp
flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build --preset linux-ninja-release --parallel 3 -- -k 0 > "$job/build-all.log" 2>&1
flock -s /root/jobs/fx/cx/measure.lock ctest --preset linux-ninja-release -L '^(unit|contract)$' > "$job/test-unit-contract.log" 2>&1
cd /root/worktree/zlink-3867c
scripts/format/format.sh --check cpp > "$job/format-final.log" 2>&1
