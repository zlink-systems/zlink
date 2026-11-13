#!/bin/bash
# Run a C++ compile command in one of two build slots (cpp-build.lock, .2). Use -j3.
# Usage: cpp-build-slot.sh <command...>   — wrap the compile only, never tests or pre-pr.
# Waits for MemAvailable >= 12 GiB, holds measure.lock shared, pins CPUs 0-11.
set -u
. /root/jobs/fx/cx/measure-gate.sh
BUSY=211
while :; do
  while [ "$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)" -lt 12 ]; do sleep 15; done
  for L in /root/jobs/fx/cx/cpp-build.lock /root/jobs/fx/cx/cpp-build.2.lock; do
    measure_gate_pass
    flock -n -E $BUSY "$L" flock -s /root/jobs/fx/cx/measure.lock taskset -c 0-11 "$@"
    rc=$?
    [ $rc -ne $BUSY ] && exit $rc
  done
  sleep 5
done
