#!/bin/bash
set -eu
r=/root/jobs/fx/cx/v21/s033/cbclose2
cmake -S "$r/main-tree/bindings/cpp" -B "$r/build-main" -DZLINK_CPP_BUILD_TESTS=ON -DZLINK_CPP_BUILD_BENCHMARKS=ON -DZLINK_CPP_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core/1.23.0/linux-x64 -DZLINK_CORE_DIR=/root/.cache/zlink/core/1.23.0/linux-x64 -DCMAKE_BUILD_TYPE=Release
cmake --build "$r/build-main" --target test_cpp_submit_close_race test_cpp_contract_submit_close_admission cpp_perf_pair cpp_perf_pubsub cpp_perf_dealer_router_reqrep -j3
