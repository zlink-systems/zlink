#!/usr/bin/env bash
set -euo pipefail
exec /root/jobs/fx/cx/cpp-build-slot.sh bash -c 'ulimit -v 26214400; exec make -B -C /root/worktree/zlink-v21-s033/f4162/.build-4162 -j3'
