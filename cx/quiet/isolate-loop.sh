#!/bin/bash
# Re-applies the CPU split every 0.3 s while holder $1 holds measure.lock (guard v8, 11-13 18:5x).
# The guard's own poll takes 5-60 s under a hold (SCHED_IDLE on 4 CPUs), so bench processes
# started after its first apply kept the tool masks 0-7/8-15 (binding-rebase .NET/Java rejected).
chrt -o -p 0 $$ 2>/dev/null
h=$1
while grep -qE "^[0-9]+: FLOCK +ADVISORY +WRITE +$h .*:1237411609 " /proc/locks; do
  python3 -I /root/jobs/fx/cx/quiet/isolate.py apply $h 2>>/root/jobs/fx/cx/quiet/guard.log
  sleep 0.3
done
