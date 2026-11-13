#!/usr/bin/env bash
# PM decision 2026-11-10 15:02Z: one session B/C/R runs-10, order B->C->R then R->C->B.
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/s4163
RUNS="$JOB/runs"
stage() {
  local kind="$1" round="$2" root="$3" prefix="$4" sha_expected="$5"
  local sha out started
  sha="$(sha256sum "$prefix/lib/libzlink.so.1.23.0" | cut -d' ' -f1)"
  [[ "$sha" == "$sha_expected" ]] || { echo "SHA mismatch $kind $sha" >&2; exit 4; }
  out="$RUNS/$kind-r$round-single-ROUTER_ROUTER-inproc"
  [[ ! -e "$out.txt" ]] || exit 2
  started="$(date -u +%FT%T.%NZ)"
  flock -o /root/jobs/fx/cx/measure.lock taskset -c 8-15 env \
    ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX="$prefix" PERF_NO_AUTOBUILD=1 PERF_PRINT_AUTO_HWM_DETAIL=0 \
    bash -c 'python3 "$4/stage-preflight.py" "$4"; cd "$1"; exec bash "$2" --reuse-build --pattern ROUTER_ROUTER --transports inproc --msg-sizes 256,1024 --runs 5 --output "$3" --results-dir "$4/runs/saved-results"' \
    _ "$root" "$root/bindings/c/perf/run_benchmarks.sh" "$out.txt" "$JOB" > "$out.log" 2>&1
  printf '%s\t%s\t%s\t%s\t%s\n' "$kind" "$round" "$started" "$(date -u +%FT%T.%NZ)" "$sha" >> "$RUNS/times.tsv"
  echo "END $kind r$round"
}
B=(/root/jobs/fx/cx/v21/s033/core124b/src-1.23 /root/jobs/fx/cx/v21/s033/core124/prefix-1.23 c661f48b468c2943582e0a297b62ce71b748bf36e9d33b220282c090676c498a)
C=(/root/jobs/fx/cx/v21/s033/core124b/src-1.24 /root/jobs/fx/cx/v21/s033/core124/prefix-1.24 2e0aa58e221f0cbe4755d440b20f9c71dee67f692b2fee554d44a913f8323d64)
R=(/root/jobs/fx/cx/v21/s033/f4163b/src-R2 /root/jobs/fx/cx/v21/s033/f4163b/prefix-R "$(sha256sum /root/jobs/fx/cx/v21/s033/f4163b/prefix-R/lib/libzlink.so.1.23.0 | cut -d' ' -f1)")
stage B 1 "${B[@]}"; stage C 1 "${C[@]}"; stage R 1 "${R[@]}"
stage R 2 "${R[@]}"; stage C 2 "${C[@]}"; stage B 2 "${B[@]}"
echo "session done $(date -u +%T)"
