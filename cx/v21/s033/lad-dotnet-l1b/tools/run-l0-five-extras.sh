#!/usr/bin/env bash
set -euo pipefail
export IMPLEMENTATIONS='zlink-framework-dotnet,zlink-dotnet'
export PATTERNS='request-serial'
export PAYLOAD_SIZES='1024,4096'
export ZLINK_DOTNET_LADDER_CLIENT=raw
exec bash /root/worktree/zlink-v21-s033/lad-dotnet/framework/bench/grpc/dotnet/run_local.sh
