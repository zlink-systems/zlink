#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cstage
export PATH="$job/tools:$PATH"
export CSTAGE_BUILD_JOBS=2
bash "$job/tools/core-compile.sh"
cp -a /root/worktree/zlink-cstage/.artifacts/wsl/install/zlink-core/1.23.0/. "$job/package-staged/core/"
