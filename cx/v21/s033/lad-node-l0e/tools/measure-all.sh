#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-l0e
WORKTREE=/root/worktree/zlink-v21-s033/lad-node
RUNNER="$JOB/tools/node-run-local.sh"
RUN_GROUP="$JOB/tools/run-group.sh"
record_progress() {
  local label="$1" dirty=false path
  [[ -z "$(git -C "$WORKTREE" status --porcelain)" ]] || dirty=true
  {
    printf '\n## Before %s\n' "$label"
    printf 'commit=%s dirty=%s base=%s\n' \
      "$(git -C "$WORKTREE" rev-parse HEAD)" "$dirty" \
      "$(git -C "$WORKTREE" merge-base HEAD origin/main)"
    printf 'Node sources are executed directly from this worktree; framework build outputs and native binding hashes:\n'
    for path in \
      framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js \
      framework/languages/node/packages/nestjs/dist/index.js \
      framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node \
      framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 \
      .artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz; do
      [[ -f "$WORKTREE/$path" ]] || { echo "missing worktree artifact: $path" >&2; return 1; }
      case "$(realpath "$WORKTREE/$path")" in "$WORKTREE"/*) ;; *) echo "artifact outside worktree: $path" >&2; return 1 ;; esac
      printf '%s sha256=%s\n' "$path" "$(sha256sum "$WORKTREE/$path" | cut -d' ' -f1)"
    done
    [[ "$(node -p "require('$WORKTREE/framework/languages/node/node_modules/@zlink-systems/zlink/provenance/core-package-provenance.json').version")" == 1.25.0 ]]
    printf '\nBinding receive excerpt `zlink-raw-server/main.js:64-90`:\n'
    nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-raw-server/main.js" | sed -n '64,90p'
    nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-raw-server/main.js" | sed -n '121,142p'
    printf '\nL0 receive excerpt `zlink-framework-server/main.js:59-105`:\n'
    nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-framework-server/main.js" | sed -n '59,105p'
    printf '\nBinding reply excerpt `zlink-raw-server/main.js:77-90` and L0 reply excerpt `zlink-framework-server/main.js:74-86`:\n'
    nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-raw-server/main.js" | sed -n '77,90p'
    nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-framework-server/main.js" | sed -n '74,86p'
    printf '\nPer message: both receive into one reusable `Received`, no wait, and drain up to 256 messages before `setImmediate`; both call the same ordinary JS metrics counters at `shared/bench-server-metrics.js:33-46` (0 atomics). Each request decodes the existing body, allocates one new protobuf-encoded body Buffer and one response operation/message, submits a new envelope plus body as two parts; no request parts are copied/reused. L0 adds no per-message work besides its direct `handleL0` call in the measured raw-wire cells.\n'
  } >> "$JOB/progress.md"
}
label=l0e-six-cells
record_progress "$label"
PATTERNS=send-saturation,request-serial,request-backpressure \
PAYLOAD_SIZES=1024,4096 RUNS=3 IMPLEMENTATIONS=zlink-node,zlink-framework-node \
ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1 \
  bash "$RUN_GROUP" "$label" "$RUNNER" "$JOB/raw/six-cells/bench/node"
