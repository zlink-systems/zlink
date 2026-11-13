#!/bin/bash
# Memory guard (PM 2026-11-02): below 6 GiB MemAvailable, pause (SIGSTOP) the youngest running
# compile process, one per tick, always leaving one running; above 9 GiB resume the oldest paused one.
# Ninja waits on a paused child, so no new compile units start while memory is short.
# Tests are never paused (wall-clock timeouts); new tests wait in test-slot.sh instead.
LOG=/root/jobs/fx/cx/mem-guard.log
paused=()
avail() { awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo; }
trap '((${#paused[@]})) && kill -CONT "${paused[@]}" 2>/dev/null; exit 0' TERM INT
while :; do
  a=$(avail)
  live=(); for p in "${paused[@]}"; do kill -0 $p 2>/dev/null && live+=($p); done; paused=("${live[@]}")
  if [ "$a" -lt 6144 ]; then
    mapfile -t run < <(ps -eo pid=,stat=,etimes=,comm= | awk '$4 ~ /^(cc1plus|ld|ld\.bfd|ld\.gold|collect2)$/ && $2 !~ /T/ {print $3, $1}' | sort -n | awk '{print $2}')
    if [ ${#run[@]} -gt 1 ] && kill -STOP ${run[0]} 2>/dev/null; then
      paused+=(${run[0]}); echo "$(date +%T) avail=${a}MiB pause ${run[0]} (running ${#run[@]})" >> $LOG
    fi
  elif [ "$a" -ge 9216 ] && [ ${#paused[@]} -gt 0 ]; then
    kill -CONT ${paused[0]} 2>/dev/null; echo "$(date +%T) avail=${a}MiB resume ${paused[0]}" >> $LOG
    paused=("${paused[@]:1}")
  fi
  sleep 2
done
