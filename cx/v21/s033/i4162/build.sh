#!/usr/bin/env bash
set -euo pipefail
exec /root/jobs/fx/cx/cpp-build-slot.sh bash -c 'ulimit -v 26214400; exec make -C /root/worktree/zlink-v21-s033/i4162/.build-i4162 -j3'
