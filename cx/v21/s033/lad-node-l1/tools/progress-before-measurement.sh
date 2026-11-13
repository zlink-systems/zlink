#!/usr/bin/env bash
set -euo pipefail
label="$1"
variant="$2"
JOB=/root/jobs/fx/cx/v21/s033/lad-node-l1
WORKTREE=/root/worktree/zlink-v21-s033/lad-node
{
  printf '\n## Before %s\n' "$label"
  printf 'built commit=%s dirty=%s base=%s\n' \
    "$(git -C "$WORKTREE" rev-parse HEAD)" \
    "$(test -z "$(git -C "$WORKTREE" status --porcelain)" && echo false || echo true)" \
    "$(git -C "$WORKTREE" merge-base HEAD origin/main)"
  for path in \
    framework/bench/grpc/node/zlink-framework-server/main.js \
    framework/languages/node/packages/framework/dist/runtime/host/application-job-queue.js \
    framework/languages/node/packages/framework/dist/index.js \
    framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node \
    framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0; do
    [[ -f "$WORKTREE/$path" ]]
    case "$(realpath "$WORKTREE/$path")" in "$WORKTREE"/*) ;; *) exit 1 ;; esac
    printf '%s sha256=%s\n' "$path" "$(sha256sum "$WORKTREE/$path" | cut -d' ' -f1)"
  done
  printf '\nBinding receive/reply: zlink-raw-server/main.js:64-117\n'
  nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-raw-server/main.js" | sed -n '64,117p'
  printf '\nL0 receive/reply: zlink-framework-server/main.js:93-155\n'
  nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-framework-server/main.js" | sed -n '93,155p'
  if [[ "$variant" == L1 ]]; then
    printf '\nL1 permit receive/reply: zlink-framework-server/main.js:157-214\n'
    nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-framework-server/main.js" | sed -n '157,214p'
  fi
  printf '\nPer-message waits/allocations/atomics/copies: binding and L0/L0R use one reusable Received, DontWait, batch 256, setImmediate; no queue wait, no JS atomics. L1 awaits ApplicationJobQueue.acquire(remote) before each receive and releases at handler entry. All paths decode the protobuf body, allocate a new encoded body and reply operation, and submit two parts; they do not copy or reuse request parts.\n'
} >> "$JOB/progress.md"
