#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/l3868-s1
TOOLS="$JOB/tools"
"$TOOLS/run-group.sh" "$TOOLS/plan-binding.json"
python3 "$TOOLS/summarize.py" --binding > "$JOB/raw/aggregate-binding.json"
for pair in L0-L1 L2-L3 L4-L7; do
  "$TOOLS/run-group.sh" "$TOOLS/plan-$pair.json"
  python3 "$TOOLS/summarize.py" --repeat-plan "$TOOLS/repeat-$pair.json" > "$JOB/raw/aggregate-$pair.json"
  if python3 - "$TOOLS/repeat-$pair.json" <<'PY'
import json,sys
raise SystemExit(0 if json.load(open(sys.argv[1])) else 1)
PY
  then
    "$TOOLS/run-group.sh" "$TOOLS/repeat-$pair.json"
    python3 "$TOOLS/summarize.py" > "$JOB/raw/aggregate-$pair-repeated.json"
  fi
  python3 "$TOOLS/write-report-s1.py"
done
