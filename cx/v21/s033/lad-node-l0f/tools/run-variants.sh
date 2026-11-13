#!/usr/bin/env bash
set -euo pipefail
WT=/root/worktree/zlink-v21-s033/lad-node
BASE="$OUTPUT_DIR"
RUNNER=/root/jobs/fx/cx/v21/s033/lad-node-l0f/tools/node-run-local.sh
run_case() {
  local name="$1" target="$2" two="$3" lazy="$4"
  local out="$BASE/$name"
  /root/jobs/fx/cx/v21/s033/lad-node-l0f/tools/progress-before-measurement.sh "$name"
  local -a env_args=("OUTPUT_DIR=$out" "RUN_STAMP=4350l0f-$name" "IMPLEMENTATIONS=zlink-node"
    "PATTERNS=request-serial" "PAYLOAD_SIZES=4096" "RUNS=5" "BENCH_TARGET_SERVER=$target" "SKIP_BUILD=1")
  [[ "$target" == zlink-framework-node ]] && env_args+=(ZLINK_LADDER_L0_SERVER=1)
  [[ "$two" == 1 ]] && env_args+=(ZLINK_LADDER_L0_TWO_ROUTERS=1)
  [[ "$lazy" == 1 ]] && env_args+=(ZLINK_LADDER_L0_LAZY_FRAMEWORK=1)
  env -u ZLINK_LADDER_L0_SERVER -u ZLINK_LADDER_L0_TWO_ROUTERS -u ZLINK_LADDER_L0_LAZY_FRAMEWORK \
    "${env_args[@]}" bash "$RUNNER"
}
run_case binding zlink-node 0 0
run_case current zlink-framework-node 0 0
run_case l0-a zlink-framework-node 1 0
run_case l0-b zlink-framework-node 0 1
read -r base_rate a_rate b_rate <<EOF_RATES
$(python3 - "$BASE" <<'PYRATES'
import glob, json, statistics, sys
base=sys.argv[1]
def median(name):
    files=glob.glob(f"{base}/{name}/run*/zlink-node-request-serial-4096/results.json")
    return statistics.median(json.load(open(p))["cells"][0]["throughput_per_second"] for p in files)
print(median('current'), median('l0-a'), median('l0-b'))
PYRATES
)
EOF_RATES
if python3 - "$base_rate" "$a_rate" "$b_rate" <<'PYDECIDE'
import sys
base, a, b = map(float, sys.argv[1:])
sys.exit(0 if a > base and b > base else 1)
PYDECIDE
then
  run_case l0-ab zlink-framework-node 1 1
else
  printf '
L0+a+b not measured: both standalone variants did not improve the current L0 median (current=%s, a=%s, b=%s).
' \
    "$base_rate" "$a_rate" "$b_rate" >> /root/jobs/fx/cx/v21/s033/lad-node-l0f/progress.md
fi
