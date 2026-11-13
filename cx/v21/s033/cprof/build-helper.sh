#!/usr/bin/env bash
set -euo pipefail
cmake -S /root/jobs/fx/cx/v21/s033/cprof -B /root/jobs/fx/cx/v21/s033/cprof/helper-build
cmake --build /root/jobs/fx/cx/v21/s033/cprof/helper-build --parallel 3
cp /root/jobs/fx/cx/v21/s033/cprof/helper-build/alloc_counter.so /root/jobs/fx/cx/v21/s033/cprof/alloc_counter.so
