#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/salloc
cmake --build /root/worktree/zlink-3867a/framework/bench/grpc/cpp/build --parallel 3 -- -k 0
cmake --build "$job/probe-build" --parallel 3 -- -k 0
for target in test_cpp_framework_mesh_shutdown_seal test_cpp_framework_actor_late_result \
  test_cpp_framework_route_mesh_weight_update test_cpp_framework_instance_spot_activation \
  test_cpp_framework_request_call_allocations test_cpp_framework_spot_close_conformance; do
  cmake --build "$job/build" --target "$target" --parallel 3 -- -k 0
done
cmake --build "$job/build" --parallel 3 -- -k 0
