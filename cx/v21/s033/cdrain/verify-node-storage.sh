#!/bin/bash
set -euo pipefail
ulimit -c 0
job=/root/jobs/fx/cx/v21/s033/cdrain
slot=/root/jobs/fx/cx/cpp-build-slot.sh
"$slot" cmake --build "$job/test-build" --parallel 3 --target test_cpp_framework_local_enqueue_allocations test_cpp_framework_gate_less_drain test_cpp_framework_mailbox_owner_dispatch test_cpp_framework_manual_dispatch_owner test_cpp_framework_handler_entry_permit test_cpp_framework_mailbox_fifo_cost test_cpp_framework_offload_executor zlink_cpp_framework_mesh_node_vertical_test > "$job/build-storage-focused.log" 2>&1
ctest --test-dir "$job/test-build" -R '^(test_cpp_framework_(local_enqueue_allocations|gate_less_drain|mailbox_owner_dispatch|manual_dispatch_owner|handler_entry_permit|mailbox_fifo_cost|offload_executor)|zlink_cpp_framework_mesh_node_vertical_test)$' --output-on-failure --stop-on-failure > "$job/test-storage-focused.log" 2>&1
"$slot" cmake --build "$job/test-build" --parallel 3 > "$job/build-storage-all.log" 2>&1
python3 "$job/instrument-test-commands.py"
ctest --test-dir "$job/test-build" -E '^test_cpp_(framework|stream_connector)_install_consumer$' --output-on-failure --stop-on-failure --output-junit "$job/test-all-storage.xml" > "$job/test-all-storage.log" 2>&1
