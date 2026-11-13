#!/bin/bash
set -euo pipefail
cd /root/worktree/zlink-v21-s033/i3560b
trace=/sys/kernel/tracing/uprobe_events
printf '%s\n' 'p:i3560b/wake /root/jobs/fx/cx/v21/s033/core124/prefix-1.24/lib/libzlink.so.1.23.0:0xb0330' >> "$trace"
trap 'printf "%s\n" "-:i3560b/wake" >> "$trace"' EXIT
PROBE_MODE=wakeperf I3560B_CHILD=1 python3 /root/jobs/fx/cx/v21/s033/i3560b/measure.py
