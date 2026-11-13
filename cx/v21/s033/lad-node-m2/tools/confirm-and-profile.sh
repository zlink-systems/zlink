#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-m2
WORKTREE=/root/worktree/zlink-v21-s033/lad-node
RUNNER="$WORKTREE/framework/bench/grpc/node/run_local.sh"
python3 - "$JOB/raw/aggregates/session.json" <<'PY' > "$JOB/raw/confirm-pairs.txt"
import json,sys
for row in json.load(open(sys.argv[1]))['rows']:
  for pair in row['confirm_pairs']:
    print(row['pattern'],row['payload'],pair)
PY
while read -r pattern payload pair; do
  [[ -n "${pattern:-}" ]] || continue
  left="${pair%%-*}"; right="${pair#*-}"
  for variant in "$left" "$right"; do
    impl=zlink-framework-node
    args=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1)
    [[ "$variant" == L0R ]] && args+=(ZLINK_LADDER_L0R=1)
    [[ "$variant" == L1 ]] && args+=(ZLINK_LADDER_L1_PERMIT=1)
    "$JOB/tools/progress-before-measurement.sh" "confirm-$variant-$pattern-$payload" "$variant"
    env -u ZLINK_LADDER_L0R -u ZLINK_LADDER_L1_PERMIT "${args[@]}" \
      IMPLEMENTATIONS="$impl" BENCH_TARGET_SERVER="$impl" PATTERNS="$pattern" PAYLOAD_SIZES="$payload" \
      RUNS=2 SKIP_BUILD=1 WARMUP_SECONDS=3 DURATION_SECONDS=5 SOURCE_CPUS=0-7 TARGET_CPUS=8-15 \
      OUTPUT_DIR="$JOB/raw/session/confirm/$variant" RUN_STAMP="4350-4351-confirm-$variant-$pattern-$payload" \
      bash "$RUNNER" >"$JOB/raw/confirm-$variant-$pattern-$payload-driver.log" 2>&1
    python3 - "$JOB/raw/session/confirm/$variant" "$impl" "$pattern" "$payload" <<'PY'
import json,pathlib,sys
base=pathlib.Path(sys.argv[1]); impl,pattern,payload=sys.argv[2],sys.argv[3],int(sys.argv[4])
for i in (1,2):
 p=base/f'run{i}/{impl}-{pattern}-{payload}/results.json'; c=json.loads(p.read_text())['cells'][0]
 if c['errors'] or c['server_errors'] or c['target_stats']['errors'] or c['submitted']!=c['completed'] or c['submitted']!=c['target_stats']['received']: raise SystemExit(f'broken result: {p}')
PY
  done
done < "$JOB/raw/confirm-pairs.txt"

# The module-load comparison is specifically requested for request-serial 4K.
for variant in L0 L0R; do
  profile_dir="$JOB/raw/profile/module-load/$variant"
  mkdir -p "$profile_dir"
  args=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1)
  [[ "$variant" == L0R ]] && args+=(ZLINK_LADDER_L0R=1)
  "$JOB/tools/progress-before-measurement.sh" "profile-module-$variant-request-serial-4096" "$variant"
  env -u ZLINK_LADDER_L0R -u ZLINK_LADDER_L1_PERMIT "${args[@]}" \
    NODE_OPTIONS="--cpu-prof --cpu-prof-dir=$profile_dir --require=$JOB/tools/memory-at-sigterm.cjs" \
    IMPLEMENTATIONS=zlink-framework-node BENCH_TARGET_SERVER=zlink-framework-node PATTERNS=request-serial PAYLOAD_SIZES=4096 \
    RUNS=1 SKIP_BUILD=1 WARMUP_SECONDS=3 DURATION_SECONDS=5 SOURCE_CPUS=0-7 TARGET_CPUS=8-15 \
    OUTPUT_DIR="$JOB/raw/profile-bench/module-$variant" RUN_STAMP="4350-module-profile-$variant-request-serial-4096" \
    bash "$RUNNER" >"$JOB/raw/profile-module-$variant-driver.log" 2>&1
done
