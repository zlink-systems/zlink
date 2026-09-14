#!/usr/bin/env bash
# Compare a saved cumulative stage with one newly built optimization stage.
set -euo pipefail
if (($# != 5)); then
  echo 'Usage: run_optimization.sh BEFORE_DRIVER BEFORE_RESULTS AFTER_DRIVER OUTPUT LABEL' >&2
  exit 2
fi
before_driver="$1"
before_results="$2"
after_driver="$3"
output="$4"
label="$5"
duration_seconds=5
warmup_seconds=2
[[ "$label" =~ ^[a-z0-9-]+$ ]] || exit 2
[[ ! -e "$output/summary.json" ]] || { echo 'Completed comparison exists.' >&2; exit 2; }
mkdir -p "$output"
for dependency in Systems.Zlink.dll Google.Protobuf.dll WithGrpcBench.Shared.dll Zlink.Framework.Contracts.dll runtimes/linux-x64/native/libzlink.so; do
  cmp "$before_driver/$dependency" "$after_driver/$dependency"
done
sha256sum "$before_driver/Zlink.Framework.dll" "$after_driver/Zlink.Framework.dll" \
  "$before_driver/Zlink.Framework.Codecs.Protobuf.dll" "$after_driver/Zlink.Framework.Codecs.Protobuf.dll" > "$output/framework.sha256"
for variant in before after; do
  driver="$before_driver"
  [[ "$variant" != after ]] || driver="$after_driver"
  dotnet "$driver/Zlink.Framework.UnitTests.dll" --role info --fixed-codec-info > "$output/$variant.info.log"
  rg '^\{' "$output/$variant.info.log" | jq -e --arg label "$label" --arg variant "$variant" \
    '.featureSelection == "compile-time"
    and (if $label == "worker-channel-no-permit" then
      .mailbox == false and .handoffPrimitive == "bcl-channel-control"
      and .workers == 1 and .dedicatedIngress == true
      and (if $variant == "after" then
        .permitBatchSize == 0 and .noPermitExplicit == true
        and .capacityComparison == "host-permit-removed-no-replacement-bound"
        else .permitBatchSize == 1 and (.noPermitExplicit != true) end)
      elif $label == "worker-channel-handoff" and $variant == "after" then
      .mailbox == false and .handoffPrimitive == "bcl-channel-control"
      and .workers == 1 and .permitBatchSize == 1 and .dedicatedIngress == true
      else .mailbox == true end)
    and .envelopeHeader and .applicationWire
    and (.workers == 0 or .workers == 1) and (.permitBatchSize == 0 or .permitBatchSize == 1)
    and (if .permitBatchSize == 1 or .workers == 1 then .freshReceived == true else true end)' > /dev/null
  dotnet "$driver/Zlink.Framework.UnitTests.dll" --role info --fixed-codec-fidelity > "$output/$variant.fidelity.log"
  # Request deadlines change; retain semantic validation and compare stable body bytes.
  rg '^\{' "$output/$variant.fidelity.log" | jq -s 'map({kind,payloadBytes,headerBytes,payloadBodyBytes,payloadBodySha256})' > "$output/$variant.fidelity.json"
done
cmp "$output/before.fidelity.json" "$output/after.fidelity.json"
[[ ! -e "$output/cost-before-driver" ]] || { echo 'Cost driver exists.' >&2; exit 2; }
cp -a "$after_driver" "$output/cost-before-driver"
cp "$before_driver/Zlink.Framework.dll" "$output/cost-before-driver/Zlink.Framework.dll"
for operation in state-lane mailbox; do
  dotnet "$output/cost-before-driver/Zlink.Framework.UnitTests.dll" --role info "--$operation-cost" --output "$output/$operation-before.json"
  dotnet "$after_driver/Zlink.Framework.UnitTests.dll" --role info "--$operation-cost" --output "$output/$operation-after.json"
done
if jq -e '.recordsPerTurn == 1' "$output/mailbox-after.json" > /dev/null; then
  # Existing queue backlog only; this diagnostic never reads or delays live input.
  dotnet "$after_driver/Zlink.Framework.UnitTests.dll" --role info --mailbox-cost \
    --records-per-turn 64 --iterations 3125 --output "$output/mailbox-backlog-after.json"
fi
if [[ "$label" == wire-length ]]; then
  dotnet "$output/cost-before-driver/Zlink.Framework.UnitTests.dll" --role info --wire-cost \
    --iterations 20000 --output "$output/wire-before.json"
  dotnet "$after_driver/Zlink.Framework.UnitTests.dll" --role info --wire-cost \
    --iterations 20000 --output "$output/wire-after.json"
fi
if [[ "$label" == host-permit-static ]]; then
  dotnet "$output/cost-before-driver/Zlink.Framework.UnitTests.dll" --role info --permit-cost \
    --output "$output/permit-before.json"
  dotnet "$after_driver/Zlink.Framework.UnitTests.dll" --role info --permit-cost \
    --output "$output/permit-after.json"
fi
while IFS= read -r diagnostic_variable; do unset "$diagnostic_variable"; done \
  < <(compgen -v | rg '^BENCH_DIAGNOSTIC_' || true)
env BENCH_DIAGNOSTIC_SOURCE_DLL="$after_driver/Zlink.Framework.UnitTests.dll" \
  BENCH_DIAGNOSTIC_TARGET_DLL="$after_driver/Zlink.Framework.UnitTests.dll" \
  bash framework/bench/grpc/dotnet/run_local.sh --skip-build --scenario all --implementation zlink-dotnet \
    --duration-seconds "$duration_seconds" --warmup-seconds "$warmup_seconds" \
    --output "$output/fixed-$label" > "$output/measurement.log" 2>&1
for results_directory in "$before_results" "$output/fixed-$label"; do
  for pattern in request-serial request-backpressure send-saturation; do
    jq -e '(.cells | length) == 1 and all(.cells[];
      .errors == 0 and .server_errors == 0 and .abandoned == 0 and .drain_bound_hit == false
      and .target_stats.errors == 0 and .submitted == .server_received_at_close
      and (if .pattern == "send-saturation" then true else .submitted == .completed end))' \
      "$results_directory/zlink-dotnet-$pattern-4096/results.json" > /dev/null
  done
done
# Validate the measured configuration; saved baseline runs retain their own settings.
for pattern in request-serial request-backpressure send-saturation; do
  jq -e --argjson duration "$duration_seconds" --argjson warmup "$warmup_seconds" \
    'all(.cells[]; .trigger.durationMs == ($duration * 1000) and .trigger.warmup == $warmup)' \
    "$output/fixed-$label/zlink-dotnet-$pattern-4096/results.json" > /dev/null
done
jq -s '[.[] | .cells[]]' "$before_results"/zlink-dotnet-*-4096/results.json > "$output/before.cells.json"
jq -s --slurpfile before "$output/before.cells.json" --argjson cores "$(nproc)" --arg label "$label" '
  {runs:1,warmupSeconds:.[0].cells[0].trigger.warmup,
    durationSeconds:(.[0].cells[0].trigger.durationMs / 1000),
    beforeWarmupSeconds:$before[0][0].trigger.warmup,
    beforeDurationSeconds:($before[0][0].trigger.durationMs / 1000),label:$label,comparison:
    ([.[] | .cells[]] | map(. as $after | ($before[0] | map(select(.pattern == $after.pattern))[0]) as $previous
      | {pattern:.pattern,beforeQps:$previous.throughput_per_second,afterQps:.throughput_per_second,
         decreasePercent:(100*(1-.throughput_per_second/$previous.throughput_per_second)),
         beforeCpuUs:($previous.server_cpu_percent*$cores*10000/$previous.throughput_per_second),
         afterCpuUs:(.server_cpu_percent*$cores*10000/.throughput_per_second)}))}' \
  "$output/fixed-$label"/zlink-dotnet-*-4096/results.json > "$output/summary.json"
cat "$output/summary.json"
