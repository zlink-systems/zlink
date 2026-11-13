#!/bin/bash
# kairos: behavioural fail-before for #4162 reference values: Python verdict-path tests at HEAD, then product .py at 65fc5b9902^.
J=/root/jobs/fx/cx/v21/s033/f4162r W=/root/worktree/zlink-v21-s033/f4162r
cd $W || exit 1
PROD=$(git show --name-only --format= 65fc5b9902 -- '*.py' | grep -v '/tests/')
TESTS="bindings/c/perf/single/tests/test_multi_run_comparison_policy.py bindings/c/perf/tests/test_perf_regression_gate.py bindings/python/tests/test_perf_runner.py"
echo "product: $PROD"
run() { : > $J/py-$1.log; for t in $TESTS; do (cd $(dirname $t) && taskset -c 8-15 python3 -B -m unittest $(basename $t .py)) >> $J/py-$1.log 2>&1; echo "$1 $(basename $t) rc=$?"; done; grep -E "^(FAIL|ERROR):|^Ran |^(OK|FAILED)" $J/py-$1.log | head -30; }
run after
git checkout -q 65fc5b9902^ -- $PROD; git diff --stat HEAD | tail -1
run before
git checkout -q HEAD -- $PROD; git status --short | grep -v '^??' | head -3
