#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/sfix
build=/root/jobs/fx/cx/v21/s033/salloc/build
check_memory() {
 while (( $(awk '/MemAvailable/{print $2}' /proc/meminfo) < 14680064 )); do sleep 5; done
 awk '/MemAvailable/{print strftime("%FT%T"),$2}' /proc/meminfo >> "$job/compile-memory.tsv"
}
for target in test_cpp_framework_mesh_shutdown_seal test_cpp_framework_actor_late_result \
 test_cpp_framework_route_mesh_weight_update test_cpp_framework_instance_spot_activation \
 test_cpp_framework_request_call_allocations test_cpp_framework_spot_close_conformance; do
 check_memory
 /root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$build" --target "$target" --parallel 3 -- -k 0
done
check_memory
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$build" --parallel 3 -- -k 0
