#!/usr/bin/env bash
set -euo pipefail
BASE=/root/jobs/fx/cx/v21/s033/jbind2
LOCK=/root/jobs/fx/cx/measure.lock
run_side() {
  local pair="$1" side="$2" dir="$3" stamp
  stamp="jbind2-pair${pair}-${side}"
  local load avail waited=0 over=0
  while :; do
    load=$(cut -d' ' -f1 /proc/loadavg)
    avail=$(awk '/MemAvailable/ {printf "%.0f", $2/1024}' /proc/meminfo)
    if awk -v l="$load" 'BEGIN {exit !(l <= 4.0)}' && ((avail >= 12288)); then break; fi
    if ((avail < 12288)); then echo "$(date -Is) wait MemAvailable=${avail}MiB" >>"$BASE/logs/waits.log"; fi
    if awk -v l="$load" 'BEGIN {exit !(l > 4.0)}'; then
      waited=$((waited+30)); echo "$(date -Is) wait load=${load} waited=${waited}s" >>"$BASE/logs/waits.log"
      if ((waited >= 1200)); then over=1; break; fi
    fi
    sleep 30
  done
  {
    echo "pair=$pair side=$side stamp=$stamp time=$(date -Is) load1=$(cut -d' ' -f1 /proc/loadavg) memavailable_mib=$avail load_waited_seconds=$waited load_over_4_timeout=$over"
    uptime
    ps -eo pid,pcpu,comm --sort=-pcpu | head -11
    grep MHz /proc/cpuinfo | sort | uniq -c
  } >"$BASE/logs/${stamp}-system.txt"
  local args=(env RUN_STAMP="$stamp" OUTPUT_DIR="$BASE/results/$stamp" IMPLEMENTATIONS=zlink-java PATTERNS=request-serial,request-backpressure,send-saturation PAYLOAD_SIZES=1024 RUNS=1 SKIP_BUILD=1 WARMUP_SECONDS=10 DURATION_SECONDS=10 ZLINK_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core/1.23.0/linux-x64 ZLINK_LIBRARY_PATH=/root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so)
  "${args[@]}" bash "$dir/framework/bench/grpc/java/run_local.sh" >"$BASE/logs/${stamp}.log" 2>&1
}
if [[ "${1:-}" == "__pair" ]]; then
  pair="$2"; first="$3"; second="$4"
  run_side "$pair" "$first" "$BASE/$first"
  run_side "$pair" "$second" "$BASE/$second"
  exit 0
fi
for pair in $(seq 1 6); do
  if ((pair % 2 == 1)); then first=main; second=pr; else first=pr; second=main; fi
  flock -o "$LOCK" "$BASE/run_pairs.sh" __pair "$pair" "$first" "$second"
done
