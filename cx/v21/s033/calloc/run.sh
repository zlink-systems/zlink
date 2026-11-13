#!/usr/bin/env bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/calloc
mode=${1:?mode}
export CAPTURE_MODE=allocstack PATTERNS=request-backpressure PAYLOAD_SIZES=1024
export IMPLEMENTATIONS=${2:-zlink-framework-cpp,zlink-cpp}
export RUNS=1 WARMUP_SECONDS=10 DURATION_SECONDS=20 SKIP_BUILD=1 RUN_STAMP=calloc-$mode
export OUTPUT_DIR=$job/out-$mode
memavail_kb=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
((memavail_kb >= 12582912)) || exit 3
exec bash "$job/instrumented/run_local.sh"
