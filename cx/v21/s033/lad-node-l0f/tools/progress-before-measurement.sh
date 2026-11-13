#!/usr/bin/env bash
set -euo pipefail
label="$1"
WT=/root/worktree/zlink-v21-s033/lad-node
JOB=/root/jobs/fx/cx/v21/s033/lad-node-l0f
{
  printf '\n## Before %s\nHEAD=%s dirty=%s base=%s\n' "$label" \
    "$(git -C "$WT" rev-parse HEAD)" \
    "$(if [[ -z "$(git -C "$WT" status --porcelain)" ]]; then echo false; else echo true; fi)" \
    "$(git -C "$WT" merge-base HEAD origin/main)"
  for p in framework/bench/grpc/node/zlink-framework-server/main.js framework/bench/grpc/node/run_local.sh framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js framework/languages/node/packages/framework-codec-protobuf/dist/server/framework.cjs framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 .artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz; do
    printf '%s sha256=%s\n' "$p" "$(sha256sum "$WT/$p" | cut -d' ' -f1)"
  done
  printf 'Binding receive `zlink-raw-server/main.js:64-90,121-142`:\n'
  nl -ba "$WT/framework/bench/grpc/node/zlink-raw-server/main.js" | sed -n '64,90p;121,142p'
  printf 'L0 receive/reply `zlink-framework-server/main.js:76-105`:\n'
  nl -ba "$WT/framework/bench/grpc/node/zlink-framework-server/main.js" | sed -n '76,105p'
  printf 'Reply: each path decodes the request body, records ordinary JS metrics, makes a new protobuf body and response operation, and submits a fresh envelope plus body. No per-message wait, atomics, or request-part copy; batch=256, setImmediate pump. L0 directly calls handleL0. Variant=%s.\n' "$label"
} >> "$JOB/progress.md"
