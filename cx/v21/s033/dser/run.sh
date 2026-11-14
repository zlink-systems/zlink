#!/usr/bin/env bash
# .NET L1/L2 request-serial recheck, binding perf timing (warmup 1s, active 5s, 3s gap, no warm run).
set -uo pipefail
job=/root/jobs/fx/cx/v21/s033/dser
root=/root/worktree/zlink-v21-s033/lfx-dotnet
setgov(){ for cpu in $(seq 0 15); do echo "$1" > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; }
trap 'setgov powersave' EXIT
setgov performance
echo "hold start $(date -u +%FT%TZ) HEAD=$(git -C $root rev-parse HEAD) dirty=$(git -C $root status --porcelain | wc -l)" >> $job/results.txt
for size in 1024 4096; do
  for pair in 1 2 3; do
    for level in L1 L2; do
      out=$job/raw/$level/serial-$size/pair$pair; mkdir -p $out
      rate=$(/root/jobs/fx/cx/v21/s033/lfx-dotnet/tools/dotnet/run-one.sh request-serial $size $out dser-$level-$size-$pair-$(date -u +%H%M%S) 1 $level 2>$out/err.txt) || rate="INVALID($(tail -n1 $out/err.txt))"
      echo "$(date -u +%T) $level serial-$size pair$pair $rate $out" >> $job/results.txt
      sleep 3
    done
  done
done
echo "hold end $(date -u +%FT%TZ)" >> $job/results.txt
