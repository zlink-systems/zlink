#!/bin/bash
set -euo pipefail
mapfile -t targets < /root/jobs/fx/cx/v21/s033/psched7/unit-build-targets.txt
exec /root/jobs/fx/cx/cpp-build-slot.sh cmake --build /root/worktree/zlink-v21-s033/psched5/.psched5-tests --parallel 3 --target "${targets[@]}"
