#!/bin/bash
set -euo pipefail
export PATH="/root/jobs/fx/cx/v21/s033/wire/bin:$PATH"
export ZLINK_LOCAL_PACKAGE_ROOT=/root/worktree/zlink-ratio040/.artifacts/wsl
export VCPKG_MAX_CONCURRENCY=3
cd /root/worktree/zlink-3867w
exec bash framework/bench/grpc/cpp/build.sh
