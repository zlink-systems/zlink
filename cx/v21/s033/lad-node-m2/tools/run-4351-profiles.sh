#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-m2
WORKTREE=/root/worktree/zlink-v21-s033/lad-node
RUNNER="$WORKTREE/framework/bench/grpc/node/run_local.sh"
BASE="$JOB/raw"

mapfile -t tasks < <(python3 - "$JOB/raw/aggregates/confirm-4351.json" <<'PY'
import json,sys
for r in json.load(open(sys.argv[1]))['rows']:
    if r['pattern']=='request-serial' and 1-r['medians']['L0R']/r['medians']['L0'] > .02:
        print(f"module-{r['payload']} L0 L0R request-serial {r['payload']}")
    if 1-r['medians']['L1']/r['medians']['L0'] > .07:
        print(f"permit-{r['pattern']}-{r['payload']} L0 L1 {r['pattern']} {r['payload']}")
PY
)

for task in "${tasks[@]}"; do
  read -r reason left right pattern payload <<<"$task"
  for variant in "$left" "$right"; do
    profile_dir="$BASE/profile/4351/$reason/$variant"
    output_dir="$BASE/profile-bench/4351/$reason/$variant"
    mkdir -p "$profile_dir"
    variant_env=()
    [[ "$variant" == L0R ]] && variant_env+=(ZLINK_LADDER_L0R=1)
    [[ "$variant" == L1 ]] && variant_env+=(ZLINK_LADDER_L1_PERMIT=1)
    "$JOB/tools/progress-before-measurement.sh" "profile-$reason-$variant" "$variant"
    started="$(date -u +%FT%TZ)"
    printf '\nProfile run start %s %s-%s: %s\n' "$reason" "$variant" "$pattern-$payload" "$started" >> "$JOB/progress.md"
    env -u ZLINK_LADDER_L0R -u ZLINK_LADDER_L1_PERMIT \
      ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1 "${variant_env[@]}" \
      NODE_OPTIONS="--cpu-prof --cpu-prof-dir=$profile_dir --require=$JOB/tools/memory-at-sigterm.cjs" \
      IMPLEMENTATIONS=zlink-framework-node BENCH_TARGET_SERVER=zlink-framework-node \
      PATTERNS="$pattern" PAYLOAD_SIZES="$payload" RUNS=1 SKIP_BUILD=1 \
      WARMUP_SECONDS=3 DURATION_SECONDS=5 SOURCE_CPUS=0-7 TARGET_CPUS=8-15 \
      OUTPUT_DIR="$output_dir" RUN_STAMP="4351-profile-$reason-$variant" \
      bash "$RUNNER" >"$JOB/raw/profile-4351-$reason-$variant-driver.log" 2>&1
    raw="$output_dir/run1/zlink-framework-node-$pattern-$payload/results.json"
    python3 - "$raw" "$JOB/progress.md" "$reason" "$variant" <<'PY'
import json,pathlib,sys
raw,progress,reason,variant=sys.argv[1:]
p=pathlib.Path(raw); c=json.loads(p.read_text())['cells'][0]
if c['errors'] or c['server_errors'] or c['target_stats']['errors'] or c['submitted']!=c['completed'] or c['submitted']!=c['target_stats']['received']:
    raise SystemExit(f'broken profile result: {p}')
with open(progress,'a') as f:
    f.write(f"\nProfile result {reason} {variant}: {c['throughput_per_second']} msg/s; errors=0 submitted=completed=received={c['submitted']}. Raw: {p.relative_to('/root/jobs/fx/cx/v21/s033/lad-node-m2')}.\n")
PY
  done
done
