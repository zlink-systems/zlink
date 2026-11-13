#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cdiag
bash "$job/measure-pm.sh" > "$job/measure-pm.log" 2>&1
bash "$job/measure-perf99.sh" > "$job/measure-perf99.log" 2>&1
bash "$job/measure-mem.sh" > "$job/measure-mem.log" 2>&1
