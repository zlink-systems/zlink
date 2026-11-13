#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-dotnet-m1
ROOT=/root/worktree/zlink-v21-s033/lad-dotnet
export IMPLEMENTATIONS='zlink-framework-dotnet,zlink-dotnet'
export PATTERNS='request-serial'
export PAYLOAD_SIZES='1024,4096'
export ZLINK_DOTNET_LADDER_CLIENT=raw
export WARMUP_SECONDS=10 DURATION_SECONDS=5 RUNS=1 SKIP_BUILD=1
export OUTPUT_DIR="${OUTPUT_DIR:-$JOB/raw/manual-request-serial}"
exec bash "$ROOT/framework/bench/grpc/dotnet/run_local.sh"
