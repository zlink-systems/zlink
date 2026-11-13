#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-m2
BASE="$JOB/raw/session"
python3 "$JOB/tools/aggregate-session.py"
mapfile -t pairs < <(python3 - "$JOB/raw/aggregates/session.json" <<'PY'
import json,sys
rows=json.load(open(sys.argv[1]))['rows']
serial=next(r for r in rows if (r['pattern'],r['payload'])==('request-serial',4096))
if serial['module_cost_l0r_vs_l0']>0.02:
  print('module L0 L0R request-serial 4096')
step=[r for r in rows if r['l1_loss_l1_vs_l0']>0.07]
if step:
  r=max(step,key=lambda x:x['l1_loss_l1_vs_l0'])
  print(f"permit L0 L1 {r['pattern']} {r['payload']}")
PY
)
for pair_spec in "${pairs[@]}"; do
  read -r reason left right pattern payload <<<"$pair_spec"
  for variant in "$left" "$right"; do
    "$JOB/tools/progress-before-measurement.sh" "profile-$reason-$variant-$pattern-$payload" "$variant"
    profile_dir="$JOB/raw/profile/$reason/$variant"
    mkdir -p "$profile_dir"
    variant_env=()
    [[ "$variant" == L0R ]] && variant_env+=(ZLINK_LADDER_L0R=1)
    [[ "$variant" == L1 ]] && variant_env+=(ZLINK_LADDER_L1_PERMIT=1)
    env -u ZLINK_LADDER_L0R -u ZLINK_LADDER_L1_PERMIT \
      ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1 "${variant_env[@]}" \
      NODE_OPTIONS="--cpu-prof --cpu-prof-dir=$profile_dir --require=$JOB/tools/memory-at-sigterm.cjs" \
      IMPLEMENTATIONS=zlink-framework-node BENCH_TARGET_SERVER=zlink-framework-node \
      PATTERNS="$pattern" PAYLOAD_SIZES="$payload" RUNS=1 SKIP_BUILD=1 \
      WARMUP_SECONDS=3 DURATION_SECONDS=5 SOURCE_CPUS=0-7 TARGET_CPUS=8-15 \
      OUTPUT_DIR="$JOB/raw/profile-bench/$reason/$variant" RUN_STAMP="4351-profile-$reason-$variant-$pattern-$payload" \
      bash /root/worktree/zlink-v21-s033/lad-node/framework/bench/grpc/node/run_local.sh \
      >"$JOB/raw/profile-$reason-$variant-driver.log" 2>&1
  done
done
