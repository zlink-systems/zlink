#!/usr/bin/env bash
set -euo pipefail
TOOLS=/root/jobs/fx/cx/v21/s033/lad-node-s1/tools/node
JOB=/root/jobs/fx/cx/v21/s033/lad-node-s1
ROOT=/root/worktree/zlink-v21-s033/lad-node
variant=$1 pattern=$2 size=$3 phase=$4
impl=zlink-node
runner=$TOOLS/run_local_binding.sh
env_args=()
if [[ "$variant" != binding ]]; then
  impl=zlink-framework-node
  runner=$TOOLS/run_local_framework.sh
  case "$variant" in
    L0) env_args+=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1) ;;
    L1) env_args+=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1 ZLINK_LADDER_L1_PERMIT=1) ;;
    L2) env_args+=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1 ZLINK_LADDER_L2_ENVELOPE=1 ZLINK_LADDER_L2_CLIENT=1) ;;
    L3) env_args+=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1 ZLINK_LADDER_L3_SERVICE_WIRE=1 ZLINK_LADDER_L3_CLIENT=1) ;;
    L4) env_args+=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1 ZLINK_LADDER_L4_SERVER=1 ZLINK_LADDER_L4_CLIENT=1) ;;
    L5_7) env_args+=(ZLINK_LADDER_L5_7_SERVER=1 ZLINK_LADDER_L5_7_CLIENT=1) ;;
    *) echo "unknown variant $variant" >&2; exit 2 ;;
  esac
fi
stage_root="$JOB/raw/node/$variant/$pattern-$size/$phase"
attempt=1
while [[ -e "$stage_root/attempt$attempt" ]]; do attempt=$((attempt+1)); done
out="$stage_root/attempt$attempt"
mkdir -p "$out"
# Record the exact state and the required parity excerpts before every warm or counted run.
commit=$(git -C "$ROOT" rev-parse HEAD)
dirty=clean; [[ -z "$(git -C "$ROOT" status --porcelain)" ]] || dirty=dirty
base=$(git -C "$ROOT" merge-base HEAD origin/main)
addon="$ROOT/framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node"
addon_sha=$(sha256sum "$addon" | awk '{print $1}')
framework_art="$ROOT/framework/languages/node/packages/framework/dist/runtime/channels/channel-dispatchers.js"
framework_sha=$(sha256sum "$framework_art" | awk '{print $1}')
{
  printf '\n- %s %s %s phase=%s: HEAD=%s dirty=%s merge_base=%s; addon=%s sha256=%s; framework_artifact=%s sha256=%s. ' "$(date -u +%FT%TZ)" "$variant" "$pattern-$size" "$phase" "$commit" "$dirty" "$base" "$addon" "$addon_sha" "$framework_art" "$framework_sha"
  printf 'call-shape=call-shape.md; receive binding `zlink-raw-server/main.js:47-53,64-75` vs L0 `zlink-framework-server/main.js:142-155` (DontWait/NoData/batch); reply binding `zlink-raw-server/main.js:77-86` vs L-step `zlink-framework-server/main.js:116-133,168-176` (L0 new body; L2 envelope; L3 service wire; L4 typed codec; L5~L7 operation path).\n'
} >> "$JOB/progress.md"
if [[ "$variant" == binding ]]; then
  args=(IMPLEMENTATIONS=zlink-node BENCH_TARGET_SERVER=zlink-node)
else
  args=(IMPLEMENTATIONS=zlink-framework-node BENCH_TARGET_SERVER=zlink-framework-node)
fi
stamp="lad-node-s1-${variant}-${pattern}-${size}-${phase}-$(date -u +%H%M%S)"
env -u ZLINK_LADDER_L0R -u ZLINK_LADDER_L2R_ENVELOPE -u ZLINK_LADDER_L3R_SERVICE_WIRE \
  "${env_args[@]}" "${args[@]}" PATTERNS="$pattern" PAYLOAD_SIZES="$size" RUNS=1 SKIP_BUILD=1 \
  WARMUP_SECONDS=3 DURATION_SECONDS=5 SOURCE_CPUS=0-5 TARGET_CPUS=8-13 \
  SCHEDULING_RECORD=1 OUTPUT_DIR="$out" RUN_STAMP="$stamp" \
  bash "$runner" >"$out/driver.log" 2>&1
result="$out/run1/$impl-$pattern-$size/results.json"
sched="$out/run1/$impl-$pattern-$size/scheduling.txt"
[[ -f "$result" && -f "$sched" ]] || { echo "missing raw result or scheduling evidence: $result $sched" >&2; exit 1; }
python3 - "$result" "$sched" "$pattern" <<'PY'
import json, pathlib, sys
result, sched, pattern=sys.argv[1:]
c=json.loads(pathlib.Path(result).read_text())['cells'][0]
t=pathlib.Path(sched).read_text()
assert not c.get('contaminated', False), f'contaminated: {result}'
assert c.get('errors',0)==0 and c.get('server_errors',0)==0 and c['target_stats']['errors']==0
assert c['submitted']==c['completed']==c['target_stats']['received']
assert 'scheduling_invalid=' not in t, f'non-SCHED_OTHER: {sched}'
for phase,expected in [('source_start','0-5'),('target_start','8-13'),('mid_active_source','0-5'),('mid_active_target','8-13')]:
    lines=[line for line in t.splitlines() if line.startswith(phase+'_affinity ') ]
    assert lines, f'missing per-thread affinity for {phase}: {sched}'
    assert all(line.endswith('cpus='+expected) for line in lines), f'wrong affinity {phase}: {lines}'
    tidlines=[line for line in t.splitlines() if line.startswith(phase+'_tid=')]
    assert len(tidlines)==len(lines), f'thread affinity count mismatch {phase}: {sched}'
for phase in ('source_start','target_start','mid_active_source','mid_active_target'):
    assert any('current scheduling policy: SCHED_OTHER' in line for line in t.splitlines()), f'missing SCHED_OTHER evidence: {sched}'
print(c['throughput_per_second'])
PY
