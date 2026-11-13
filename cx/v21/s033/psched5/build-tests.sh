#!/bin/bash
set -euo pipefail
j=/root/jobs/fx/cx/v21/s033/psched5
bash "$j/configure-tests.sh" > "$j/configure-tests.log" 2>&1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build /root/worktree/zlink-v21-s033/psched5/.psched5-tests --parallel 3 --target test_cpp_framework_scheduler_drain test_cpp_framework_mailbox_owner_dispatch > "$j/build-tests.log" 2>&1
ctest --test-dir /root/worktree/zlink-v21-s033/psched5/.psched5-tests -R '^test_cpp_framework_(scheduler_drain|mailbox_owner_dispatch)' --output-on-failure > "$j/tests-original.log" 2>&1
du -sk /root/worktree/zlink-v21-s033/psched5/.psched5-tests > "$j/test-build-size.txt"
