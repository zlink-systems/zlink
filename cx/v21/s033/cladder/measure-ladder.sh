#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/cladder
# A failed setup check must stop the campaign, not produce a partial median.
if ! rg -q '^9 .* OK$' "$job/smoke-final.log";then echo 'full smoke has not passed' >&2;exit 1;fi
for rung in 1 2 3 4 5 6 7 8 9;do
 bash "$job/measure.sh" "$rung" none 3 > "$job/rung$rung.log" 2>&1
 python3 "$job/analyze.py" > "$job/summary.log"
done
if [[ ! -f "$job/raw/binding-reference-window100/run3/zlink-cpp-request-backpressure-1024/results.json" ]];then
 bash "$job/measure.sh" ref100 none 3 binding-reference-window100 > "$job/binding-reference-window100.log" 2>&1
fi
LADDER_UNBOUNDED=1 bash "$job/measure.sh" 9 none 3 rung9-unbounded > "$job/rung9-unbounded.log" 2>&1
python3 "$job/analyze.py" > "$job/summary.log"
