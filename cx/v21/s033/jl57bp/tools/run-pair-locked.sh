#!/usr/bin/env bash
set -euo pipefail
for size in 1024 4096; do
 bash /root/jobs/fx/cx/v21/s033/jl57bp/tools/run-group.sh "${1:?mode}" "$size" off
done
