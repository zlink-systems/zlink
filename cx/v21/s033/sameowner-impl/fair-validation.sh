#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/sameowner-impl
if [ "${1:-}" != --locked ]; then
  exec flock -x /root/jobs/fx/cx/test.lock bash "$job/fair-validation.sh" --locked "${1:-$job/validate-final.py}"
fi
. /root/jobs/fx/cx/measure-gate.sh
while :; do
  while [ "$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)" -lt 8 ]; do sleep 5; done
  measure_gate_pass
  if [ "$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)" -ge 8 ]; then break; fi
done
exec flock -s /root/jobs/fx/cx/measure.lock taskset -c 12-15 python3 "${2:-$job/validate-final.py}"
