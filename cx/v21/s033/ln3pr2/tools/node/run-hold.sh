#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/ln3pr2
TOOLS="$JOB/tools/node"
PROGRESS="$JOB/progress.md"
started=$SECONDS
run_one() {
  local variant="$1" step="$2" pattern="$3" payload="$4" attempt="${5:-1}"
  if (( SECONDS - started >= 470 )); then
    printf -- '- %s stop-before-run elapsed=%ss; remaining measurements continue in a later hold.\n' "$(date -u +%FT%TZ)" "$((SECONDS-started))" >> "$PROGRESS"
    return 75
  fi
  "$TOOLS/measure-one.sh" "$variant" "$step" "$pattern" "$payload" "$attempt"
  sleep 3
}
for pair in 'send-saturation 1024' 'send-saturation 4096' 'request-serial 1024' 'request-serial 4096' 'request-backpressure 1024' 'request-backpressure 4096'; do
  read -r pattern payload <<< "$pair"
  run_one A L2 "$pattern" "$payload"
  run_one B L2 "$pattern" "$payload"
  run_one A L3 "$pattern" "$payload"
  run_one B L3 "$pattern" "$payload"
  delta="$(python3 - "$JOB/raw/${pattern}-${payload}" <<'PY'
import pathlib, sys
p=pathlib.Path(sys.argv[1])
a=float((p/'A-L3/counted/attempt1/msgps.txt').read_text())
b=float((p/'B-L3/counted/attempt1/msgps.txt').read_text())
print(100*(b/a-1))
PY
)"
  abs_delta="$(awk -v d="$delta" 'BEGIN { if (d<0) d=-d; printf "%.4f", d }')"
  if awk -v d="$abs_delta" 'BEGIN { exit !(d>=3 && d<=9) }'; then
    printf -- '- %s %s-%s |B-L3/A-L3−1|=%s%% is in the 3–9%% band; add two counted A-L3/B-L3 pairs.\n' \
      "$(date -u +%FT%TZ)" "$pattern" "$payload" "$abs_delta" >> "$PROGRESS"
    for attempt in 2 3; do
      run_one A L3 "$pattern" "$payload" "$attempt"
      run_one B L3 "$pattern" "$payload" "$attempt"
    done
  fi
done
printf -- '- %s hold complete elapsed=%ss\n' "$(date -u +%FT%TZ)" "$((SECONDS-started))" >> "$PROGRESS"
