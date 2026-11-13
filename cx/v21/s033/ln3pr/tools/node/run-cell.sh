#!/usr/bin/env bash
set -euo pipefail

JOB=/root/jobs/fx/cx/v21/s033/ln3pr
variant=$1 pattern=$2 size=$3 phase=$4
side=${variant%%-*}
step=${variant#*-}
case "$side" in
  A) ROOT=/root/worktree/zlink-v21-s033/ln3-base ;;
  B) ROOT=/root/worktree/zlink-v21-s033/ln3-pr ;;
  *) echo "unknown side: $side" >&2; exit 2 ;;
esac
impl=zlink-framework-node
runner="$ROOT/framework/bench/grpc/node/run_local.sh"
case "$step" in
  L2) ladder=(ZLINK_LADDER_L2_ENVELOPE=1 ZLINK_LADDER_L2_CLIENT=1) ;;
  L3) ladder=(ZLINK_LADDER_L3_SERVICE_WIRE=1 ZLINK_LADDER_L3_CLIENT=1) ;;
  *) echo "unknown step: $step" >&2; exit 2 ;;
esac
ladder+=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1)

stage_root="$JOB/raw/$side/$step/$pattern-$size/$phase"
attempt=1
while [[ -e "$stage_root/attempt$attempt" ]]; do attempt=$((attempt+1)); done
out="$stage_root/attempt$attempt"
mkdir -p "$out"

commit=$(git -C "$ROOT" rev-parse HEAD)
dirty=clean; [[ -z "$(git -C "$ROOT" status --porcelain)" ]] || dirty=dirty
base=$(git -C "$ROOT" merge-base HEAD origin/main)
dist="$ROOT/framework/languages/node/packages/framework/dist/runtime"
addon="$ROOT/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node"
printf '\n- %s %s %s-%s phase=%s HEAD=%s dirty=%s merge_base=%s addon_sha256=%s\n' \
  "$(date -u +%FT%TZ)" "$variant" "$pattern" "$size" "$phase" "$commit" "$dirty" "$base" "$(sha256sum "$addon" | awk '{print $1}')" >> "$JOB/progress.md"
for f in foundation/service-wire-m6a-codec.js foundation/raw-service-mesh-runtime.js backend/node/node-raw-binding-port.js; do
  printf '  artifact=%s sha256=%s\n' "$dist/$f" "$(sha256sum "$dist/$f" | awk '{print $1}')" >> "$JOB/progress.md"
done
printf '  receive binding zlink-raw-server/main.js:47-53,64-75 vs L2/L3 zlink-framework-server/main.js:142-155; reply binding zlink-raw-server/main.js:77-86 vs L2/L3 zlink-framework-server/main.js:168-176; costs: call-shape.md § Per-message costs.\n' >> "$JOB/progress.md"

env -u ZLINK_LADDER_L0R -u ZLINK_LADDER_L2R_ENVELOPE -u ZLINK_LADDER_L3R_SERVICE_WIRE \
  "${ladder[@]}" IMPLEMENTATIONS="$impl" BENCH_TARGET_SERVER="$impl" \
  PATTERNS="$pattern" PAYLOAD_SIZES="$size" RUNS=1 SKIP_BUILD=1 \
  WARMUP_SECONDS=10 DURATION_SECONDS=5 SOURCE_CPUS=0-5 TARGET_CPUS=8-13 \
  SCHEDULING_RECORD=1 OUTPUT_DIR="$out" RUN_STAMP="ln3pr-${variant}-${pattern}-${size}-${phase}-$(date -u +%H%M%S)" \
  bash "$runner" >"$out/driver.log" 2>&1

result="$out/run1/$impl-$pattern-$size/results.json"
sched="$out/run1/$impl-$pattern-$size/scheduling.txt"
[[ -f "$result" && -f "$sched" ]] || { echo "missing raw result or scheduling evidence: $result $sched" >&2; exit 1; }
python3 - "$result" "$sched" <<'PY'
import json, pathlib, sys
result, sched = map(pathlib.Path, sys.argv[1:])
c=json.loads(result.read_text())['cells'][0]
t=sched.read_text()
assert c.get('errors',0)==0 and c.get('server_errors',0)==0 and c['target_stats']['errors']==0
assert c['submitted']==c['completed']==c['target_stats']['received']
assert 'scheduling_invalid=' not in t
for phase,expected in [('source_start','0-5'),('target_start','8-13'),('mid_active_source','0-5'),('mid_active_target','8-13')]:
    lines=[line for line in t.splitlines() if line.startswith(phase+'_affinity ')]
    assert lines and all(line.endswith('cpus='+expected) for line in lines), f'bad affinity {phase}: {lines}'
    tids=[line for line in t.splitlines() if line.startswith(phase+'_tid=')]
    assert len(tids)==len(lines), f'thread count mismatch {phase}'
policies=[line for line in t.splitlines() if 'current scheduling policy:' in line]
assert policies and all('current scheduling policy: SCHED_OTHER' in line for line in policies)
print(c['throughput_per_second'])
PY
python3 - "$result" "$sched" "$JOB" "$variant" "$pattern" "$size" "$phase" <<'PY'
import json,pathlib,sys
result,sched,job,variant,pattern,size,phase=sys.argv[1:]
c=json.loads(pathlib.Path(result).read_text())['cells'][0]
rel=pathlib.Path(result).relative_to(job)
with open(pathlib.Path(job)/'progress.md','a') as f:
    f.write(f"  result {variant} {pattern}-{size} {phase}={c['throughput_per_second']} msg/s errors=0 submitted=completed=received={c['submitted']} raw={rel} scheduling={pathlib.Path(sched).relative_to(job)}\n")
PY
