#!/usr/bin/env bash
set -euo pipefail
base=/root/jobs/fx/cx/v21/s033/jcount
runner=/root/jobs/fx/cx/v21/s033/jcount/java/run_local.sh
env SKIP_BUILD=1 RUNS=3 IMPLEMENTATIONS=zlink-framework-java PATTERNS=request-backpressure,request-serial,send-saturation PAYLOAD_SIZES=1024,4096 WARMUP_SECONDS=30 DURATION_SECONDS=5 RUN_STAMP=jcount-fw-r3 OUTPUT_DIR="$base/raw/framework" "$runner"
env SKIP_BUILD=1 RUNS=3 IMPLEMENTATIONS=zlink-java PATTERNS=request-backpressure,request-serial,send-saturation PAYLOAD_SIZES=1024,4096 WARMUP_SECONDS=30 DURATION_SECONDS=5 RUN_STAMP=jcount-binding-r3 OUTPUT_DIR="$base/raw/binding" "$runner"
