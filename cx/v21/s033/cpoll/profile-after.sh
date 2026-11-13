#!/bin/bash
set -euo pipefail
export TMPDIR=/root/jobs/fx/cx/v21/s033/cpoll/tmp
exec flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD CAPTURE_MODE=profile ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR=/root/jobs/fx/cx/v21/s033/cpoll/build IMPLEMENTATIONS=zlink-framework-cpp PATTERNS=request-backpressure PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=20 SKIP_BUILD=1 RUN_STAMP=cpoll-after OUTPUT_DIR=/root/jobs/fx/cx/v21/s033/cpoll/profile-after bash /root/jobs/fx/cx/v21/s033/cpoll/profile-B.sh
