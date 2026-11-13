#!/bin/bash
set -euo pipefail
ulimit -c 0
job=/root/jobs/fx/cx/v21/s033/csubmit
flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$job/test-build" --parallel 3 --target test_cpp_framework_operation_registry test_cpp_framework_request_call_allocations test_cpp_framework_envelope_allocations test_cpp_framework_send_allocations test_cpp_framework_actor_late_result test_cpp_framework_raw_route_port_contract test_cpp_framework_request_native_counts test_cpp_framework_service_wire_codec test_cpp_framework_task_completion_source > "$job/build-focused-final.log" 2>&1
flock -s /root/jobs/fx/cx/measure.lock ctest --test-dir "$job/test-build" -R '^(test_cpp_framework_(operation_registry|registry_.*|request_deadline_index_contract|request_call_allocations|envelope_allocations|send_allocations|actor_late_result|raw_route_port_contract|request_native_counts|service_wire_codec|task_completion_source))$' --parallel 3 --output-on-failure --stop-on-failure --output-junit "$job/test-focused.xml" > "$job/test-focused.log" 2>&1
