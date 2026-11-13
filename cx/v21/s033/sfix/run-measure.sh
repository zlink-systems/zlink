#!/bin/bash
set -euo pipefail
cd /root/worktree/zlink-3867a
patch=/root/jobs/fx/cx/ratio034/pin-condition-b.patch
job=/root/jobs/fx/cx/v21/s033/sfix
git apply --include=framework/bench/grpc/cpp/run_local.sh "$patch"
trap 'git apply -R --include=framework/bench/grpc/cpp/run_local.sh "$patch"' EXIT
bash "$job/census-run.sh" > "$job/census.log" 2>&1
bash "$job/ab.sh" > "$job/ab.log" 2>&1
python3 "$job/summarize.py" > "$job/ab-summary.log"
