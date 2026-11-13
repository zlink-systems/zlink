#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/f4163b
bash "$JOB/build-probe.sh" R > "$JOB/logs/probe-R-stage.log" 2>&1
python3 "$JOB/capture-build.py" R probe-R
bash "$JOB/build-probe.sh" L > "$JOB/logs/probe-L-stage.log" 2>&1
python3 "$JOB/capture-build.py" L probe-L
bash "$JOB/probe-run.sh" probe-C /root/jobs/fx/cx/v21/s033/core124b/src-1.24 /root/jobs/fx/cx/v21/s033/f4163/prefix-probe-C
bash "$JOB/probe-run.sh" probe-R "$JOB/src-R"
bash "$JOB/probe-run.sh" probe-L "$JOB/src-L"
