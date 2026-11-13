#!/usr/bin/env bash
set -euo pipefail
ROOT=/root/worktree/zlink-v21-s033/lad-dotnet
OUT=/root/jobs/fx/cx/v21/s033/lad-dotnet-l0e/raw/cross-l0
RUNNER="$ROOT/framework/bench/grpc/dotnet/run_local.sh"
export PATTERNS=send-saturation PAYLOAD_SIZES=1024
export ZLINK_DOTNET_LADDER_CLIENT=raw
unset ZLINK_DOTNET_RAW_TARGET
export IMPLEMENTATIONS=zlink-dotnet TARGET_IMPLEMENTATION=zlink-dotnet
export OUTPUT_DIR="$OUT/raw-raw"
bash "$RUNNER"
export ZLINK_DOTNET_RAW_TARGET=framework
export IMPLEMENTATIONS=zlink-dotnet TARGET_IMPLEMENTATION=zlink-framework-dotnet
export OUTPUT_DIR="$OUT/framework-raw"
bash "$RUNNER"
unset ZLINK_DOTNET_RAW_TARGET
export IMPLEMENTATIONS=zlink-framework-dotnet TARGET_IMPLEMENTATION=zlink-framework-dotnet
export OUTPUT_DIR="$OUT/framework-l0-client"
bash "$RUNNER"
