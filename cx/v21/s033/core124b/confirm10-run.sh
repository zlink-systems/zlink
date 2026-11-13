#!/usr/bin/env bash
# PM decision 2026-11-10 12:38Z: runs-10 for three candidate-leaning cells, B1/C/B2 alternating (two rounds of 5: B1 C B2, then B2 C B1).
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/core124b
OUT=$JOB/confirm10
while pgrep -f '[b]uild-after-gate.sh' >/dev/null; do sleep 60; done
echo "psmeas chain gone $(date -u +%T)"
groups=("single ROUTER_ROUTER inproc 256,1024" "multi DEALER_DEALER tcp 256")
for round in 1 2; do
  order="B1 C B2"; [[ $round == 2 ]] && order="B2 C B1"
  for g in "${groups[@]}"; do
    read -r suite pattern transport sizes <<<"$g"
    runner=run_benchmarks.sh; [[ $suite == multi ]] && runner=run_benchmarks_multi.sh
    for kind in $order; do
      if [[ $kind == C ]]; then version=1.24; sha=2e0aa58e221f0cbe4755d440b20f9c71dee67f692b2fee554d44a913f8323d64
      else version=1.23; sha=c661f48b468c2943582e0a297b62ce71b748bf36e9d33b220282c090676c498a; fi
      root=$JOB/src-$version; prefix=/root/jobs/fx/cx/v21/s033/core124/prefix-$version; core=$prefix/lib/libzlink.so.1.23.0
      [[ "$(sha256sum "$core" | cut -d' ' -f1)" == "$sha" ]] || { echo "Core SHA mismatch $core" >&2; exit 4; }
      mem=$(awk '/MemAvailable/{print $2}' /proc/meminfo); (( mem >= 12*1024*1024 )) || { echo "MemAvailable low: $mem kB" >&2; exit 3; }
      out=$OUT/$kind-r$round-$suite-$pattern-$transport
      [[ ! -e $out.txt ]] || { echo "exists $out.txt" >&2; exit 2; }
      started=$(date -u +%FT%T.%NZ)
      flock -o /root/jobs/fx/cx/measure.lock taskset -c 8-15 env ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX="$prefix" PERF_NO_AUTOBUILD=1 PERF_PRINT_AUTO_HWM_DETAIL=0 \
        bash -c 'cd "$1"; exec bash "$2" --reuse-build --pattern "$3" --transports "$4" --msg-sizes "$5" --runs 5 --output "$6"' \
        _ "$root" "$root/bindings/c/perf/$runner" "$pattern" "$transport" "$sizes" "$out.txt" > "$out.log" 2>&1
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$kind" "$round" "$suite" "$pattern" "$transport" "$started" "$(date -u +%FT%T.%NZ)" >> "$OUT/times.tsv"
      echo "END $kind r$round $suite $pattern $transport"
    done
  done
done
echo "confirm10 done $(date -u +%T)"
