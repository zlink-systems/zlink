#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-m2
WORKTREE=/root/worktree/zlink-v21-s033/lad-node
RUNNER="$WORKTREE/framework/bench/grpc/node/run_local.sh"
BASE="$JOB/raw/session"
variants=(binding L0 L0R L1)
patterns=(send-saturation request-serial request-backpressure)
payloads=(1024 4096)
for pattern in "${patterns[@]}"; do
  for payload in "${payloads[@]}"; do
    for variant in "${variants[@]}"; do
      impl=zlink-node
      ladder_env=()
      if [[ "$variant" != binding ]]; then
        impl=zlink-framework-node
        ladder_env+=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1)
        [[ "$variant" == L0R ]] && ladder_env+=(ZLINK_LADDER_L0R=1)
        [[ "$variant" == L1 ]] && ladder_env+=(ZLINK_LADDER_L1_PERMIT=1)
      fi
      "$JOB/tools/progress-before-measurement.sh" "${variant}-${pattern}-${payload}" "$variant"
      env -u ZLINK_LADDER_L0R -u ZLINK_LADDER_L1_PERMIT \
        "${ladder_env[@]}" IMPLEMENTATIONS="$impl" BENCH_TARGET_SERVER="$impl" \
        PATTERNS="$pattern" PAYLOAD_SIZES="$payload" RUNS=1 SKIP_BUILD=1 \
        WARMUP_SECONDS=3 DURATION_SECONDS=5 SOURCE_CPUS=0-7 TARGET_CPUS=8-15 \
        OUTPUT_DIR="$BASE/$variant" RUN_STAMP="4350-4351-$variant-$pattern-$payload" \
        bash "$RUNNER" >"$JOB/raw/${variant}-${pattern}-${payload}-driver.log" 2>&1
      result="$BASE/$variant/run1/$impl-$pattern-$payload/results.json"
      python3 - "$JOB/progress.md" "$result" "$variant" "$pattern" "$payload" <<'PY'
import json, pathlib, sys
progress,raw,variant,pattern,payload=sys.argv[1:]
c=json.loads(pathlib.Path(raw).read_text())['cells'][0]
rel=pathlib.Path(raw).relative_to('/root/jobs/fx/cx/v21/s033/lad-node-m2')
with open(progress,'a') as f:
  f.write(f"\nFirst result {variant} {pattern}-{payload}: {c['throughput_per_second']} msg/s; errors={c['errors']} server_errors={c['server_errors']}; submitted={c['submitted']} completed={c['completed']} target_received={c['target_stats']['received']}. Raw: {rel}.\n")
PY
      python3 - "$result" <<'PY'
import json,sys
p=sys.argv[1]; c=json.load(open(p))['cells'][0]
if c['errors'] or c['server_errors'] or c['target_stats']['errors'] or c['submitted']!=c['completed'] or c['submitted']!=c['target_stats']['received']:
  raise SystemExit(f'broken result: {p}')
PY
    done
  done
done
