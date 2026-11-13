#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cdesign
while (( $(awk '/MemAvailable/{print $2}' /proc/meminfo) < 14680064 )); do sleep 5; done
awk '/MemAvailable/{print strftime("%FT%T"),$2}' /proc/meminfo >> "$job/compile-memory.tsv"
exec /root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$job/build" --parallel 3
