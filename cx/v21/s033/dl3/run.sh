#!/usr/bin/env bash
# .NET L2->L3 quick compare, 6 cells x 1 run, binding perf timing (warmup 1s, active 5s, 3s gap, no warm run).
set -uo pipefail
job=/root/jobs/fx/cx/v21/s033/dl3
root=/root/worktree/zlink-v21-s033/lfx-dotnet
setgov(){ for cpu in $(seq 0 15); do echo "$1" > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; }
trap 'setgov powersave' EXIT
setgov performance
echo "hold start $(date -u +%FT%TZ) HEAD=$(git -C $root rev-parse HEAD) dirty=$(git -C $root status --porcelain | wc -l)" >> $job/results.txt
for cell in "send-saturation 1024" "send-saturation 4096" "request-serial 1024" "request-serial 4096" "request-backpressure 1024" "request-backpressure 4096"; do
  read -r pattern size <<< "$cell"
  for level in L2 L3; do
    out=$job/raw/$level/$pattern-$size; mkdir -p $out
    rate=$(/root/jobs/fx/cx/v21/s033/lfx-dotnet/tools/dotnet/run-one.sh $pattern $size $out dl3-$level-$pattern-$size-$(date -u +%H%M%S) 1 $level 2>$out/err.txt) || rate="INVALID($(tail -n1 $out/err.txt))"
    echo "$(date -u +%T) $level $pattern-$size $rate $out" >> $job/results.txt
    sleep 3
  done
done
echo "hold end $(date -u +%FT%TZ)" >> $job/results.txt
