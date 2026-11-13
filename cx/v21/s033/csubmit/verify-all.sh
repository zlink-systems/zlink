#!/bin/bash
set -euo pipefail
ulimit -c 0
job=/root/jobs/fx/cx/v21/s033/csubmit
flock -s /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$job/test-build" --parallel 3 > "$job/build-all.log" 2>&1
python3 "$job/instrument-test-commands.py"
flock -s /root/jobs/fx/cx/measure.lock ctest --test-dir "$job/test-build" --parallel 3 --output-on-failure --stop-on-failure --output-junit "$job/test-all.xml" -E '^test_cpp_(framework|stream_connector)_install_consumer$' > "$job/test-all.log" 2>&1
flock -s /root/jobs/fx/cx/measure.lock bash /root/worktree/zlink-3867s/scripts/format/format.sh --check cpp > "$job/format.log" 2>&1
