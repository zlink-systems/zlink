#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-node-s1
TOOLS=$JOB/tools/node
START=$SECONDS
# A hold covers no more than two steps. Warm every changed cell, retain warm raw, count exactly once.
while (($#)); do
  variant=$1; shift
  for pair in 'send-saturation 1024' 'send-saturation 4096' 'request-serial 1024' 'request-serial 4096' 'request-backpressure 1024' 'request-backpressure 4096'; do
    read -r pattern size <<<"$pair"
    "$TOOLS/run-cell.sh" "$variant" "$pattern" "$size" warm
    "$TOOLS/run-cell.sh" "$variant" "$pattern" "$size" counted
  done
done
(( SECONDS - START < 1500 )) || { echo '30-minute hold safety boundary reached' >&2; exit 124; }
