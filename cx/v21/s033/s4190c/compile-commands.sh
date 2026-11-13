#!/bin/bash
set -euo pipefail
cmake --build framework/bench/grpc/cpp/build -j3 --target bench_cpp_client bench_cpp_submit_threads_test bench_cpp_framework_completion_test > /root/jobs/fx/cx/v21/s033/s4190c/build-cpp-metadata.log 2>&1
bash /root/jobs/fx/cx/v21/s033/s4190c/c-build-commands.sh
