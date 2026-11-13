#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/psmeas
until [ -s /root/jobs/fx/cx/v21/s033/f4163b/report.md ] && ! pgrep -f '[f]4163b' >/dev/null; do sleep 60; done
echo "f4163b done $(date -u +%T)"
for v in base branch; do strings /root/worktree/zlink-v21-s033/psmeas-$v/.psmeas-build/bench_cpp_client | grep -qF -- --target-rate; done
bash "$job/measure.sh"
python3 "$job/summarize.py"
test -s "$job/report.md"
rm -rf /root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build /root/worktree/zlink-v21-s033/psmeas-branch/.psmeas-build
echo "measure done $(date -u +%T)"
