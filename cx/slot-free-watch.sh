#!/bin/bash
# exits when at least one C++ build slot stays free for 60s (probes with flock -n)
free_for=0
while :; do
  free=0
  for L in /root/jobs/fx/cx/cpp-build.lock /root/jobs/fx/cx/cpp-build.2.lock; do
    flock -n "$L" true && free=$((free+1))
  done
  if [ $free -gt 0 ]; then free_for=$((free_for+15)); else free_for=0; fi
  [ $free_for -ge 60 ] && { echo "free slots=$free at $(date +%T)"; exit 0; }
  sleep 15
done
