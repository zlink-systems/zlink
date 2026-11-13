#!/usr/bin/env bash
# usage: cpp-slot.sh <command...>  — runs a C++ build/test command holding one of 5 machine-wide C++ slots.
# Blocks (no timeout) until a slot is free. User rule (2026-10-17 07:47 UTC): at most 5 concurrent C++ builds.
set -u
d=/root/jobs/fx/cx/cpp-slots; mkdir -p "$d"
while true; do
  for s in 1 2 3 4 5; do
    exec {fd}>"$d/slot$s.lock"
    if flock -n "$fd"; then
      # Memory guard (2026-10-18): a C++ build starts only with >= 8 GiB available,
      # so 5 slots cannot exhaust the 30 GiB box and get the supervisor's shells reaped.
      while [ "$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)" -lt 8 ]; do sleep 20; done
      echo "[cpp-slot] slot $s acquired by $$ ($PWD)" >&2
      "$@"; rc=$?
      flock -u "$fd"; exit $rc
    fi
    exec {fd}>&-
  done
  flock -w 30 "$d/slot1.lock" true 2>/dev/null || true
done
