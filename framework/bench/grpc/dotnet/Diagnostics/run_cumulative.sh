#!/usr/bin/env bash
set -euo pipefail

bench_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "${bench_root}/../../../.." && pwd)"
stage=core
driver=""
output=""
duration=10
warmup=3
while (($#)); do
  case "$1" in
    -h|--help)
      printf '%s\n' 'Usage: bash Diagnostics/run_cumulative.sh [options]
Measure one cumulative boundary, all three patterns, once each.
  --stage core|codec|envelope|wire  Boundary to inspect (default core).
  --driver DLL    Reuse a diagnostic build; otherwise build into OUTPUT/driver.
  --output DIR    New artifact directory (default temporary directory).
  --duration N    Active seconds (default 10).
  --warmup N      Warmup seconds (default 3).

core compares the normal binding benchmark with the stripped diagnostic.
codec compares stripped core with the actual Framework body codec.
envelope retains codec and adds the actual JSON envelope header.
wire retains codec/header and adds application/service wire framing.
Only the preceding stage and this stage run. No later feature runs automatically.
Core/binding libraries and the production Framework runtime are not modified.'
      exit 0 ;;
    --stage) stage="$2"; shift 2 ;;
    --driver) driver="$2"; shift 2 ;;
    --output) output="$2"; shift 2 ;;
    --duration) duration="$2"; shift 2 ;;
    --warmup) warmup="$2"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done
case "$stage" in
  core) versions=(binding-core core) ;;
  codec) versions=(core codec) ;;
  envelope) versions=(codec envelope) ;;
  wire) versions=(envelope wire) ;;
  *) echo "Unknown stage: $stage" >&2; exit 2 ;;
esac
for value in "$duration" "$warmup"; do
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo 'Positive duration required.' >&2; exit 2; }
done
[[ -n "$output" ]] || output="$(mktemp -d "${repo_root}/.artifacts/cumulative-${stage}.XXXXXX")"
mkdir -p "$output"
output="$(cd "$output" && pwd)"
[[ ! -e "${output}/summary.json" ]] || { echo 'Output already contains a completed comparison.' >&2; exit 2; }
mkdir -p "${output}/tmp"
export TMPDIR="${output}/tmp"
cd "$repo_root"
# Independent-ablation settings must not leak into the cumulative baseline.
while IFS= read -r diagnostic_variable; do
  unset "$diagnostic_variable"
done < <(compgen -v | rg '^BENCH_DIAGNOSTIC_' || true)
if [[ -z "$driver" ]]; then
  dotnet build framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj \
    -c Release -f net8.0 -m:1 -nr:false -p:UseSharedCompilation=false \
    -p:MessagingFeatureRamp=true -p:OutputPath="${output}/driver/" --verbosity quiet \
    >"${output}/build.log" 2>&1 || { tail -n 20 "${output}/build.log"; exit 1; }
  driver="${output}/driver/Zlink.Framework.UnitTests.dll"
fi
[[ -f "$driver" ]] || { echo "Driver not found: $driver" >&2; exit 2; }
driver="$(realpath "$driver")"
sha256sum "$driver" "$(dirname "$driver")/Zlink.Framework.dll" >"${output}/build.sha256"
results=()
for version in "${versions[@]}"; do
  cell="${output}/${version}"
  [[ ! -e "$cell" ]] || { echo "Cell already exists: $cell" >&2; exit 2; }
  environment=()
  if [[ "$version" != binding-core ]]; then
    environment+=(BENCH_DIAGNOSTIC_STAGE="$version" BENCH_DIAGNOSTIC_DIRECT_BODY_OWNER=1
      BENCH_DIAGNOSTIC_SOURCE_DLL="$driver" BENCH_DIAGNOSTIC_TARGET_DLL="$driver")
    env "${environment[@]}" dotnet "$driver" --payload-fidelity >"${output}/${version}.fidelity.log" 2>&1
  fi
  env "${environment[@]}" bash "${bench_root}/run_local.sh" --skip-build --scenario all \
    --implementation zlink-dotnet --duration-seconds "$duration" --warmup-seconds "$warmup" \
    --output "$cell" >"${output}/${version}.log" 2>&1 || { tail -n 25 "${output}/${version}.log"; exit 1; }
  for pattern in request-serial request-backpressure send-saturation; do
    result="${cell}/zlink-dotnet-${pattern}-4096/results.json"
    jq -e '(.cells | length) == 1 and all(.cells[];
      .errors == 0 and .server_errors == 0 and .abandoned == 0
      and .drain_bound_hit == false and .target_stats.errors == 0
      and .submitted == .server_received_at_close
      and (if .pattern == "send-saturation" then true else .completed == .submitted end))' \
      "$result" >/dev/null || { echo "Invalid cell: $result" >&2; exit 1; }
    jq -c --arg version "$version" '.cells[] | {version:$version,pattern,throughput_per_second,
      client_cpu_percent,server_cpu_percent,submitted,completed,server_received_at_close,errors,drain_ms}' "$result"
    results+=("$result")
  done
done
jq -s --arg stage "$stage" --arg before "${versions[0]}" --arg after "${versions[1]}" \
  --argjson logicalCpuCount "$(nproc)" --argjson duration "$duration" --argjson warmup "$warmup" '
  [ .[] | .cells[] | {version:.trigger.runId,pattern,throughput_per_second,
      client_cpu_percent,server_cpu_percent,client_memory_mb,server_memory_mb,
      submitted,completed,server_received_at_close,errors,server_errors,drain_ms} ] as $cells
  | {diagnosticOnly:true,stage:$stage,runs:1,logicalCpuCount:$logicalCpuCount,
      durationSeconds:$duration,warmupSeconds:$warmup,cells:$cells,
      comparison:($cells | group_by(.pattern) | map(
        (map(select(.version == $before))[0]) as $b
        | (map(select(.version == $after))[0]) as $a
        | {pattern:.[0].pattern,before:$before,after:$after,
           beforeQps:$b.throughput_per_second,afterQps:$a.throughput_per_second,
           throughputLossPercent:(100 * (1 - $a.throughput_per_second / $b.throughput_per_second)),
           beforeServerCpuUsPerMessage:($b.server_cpu_percent * $logicalCpuCount * 10000 / $b.throughput_per_second),
           afterServerCpuUsPerMessage:($a.server_cpu_percent * $logicalCpuCount * 10000 / $a.throughput_per_second)}))}
' "${results[@]}" >"${output}/summary.json"
jq -c '.comparison[]' "${output}/summary.json"
printf 'Cumulative boundary results: %s\n' "$output"
