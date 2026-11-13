#!/usr/bin/env bash
set -euo pipefail
repo=/root/worktree/zlink-v21-s033/i3872
job=/root/jobs/fx/cx/v21/s033/i3872
runner="$repo/framework/bench/grpc/node/run_local.sh"
measure_one() {
  local round="$1" label="$2" implementation="$3" pattern="$4" payload="$5"
  local stamp="i3872-r${round}-${label}-${implementation}"
  local out="$job/raw/${stamp}"
  local mem
  mem=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
  if ((mem < 12582912)); then echo "MemAvailable below 12 GiB: ${mem} KiB" >&2; return 3; fi
  echo "BEGIN round=${round} cell=${label} impl=${implementation} MemAvailable=${mem}KiB $(date -Is)" >&2
  env IMPLEMENTATIONS="$implementation" PATTERNS="$pattern" PAYLOAD_SIZES="$payload" RUNS=1 \
    WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 RUN_STAMP="$stamp" OUTPUT_DIR="$out" \
    bash "$runner" >"$out.stdout" 2>"$out.stderr"
  echo "END round=${round} cell=${label} impl=${implementation} $(date -Is)" >&2
}
cells=(
  'send1024 send-saturation 1024'
  'send4096 send-saturation 4096'
  'serial1024 request-serial 1024'
  'serial4096 request-serial 4096'
  'backpressure1024 request-backpressure 1024'
)
for round in 1 2 3; do
  index=0
  for row in "${cells[@]}"; do
    read -r label pattern payload <<<"$row"
    if (((round + index) % 2 == 1)); then
      first=zlink-framework-node; second=zlink-node
    else
      first=zlink-node; second=zlink-framework-node
    fi
    measure_one "$round" "$label" "$first" "$pattern" "$payload"
    measure_one "$round" "$label" "$second" "$pattern" "$payload"
    ((index+=1))
  done
done
