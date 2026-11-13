#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/f4163b
RUNS="$JOB/runs"
B=/root/jobs/fx/cx/v21/s033/core124b/src-1.23
C=/root/jobs/fx/cx/v21/s033/core124b/src-1.24
P_B=/root/jobs/fx/cx/v21/s033/core124/prefix-1.23
P_C=/root/jobs/fx/cx/v21/s033/core124/prefix-1.24
P_M="$JOB/prefix-mid"
SHA_B=c661f48b468c2943582e0a297b62ce71b748bf36e9d33b220282c090676c498a
SHA_C=2e0aa58e221f0cbe4755d440b20f9c71dee67f692b2fee554d44a913f8323d64
SHA_M=18bd2797edb71eaead4b79c0192c21df1d50867d8f1c221aeec2cb89bb4b96ae
stage() {
  local kind="$1" round="$2" version="$3" root="$4" prefix="$5" expected_sha="$6" label="$7"
  local sha runtime out started
  runtime="$prefix/lib/libzlink.so.1.23.0"
  sha="$(sha256sum "$runtime" | cut -d' ' -f1)"
  [[ "$sha" == "$expected_sha" ]] || { echo "Core SHA mismatch $runtime: $sha" >&2; exit 4; }
  out="$RUNS/$label-single-ROUTER_ROUTER-inproc"
  [[ ! -e "$out.txt" && ! -e "$out.log" ]] || { echo "output exists: $out" >&2; exit 2; }
  started="$(date -u +%FT%T.%NZ)"
  flock -o /root/jobs/fx/cx/measure.lock taskset -c 8-15 env \
    ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX="$prefix" PERF_NO_AUTOBUILD=1 PERF_PRINT_AUTO_HWM_DETAIL=0 \
    bash -c 'python3 "$4/stage-preflight.py" "$4"; cd "$1"; exec bash "$2" --reuse-build --pattern ROUTER_ROUTER --transports inproc --msg-sizes 256,1024 --runs 5 --output "$3" --results-dir "$4/runs/saved-results"' \
    _ "$root" "$root/bindings/c/perf/run_benchmarks.sh" "$out.txt" "$JOB" > "$out.log" 2>&1
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$kind" "$round" "$version" "$started" "$(date -u +%FT%T.%NZ)" "$sha" "$out.txt" >> "$RUNS/times.tsv"
  echo "END $kind r$round"
}
P_R="$JOB/prefix-R"
P_L="$JOB/prefix-L"
SHA_R="$(sha256sum "$P_R/lib/libzlink.so.1.23.0" | cut -d' ' -f1)"
SHA_L="$(sha256sum "$P_L/lib/libzlink.so.1.23.0" | cut -d' ' -f1)"
[[ "$SHA_L" != "$SHA_C" ]] || { echo 'L must differ from C'; exit 4; }
stage C 1 candidate "$C" "$P_C" "$SHA_C" C-r1
stage R 1 caller-revert "$JOB/src-R" "$P_R" "$SHA_R" R-r1
stage L 1 layout-only "$JOB/src-L" "$P_L" "$SHA_L" L-r1
stage L 2 layout-only "$JOB/src-L" "$P_L" "$SHA_L" L-r2
stage R 2 caller-revert "$JOB/src-R" "$P_R" "$SHA_R" R-r2
stage C 2 candidate "$C" "$P_C" "$SHA_C" C-r2
