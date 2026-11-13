#!/bin/bash
set -euo pipefail
ulimit -c 0
job=/root/jobs/fx/cx/v21/s033/cdrain
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$job/test-build" --target test_cpp_framework_gate_less_drain --parallel 3 > "$job/build-wait-boundary.log" 2>&1
ctest --test-dir "$job/test-build" -R '^(test_cpp_framework_gate_less_drain|zlink_cpp_framework_mesh_node_vertical_test)$' --output-on-failure --stop-on-failure > "$job/test-isolated.log" 2>&1
ctest --test-dir "$job/test-build" --output-on-failure --parallel 3 --stop-on-failure -E '^test_cpp_(framework_install_consumer|stream_connector_install_consumer)$' > "$job/test-all-final.log" 2>&1
