#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/salloc
cmake -S /root/worktree/zlink-3867a/framework/languages/cpp -B "$job/build"  -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=ON
cmake --build "$job/probe-build" --parallel 3 -- -k 0
cmake --build /root/worktree/zlink-3867a/framework/bench/grpc/cpp/build --parallel 3 -- -k 0
cmake --build "$job/build" --parallel 3 --target \
 test_cpp_framework_request_call_allocations test_cpp_framework_request_native_counts \
 test_cpp_framework_request_encoder_allocations test_cpp_framework_async_task \
 test_cpp_framework_raw_route_port_contract test_cpp_framework_service_wire_codec -- -k 0
