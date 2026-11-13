#!/bin/bash
# Run a heavy test command (C++ CTest, .NET, Java) in the single test slot (test.lock).
# Usage: test-slot.sh <cpus> <command...>   — budget is 2 build slots + 1 test slot (PM 2026-11-02).
# Waits for MemAvailable >= 8 GiB, holds measure.lock shared, pins to <cpus>.
set -u
. /root/jobs/fx/cx/measure-gate.sh
CPUS=$1; shift
while :; do
  while [ "$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)" -lt 8 ]; do sleep 15; done
  measure_gate_pass
  flock -n -E 211 /root/jobs/fx/cx/test.lock flock -s /root/jobs/fx/cx/measure.lock taskset -c "$CPUS" "$@"
  rc=$?; [ $rc -ne 211 ] && exit $rc
  sleep 5
done
