#!/bin/bash
set -euo pipefail
cd /root/worktree/zlink-v21-s033/i3560b
for mode in stacks api cpu syscalls; do
  PROBE_MODE="$mode" python3 /root/jobs/fx/cx/v21/s033/i3560b/measure.py
 done
