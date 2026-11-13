#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cladder
python3 "$job/analyze.py" > "$job/summary.log"
python3 "$job/profile-analyze.py" > "$job/profile-analysis.log"
