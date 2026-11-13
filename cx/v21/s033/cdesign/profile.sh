#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cdesign
export PATTERNS=request-backpressure,send-saturation PAYLOAD_SIZES=1024 IMPLEMENTATIONS=zlink-framework-cpp RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 RUN_STAMP=profile-$1 OUTPUT_DIR=$job/profile-$1
exec flock -o /root/jobs/fx/cx/measure.lock bash "$job/runners/$1-profile.sh"
