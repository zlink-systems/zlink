#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/ln3pr
TOOLS="$JOB/tools/node"
start=$SECONDS
for cell in "$@"; do
  pattern=${cell%-*}; size=${cell##*-}
  for variant in A-L2 B-L2 A-L3 B-L3; do
    "$TOOLS/run-cell.sh" "$variant" "$pattern" "$size" warm
    "$TOOLS/run-cell.sh" "$variant" "$pattern" "$size" counted
  done
  (( SECONDS - start < 570 )) || { echo '9-minute safety boundary reached; stop this hold' >&2; exit 124; }
done
