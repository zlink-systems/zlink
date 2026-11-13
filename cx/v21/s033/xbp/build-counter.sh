#!/bin/bash
set -euo pipefail
ulimit -v 26214400
cc -shared -fPIC -O3 -Wall -Wextra -o /root/jobs/fx/cx/v21/s033/xbp/counter.so /root/jobs/fx/cx/v21/s033/xbp/counter.c -Wl,--no-as-needed -L/root/worktree/zlink-v21-s033/xbp/.artifacts/wsl/install/zlink-core/1.23.0/lib -Wl,-rpath,/root/worktree/zlink-v21-s033/xbp/.artifacts/wsl/install/zlink-core/1.23.0/lib -lzlink -ldl -lpthread
