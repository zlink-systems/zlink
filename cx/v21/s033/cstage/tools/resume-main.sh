#!/bin/bash
set -euo pipefail
root=/root/worktree/zlink-cstage
job=/root/jobs/fx/cx/v21/s033/cstage
export PATH="$job/tools:$PATH"
export ZLINK_LOCAL_PACKAGE_ROOT="$root/.artifacts/wsl"
cd "$root/framework/bench/grpc"
bash cpp/build.sh
mkdir -p "$job/bin-main" "$job/package-main"
cp cpp/build/bench_cpp_client cpp/build/bench_cpp_zlink_server "$job/bin-main/"
cp -a "$root/.artifacts/wsl/install/zlink-core/1.23.0" "$job/package-main/core"
cp -a "$root/.artifacts/wsl/install/zlink-cpp/1.23.0" "$job/package-main/cpp"
