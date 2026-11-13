#!/usr/bin/env bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/pool
base=/root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build
branch=/root/worktree/zlink-v21-s033/pool/.pool-build

for round in 1 2 3; do
  case "$round" in
    1) order=(base pool8 pool4 pool2) ;;
    2) order=(pool8 pool4 pool2 base) ;;
    3) order=(pool4 pool2 base pool8) ;;
  esac
  for variant in "${order[@]}"; do
    build=$base; pool_count=0
    case "$variant" in
      pool8) build=$branch; pool_count=8 ;;
      pool4) build=$branch; pool_count=4 ;;
      pool2) build=$branch; pool_count=2 ;;
    esac
    env_args=(env -u LD_PRELOAD -u ZLINK_PERF_COMPLETION_POOL ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0)
    if (( pool_count > 0 )); then env_args+=(ZLINK_PERF_COMPLETION_POOL="$pool_count"); fi
    echo "round=$round variant=$variant begin $(date -Is)" >&2

    for pattern in send-saturation request-backpressure; do
      flock -o /root/jobs/fx/cx/measure.lock "${env_args[@]}" \
        BUILD_DIR="$build" IMPLEMENTATIONS=zlink-framework-cpp PATTERNS="$pattern" PAYLOAD_SIZES=1024 \
        RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 \
        RUN_STAMP="pool-$variant-$round-$pattern" \
        OUTPUT_DIR="$job/measure/$round/$variant/throughput-$pattern" \
        python3 "$job/processes.py" bash "$job/run-B.sh"
    done

    flock -o /root/jobs/fx/cx/measure.lock "${env_args[@]}" \
      BUILD_DIR="$build" IMPLEMENTATIONS=zlink-framework-cpp PATTERNS=send-saturation PAYLOAD_SIZES=1024 \
      RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=30 SKIP_BUILD=1 PM=1 \
      FRAMEWORK_TARGET=bench_cpp_pm_server RUN_STAMP="pool-pm-$variant-$round" \
      OUTPUT_DIR="$job/measure/$round/$variant/pm" \
      python3 "$job/processes.py" bash "$job/run-B.sh"

    flock -o /root/jobs/fx/cx/measure.lock "${env_args[@]}" \
      BUILD_DIR="$build" IMPLEMENTATIONS=zlink-framework-cpp PATTERNS=request-backpressure PAYLOAD_SIZES=1024 \
      RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=20 SKIP_BUILD=1 TARGET_RATE=48000 \
      BENCH_COLLECTOR="$job/collect-22k.py" RUN_STAMP="pool-22k-$variant-$round" \
      OUTPUT_DIR="$job/measure/$round/22k/$variant-zlink-framework-cpp/runner" \
      python3 "$job/processes.py" bash "$job/run-B.sh"

    echo "round=$round variant=$variant end $(date -Is)" >&2
  done
done
