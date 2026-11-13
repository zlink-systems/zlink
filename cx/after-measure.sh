#!/bin/bash
# Waits for r4190m and f4162e to end, then runs queued verification: Java PR tests in sequence, two pre-prs in parallel.
cd /root/zlink
for j in r4190m f4162e; do until bash scripts/dev/job.sh status $j 2>&1 | tail -1 | grep -q "완료\|죽음"; do sleep 60; done; done
echo "measurements ended $(date -u +%T)"
( /root/jobs/fx/cx/v21/s033/trole-pr/pre-pr.sh > /root/jobs/fx/cx/v21/s033/trole-pr/pre-pr2.log 2>&1; echo "trole2 pre-pr done $(date -u +%T)" ) &
( /root/jobs/fx/cx/v21/s033/i3541c/pre-pr.sh > /root/jobs/fx/cx/v21/s033/i3541c/pre-pr.log 2>&1; echo "i3541 pre-pr done $(date -u +%T)" ) &
/root/jobs/fx/cx/r4266.sh > /root/jobs/fx/cx/r4266.log 2>&1; echo "r4266 done $(date -u +%T)"
/root/jobs/fx/cx/r4268.sh > /root/jobs/fx/cx/r4268.log 2>&1; echo "r4268 done $(date -u +%T)"
wait
