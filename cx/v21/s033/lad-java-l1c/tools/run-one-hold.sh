#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-java-l1c
RUNNER="$JOB/tools/java/run-step-locked.sh"
start=$(date +%s)
check_time() {
  local elapsed=$(( $(date +%s) - start ))
  if (( elapsed >= 570 )); then
    echo "9-minute safety stop; elapsed=${elapsed}s" >&2
    exit 124
  fi
}
printf 'hold_start_utc=%s\n' "$(date -u +%FT%TZ)" >> "$JOB/raw/holds.txt"
trap 'printf "hold_end_utc=%s\n" "$(date -u +%FT%TZ)" >> "$JOB/raw/holds.txt"' EXIT
cells=('send-saturation 1024' 'send-saturation 4096' 'request-serial 1024' 'request-serial 4096' 'request-backpressure 1024' 'request-backpressure 4096')
for cell in "${cells[@]}"; do
  read -r pattern size <<< "$cell"
  for step in L0 L1; do
    check_time
    bash "$RUNNER" "$step" initial "$pattern" "$size"
  done
done
python3 - "$JOB" <<'PY'
import json,pathlib,sys
job=pathlib.Path(sys.argv[1]); raw=job/'raw/java'
border=[]
for pattern in ('send-saturation','request-serial','request-backpressure'):
  for size in (1024,4096):
    vals={}
    for step in ('L0','L1'):
      p=raw/step/f'{pattern}-{size}'/'counted/attempt1/run1'/f'zlink-framework-java-{pattern}-{size}'/'results.json'
      vals[step]=float(json.loads(p.read_text())['cells'][0]['throughput_per_second'])
    loss=(1-vals['L1']/vals['L0'])*100
    if 5 <= loss <= 9:
      border.append((pattern,size,loss))
(job/'raw/borderline.txt').write_text('\n'.join(f'{p} {s} loss={l:.3f}%' for p,s,l in border)+('\n' if border else 'none\n'))
print('borderline cells:',border)
PY
while read -r pattern size loss; do
  [[ -n "${pattern:-}" && "$pattern" != none ]] || continue
  for pair in 2 3; do
    for step in L0 L1; do
      check_time
      bash "$RUNNER" "$step" initial "$pattern" "$size"
    done
  done
done < "$JOB/raw/borderline.txt"
printf 'hold_complete_utc=%s\n' "$(date -u +%FT%TZ)" >> "$JOB/raw/holds.txt"
