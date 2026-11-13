#!/bin/bash
# Every 3 minutes: report finished jobs once, and jobs whose log has not grown for 6+ minutes
# while the codex process has no child process (likely stuck on the model or on stdin).
declare -A seen size idle
while true; do
  for log in /root/jobs/fx/cx/*/codex*.log /root/jobs/fx/cx/*/*/codex*.log; do
    case "$log" in *stalled*) continue;; esac
    [ -f "$log" ] || continue
    [ -n "${seen[$log]}" ] && continue
    # Ignore logs that were already quiet for 90+ minutes when first seen (old jobs).
    if [ -z "${size[$log]}" ] && [ $(( $(date +%s) - $(stat -c %Y "$log") )) -gt 5400 ]; then seen[$log]=1; continue; fi
    if tail -1 "$log" | grep -q '^exit='; then
      [ $(( $(date +%s) - $(stat -c %Y "$log") )) -lt 400 ] && echo "DONE ${log#/root/jobs/fx/cx/} $(tail -1 "$log")"
      seen[$log]=1; continue
    fi
    s=$(stat -c %s "$log")
    if [ "${size[$log]}" = "$s" ]; then
      idle[$log]=$(( ${idle[$log]:-0} + 1 ))
      if [ ${idle[$log]} -eq 2 ]; then
        stuck=$(tail -c 200 "$log" | tr '\n' ' ' | grep -c "Reading additional input from stdin")
        echo "IDLE6m ${log#/root/jobs/fx/cx/} stdin=$stuck last: $(tail -c 120 "$log" | tr '\n' ' ')"
      fi
    else
      idle[$log]=0
    fi
    size[$log]=$s
  done
  echo "TICK $(date +%H:%M) load $(cut -d' ' -f1 /proc/loadavg) mem_avail $(free -g | awk '/Mem:/{print $7}')G" > /root/jobs/fx/cx/watch-last.txt
  sleep 180
done
