#!/usr/bin/env bash
# Supervisor quick compare L1->L2, binding perf timing: warmup 1s, active 5s, 3s gap, no warm run. Run inside flock -x.
set -uo pipefail
job=/root/jobs/fx/cx/v21/s033/lfx-java
export RAW_VARIANT=q1114
printf 'hold_start_utc=%s q1114 L1-L2\n' "$(date -u +%FT%TZ)" >> $job/raw/holds.txt
for item in "send-saturation 1024" "send-saturation 4096" "request-serial 1024" "request-serial 4096" "request-backpressure 1024" "request-backpressure 4096"; do
  read -r pattern size <<< "$item"
  for step in L1 L2; do
    bash $job/tools/run-one-q.sh $step $pattern $size counted-1 || bash $job/tools/run-one-q.sh $step $pattern $size counted-2
    sleep 3
  done
done
printf 'hold_end_utc=%s q1114 L1-L2\n' "$(date -u +%FT%TZ)" >> $job/raw/holds.txt
cat $job/raw/q1114/index.tsv
