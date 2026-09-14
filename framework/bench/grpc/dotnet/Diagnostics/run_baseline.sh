#!/usr/bin/env bash
set -euo pipefail
bench_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "${bench_root}/../../../.." && pwd)"
duration=10
warmup=3
repetitions=1
skip_build=0
feature=""
before_driver=""
output=""
while (($#)); do
  case "$1" in
    -h|--help)
      printf '%s\n' 'Usage: bash Diagnostics/run_baseline.sh [options]
Record cumulative feature boundaries and Core controls, all three patterns.
The existing Client submission/reply, eight-stream send and drain loops are reused.
  --duration N       Active seconds (default 10).
  --warmup N         Warmup seconds (default 3).
  --repetitions N    Alternating Core/baseline pairs (default 1).
  --output DIR      Artifact directory (default temporary .artifacts directory).
  --skip-build      Reuse OUTPUT/driver build.
  --feature codec|envelope|wire  Add one cumulative feature; record all three patterns.
  --before-driver DLL  Include the pinned pre-fix codec executable in that comparison.
No managed/permit/handler stage is enabled. Inspect the summary before proceeding.'
      exit 0 ;;
    --duration) duration="$2"; shift 2 ;;
    --warmup) warmup="$2"; shift 2 ;;
    --repetitions) repetitions="$2"; shift 2 ;;
    --output) output="$2"; shift 2 ;;
    --skip-build) skip_build=1; shift ;;
    --feature) feature="$2"; shift 2 ;;
    --before-driver) before_driver="$2"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done
[[ -z "$feature" || "$feature" == codec || "$feature" == envelope || "$feature" == wire ]] || { echo 'Only codec, envelope or wire may be added.' >&2; exit 2; }
[[ -z "$before_driver" || ( "$feature" == codec && -f "$before_driver" ) ]] || {
  echo 'Pre-fix driver requires --feature codec and an existing DLL.' >&2; exit 2;
}
for value in "$duration" "$warmup" "$repetitions"; do
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo 'Positive integer required.' >&2; exit 2; }
done
[[ -n "$output" ]] || output="$(mktemp -d "${repo_root}/.artifacts/baseline-three.XXXXXX")"
mkdir -p "$output"
output="$(cd "$output" && pwd)"
mkdir -p "${output}/tmp"
export TMPDIR="${output}/tmp"
cd "$repo_root"
unset BENCH_DIAGNOSTIC_TARGET_DLL BENCH_DIAGNOSTIC_SOURCE_DLL BENCH_DIAGNOSTIC_STAGE BENCH_DIAGNOSTIC_NO_DEADLINE BENCH_DIAGNOSTIC_MINIMAL_HEADER_READ
driver_dir="${output}/driver"
prefix=baseline
if [[ -n "$feature" ]]; then driver_dir="${output}/driver-${feature}"; prefix="$feature"; fi
if ((skip_build == 0)); then
  dotnet build framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj \
    -c Release -f net8.0 -m:1 -nr:false -p:UseSharedCompilation=false \
    -p:MessagingFeatureRamp=true -p:OutputPath="${driver_dir}/" --verbosity quiet \
    >"${output}/baseline-build.log" 2>&1 || { tail -n 20 "${output}/baseline-build.log"; exit 1; }
fi
driver="${driver_dir}/Zlink.Framework.UnitTests.dll"
[[ -f "$driver" ]] || { echo "Missing driver: $driver" >&2; exit 2; }
results=()
for repetition in $(seq 1 "$repetitions"); do
  versions=(core stripped)
  [[ -z "$before_driver" ]] || versions+=(codec-before)
  [[ -z "$feature" ]] || versions+=(codec)
  [[ "$feature" != envelope && "$feature" != wire ]] || versions+=(envelope)
  [[ "$feature" != wire ]] || versions+=(wire)
  if ((repetition % 2 == 0)); then
    reversed=()
    for ((index=${#versions[@]}-1; index>=0; index--)); do reversed+=("${versions[index]}"); done
    versions=("${reversed[@]}")
  fi
  for version in "${versions[@]}"; do
    cell="${output}/${prefix}-r${repetition}/${version}"
    mkdir -p "$cell"
    (
      if [[ "$version" == stripped ]]; then export BENCH_DIAGNOSTIC_TARGET_DLL="$driver"; fi
      if [[ -n "$feature" && "$version" != core ]]; then
        export BENCH_DIAGNOSTIC_TARGET_DLL="$driver" BENCH_DIAGNOSTIC_SOURCE_DLL="$driver"
        export BENCH_DIAGNOSTIC_STAGE=core
        if [[ "$version" == codec* ]]; then export BENCH_DIAGNOSTIC_STAGE=codec; fi
        if [[ "$version" == envelope ]]; then export BENCH_DIAGNOSTIC_STAGE=envelope; fi
        if [[ "$version" == wire ]]; then export BENCH_DIAGNOSTIC_STAGE=wire; fi
        if [[ "$version" == codec-before ]]; then
          export BENCH_DIAGNOSTIC_TARGET_DLL="$before_driver" BENCH_DIAGNOSTIC_SOURCE_DLL="$before_driver"
        fi
      fi
      bash "${bench_root}/run_local.sh" --skip-build --scenario all --implementation zlink-dotnet \
        --duration-seconds "$duration" --warmup-seconds "$warmup" --output "$cell"
    ) >"${cell}/runner.log" 2>&1 || { tail -n 25 "${cell}/runner.log"; exit 1; }
    for pattern in request-serial request-backpressure send-saturation; do
      result="${cell}/zlink-dotnet-${pattern}-4096/results.json"
      jq -e 'all(.cells[]; .errors == 0 and .server_errors == 0 and .abandoned == 0
        and .drain_bound_hit == false and .target_stats.errors == 0
        and .submitted == .server_received_at_close
        and (if .pattern == "send-saturation" then true else .completed == .submitted end))' \
        "$result" >/dev/null || { echo "Invalid baseline cell: $result" >&2; exit 1; }
      jq -c --arg version "$version" '.cells[] | {version:$version,pattern,throughput_per_second,
        submitted,completed,received:.server_received_at_close,errors,drain_ms}' "$result"
      results+=("$result")
    done
  done
done
jq -s --arg feature "$feature" '
  def median: sort | if length % 2 == 1 then .[length/2|floor]
    else (.[length/2-1] + .[length/2])/2 end;
  [ .[] | .cells[] | {version:(.trigger.runId),pattern,throughput_per_second,
    latency_mean_ms,client_cpu_percent,client_memory_mb,server_cpu_percent,server_memory_mb,
    submitted,completed,received:.server_received_at_close,errors,abandoned,drain_ms} ] as $cells
  | {diagnosticOnly:true, stage:(if $feature=="" then "stripped-baseline" else $feature end), cells:$cells,
    summary:($cells | group_by(.pattern) | map(
      . as $group
      | ($group | map(select(.version=="core") | .throughput_per_second) | median) as $core
      | ($group | map(select(.version=="stripped") | .throughput_per_second) | median) as $stripped
      | ($group | map(select(.version=="codec") | .throughput_per_second) | if length==0 then null else median end) as $codec
      | ($group | map(select(.version=="codec-before") | .throughput_per_second) | if length==0 then null else median end) as $before
      | ($group | map(select(.version=="envelope") | .throughput_per_second) | if length==0 then null else median end) as $envelope
      | ($group | map(select(.version=="wire") | .throughput_per_second) | if length==0 then null else median end) as $wire
      | {pattern:.[0].pattern,coreMedianQps:$core,strippedMedianQps:$stripped,
         strippedToCoreRatio:($stripped/$core)}
        + (if $codec==null then {} else {codecMedianQps:$codec,
          codecToStrippedRatio:($codec/$stripped),codecToCoreRatio:($codec/$core)} end)
        + (if $before==null then {} else {codecBeforeMedianQps:$before,
          codecChangeRatio:($codec/$before)} end)
        + (if $envelope==null then {} else {envelopeMedianQps:$envelope,
          envelopeToCodecRatio:($envelope/$codec),envelopeToCoreRatio:($envelope/$core)} end)
        + (if $wire==null then {} else {wireMedianQps:$wire,
          wireToEnvelopeRatio:($wire/$envelope),wireToCoreRatio:($wire/$core)} end)))}
' "${results[@]}" >"${output}/${prefix}-summary.json"
jq '.summary' "${output}/${prefix}-summary.json"
printf 'Baseline artifacts: %s\n' "$output"
