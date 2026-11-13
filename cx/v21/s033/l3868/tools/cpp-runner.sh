#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/l3868
export ZLINK_CPP_LADDER_LEVEL=0
export OUTPUT_DIR="$JOB/raw/L0-owner/send-saturation-1024"
IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp PATTERNS=send-saturation PAYLOAD_SIZES=1024 \
  "$JOB/tools/cpp-cell-runner.sh"
"$JOB/tools/diagnose-if-low.sh" "$JOB/raw/L0-owner/send-saturation-1024" send-saturation 1024
export OUTPUT_DIR="$JOB/raw/cross/L0-owner"
"$JOB/tools/cross.sh"
export OUTPUT_DIR="$JOB/raw/L0-owner"
SKIP_FIRST_CELL=1 "$JOB/tools/pairs.sh"
