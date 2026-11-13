#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-m1
WORKTREE=/root/worktree/zlink-v21-s033/lad-node
RUNNER="$WORKTREE/framework/bench/grpc/node/run_local.sh"
left="$1"; right="$2"; pattern="$3"; payload="$4"
for variant in "$left" "$right"; do
  impl=zlink-framework-node
  args=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1)
  [[ "$variant" == L0R ]] && args+=(ZLINK_LADDER_L0R=1)
  [[ "$variant" == L1 ]] && args+=(ZLINK_LADDER_L1_PERMIT=1)
  "$JOB/tools/progress-before-measurement.sh" "confirm-$variant-$pattern-$payload" "$variant"
  env -u ZLINK_LADDER_L0R -u ZLINK_LADDER_L1_PERMIT "${args[@]}" \
    IMPLEMENTATIONS="$impl" BENCH_TARGET_SERVER="$impl" PATTERNS="$pattern" PAYLOAD_SIZES="$payload" \
    RUNS=3 SKIP_BUILD=1 WARMUP_SECONDS=3 DURATION_SECONDS=5 SOURCE_CPUS=0-7 TARGET_CPUS=8-15 \
    OUTPUT_DIR="$JOB/raw/session/confirm/$variant" RUN_STAMP="4350-4351-confirm-$variant-$pattern-$payload" \
    bash "$RUNNER" >"$JOB/raw/confirm-$variant-$pattern-$payload-driver.log" 2>&1
  python3 - "$JOB/raw/session/confirm/$variant" "$impl" "$pattern" "$payload" <<'PY'
import json,pathlib,sys
base=pathlib.Path(sys.argv[1]); impl,pattern,payload=sys.argv[2],sys.argv[3],int(sys.argv[4])
for i in range(1,4):
 p=base/f'run{i}/{impl}-{pattern}-{payload}/results.json'; c=json.loads(p.read_text())['cells'][0]
 if c['errors'] or c['server_errors'] or c['target_stats']['errors'] or c['submitted']!=c['completed'] or c['submitted']!=c['target_stats']['received']: raise SystemExit(f'broken result: {p}')
PY
done
