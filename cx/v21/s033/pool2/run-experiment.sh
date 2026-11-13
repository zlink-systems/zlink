#!/usr/bin/env bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/pool2
base=/root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build
branch=/root/worktree/zlink-v21-s033/pool/.pool-build
for round in 1 2 3 4 5; do
  case "$round" in
    1) order=(base pool8 pool3 pool2) ;;
    2) order=(pool8 pool3 pool2 base) ;;
    3) order=(pool3 pool2 base pool8) ;;
    4) order=(pool2 base pool8 pool3) ;;
    5) order=(base pool3 pool8 pool2) ;;
  esac
  for variant in "${order[@]}"; do
    build=$base; pool_count=0
    case "$variant" in
      pool8) build=$branch; pool_count=8 ;;
      pool3) build=$branch; pool_count=3 ;;
      pool2) build=$branch; pool_count=2 ;;
    esac
    out="$job/measure/$round/$variant"
    mkdir -p "$out"
    env_args=(env -u LD_PRELOAD -u ZLINK_PERF_COMPLETION_POOL -u ZLINK_PERF_COMPLETION_QUEUE_LOG \
      ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0)
    if (( pool_count > 0 )); then env_args+=(ZLINK_PERF_COMPLETION_POOL="$pool_count"); fi
    queue_args=()
    if (( pool_count > 0 )); then queue_args+=(QUEUE_LOG_DIR="$out/queue"); fi
    echo "round=$round variant=$variant begin $(date -Is)" >&2
    flock -o /root/jobs/fx/cx/measure.lock "${env_args[@]}" \
      BUILD_DIR="$build" IMPLEMENTATIONS=zlink-framework-cpp PATTERNS=send-saturation PAYLOAD_SIZES=1024 \
      RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 \
      BENCH_COLLECTOR="$job/collect-send-saturation.py" "${queue_args[@]}" \
      RUN_STAMP="pool2-$variant-$round-send-saturation" \
      OUTPUT_DIR="$out/throughput-send-saturation" \
      python3 "$job/processes.py" bash "$job/run-B.sh"
    echo "round=$round variant=$variant end $(date -Is)" >&2
  done
done
