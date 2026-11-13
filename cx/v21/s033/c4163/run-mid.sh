#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/c4163
RUNS="$JOB/runs"
mkdir -p "$RUNS"
stage() {
  local kind="$1" round="$2" version="$3" root="$4" prefix="$5" expected_sha="$6"
  local sha runtime out started
  runtime="$prefix/lib/libzlink.so.1.23.0"
  sha="$(sha256sum "$runtime" | cut -d' ' -f1)"
  [[ "$sha" == "$expected_sha" ]] || { echo "Core SHA mismatch $runtime: $sha" >&2; exit 4; }
  out="$RUNS/$kind-r$round-single-ROUTER_ROUTER-inproc"
  [[ ! -e "$out.txt" && ! -e "$out.log" ]] || { echo "output exists: $out" >&2; exit 2; }
  started="$(date -u +%FT%T.%NZ)"
  flock -o /root/jobs/fx/cx/measure.lock taskset -c 8-15 env \
    ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX="$prefix" PERF_NO_AUTOBUILD=1 PERF_PRINT_AUTO_HWM_DETAIL=0 \
    bash -c 'python3 - "$1" <<'"'"'PY'"'"'
import json,pathlib,shutil,sys,time
mem=int(next(x.split()[1] for x in pathlib.Path("/proc/meminfo").read_text().splitlines() if x.startswith("MemAvailable:")))*1024
usage=sum(p.stat().st_size for p in pathlib.Path(sys.argv[1]).rglob("*") if p.is_file())
r=dict(stage="measure",captured_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ",time.gmtime()),MemAvailable_bytes=mem,job_bytes=usage,job_cap_bytes=25*1024**3,root_free_bytes=shutil.disk_usage("/").free,allowed=mem>=12*1024**3 and usage<25*1024**3)
with open(pathlib.Path(sys.argv[1])/"measure-preflight.jsonl","a") as f:f.write(json.dumps(r)+"\\n")
print(json.dumps(r),flush=True)
if not r["allowed"]:raise SystemExit(3)
PY
    cd "$2"; exec bash "$3" --reuse-build --pattern ROUTER_ROUTER --transports inproc --msg-sizes 256,1024 --runs 5 --output "$4"' \
    _ "$JOB" "$root" "$root/bindings/c/perf/run_benchmarks.sh" "$out.txt" > "$out.log" 2>&1
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$kind" "$round" "$version" "$started" "$(date -u +%FT%T.%NZ)" "$sha" "$out.txt" >> "$RUNS/times.tsv"
  echo "END $kind r$round"
}
B=/root/jobs/fx/cx/v21/s033/core124b/src-1.23
C=/root/jobs/fx/cx/v21/s033/core124b/src-1.24
P_B=/root/jobs/fx/cx/v21/s033/core124/prefix-1.23
P_C=/root/jobs/fx/cx/v21/s033/core124/prefix-1.24
P_M="$JOB/prefix-mid"
SHA_B=c661f48b468c2943582e0a297b62ce71b748bf36e9d33b220282c090676c498a
SHA_C=2e0aa58e221f0cbe4755d440b20f9c71dee67f692b2fee554d44a913f8323d64
SHA_M=18bd2797edb71eaead4b79c0192c21df1d50867d8f1c221aeec2cb89bb4b96ae
stage B 1 1.23 "$B" "$P_B" "$SHA_B"
stage M 1 mid "$JOB/src-mid" "$P_M" "$SHA_M"
stage C 1 1.24 "$C" "$P_C" "$SHA_C"
stage C 2 1.24 "$C" "$P_C" "$SHA_C"
stage M 2 mid "$JOB/src-mid" "$P_M" "$SHA_M"
stage B 2 1.23 "$B" "$P_B" "$SHA_B"
