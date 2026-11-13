#!/usr/bin/env bash
# usage: cpp-slot.sh <command...>  — runs a Java Gradle gate holding one of 2 machine-wide C++ slots.
# Blocks (no timeout) until a slot is free. User rule: at most 2 concurrent Java Gradle gates.
set -u
d=/root/jobs/fx/cx/java-slots; mkdir -p "$d"
while true; do
  for s in 1 2; do
    exec {fd}>"$d/slot$s.lock"
    if flock -n "$fd"; then
      echo "[java-slot] slot $s acquired by $$ ($PWD)" >&2
      "$@"; rc=$?
      flock -u "$fd"; exit $rc
    fi
    exec {fd}>&-
  done
  flock -w 30 "$d/slot1.lock" true 2>/dev/null || true
done
