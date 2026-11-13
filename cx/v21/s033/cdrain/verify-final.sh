#!/bin/bash
set -euo pipefail
ulimit -c 0
job=/root/jobs/fx/cx/v21/s033/cdrain
if rg -q '^FAILED:' "$job/build-manual-adapter.log"; then exit 1; fi
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$job/test-build" --parallel 3 > "$job/build-final-noop.log" 2>&1
ctest --test-dir "$job/test-build" -R '^test_cpp_framework_(manual_dispatch_owner|handler_entry_permit|mailbox_owner_dispatch)$' --output-on-failure --stop-on-failure > "$job/test-manual-final.log" 2>&1
ctest --test-dir "$job/test-build" -E '^test_cpp_(framework|stream_connector)_install_consumer$' --output-on-failure --stop-on-failure --output-junit "$job/test-all.xml" > "$job/test-all-complete.log" 2>&1
