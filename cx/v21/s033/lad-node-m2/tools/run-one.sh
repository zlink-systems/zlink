#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-m2
v="$1"; pattern="$2"; payload="$3"
impl=zlink-node; args=()
if [[ "$v" != binding ]]; then
  impl=zlink-framework-node
  args+=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1)
  [[ "$v" == L0R ]] && args+=(ZLINK_LADDER_L0R=1)
  [[ "$v" == L1 ]] && args+=(ZLINK_LADDER_L1_PERMIT=1)
fi
/root/jobs/fx/cx/v21/s033/lad-node-m2/tools/progress-before-measurement.sh "confirm-$v-$pattern-$payload" "$v"
env -u ZLINK_LADDER_L0R -u ZLINK_LADDER_L1_PERMIT "${args[@]}" \
  IMPLEMENTATIONS="$impl" BENCH_TARGET_SERVER="$impl" PATTERNS="$pattern" \
  PAYLOAD_SIZES="$payload" RUNS=1 SKIP_BUILD=1 WARMUP_SECONDS=3 DURATION_SECONDS=5 \
  SOURCE_CPUS=0-7 TARGET_CPUS=8-15 \
  OUTPUT_DIR="$JOB/raw/session/confirm/$v" RUN_STAMP="4351-confirm-$v-$pattern-$payload" \
  bash /root/worktree/zlink-v21-s033/lad-node/framework/bench/grpc/node/run_local.sh \
  >"$JOB/raw/confirm-$v-$pattern-$payload-driver.log" 2>&1 &
runner_pid=$!
result="$JOB/raw/session/confirm/$v/run1/$impl-$pattern-$payload/results.json"
while kill -0 "$runner_pid" 2>/dev/null && [[ ! -f "$result" ]]; do sleep 0.5; done
if [[ -f "$result" ]]; then
  python3 - "$JOB/progress.md" "$result" "$v" "$pattern" "$payload" <<'PY'
import json, pathlib, sys
progress,raw,variant,pattern,payload=sys.argv[1:]
c=json.loads(pathlib.Path(raw).read_text())['cells'][0]
rel=pathlib.Path(raw).relative_to('/root/jobs/fx/cx/v21/s033/lad-node-m2')
with open(progress,'a') as f:
  f.write(f"\nFirst repeat {variant} {pattern}-{payload}: {c['throughput_per_second']} msg/s; errors={c['errors']} server_errors={c['server_errors']}; submitted={c['submitted']} completed={c['completed']} target_received={c['target_stats']['received']}. Raw: {rel}.\n")
PY
fi
wait "$runner_pid"
python3 - "$JOB/raw/session/confirm/$v" "$impl" "$pattern" "$payload" <<'PY'
import json, pathlib, sys
base=pathlib.Path(sys.argv[1]); impl,pattern,payload=sys.argv[2],sys.argv[3],int(sys.argv[4])
for run in (1,2):
  path=base/f'run{run}'/f'{impl}-{pattern}-{payload}'/'results.json'
  cell=json.loads(path.read_text())['cells'][0]
  if cell['errors'] or cell['server_errors'] or cell['target_stats']['errors'] or cell['submitted']!=cell['completed'] or cell['submitted']!=cell['target_stats']['received']:
    raise SystemExit(f'broken result: {path}')
PY
