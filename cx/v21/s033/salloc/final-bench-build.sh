#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/salloc
cmake -S "$job/probe" -B "$job/probe-build" -G Ninja
cmake --build "$job/probe-build" --parallel 3 -- -k 0
cmake --build /root/worktree/zlink-3867a/framework/bench/grpc/cpp/build --parallel 3 -- -k 0
