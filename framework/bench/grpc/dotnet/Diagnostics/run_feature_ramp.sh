#!/usr/bin/env bash
set -euo pipefail
bench_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "${bench_root}/../../../.." && pwd)"
duration=10
warmup=3
repetitions=1
controls=1
skip_build=0
stages=(core)
output=""
while (($#)); do
  case "$1" in
    -h|--help)
      printf '%s\n' 'Usage: bash Diagnostics/run_feature_ramp.sh [options]
Diagnostic request-serial feature addition; 64-byte request, 4096-byte response.
  --duration N       Active seconds per cell (default 10).
  --warmup N         Warmup seconds per cell (default 3).
  --repetitions N    Alternating repetitions (default 1).
  --stages LIST      Explicit serial diagnostic stages (default core only).
  --output DIR      Output directory (default temporary directory).
  --no-controls     Skip normal run_local Core/Framework endpoint controls.
  --skip-build      Reuse OUTPUT/driver build.

core: Core-like blocking Recv, reusable receipt, RawWire, inline metrics/reply.
codec: add actual Framework registered Protobuf encode/decode, same raw header.
envelope: add actual Framework envelope codec and registered Protobuf serializer.
wire: add actual application/service-wire packing, validation and payload view.
managed: add actual managed node admission, monitors, poller, maintenance,
         mailbox/claims and persistent worker pump; no host-shared job permits.
permits: add actual default balanced application job queue and ingress permits.
full: public hosted Framework route client and typed route handler.
Diagnostic rows are not production Framework benchmark results.'
      exit 0 ;;
    --duration) duration="$2"; shift 2 ;;
    --warmup) warmup="$2"; shift 2 ;;
    --repetitions) repetitions="$2"; shift 2 ;;
    --stages) read -r -a stages <<<"$2"; shift 2 ;;
    --output) output="$2"; shift 2 ;;
    --no-controls) controls=0; shift ;;
    --skip-build) skip_build=1; shift ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done
for value in "$duration" "$warmup" "$repetitions"; do
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo 'Durations and repetitions must be positive integers.' >&2; exit 2; }
done
for stage in "${stages[@]}"; do
  case "$stage" in core|codec|envelope|wire|managed|permits|full) ;; *) echo "Unknown stage: $stage" >&2; exit 2;; esac
done
[[ -n "$output" ]] || output="$(mktemp -d "${repo_root}/.artifacts/feature-ramp.XXXXXX")"
mkdir -p "$output"
output="$(cd "$output" && pwd)"
mkdir -p "${output}/tmp"
export TMPDIR="${output}/tmp"
cd "$repo_root"
if ((skip_build == 0)); then
  dotnet build framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj \
    -c Release -f net8.0 -m:1 -nr:false -p:UseSharedCompilation=false -p:MessagingFeatureRamp=true -p:OutputPath="${output}/driver/" \
    --verbosity quiet >"${output}/build.log" 2>&1 || { tail -n 20 "${output}/build.log"; exit 1; }
fi
driver="${output}/driver/Zlink.Framework.UnitTests.dll"
[[ -f "$driver" ]] || { echo "Driver not built: $driver" >&2; exit 2; }
target_pid=""
cleanup() {
  if [[ -n "$target_pid" ]]; then
    kill -TERM -- "-${target_pid}" 2>/dev/null || true
    for attempt in {1..30}; do
      kill -0 "$target_pid" 2>/dev/null || break
      sleep 0.1
    done
    if kill -0 "$target_pid" 2>/dev/null; then kill -KILL -- "-${target_pid}" 2>/dev/null || true; fi
    wait "$target_pid" 2>/dev/null || true
    target_pid=""
  fi
}
trap cleanup EXIT
for repetition in $(seq 1 "$repetitions"); do
  run_stages=("${stages[@]}")
  if ((repetition % 2 == 0)); then
    run_stages=()
    for ((index=${#stages[@]}-1; index>=0; index--)); do run_stages+=("${stages[index]}"); done
  fi
  if ((controls)); then
    bash "${bench_root}/run_local.sh" --skip-build --scenario request-serial \
      --implementation zlink-dotnet --duration-seconds "$duration" --warmup-seconds "$warmup" \
      --output "${output}/r${repetition}/control-core" >"${output}/control-core-r${repetition}.log" 2>&1
  fi
  for stage in "${run_stages[@]}"; do
    cell="${output}/r${repetition}/${stage}"
    mkdir -p "$cell"
    if ss -H -ltn | awk '{print $4}' | rg -q ':523[45]$'; then echo 'Diagnostic ports 5234/5235 are occupied.' >&2; exit 1; fi
    DOTNET_gcServer=1 setsid dotnet "$driver" --role target --stage "$stage" >"${cell}/target.log" 2>&1 &
    target_pid=$!
    ready=0
    for attempt in {1..200}; do
      if curl --silent --fail http://127.0.0.1:5235/ready >/dev/null; then ready=1; break; fi
      kill -0 "$target_pid" 2>/dev/null || { tail -n 20 "${cell}/target.log"; exit 1; }
      sleep 0.1
    done
    ((ready)) || { tail -n 20 "${cell}/target.log"; exit 1; }
    dotnet "$driver" --role source --stage "$stage" --duration "$duration" --warmup "$warmup" \
      --output "${cell}/results.json" >"${cell}/source.log" 2>&1 || {
      tail -n 20 "${cell}/source.log"; tail -n 20 "${cell}/target.log"; exit 1;
    }
    cleanup
    jq -c '{stage,throughputPerSecond,meanUs,submitted,completed,errors}' "${cell}/results.json"
  done
  if ((controls)); then
    bash "${bench_root}/run_local.sh" --skip-build --scenario request-serial \
      --implementation zlink-framework-dotnet --duration-seconds "$duration" --warmup-seconds "$warmup" \
      --output "${output}/r${repetition}/control-framework" >"${output}/control-framework-r${repetition}.log" 2>&1
  fi
done
printf 'Feature-ramp results: %s\n' "$output"
