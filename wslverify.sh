#!/bin/bash
set -u
cd /home/hep7/project/zlink
git checkout -q -- . && git clean -fdq -- framework/languages/cpp && git fetch -q origin && git reset -q --hard origin/main
git apply /mnt/d/project/zlink-465-cpp/cpp465.patch || { echo "APPLY FAILED"; exit 1; }
B=framework/languages/cpp/build/linux-ninja-release
(cd "$B" && cmake . 2>&1 | grep -E "error|incomplete" | head -3; cmake --build . -j 8 2>&1 | grep -E "error|FAILED" | head -8
 echo "--- listener_identity ---"; ./test_cpp_framework_listener_identity 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]|tests from .* ran|Failure|Expected|Which is|Value of|Actual" | head -20
 echo "--- contracts ---"; ctest -R "layout_contract|label_contract|contract_headers|target_contract" 2>&1 | grep -E "tests passed"
 echo "--- full ctest ---"; ctest 2>&1 | grep -E "tests passed|\(Failed\)" | tail -6)
git checkout -q -- . && git clean -fdq -- framework/languages/cpp
