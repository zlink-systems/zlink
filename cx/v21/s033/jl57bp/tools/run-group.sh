#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/jl57bp
ROOT=/root/worktree/zlink-v21-s033/lad-java-l57p
mode=${1:?}; size=${2:?}; trace=${3:-off}
cd "$ROOT"
printf 'hold_start_utc=%s mode=%s size=%s trace=%s\n' "$(date -u +%FT%TZ)" "$mode" "$size" "$trace" >> "$JOB/raw/holds.txt"
trap 'printf "hold_end_utc=%s mode=%s size=%s trace=%s\n" "$(date -u +%FT%TZ)" "$mode" "$size" "$trace" >> "$JOB/raw/holds.txt"' EXIT
export SKIP_BUILD=1 RUNS=1 IMPLEMENTATIONS=zlink-framework-java PATTERNS=request-backpressure PAYLOAD_SIZES="$size"
export WARMUP_SECONDS=10 DURATION_SECONDS=5 SOURCE_CPUS=0-5 TARGET_CPUS=8-13
unset ZLINK_BENCH_LADDER_L0 ZLINK_BENCH_LADDER_L1 ZLINK_BENCH_LADDER_L2 ZLINK_BENCH_LADDER_L3 ZLINK_BENCH_LADDER_L4 ZLINK_BENCH_LADDER_L5_7 BENCH_FLOW_TRACE
[[ "$mode" != *L5_7* ]] || export ZLINK_BENCH_LADDER_L5_7=1
[[ "$trace" != on ]] || export BENCH_FLOW_TRACE=1
for kind in warm counted; do
 [[ "$trace" != on || "$kind" != counted ]] || break
 out="$JOB/raw/$mode/request-backpressure-$size/$kind"
 [[ ! -e "$out" ]] || { echo "raw already exists: $out"; exit 2; }
 mkdir -p "$out"
 export OUTPUT_DIR="$out" RUN_STAMP="jl57bp-$mode-$size-$kind-$(date -u +%Y%m%dT%H%M%SZ)"
 python3 "$JOB/tools/preflight.py" "$mode" "$size" "$kind" "$out"
 bash "$JOB/tools/java/run_local_binding.sh" > "$out/driver.log" 2>&1
 python3 "$JOB/tools/summarize.py" "$out"
done
