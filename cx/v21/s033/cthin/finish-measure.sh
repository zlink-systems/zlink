#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cthin
bash "$job/measure.sh" profile profile2 zlink-framework-cpp > "$job/profile2.log" 2>&1
bash "$job/measure.sh" none none zlink-cpp,zlink-framework-cpp > "$job/none.log" 2>&1
flock -o /root/jobs/fx/cx/measure.lock taskset -c 0-15 bash "$job/analyze-locked.sh"
