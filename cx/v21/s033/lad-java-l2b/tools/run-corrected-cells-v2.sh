#!/usr/bin/env bash
set -euo pipefail
JAVA_RUNNER=/root/worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java/run_local.sh
RAW=/root/jobs/fx/cx/v21/s033/lad-java-l2b/raw/v2
RUN_STAMP=4352L2bV2
COMMON=(IMPLEMENTATIONS=zlink-java,zlink-framework-java SKIP_BUILD=1)
env "${COMMON[@]}" RUNS=5 PATTERNS=request-backpressure PAYLOAD_SIZES=1024 RUN_STAMP="$RUN_STAMP-reqbp-1K" OUTPUT_DIR="$RAW/request-backpressure-1K" "$JAVA_RUNNER"
env "${COMMON[@]}" RUNS=5 PATTERNS=send-saturation PAYLOAD_SIZES=1024 RUN_STAMP="$RUN_STAMP-sendsat-1K" OUTPUT_DIR="$RAW/send-saturation-1K" "$JAVA_RUNNER"
env "${COMMON[@]}" RUNS=3 PATTERNS=request-backpressure PAYLOAD_SIZES=4096 RUN_STAMP="$RUN_STAMP-reqbp-4K" OUTPUT_DIR="$RAW/request-backpressure-4K" "$JAVA_RUNNER"
