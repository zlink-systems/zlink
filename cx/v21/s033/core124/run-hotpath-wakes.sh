#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/core124
BUILD="$JOB/src-1.24/core/build/core-release-gate-tests"
preflight() {
  python3 - "$JOB/test-preflight.jsonl" "$1" <<'PY'
import json,pathlib,sys,time
mem=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))*1024
r={'stage':sys.argv[2],'captured_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'MemAvailable_bytes':mem,'required_bytes':12*1024**3,'allowed':mem>=12*1024**3}
with open(sys.argv[1],'a') as f:f.write(json.dumps(r)+'\n')
print(json.dumps(r))
if not r['allowed']:raise SystemExit(3)
PY
}
for spec in 'hotpath_gate:-R ^hotpath_gate$' 'wake_invariants:-R ^test_wake_invariant --repeat until-fail:10'; do
  name=${spec%%:*}; args=${spec#*:}; log="$JOB/logs/$name.log"
  [[ ! -e "$log" ]] || { echo "Refusing to overwrite $log" >&2; exit 2; }
  preflight "$name"
  started=$(date -u +%FT%T.%NZ); echo "START $name $started"
  set +e
  if [[ $name == hotpath_gate ]]; then
    ctest --test-dir "$BUILD" -R '^hotpath_gate$' --output-on-failure > "$log" 2>&1
  else
    ctest --test-dir "$BUILD" -R '^test_wake_invariant' --repeat until-fail:10 --output-on-failure > "$log" 2>&1
  fi
  rc=$?
  set -e
  ended=$(date -u +%FT%T.%NZ); printf '%s\t%s\t%s\t%s\n' "$name" "$started" "$ended" "$rc" >> "$JOB/test-times.tsv"
  echo "END $name $ended rc=$rc"
  (( rc == 0 )) || { tail -60 "$log" >&2; exit "$rc"; }
done
