#!/usr/bin/env bash
set -euo pipefail
export CAPTURE_MODE=profile PATTERNS=send-saturation PAYLOAD_SIZES=1024
export IMPLEMENTATIONS=zlink-framework-cpp,zlink-cpp RUNS=1 WARMUP_SECONDS=10 DURATION_SECONDS=20 SKIP_BUILD=1 RUN_STAMP=cadvice-send-profile
export OUTPUT_DIR=/root/jobs/fx/cx/v21/s033/cadvice/out-profile
python3 /root/jobs/fx/cx/v21/s033/cadvice/monitor_run.py
