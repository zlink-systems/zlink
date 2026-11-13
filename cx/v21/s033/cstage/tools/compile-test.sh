#!/bin/bash
set -euo pipefail
root=/root/worktree/zlink-cstage
job=/root/jobs/fx/cx/v21/s033/cstage
g++ -std=c++20 -O2 -g -pthread -I"$root/bindings/cpp/src" -I"$job/package-staged/cpp/include" -I"$job/package-staged/core/include" "$root/bindings/cpp/tests/prototype/staged_request.cpp" -L"$job/package-staged/cpp/lib" -L"$job/package-staged/core/lib" -Wl,-rpath,"$job/package-staged/core/lib" -lzlink_cpp -lzlink -o "$job/prototype_test"
