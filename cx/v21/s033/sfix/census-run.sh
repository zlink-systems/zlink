#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/sfix
for variant in before after; do
 if [[ $variant == after ]]; then tree=/root/worktree/zlink-3867a; stamp=final; else tree=/root/worktree/zlink-ratio040; stamp=baseline; fi
 env SALLOC_TREE="$tree" IMPLEMENTATIONS=zlink-framework-cpp PATTERNS=request-backpressure PAYLOAD_SIZES=1024 RUNS=1 \
  WARMUP_SECONDS=10 DURATION_SECONDS=20 SKIP_BUILD=1 CAPTURE_MODE=allocstack RUN_STAMP="$stamp" \
  OUTPUT_DIR="$job/census-$variant" \
  flock -o /root/jobs/fx/cx/measure.lock bash /root/jobs/fx/cx/v21/s033/salloc/census.sh
 python3 /root/jobs/fx/cx/v21/s033/salloc/analyze.py "$job/census-$variant" "$job/$variant.json" > "$job/census-$variant-analysis.log"
done
