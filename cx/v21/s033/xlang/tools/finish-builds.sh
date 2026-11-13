#!/usr/bin/env bash
set -euo pipefail
export PATH="/root/jobs/fx/cx/v21/s033/xlang/bin:$PATH"
cd /root/worktree/zlink-xlang/framework/bench/grpc
/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash /root/jobs/fx/cx/v21/s033/xlang/tools/build-http-node.sh node > /root/jobs/fx/cx/v21/s033/xlang/build-http-node.log 2>&1
/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash dotnet/build.sh > /root/jobs/fx/cx/v21/s033/xlang/build-dotnet-retry.log 2>&1
/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash node/build.sh > /root/jobs/fx/cx/v21/s033/xlang/build-node-retry.log 2>&1
touch /root/jobs/fx/cx/v21/s033/xlang/BUILD_EXTRAS_DONE
