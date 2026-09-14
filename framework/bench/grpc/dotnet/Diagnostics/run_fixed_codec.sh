#!/usr/bin/env bash
set -euo pipefail
bench_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "${bench_root}/../../../.." && pwd)"
output=""
duration=10
warmup=3
skip_build=0
feature=codec
production_comparison=0
previous_output=""
while (($#)); do
  case "$1" in
    -h|--help)
      printf '%s\n' 'Usage: bash Diagnostics/run_fixed_codec.sh [options]
Compare two compile-time-fixed feature paths, all three patterns, once each.
  --feature codec|envelope|wire|mailbox  Add one feature to its preceding stage (default codec).
  --output DIR    Artifact directory (default temporary directory).
  --duration N    Active seconds (default 10).
  --warmup N      Warmup seconds (default 3).
  --skip-build    Reuse OUTPUT/<stage>-driver builds.
  --previous-output DIR  Reuse the preceding stage executable and results; measure only the new stage.
  --production-comparison  Also run the production Core/Framework comparison.

codec compares direct Protobuf with Framework body codec.
envelope compares body codec with the same codec plus actual JSON envelope handling.
wire compares the envelope stage with the same stage plus application/service wire framing.
mailbox adds queue/claim/drain/release on the same receiver thread to the wire stage.
Both use the same stripped transport loop and new Message(size) body owners.
No environment variable selects a feature or enables later Framework features.
Production Core/Framework measurement is opt-in and remains separate from
the fixed feature ladder.'
      exit 0 ;;
    --output) output="$2"; shift 2 ;;
    --duration) duration="$2"; shift 2 ;;
    --warmup) warmup="$2"; shift 2 ;;
    --skip-build) skip_build=1; shift ;;
    --production-comparison) production_comparison=1; shift ;;
    --previous-output) previous_output="$2"; shift 2 ;;
    --feature) feature="$2"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done
case "$feature" in
  codec) variants=(core codec) ;;
  envelope) variants=(codec envelope) ;;
  wire) variants=(envelope wire) ;;
  mailbox) variants=(wire mailbox) ;;
  *) echo "Unsupported feature: $feature" >&2; exit 2 ;;
esac
for value in "$duration" "$warmup"; do
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo 'Positive duration required.' >&2; exit 2; }
done
[[ -n "$output" ]] || output="$(mktemp -d "${repo_root}/.artifacts/fixed-${feature}.XXXXXX")"
mkdir -p "$output"
output="$(cd "$output" && pwd)"
[[ ! -e "${output}/summary.json" ]] || { echo 'Completed comparison already exists.' >&2; exit 2; }
mkdir -p "${output}/tmp"
export TMPDIR="${output}/tmp"
cd "$repo_root"
if [[ -n "$previous_output" ]]; then
  previous_output="$(cd "$previous_output" && pwd)"
  [[ -f "${previous_output}/summary.json" ]] || { echo 'Previous comparison summary missing.' >&2; exit 2; }
fi
declare -A driver_directories
while IFS= read -r diagnostic_variable; do unset "$diagnostic_variable"; done \
  < <(compgen -v | rg '^BENCH_DIAGNOSTIC_' || true)
for variant in "${variants[@]}"; do
  driver_directory="${output}/${variant}-driver"
  if [[ -n "$previous_output" && "$variant" == "${variants[0]}" ]]; then
    driver_directory="${previous_output}/${variant}-driver"
  elif ((skip_build == 0)); then
    dotnet build framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj \
      -c Release -f net8.0 -m:1 -nr:false -p:UseSharedCompilation=false \
      -p:MessagingFeatureRamp=true -p:MessagingFixedCodec="$variant" \
      -p:OutputPath="${output}/${variant}-driver/" --verbosity quiet \
      >"${output}/${variant}-build.log" 2>&1 || { tail -n 20 "${output}/${variant}-build.log"; exit 1; }
  fi
  driver_directories[$variant]="$driver_directory"
  driver="${driver_directory}/Zlink.Framework.UnitTests.dll"
  [[ -f "$driver" ]] || { echo "Missing fixed driver: $driver" >&2; exit 2; }
  # Deliberately invalid feature settings prove that the fixed build ignores them.
  env BENCH_DIAGNOSTIC_STAGE=invalid BENCH_DIAGNOSTIC_PERMIT_BATCH_SIZE=invalid \
    BENCH_DIAGNOSTIC_INLINE_MAILBOX=1 BENCH_DIAGNOSTIC_DIRECT_BODY_OWNER=0 \
    dotnet "$driver" --role info --fixed-codec-info >"${output}/${variant}.info.log" 2>&1
  rg '^\{' "${output}/${variant}.info.log" | jq -e --arg variant "$variant" \
    '.fixedCodec == $variant and .featureSelection == "compile-time" and .directBodyOwner == true
      and .envelopeHeader == ($variant == "envelope" or $variant == "wire" or $variant == "mailbox")
      and .applicationWire == ($variant == "wire" or $variant == "mailbox")
      and .mailbox == ($variant == "mailbox") and .workers == 0
      and .permitBatchSize == 0 and .ingressYieldInterval == 0' >/dev/null
  dotnet "$driver" --role info --fixed-codec-fidelity >"${output}/${variant}.fidelity.log" 2>&1
  rg '^\{' "${output}/${variant}.fidelity.log" | jq -s 'map(del(.fixedCodec))' \
    >"${output}/${variant}.fidelity.json"
  jq -e 'length == 3' "${output}/${variant}.fidelity.json" >/dev/null
  jq 'map({kind,payloadBytes,payloadBodyBytes,payloadBodySha256})' "${output}/${variant}.fidelity.json" \
    >"${output}/${variant}.body-fidelity.json"
done
cmp "${output}/${variants[0]}.body-fidelity.json" "${output}/${variants[1]}.body-fidelity.json"
if [[ "$feature" == codec ]]; then
  cmp "${output}/core.fidelity.json" "${output}/codec.fidelity.json"
fi
before_directory="${driver_directories[${variants[0]}]}"
after_directory="${driver_directories[${variants[1]}]}"
for dependency in Systems.Zlink.dll Google.Protobuf.dll WithGrpcBench.Shared.dll \
  Zlink.Framework.dll Zlink.Framework.Contracts.dll Zlink.Framework.Codecs.Protobuf.dll \
  runtimes/linux-x64/native/libzlink.so; do
  cmp "${before_directory}/${dependency}" "${after_directory}/${dependency}" \
    || { echo "Dependency differs: $dependency" >&2; exit 1; }
done
sha256sum "${before_directory}/Zlink.Framework.UnitTests.dll" \
  "${after_directory}/Zlink.Framework.UnitTests.dll" \
  "${before_directory}/"{Systems.Zlink,Google.Protobuf,WithGrpcBench.Shared,Zlink.Framework}.dll \
  >"${output}/build.sha256"
results=()
for variant in "${variants[@]}"; do
  driver="${driver_directories[$variant]}/Zlink.Framework.UnitTests.dll"
  cell="${output}/fixed-${variant}"
  if [[ -n "$previous_output" && "$variant" == "${variants[0]}" ]]; then
    cell="${previous_output}/fixed-${variant}"
  else
  [[ ! -e "$cell" ]] || { echo "Cell already exists: $cell" >&2; exit 2; }
  # These two settings select executables only, not their compiled codec paths.
  env BENCH_DIAGNOSTIC_SOURCE_DLL="$driver" BENCH_DIAGNOSTIC_TARGET_DLL="$driver" \
    bash "${bench_root}/run_local.sh" --skip-build --scenario all --implementation zlink-dotnet \
      --duration-seconds "$duration" --warmup-seconds "$warmup" --output "$cell" \
      >"${output}/fixed-${variant}.log" 2>&1 || { tail -n 25 "${output}/fixed-${variant}.log"; exit 1; }
  fi
  for pattern in request-serial request-backpressure send-saturation; do
    result="${cell}/zlink-dotnet-${pattern}-4096/results.json"
    jq -e '(.cells | length) == 1 and all(.cells[];
      .errors == 0 and .server_errors == 0 and .abandoned == 0 and .drain_bound_hit == false
      and .target_stats.errors == 0 and .submitted == .server_received_at_close
      and (if .pattern == "send-saturation" then true else .submitted == .completed end))' "$result" >/dev/null
    jq -c --arg variant "$variant" '.cells[] | {variant:$variant,pattern,throughput_per_second,
      client_cpu_percent,server_cpu_percent,errors,server_errors,submitted,completed,server_received_at_close}' "$result"
    results+=("$result")
  done
done
production_results=()
if ((production_comparison == 1)); then
  if ((skip_build == 0)); then
    dotnet build "${bench_root}/WithGrpcBench.sln" -c Release \
      -p:ShouldUnsetParentConfigurationAndPlatform=false --verbosity quiet \
      >"${output}/production-build.log" 2>&1 || { tail -n 20 "${output}/production-build.log"; exit 1; }
  fi
  for implementation in zlink-dotnet zlink-framework-dotnet; do
    cell="${output}/production-${implementation}"
    [[ ! -e "$cell" ]] || { echo "Cell already exists: $cell" >&2; exit 2; }
    bash "${bench_root}/run_local.sh" --skip-build --scenario all --implementation "$implementation" \
      --duration-seconds "$duration" --warmup-seconds "$warmup" --output "$cell" \
      >"${output}/production-${implementation}.log" 2>&1 || { tail -n 25 "${output}/production-${implementation}.log"; exit 1; }
    for pattern in request-serial request-backpressure send-saturation; do
      result="${cell}/${implementation}-${pattern}-4096/results.json"
      valid="$(jq -r '(.cells | length) == 1 and all(.cells[];
        .errors == 0 and .server_errors == 0 and .abandoned == 0 and .drain_bound_hit == false
        and .target_stats.errors == 0 and .submitted == .server_received_at_close
        and (if .pattern == "send-saturation" then true else .submitted == .completed end))' "$result")"
      [[ "$valid" == true ]] || echo "Production comparison cell is invalid and will not produce a throughput ratio: ${implementation}/${pattern}" >&2
      production_results+=("$result")
    done
  done
fi
jq -s --argjson logicalCores "$(nproc)" --arg feature "$feature" --arg previousOutput "$previous_output" \
  --arg before "fixed-${variants[0]}" --arg after "fixed-${variants[1]}" '
  [.[] | .cells[] | {variant:.trigger.runId,pattern,throughput_per_second,
    client_cpu_percent,server_cpu_percent,submitted,completed,errors,server_errors,server_received_at_close}] as $cells
  | {diagnosticOnly:true,featureSelection:"compile-time",feature:$feature,runs:1,previousOutput:$previousOutput,cells:$cells,
      comparison:($cells | group_by(.pattern) | map(
        (map(select(.variant == $before))[0]) as $a
        | (map(select(.variant == $after))[0]) as $b
        | {pattern:.[0].pattern,before:$before,after:$after,beforeQps:$a.throughput_per_second,afterQps:$b.throughput_per_second,
           throughputLossPercent:(100 * (1 - $b.throughput_per_second / $a.throughput_per_second)),
           beforeServerCpuUsPerMessage:($a.server_cpu_percent * $logicalCores * 10000 / $a.throughput_per_second),
           afterServerCpuUsPerMessage:($b.server_cpu_percent * $logicalCores * 10000 / $b.throughput_per_second)}))}
' "${results[@]}" >"${output}/summary.json"
if ((production_comparison == 1)); then
  jq -s '
    [.[] | .cells[] | {implementation:(if (.trigger.cellId | startswith("zlink-framework-dotnet-")) then "zlink-framework-dotnet" else "zlink-dotnet" end),pattern,throughput_per_second,
      client_cpu_percent,server_cpu_percent,submitted,completed,errors,server_errors,abandoned,drain_bound_hit,server_received_at_close,
      valid:(.errors == 0 and .server_errors == 0 and .abandoned == 0 and .drain_bound_hit == false and .target_stats.errors == 0 and .submitted == .server_received_at_close and (if .pattern == "send-saturation" then true else .submitted == .completed end))}] as $cells
    | {runs:1,cells:$cells,comparison:($cells | group_by(.pattern) | map(
      (map(select(.implementation == "zlink-dotnet"))[0]) as $core
      | (map(select(.implementation == "zlink-framework-dotnet"))[0]) as $framework
      | {pattern:.[0].pattern,valid:($core.valid and $framework.valid),
         coreQps:(if $core.valid then $core.throughput_per_second else null end),
         frameworkQps:(if $framework.valid then $framework.throughput_per_second else null end),
         frameworkVsCorePercent:(if $core.valid and $framework.valid then 100 * $framework.throughput_per_second / $core.throughput_per_second else null end),
         coreServerCpuPercent:$core.server_cpu_percent,
         frameworkServerCpuPercent:$framework.server_cpu_percent}))}' \
    "${production_results[@]}" >"${output}/production-core-framework-summary.json"
  jq --slurpfile production "${output}/production-core-framework-summary.json" \
    '.productionCoreFramework = $production[0]' "${output}/summary.json" >"${output}/summary.next.json"
  mv "${output}/summary.next.json" "${output}/summary.json"
fi
jq -c '.comparison[]' "${output}/summary.json"
printf 'Fixed codec comparison results: %s\n' "$output"
