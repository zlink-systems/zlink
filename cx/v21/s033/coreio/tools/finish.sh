#!/usr/bin/env bash
set -euo pipefail
J=/root/jobs/fx/cx/v21/s033/coreio
printf '%s combined captures reused; finish paired request counters and mailbox probes\n' "$(date -Iseconds)" >>"$J/matrix.log"
for impl in zlink-framework-cpp zlink-cpp; do
 printf '%s start final-uprobe %s\n' "$(date -Iseconds)" "$impl" >>"$J/matrix.log"
 flock -o /root/jobs/fx/cx/measure.lock bash "$J/tools/run.sh" cpp uprobe "$impl" request-backpressure
 printf '%s ok final-uprobe %s\n' "$(date -Iseconds)" "$impl" >>"$J/matrix.log"
done
