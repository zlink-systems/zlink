#!/bin/bash
set -euo pipefail
root=/root/worktree/zlink-cstage
job=/root/jobs/fx/cx/v21/s033/cstage
export PATH="$job/tools:$PATH"
export ZLINK_LOCAL_PACKAGE_ROOT="$root/.artifacts/wsl"
export CSTAGE_BUILD_JOBS=2
cd "$root/framework/bench/grpc"
bash cpp/build.sh
mkdir -p "$job/bin-staged"
cp cpp/build/bench_cpp_client cpp/build/bench_cpp_zlink_server "$job/bin-staged/"
