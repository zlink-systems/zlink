#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/i4162
WORK=/root/worktree/zlink-v21-s033/i4162
while kill -0 3007663 2>/dev/null; do sleep 2; done
[[ -x "$WORK/.build-i4162/bin/perf_router_router" ]]
cp "$WORK/.build-i4162/bin/perf_router_router" "$JOB/bin/perf_router_router"
ldd "$JOB/bin/perf_router_router" > "$JOB/raw/ldd.txt"
sha256sum "$JOB/bin/perf_router_router" /root/jobs/fx/cx/v21/s033/core124/prefix-1.24/lib/libzlink.so.1.23.0 > "$JOB/raw/binary-sha256.txt"
readelf -l "$JOB/bin/perf_router_router" > "$JOB/raw/elf-program-headers.txt"
# The observer runs on CPU 0. Each benchmark selects CPUs 8-15 itself.
flock -o /root/jobs/fx/cx/measure.lock taskset -c 0 python3 "$JOB/observe.py" --runs 20 --label natural
