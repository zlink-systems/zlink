#!/bin/bash
set -euo pipefail
ulimit -v 26214400
c++ -std=c++20 -O2 -Ibindings/cpp/tests/contract -Ibindings/cpp/include -I/root/worktree/zlink-v21-s033/xbp/.artifacts/wsl/install/zlink-core/1.23.0/include /root/jobs/fx/cx/v21/s033/xbp/counter_fixture.cpp .artifacts/wsl/install/zlink-cpp/1.23.0/lib/libzlink_cpp.a -L/root/worktree/zlink-v21-s033/xbp/.artifacts/wsl/install/zlink-core/1.23.0/lib -Wl,-rpath,/root/worktree/zlink-v21-s033/xbp/.artifacts/wsl/install/zlink-core/1.23.0/lib -lzlink -lpthread -o /root/jobs/fx/cx/v21/s033/xbp/counter-fixture
