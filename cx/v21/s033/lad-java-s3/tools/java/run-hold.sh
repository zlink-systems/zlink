#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-java-s3
start=$SECONDS
printf 'hold_start_utc=%s\n' "$(date -u +%FT%TZ)" >> "$JOB/raw/lock-window.txt"
for cell in 'send-saturation 1024' 'send-saturation 4096' 'request-serial 1024' 'request-serial 4096' 'request-backpressure 1024' 'request-backpressure 4096'; do
  read -r pattern size <<< "$cell"
  for step in L4 L5_7 L4 L5_7; do
    (( SECONDS - start < 1500 )) || { echo '25-minute safety boundary reached; stop hold' >&2; exit 124; }
    bash "$JOB/tools/java/run-step-locked.sh" "$step" initial "$pattern" "$size"
  done
done
printf 'hold_end_utc=%s\n' "$(date -u +%FT%TZ)" >> "$JOB/raw/lock-window.txt"
