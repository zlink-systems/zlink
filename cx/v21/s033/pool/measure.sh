#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/psched8
for round in 1 2 3; do
  for pattern in request-backpressure send-saturation; do
    for variant in base chained; do
      build=/root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build
      if [[ $variant == chained ]]; then build=/root/worktree/zlink-v21-s033/psched5/.psched8-build; fi
      flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$build" IMPLEMENTATIONS=zlink-cpp,zlink-framework-cpp \
        PATTERNS="$pattern" PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 \
        RUN_STAMP="psmeas-$variant-$round" OUTPUT_DIR="$job/measure/$round/$pattern/$variant" python3 "$job/processes.py" bash "$job/run-B.sh"
    done
  done
  for variant in base chained; do
    build=/root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build
      if [[ $variant == chained ]]; then build=/root/worktree/zlink-v21-s033/psched5/.psched8-build; fi
    flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 BUILD_DIR="$build" IMPLEMENTATIONS=zlink-framework-cpp \
      PATTERNS=send-saturation PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=30 SKIP_BUILD=1 PM=1 \
      FRAMEWORK_TARGET=bench_cpp_pm_server RUN_STAMP="psmeas-pm-$variant-$round" OUTPUT_DIR="$job/measure/$round/pm/$variant" python3 "$job/processes.py" bash "$job/run-B.sh"
  done
  for variant in base chained; do
    build=/root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build
      if [[ $variant == chained ]]; then build=/root/worktree/zlink-v21-s033/psched5/.psched8-build; fi
    for implementation in zlink-cpp zlink-framework-cpp; do
      rate=32000; [[ $implementation == zlink-framework-cpp ]] && rate=48000
      label="$variant-$implementation"
      out="$job/measure/$round/22k/$label"
      mkdir -p "$out"
      (
        flock -o /root/jobs/fx/cx/measure.lock env -u LD_PRELOAD ZLINK_CPP_MESH_TRACE=0 ZLINK_CPP_AUTO_CONNECT_TRACE=0 ZLINK_CPP_HOST_STOP_TRACE=0 \
          BUILD_DIR="$build" IMPLEMENTATIONS="$implementation" PATTERNS=request-backpressure PAYLOAD_SIZES=1024 RUNS=1 WARMUP_SECONDS=30 DURATION_SECONDS=20 SKIP_BUILD=1 \
          TARGET_RATE="$rate" BENCH_COLLECTOR="$job/collect-22k.py" RUN_STAMP="psmeas-22k-$label-$round" OUTPUT_DIR="$out/runner" python3 "$job/processes.py" bash "$job/run-B.sh"
      )
    done
  done
done
