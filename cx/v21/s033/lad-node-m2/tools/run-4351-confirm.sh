#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-m2
WORKTREE=/root/worktree/zlink-v21-s033/lad-node
RUNNER="$WORKTREE/framework/bench/grpc/node/run_local.sh"
BASE="$JOB/raw/session/confirm"

run_one() {
  local variant="$1" pattern="$2" payload="$3" iteration="$4"
  local impl=zlink-framework-node
  local variant_env=()
  if [[ "$variant" == binding ]]; then
    impl=zlink-node
  else
    variant_env+=(ZLINK_LADDER_L0_SERVER=1 ZLINK_LADDER_L0_CLIENT=1)
    [[ "$variant" == L0R ]] && variant_env+=(ZLINK_LADDER_L0R=1)
    [[ "$variant" == L1 ]] && variant_env+=(ZLINK_LADDER_L1_PERMIT=1)
  fi
  "$JOB/tools/progress-before-measurement.sh" "confirm-$variant-$pattern-$payload-r$iteration" "$variant"
  local started raw
  started="$(date -u +%FT%TZ)"
  printf '\nRun start %s %s-%s r%s: %s\n' "$variant" "$pattern" "$payload" "$iteration" "$started" >> "$JOB/progress.md"
  env -u ZLINK_LADDER_L0R -u ZLINK_LADDER_L1_PERMIT "${variant_env[@]}" \
    IMPLEMENTATIONS="$impl" BENCH_TARGET_SERVER="$impl" PATTERNS="$pattern" PAYLOAD_SIZES="$payload" \
    RUNS=1 SKIP_BUILD=1 WARMUP_SECONDS=3 DURATION_SECONDS=5 SOURCE_CPUS=0-7 TARGET_CPUS=8-15 \
    OUTPUT_DIR="$BASE/$pattern-$payload/$variant/run$iteration" \
    RUN_STAMP="4351-confirm-$pattern-$payload-$variant-r$iteration" \
    bash "$RUNNER" >"$JOB/raw/confirm-$pattern-$payload-$variant-r$iteration-driver.log" 2>&1
  raw="$BASE/$pattern-$payload/$variant/run$iteration/run1/$impl-$pattern-$payload/results.json"
  python3 - "$raw" "$JOB/progress.md" "$variant" "$pattern" "$payload" "$iteration" <<'PY'
import json, pathlib, sys
raw, progress, variant, pattern, payload, iteration = sys.argv[1:]
p = pathlib.Path(raw)
c = json.loads(p.read_text())['cells'][0]
if c['errors'] or c['server_errors'] or c['target_stats']['errors'] or c['submitted'] != c['completed'] or c['submitted'] != c['target_stats']['received']:
    raise SystemExit(f'broken result: {p}')
rel = p.relative_to('/root/jobs/fx/cx/v21/s033/lad-node-m2')
with open(progress, 'a') as f:
    f.write(f"\nFirst result {variant} {pattern}-{payload} r{iteration}: {c['throughput_per_second']} msg/s; errors=0 submitted=completed=received={c['submitted']}. Raw: {rel}.\n")
PY
}

for cell in send-saturation:4096 request-serial:1024 request-serial:4096; do
  pattern="${cell%:*}"; payload="${cell#*:}"
  for iteration in 1 2 3; do
    for variant in L0 L0R L1; do
      run_one "$variant" "$pattern" "$payload" "$iteration"
    done
  done
done

for iteration in 1 2 3; do
  run_one binding send-saturation 4096 "$iteration"
  run_one L0 send-saturation 4096 "$iteration"
done
