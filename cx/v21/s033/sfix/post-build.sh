#!/bin/bash
set -euo pipefail
cd /root/worktree/zlink-3867a
job=/root/jobs/fx/cx/v21/s033/sfix
echo "$(date -Is) negative control and final tests"
bash "$job/negative-control.sh"
echo "$(date -Is) tests complete; benchmark build"
while (( $(awk '/MemAvailable/{print $2}' /proc/meminfo) < 14680064 )); do sleep 5; done
awk '/MemAvailable/{print strftime("%FT%T"),$2}' /proc/meminfo >> "$job/compile-memory.tsv"
/root/jobs/fx/cx/cpp-build-slot.sh bash "$job/build-bench.sh" > "$job/build-bench.log" 2>&1
pkill -INT -f '^python3 /root/jobs/fx/cx/v21/s033/sfix/compile-governor.py$' || true
echo "$(date -Is) benchmark built; census and alternating A/B"
bash "$job/run-measure.sh"
echo "$(date -Is) measurements complete; final format"
bash scripts/format/format.sh --check cpp > "$job/format-final.log" 2>&1
git diff --check
echo "$(date -Is) checks complete"
