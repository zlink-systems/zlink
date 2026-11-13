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
    printf '\n## Before %s\ncommit=%s dirty=%s base=%s\n' "$label" \
      "$(git -C "$WORKTREE" rev-parse HEAD)" "$dirty" \
      "$(git -C "$WORKTREE" merge-base HEAD origin/main)"
    for path in framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js framework/languages/node/packages/nestjs/dist/index.js framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0 .artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz; do
      [[ -f "$WORKTREE/$path" ]] || { echo "missing worktree artifact: $path" >&2; return 1; }
      case "$(realpath "$WORKTREE/$path")" in "$WORKTREE"/*) ;; *) echo "artifact outside worktree: $path" >&2; return 1 ;; esac
      printf '%s sha256=%s\n' "$path" "$(sha256sum "$WORKTREE/$path" | cut -d' ' -f1)"
    done
    printf '\nBinding receive `zlink-raw-server/main.js:64-90` and L0 receive `zlink-framework-server/main.js:59-105` excerpts:\n'
    nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-raw-server/main.js" | sed -n '64,90p'
    nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-raw-server/main.js" | sed -n '121,142p'
    nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-framework-server/main.js" | sed -n '59,105p'
    printf '\nBinding reply `zlink-raw-server/main.js:77-90` and L0 reply `zlink-framework-server/main.js:74-86` excerpts:\n'
    nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-raw-server/main.js" | sed -n '77,90p'
    nl -ba "$WORKTREE/framework/bench/grpc/node/zlink-framework-server/main.js" | sed -n '74,86p'
    nl -ba "$WORKTREE/framework/bench/grpc/node/shared/bench-server-metrics.js" | sed -n '32,46p'
    printf '\nPer message: no waits; ordinary JS counters in `shared/bench-server-metrics.js:33-46` and no atomics; drain max 256; decode one body, allocate one new protobuf body, create a new response operation and submit a new envelope plus body as two parts. No request parts are copied/reused.\n'
  } >> "$JOB/progress.md"
}
run_pair() {
  local label="$1" output="$2" source="$3" target="$4"; shift 4
  record_progress "$label"
  env PATTERNS=send-saturation PAYLOAD_SIZES=1024 RUNS=3 IMPLEMENTATIONS="$source" BENCH_TARGET_SERVER="$target" "$@" \
    bash "$RUN_GROUP" "$label" "$RUNNER" "$output"
}
run_pair l0e-cross-raw-raw "$JOB/raw/cross-l0-client/raw-raw/bench/node" zlink-node zlink-node
run_pair l0e-cross-L0-raw-client "$JOB/raw/cross-l0-client/l0-raw/bench/node" zlink-node zlink-framework-node ZLINK_LADDER_L0_SERVER=1
run_pair l0e-cross-L0-client "$JOB/raw/cross-l0-client/l0-client/bench/node" zlink-framework-node zlink-framework-node ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1
