#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cthin
python3 "$job/analyze.py" > "$job/analysis.log" 2>&1
python3 "$job/scopes.py" > "$job/scopes.log" 2>&1
python3 "$job/quality.py" > "$job/quality.log" 2>&1
