#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/l3868
AGG=/root/worktree/zlink-v21-s033/agg-main/framework/bench/grpc/tools/bench_aggregate.py
mkdir -p "$JOB/raw/aggregates"
sha256sum "$AGG" > "$JOB/raw/aggregates/tool.sha256"
for phase in L0; do
  python3 "$AGG" --lang cpp --min-runs 3 --baseline zlink-cpp \
    --runs-glob "$JOB/raw/L0-owner/*/run*" \
    --json-out "$JOB/raw/aggregates/$phase.json" > "$JOB/raw/aggregates/$phase.md"
  for combination in raw-raw framework-raw framework-framework; do
    python3 "$AGG" --lang cpp --min-runs 3 --baseline zlink-cpp --payload-sizes 1024 \
      --runs-glob "$JOB/raw/cross/L0-owner/$combination/run*" \
      --json-out "$JOB/raw/aggregates/cross-$phase-$combination.json" \
      > "$JOB/raw/aggregates/cross-$phase-$combination.md"
  done
done
