#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-l0e
WORKTREE=/root/worktree/zlink-v21-s033/lad-node
label=l0e-cross-L0-raw-client-G5-repeat2

{
  printf '\n## Before %s\ncommit=%s dirty=%s base=%s\n' "$label" \
    "$(git -C "$WORKTREE" rev-parse HEAD)" \
    "$([[ -z "$(git -C "$WORKTREE" status --porcelain)" ]] && echo false || echo true)" \
    "$(git -C "$WORKTREE" merge-base HEAD origin/main)"
  printf 'L0 server: zlink-framework-node; client: zlink-node; send-saturation 1K; repeats 4–5.\n'
  printf 'Receive/pump excerpt `zlink-framework-server/main.js:59-105`:\n'
  nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-framework-server/main.js" | sed -n '59,105p'
  printf 'Metrics excerpt `shared/bench-server-metrics.js:33-46`:\n'
  nl -ba "$WORKTREE/framework/bench/grpc/node/shared/bench-server-metrics.js" | sed -n '33,46p'
} >> "$JOB/progress.md"

env RUNS=2 PATTERNS=send-saturation PAYLOAD_SIZES=1024 IMPLEMENTATIONS=zlink-node \
  BENCH_TARGET_SERVER=zlink-framework-node ZLINK_LADDER_L0_SERVER=1 \
  bash "$JOB/tools/run-group.sh" "$label" "$JOB/tools/node-run-local.sh" \
  "$JOB/raw/cross-l0-client/l0-raw/repeats-4-5/bench/node"
