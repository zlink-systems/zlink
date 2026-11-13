#!/usr/bin/env bash
set -euo pipefail

JOB=/root/jobs/fx/cx/v21/s033/lad-node-l0e
WORKTREE=/root/worktree/zlink-v21-s033/lad-node
RUNNER="$JOB/tools/node-run-local.sh"
RUN_GROUP="$JOB/tools/run-group.sh"
BIN_PATHS=(
  framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js
  framework/languages/node/packages/nestjs/dist/index.js
  framework/bench/grpc/node/client/main.js
  framework/bench/grpc/node/client/framework-transport.js
  framework/bench/grpc/node/zlink-framework-server/main.js
  framework/bench/grpc/node/zlink-framework-server/l0-worker.js
  framework/bench/grpc/node/zlink-framework-server/bench-handlers.js
  framework/bench/grpc/node/shared/raw-wire.js
  framework/bench/grpc/node/shared/bench-server-metrics.js
  framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node
  framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0
)

record_progress() {
  local label="$1" commit dirty base
  commit="$(git -C "$WORKTREE" rev-parse HEAD)"
  base="$(git -C "$WORKTREE" merge-base HEAD origin/main)"
  if [[ -n "$(git -C "$WORKTREE" status --porcelain)" ]]; then dirty=true; else dirty=false; fi
  local entries=()
  local path
  for path in "${BIN_PATHS[@]}"; do
    [[ -f "$WORKTREE/$path" ]] || { echo "missing binary: $WORKTREE/$path" >&2; return 1; }
    case "$(realpath "$WORKTREE/$path")" in
      "$WORKTREE"/*) ;;
      *) echo "binary is outside this worktree: $path -> $(realpath "$WORKTREE/$path")" >&2; return 1 ;;
    esac
    entries+=("$WORKTREE/$path=$(sha256sum "$WORKTREE/$path" | cut -d' ' -f1)")
  done
  local installed_core
  installed_core="$(node -p "require('$WORKTREE/framework/languages/node/node_modules/@zlink-systems/zlink/provenance/core-package-provenance.json').version")"
  [[ "$installed_core" == "1.25.0" ]] || {
    echo "installed Node binding Core is $installed_core, expected 1.25.0" >&2
    return 1
  }
  local package="$WORKTREE/.artifacts/wsl/npm/zlink-systems-zlink-1.25.0.tgz"
  [[ -f "$package" ]] || { echo "missing worktree-built binding package: $package" >&2; return 1; }
  printf '%s | commit=%s dirty=%s base=%s binary_origin=local-package-build-wsl.sh-from-worktree core=%s package_sha256=%s binaries=%s\n' \
    "$label" "$commit" "$dirty" "$base" "$installed_core" \
    "$(sha256sum "$package" | cut -d' ' -f1)" "$(IFS=';'; echo "${entries[*]}")" \
    >>"$JOB/progress.md"
  printf '%s | binding reply `zlink-raw-server/main.js:79-86`: %s | L0 reply `zlink-framework-server/l0-worker.js:50-58`: %s\n' \
    "$label" \
    "$(sed -n '79,86p' "$WORKTREE/framework/bench/grpc/node/zlink-raw-server/main.js" | tr '\n' ' ')" \
    "$(sed -n '50,58p' "$WORKTREE/framework/bench/grpc/node/zlink-framework-server/l0-worker.js" | tr '\n' ' ')" \
    >>"$JOB/progress.md"
  printf '%s | per message, both decode the body from received Message.data(), serialize a new BenchPayload with protobuf into a new Buffer, copy it into a new zlink.Message, and submit two parts with the fixed response envelope; L0 reads/parses the header only for framework-wire cross tests.\n' \
    "$label" >>"$JOB/progress.md"
}

run_group() {
  local label="$1" output="$2" source_impl="$3" target_impl="$4"
  shift 4
  record_progress "$label"
  local -a env_args=(PATTERNS=send-saturation,request-serial,request-backpressure
    PAYLOAD_SIZES=1024,4096 RUNS=3 IMPLEMENTATIONS="$source_impl")
  [[ -z "$target_impl" ]] || env_args+=(BENCH_TARGET_SERVER="$target_impl")
  if [[ "$label" == l0-final-binding ]]; then
    local runner_pid
    env "${env_args[@]}" "$@" bash "$RUN_GROUP" "$label" "$RUNNER" "$output" &
    runner_pid=$!
    local first_l0_result="$output/run1/zlink-framework-node-send-saturation-1024/results.json"
    while [[ ! -f "$first_l0_result" ]]; do
      if ! kill -0 "$runner_pid" 2>/dev/null; then
        wait "$runner_pid"
      fi
      sleep 1
    done
    local first_l0_rate
    first_l0_rate="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["cells"][0]["throughput_per_second"])' "$first_l0_result")"
    printf 'first L0 send-saturation 1K=%s msg/s raw=%s\n' \
      "$first_l0_rate" "$first_l0_result" >>"$JOB/progress.md"
    wait "$runner_pid"
  else
    env "${env_args[@]}" "$@" bash "$RUN_GROUP" "$label" "$RUNNER" "$output"
  fi
}

run_group l0-final-binding "$JOB/raw/l0-final/bench/node" \
  zlink-node,zlink-framework-node "" \
  ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1

