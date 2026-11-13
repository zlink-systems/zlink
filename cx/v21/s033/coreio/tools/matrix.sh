#!/usr/bin/env bash
set -euo pipefail
J=/root/jobs/fx/cx/v21/s033/coreio
for mode in bpf perf strace uprobe; do
 for pattern in request-backpressure send-saturation; do
  for impl in zlink-framework-cpp zlink-cpp; do
   [[ $mode == bpf && $pattern == request-backpressure && $impl == zlink-framework-cpp ]] && continue
   printf '%s start %s %s %s\n' "$(date -Iseconds)" "$mode" "$impl" "$pattern" >>"$J/matrix.log"
   if flock -o /root/jobs/fx/cx/measure.lock bash "$J/tools/run.sh" cpp "$mode" "$impl" "$pattern"; then
    printf '%s ok %s %s %s\n' "$(date -Iseconds)" "$mode" "$impl" "$pattern" >>"$J/matrix.log"
   else
    printf '%s FAIL %s %s %s\n' "$(date -Iseconds)" "$mode" "$impl" "$pattern" >>"$J/matrix.log"
   fi
  done
 done
 for impl in zlink-framework-java zlink-java; do
  printf '%s start %s %s send-saturation\n' "$(date -Iseconds)" "$mode" "$impl" >>"$J/matrix.log"
  if flock -o /root/jobs/fx/cx/measure.lock bash "$J/tools/run.sh" java "$mode" "$impl" send-saturation; then
   printf '%s ok %s %s\n' "$(date -Iseconds)" "$mode" "$impl" >>"$J/matrix.log"
  else
   printf '%s FAIL %s %s\n' "$(date -Iseconds)" "$mode" "$impl" >>"$J/matrix.log"
  fi
 done
done
