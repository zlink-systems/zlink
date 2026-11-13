#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-l1
WORKTREE=/root/worktree/zlink-v21-s033/lad-node
RUNNER="$WORKTREE/framework/bench/grpc/node/run_local.sh"
PROGRESS="$JOB/tools/progress-before-measurement.sh"
BASE="$JOB/raw/session"
variants=(binding L0 L0R L1)
patterns=(send-saturation request-serial request-backpressure)
payloads=(1024 4096)
run_cell() {
  local variant="$1" pattern="$2" payload="$3" runs="$4" phase="$5"
  local impl=zlink-node
  local -a ladder_env=()
  if [[ "$variant" != binding ]]; then
    impl=zlink-framework-node
    ladder_env+=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1)
    [[ "$variant" == L0R ]] && ladder_env+=(ZLINK_LADDER_L0R=1)
    [[ "$variant" == L1 ]] && ladder_env+=(ZLINK_LADDER_L1_PERMIT=1)
  fi
  "$PROGRESS" "$phase-$variant-$pattern-$payload" "$variant"
  env -u ZLINK_LADDER_L0R -u ZLINK_LADDER_L1_PERMIT \
    "${ladder_env[@]}" \
    IMPLEMENTATIONS="$impl" BENCH_TARGET_SERVER="$impl" \
    PATTERNS="$pattern" PAYLOAD_SIZES="$payload" RUNS="$runs" SKIP_BUILD=1 \
    WARMUP_SECONDS=30 DURATION_SECONDS=5 SOURCE_CPUS=0-7 TARGET_CPUS=8-15 \
    OUTPUT_DIR="$BASE/$phase/$variant" RUN_STAMP="4351-$phase-$variant-$pattern-$payload" \
    bash "$RUNNER" >"$JOB/raw/${phase}-${variant}-${pattern}-${payload}-driver.log" 2>&1 &
  local runner_pid=$!
  local result="$BASE/$phase/$variant/run1/$impl-$pattern-$payload/results.json"
  while kill -0 "$runner_pid" 2>/dev/null && [[ ! -f "$result" ]]; do sleep 0.5; done
  if [[ -f "$result" ]]; then
    python3 - "$JOB/progress.md" "$result" "$variant" "$pattern" "$payload" <<'PY'
import json, pathlib, sys
progress,raw,variant,pattern,payload=sys.argv[1:]
c=json.loads(pathlib.Path(raw).read_text())['cells'][0]
rel=pathlib.Path(raw).relative_to('/root/jobs/fx/cx/v21/s033/lad-node-l1')
with open(progress,'a') as f:
  f.write(f"\nFirst result {variant} {pattern}-{payload}: {c['throughput_per_second']} msg/s; errors={c['errors']} server_errors={c['server_errors']}; submitted={c['submitted']} completed={c['completed']} target_received={c['target_stats']['received']}. Raw: {rel}.\n")
PY
  fi
  wait "$runner_pid"
  python3 - "$BASE/$phase/$variant" "$impl" "$pattern" "$payload" "$runs" <<'PY'
import json, pathlib, sys
base=pathlib.Path(sys.argv[1]); impl,pattern,payload,runs=sys.argv[2],sys.argv[3],int(sys.argv[4]),int(sys.argv[5])
for run in range(1,runs+1):
  path=base/f'run{run}'/f'{impl}-{pattern}-{payload}'/'results.json'
  c=json.loads(path.read_text())['cells'][0]
  if c['errors'] or c['server_errors'] or c['target_stats']['errors'] or c['submitted']!=c['completed'] or c['submitted']!=c['target_stats']['received']:
    raise SystemExit(f'broken result: {path}')
PY
}
for pattern in "${patterns[@]}"; do
  for payload in "${payloads[@]}"; do
    for variant in "${variants[@]}"; do run_cell "$variant" "$pattern" "$payload" 3 first3; done
  done
done
python3 - "$BASE" "$(dirname "$0")/run-one.sh" <<'PY'
import json, pathlib, subprocess, sys
base=pathlib.Path(sys.argv[1]); run_one=sys.argv[2]; variants=('binding','L0','L0R','L1')
patterns=('send-saturation','request-serial','request-backpressure'); payloads=(1024,4096)
repeat=set()
for pattern in patterns:
  for payload in payloads:
    key=(pattern,payload)
    if key==('request-serial',4096): repeat.add(key); continue
    for variant in variants:
      vals=[]
      for run in range(1,4):
        p=base/'first3'/variant/f'run{run}'/f'{"zlink-node" if variant=="binding" else "zlink-framework-node"}-{pattern}-{payload}'/'results.json'
        data=json.loads(p.read_text())['cells'][0]
        if data['errors'] or data['server_errors'] or data['target_stats']['errors'] or data['submitted']!=data['completed'] or data['submitted']!=data['target_stats']['received']:
          raise SystemExit(f'broken result: {p}')
        vals.append(data['throughput_per_second'])
      med=sorted(vals)[1]; spread=max(abs(x-med) for x in vals)/med
      if spread>0.10: repeat.add(key)
for pattern,payload in sorted(repeat):
  print(f'repeat {pattern}-{payload}')
  for variant in variants:
    subprocess.run([run_one,variant,pattern,str(payload)],check=True)
PY
"$JOB/tools/profile-if-needed.sh"
