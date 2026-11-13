#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/l3868
ROOT_OUTPUT="$OUTPUT_DIR"
export IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp
for pattern in request-serial request-backpressure send-saturation; do
  for size in 1024 4096; do
    if [[ "${SKIP_FIRST_CELL:-0}" == 1 && "$pattern" == send-saturation && "$size" == 1024 ]]; then continue; fi
    export PATTERNS="$pattern" PAYLOAD_SIZES="$size" OUTPUT_DIR="$ROOT_OUTPUT/$pattern-$size"
    "$JOB/tools/cpp-cell-runner.sh"
    "$JOB/tools/diagnose-if-low.sh" "$OUTPUT_DIR" "$pattern" "$size"
  done
done
