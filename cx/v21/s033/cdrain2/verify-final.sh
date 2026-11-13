#!/bin/bash
set -euo pipefail
ulimit -c 0
job=/root/jobs/fx/cx/v21/s033/cdrain2
lock=/root/jobs/fx/cx/measure.lock
slot=/root/jobs/fx/cx/cpp-build-slot.sh
flock -s "$lock" "$slot" cmake --build "$job/test-build" --parallel 3 > "$job/build-all.log" 2>&1
python3 "$job/instrument-test-commands.py" > "$job/instrument-tests.log" 2>&1
flock -s "$lock" ctest --test-dir "$job/test-build" -E '^test_cpp_(framework|stream_connector)_install_consumer$' --output-on-failure --stop-on-failure --output-junit "$job/test-all.xml" > "$job/test-all.log" 2>&1
flock -s "$lock" "$slot" cmake --build "$job/build" --parallel 3 --target bench_cpp_client bench_cpp_framework_server bench_cpp_zlink_server bench_cpp_grpc_server bench_cpp_pm_server > "$job/build-production.log" 2>&1
