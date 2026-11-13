#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/core124b
COMPARATOR="$JOB/src-1.24/bindings/c/perf/perf_regression_gate.py"
preflight() {
  python3 - "$JOB/confirm/preflight.jsonl" "$1" <<'PY'
import json,pathlib,sys,time
mem=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))*1024
r={'stage':sys.argv[2],'captured_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'MemAvailable_bytes':mem,'allowed':mem>=12*1024**3}
with open(sys.argv[1],'a') as f:f.write(json.dumps(r)+'\n')
print(json.dumps(r))
if not r['allowed']:raise SystemExit(3)
PY
}
while IFS=$'\t' read -r suite pattern transport reason; do
  [[ -n $suite ]] || continue
  gate_out="$JOB/confirm/gate-$suite-$pattern-$transport.txt"
  if [[ -f $gate_out ]]; then echo "SKIP completed $suite $pattern $transport"; continue; fi
  if [[ $suite == single ]]; then runner=run_benchmarks.sh; transports="$transport"; else runner=run_benchmarks_multi.sh; transports="$transport"; fi
  for kind in B1 C B2; do
    if [[ $kind == C ]]; then version=1.24; commit=0005a4d6c07160bbf661a6f7aea3d8093880514e; sha=2e0aa58e221f0cbe4755d440b20f9c71dee67f692b2fee554d44a913f8323d64
    else version=1.23; commit=79567d379162e67476ba13e18cfe0f69925e63c9; sha=c661f48b468c2943582e0a297b62ce71b748bf36e9d33b220282c090676c498a; fi
    root="$JOB/src-$version"; prefix="/root/jobs/fx/cx/v21/s033/core124/prefix-$version"; core="$prefix/lib/libzlink.so.1.23.0"
    out="$JOB/confirm/$kind-$suite-$pattern-$transport"
    if awk -F '\t' -v k="$kind" -v s="$suite" -v p="$pattern" -v t="$transport" '$1==k && $2==s && $3==p && $4==t {found=1} END {exit !found}' "$JOB/confirm/times.tsv" 2>/dev/null; then
      [[ -f "$out.txt" && -f "$out.log" ]] || { echo "Completed stage is missing raw output: $out" >&2; exit 4; }
      echo "SKIP completed stage $kind $suite $pattern $transport"
      continue
    fi
    [[ ! -e "$out.txt" && ! -e "$out.log" ]] || { echo "Refusing to overwrite incomplete output $out; preserve it with .stopped first" >&2; exit 2; }
    preflight "$kind-$suite-$pattern-$transport"
    [[ "$(git -C "$root" rev-parse HEAD)" == "$commit" ]] || { echo "Unexpected source revision: $root" >&2; exit 4; }
    [[ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ]] || { echo "Modified source: $root" >&2; exit 4; }
    [[ "$(sha256sum "$core" | cut -d' ' -f1)" == "$sha" ]] || { echo "Core SHA mismatch: $core" >&2; exit 4; }
    started=$(date -u +%FT%T.%NZ); echo "START $kind $suite $pattern $transport $started ($reason)"
    set +e
    taskset -c 8-15 env ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX="$prefix" PERF_NO_AUTOBUILD=1 PERF_PRINT_AUTO_HWM_DETAIL=0 \
      bash -c 'cd "$1"; exec bash "$2" --reuse-build --pattern "$3" --transports "$4" --msg-sizes 64,256,1024,65536 --runs 5 --output "$5"' \
      _ "$root" "$root/bindings/c/perf/$runner" "$pattern" "$transports" "$out.txt" > "$out.log" 2>&1
    rc=$?
    set -e
    ended=$(date -u +%FT%T.%NZ); printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$kind" "$suite" "$pattern" "$transport" "$started" "$ended" >> "$JOB/confirm/times.tsv"
    echo "END $kind $suite $pattern $transport $ended rc=$rc"
    (( rc == 0 )) || { tail -40 "$out.log" >&2; exit "$rc"; }
    [[ "$(sha256sum "$core" | cut -d' ' -f1)" == "$sha" ]] || { echo "Core changed during confirmation" >&2; exit 4; }
  done
  [[ ! -e "$gate_out" ]] || { echo "Refusing to overwrite completed gate $gate_out" >&2; exit 2; }
  set +e
  if [[ $suite == single ]]; then
    python3 "$COMPARATOR" --baseline-single "$JOB/confirm/B1-$suite-$pattern-$transport.txt" --candidate-single "$JOB/confirm/C-$suite-$pattern-$transport.txt" > "$gate_out" 2>&1
  else
    python3 "$COMPARATOR" --baseline-multi "$JOB/confirm/B1-$suite-$pattern-$transport.txt" --candidate-multi "$JOB/confirm/C-$suite-$pattern-$transport.txt" > "$gate_out" 2>&1
  fi
  rc=$?
  set -e
  (( rc <= 1 )) || exit "$rc"
done < "$JOB/confirm/groups.tsv"
