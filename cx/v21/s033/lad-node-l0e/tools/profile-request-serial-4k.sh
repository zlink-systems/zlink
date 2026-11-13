#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-l0e
WORKTREE=/root/worktree/zlink-v21-s033/lad-node
RUNNER="$JOB/tools/node-run-local.sh"
RUN_GROUP="$JOB/tools/run-group.sh"
label=profile-request-serial-4k
{
  printf '\n## Before %s\ncommit=%s dirty=%s base=%s\n' "$label" \
    "$(git -C "$WORKTREE" rev-parse HEAD)" \
    "$(if [[ -z "$(git -C "$WORKTREE" status --porcelain)" ]]; then echo false; else echo true; fi)" \
    "$(git -C "$WORKTREE" merge-base HEAD origin/main)"
  for path in framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js framework/languages/node/packages/nestjs/dist/index.js framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 .artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz; do
    [[ -f "$WORKTREE/$path" ]] || { echo "missing worktree artifact: $path" >&2; exit 1; }
    case "$(realpath "$WORKTREE/$path")" in "$WORKTREE"/*) ;; *) echo "artifact outside worktree: $path" >&2; exit 1 ;; esac
    printf '%s sha256=%s\n' "$path" "$(sha256sum "$WORKTREE/$path" | cut -d' ' -f1)"
  done
  printf '\nBinding receive and idle excerpts `zlink-raw-server/main.js:64-90,121-142`:\n'
  nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-raw-server/main.js" | sed -n '64,90p;121,142p'
  printf '\nL0 receive and idle excerpt `zlink-framework-server/main.js:59-105`:\n'
  nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-framework-server/main.js" | sed -n '59,105p'
  printf '\nBinding and L0 reply excerpts `zlink-raw-server/main.js:77-90`; `zlink-framework-server/main.js:74-86`:\n'
  nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-raw-server/main.js" | sed -n '77,90p'
  nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-framework-server/main.js" | sed -n '74,86p'
  printf '\nPer message: no waits or atomics; batch limit 256; same BenchServerMetrics counters; one decoded body, one new protobuf body Buffer, new response operation, fixed envelope and two-part submit. L0 has one direct handleL0 call. This is a profiled diagnostic run; its throughput is not used in the six-cell comparison.\n'
} >> "$JOB/progress.md"
mkdir -p "$JOB/raw/profile/request-serial-4k"
env NODE_OPTIONS="--cpu-prof --cpu-prof-dir=$JOB/raw/profile/request-serial-4k" \
  PATTERNS=request-serial PAYLOAD_SIZES=4096 RUNS=1 \
  IMPLEMENTATIONS=zlink-node,zlink-framework-node \
  ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1 \
  bash "$RUN_GROUP" "$label" "$RUNNER" "$JOB/raw/profile/request-serial-4k/bench/node"
