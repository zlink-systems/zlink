#!/bin/bash
set -euo pipefail
source /root/jobs/fx/cx/measure-gate.sh
measure_gate_pass
exec flock -s -o /root/jobs/fx/cx/measure.lock ctest --test-dir framework/bench/grpc/c/build -R '^bench_c_' --output-on-failure
