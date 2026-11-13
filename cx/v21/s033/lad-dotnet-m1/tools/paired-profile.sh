#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-dotnet-l1b
ROOT=/root/worktree/zlink-v21-s033/lad-dotnet
SERVER_DIR="$ROOT/framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0"
L0_DIR="$JOB/raw/builds/l0-server-net8.0"
L1_DIR="$JOB/raw/builds/l1-server-net8.0"
PROFILE_ROOT="$OUTPUT_DIR"
for variant in L0 L1; do
  if [[ "$variant" == L0 ]]; then cp -a "$L0_DIR/." "$SERVER_DIR/"; else cp -a "$L1_DIR/." "$SERVER_DIR/"; fi
  out="$PROFILE_ROOT/$variant"
  mkdir -p "$out"
  hash=$(sha256sum "$SERVER_DIR/WithGrpcBench.ZLinkServer.dll" | cut -c1-64)
  printf -- '- paired-profile %s HEAD=%s dirty=no target=%s:%s cell=request-serial-4096\n' "$variant" "$(git -C "$ROOT" rev-parse HEAD)" "$SERVER_DIR/WithGrpcBench.ZLinkServer.dll" "$hash" >> "$JOB/progress.md"
  RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 SOURCE_CPUS=0-7 TARGET_CPUS=8-15 \
  TARGET_IMPLEMENTATION=zlink-framework-dotnet ZLINK_DOTNET_LADDER_CLIENT=raw ZLINK_DOTNET_RAW_TARGET=framework \
  IMPLEMENTATIONS=zlink-dotnet PATTERNS=request-serial PAYLOAD_SIZES=4096 OUTPUT_DIR="$out" RUN_STAMP="4351-l1b-profile-$variant" \
    "$JOB/tools/dotnet-profile-runner.sh" >"$out/driver.log" 2>&1
done
