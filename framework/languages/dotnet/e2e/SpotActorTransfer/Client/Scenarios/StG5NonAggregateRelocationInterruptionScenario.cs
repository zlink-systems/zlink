// Verifies PerActor and Instance Spot service units resume within one second.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StG5NonAggregateRelocationInterruptionScenario
{
    internal static Task RunPerActorAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(context, ServiceUnit.PerActorActor, AdapterProfile.Small);

    internal static Task RunPerActorSlowCaptureAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(
            context,
            ServiceUnit.PerActorActor,
            AdapterProfile.SlowCapture);

    internal static Task RunPerActorSlowRestoreAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(
            context,
            ServiceUnit.PerActorActor,
            AdapterProfile.SlowRestore);

    internal static Task RunPerActorSpotAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(context, ServiceUnit.PerActorSpot, AdapterProfile.Small);

    internal static Task RunPerActorSpotSlowCaptureAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(
            context,
            ServiceUnit.PerActorSpot,
            AdapterProfile.SlowCapture);

    internal static Task RunPerActorSpotSlowRestoreAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(
            context,
            ServiceUnit.PerActorSpot,
            AdapterProfile.SlowRestore);

    internal static Task RunInstanceSpotAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(context, ServiceUnit.InstanceSpot, AdapterProfile.Small);

    internal static Task RunInstanceSpotSlowCaptureAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(
            context,
            ServiceUnit.InstanceSpot,
            AdapterProfile.SlowCapture);

    internal static Task RunInstanceSpotSlowRestoreAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(
            context,
            ServiceUnit.InstanceSpot,
            AdapterProfile.SlowRestore);

    private static async Task RunAsync(
        SpotActorTransferScenarioContext context,
        ServiceUnit unit,
        AdapterProfile profile)
    {
        var runId = Guid.NewGuid().ToString("N");
        var prefix = unit switch
        {
            ServiceUnit.PerActorActor => "ST-G5-PER-ACTOR",
            ServiceUnit.PerActorSpot => "ST-G5-PER-ACTOR-SPOT",
            ServiceUnit.InstanceSpot => "ST-G5-INSTANCE-SPOT",
            _ => throw new ArgumentOutOfRangeException(nameof(unit))
        };
        var suffix = profile switch
        {
            AdapterProfile.Small => "SMALL",
            AdapterProfile.SlowCapture => "SLOW-CAPTURE",
            AdapterProfile.SlowRestore => "SLOW-RESTORE",
            _ => throw new ArgumentOutOfRangeException(nameof(profile))
        };
        var selector = $"{prefix}-{suffix}";
        var isInstance = unit == ServiceUnit.InstanceSpot;

        RelocationBulkSpotCreateRes created;
        await context.SetExclusivePlacementAsync(context.NodeA);
        try
        {
            created = await context.CreateBulkSpotsAsync(
                context.NodeA,
                new RelocationBulkSpotCreateReq(
                    selector,
                    $"{selector.ToLowerInvariant()}-{runId}",
                    Count: 1,
                    ApplicationStateBytes: isInstance ? 64 * 1024 : 0,
                    InstanceSpot: isInstance,
                    MaxConcurrency: 1,
                    ActorsPerSpot: isInstance ? 0 : 1,
                    ActorApplicationStateBytes:
                        isInstance ? 0 : 64 * 1024,
                    PerActor: !isInstance));
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }

        ZlinkStreamAssert.Ensure(
            created.SpotIds.Length == 1
            && (isInstance
                ? created.ActorIds.Length == 0
                : created.ActorIds.Length == 1)
            && created.NodeRids.All(rid =>
                SpotActorTransferScenarioContext.IsNode(
                    rid,
                    "actor-a")),
            $"{selector} did not create its service unit on actor-a.");

        var actorIds = unit == ServiceUnit.PerActorActor
            ? created.ActorIds
            : [];
        var spotIds = unit == ServiceUnit.PerActorActor
            ? []
            : created.SpotIds;
        var initial = isInstance
            ? created.SpotIds.Select((spotId, index) =>
                    new RelocationLocationSnapshot(
                        "spot",
                        spotId,
                        created.SpotObjectGenerations[index],
                        created.NodeRids[index]))
                .ToArray()
            : await context.GetRelocationLocationsAsync(
                    context.NodeC,
                    actorIds,
                    spotIds);
        var targetKind = actorIds.Length == 1 ? "actor" : "spot";
        var targetIds = actorIds.Length == 1 ? actorIds : spotIds;
        var traffic = new RelocationBulkWorkload(
            context,
            selector,
            targetKind,
            targetIds,
            operationsPerSecond: 20,
            submittingNode: context.NodeC,
            preservePerKindSubmissionOrder: true,
            resolveExpectedOwner: !isInstance);

        await context.SetExclusivePlacementAsync(context.NodeB);
        using var stopTraffic = new CancellationTokenSource();
        var trafficTask = traffic.RunAsync(
            TimeSpan.FromMinutes(3),
            stopTraffic.Token);
        RelocationBulkWorkloadResult result;
        RelocateHostRes relocation;
        try
        {
            await traffic.WaitForInitialEvidenceAsync(
                TimeSpan.FromSeconds(5),
                requireAllTargets: true);
            relocation = await context.RelocateAsync(
                context.NodeA,
                TimeSpan.FromMinutes(2));
            await traffic.WaitForAdditionalEvidenceAsync(
                TimeSpan.FromSeconds(5),
                requireAllTargets: true);
        }
        finally
        {
            stopTraffic.Cancel();
            result = await trafficTask;
            await context.RestoreDefaultPlacementAsync(context.NodeA);
        }

        ZlinkStreamAssert.Ensure(
            string.Equals(
                relocation.Outcome,
                "Relocated",
                StringComparison.OrdinalIgnoreCase),
            $"{selector} expected Relocated, got {relocation.Outcome} "
            + $"({relocation.Reason}).");
        await RelocationBulkWorkloadVerification.VerifyAsync(
            context,
            result);

        var finalOverride = isInstance
            ? targetIds.Select(targetId =>
                {
                    ZlinkStreamAssert.Ensure(
                        result.LatestRequestLocations.TryGetValue(
                            targetId,
                            out var observed),
                        $"{selector} did not observe a terminal Instance Spot reply.");
                    return new RelocationLocationSnapshot(
                        "spot",
                        targetId,
                        observed!.ObjectGeneration,
                        observed.NodeRid);
                })
                .ToArray()
            : null;
        var terminal = await RelocationBulkWorkloadVerification
            .VerifyRelocationTerminalsAsync(
                context,
                initial,
                actorIds,
                spotIds,
                [result],
                requireSpotWideAggregatePublication: false,
                finalOverride: finalOverride);
        var executionMode = isInstance ? null : "per_actor";
        var unitKind = isInstance
            ? "instance_spot"
            : unit == ServiceUnit.PerActorActor
                ? "actor"
                : "user_spot";
        var measurements = await context.WaitRelocationInterruptionAsync(
            context.NodeA,
            unitKind,
            minimumCount: 1,
            timeoutMilliseconds: 20_000,
            executionMode);
        var duration = measurements[^1].DurationSeconds;

        var expectsSlowAdapter = profile != AdapterProfile.Small
            && unit != ServiceUnit.PerActorSpot;
        ZlinkStreamAssert.Ensure(
            expectsSlowAdapter
                ? duration > 1
                : duration <= 1
                  && terminal.ServiceUnitSloMissed == 0
                  && terminal.MaxServiceInterruptionMilliseconds <= 1_000,
            expectsSlowAdapter
                ? $"{selector} did not expose the injected 1.25 second "
                  + $"adapter delay: runtime={duration:F6}s."
                : $"{selector} workload_slo_missed: runtime={duration:F6}s, "
                  + "handler_gap="
                  + $"{terminal.MaxServiceInterruptionMilliseconds:F3}ms.");
        ZlinkStreamAssert.Ensure(
            result.RequestFailed == 0
            && result.OneWayFailed == 0,
            $"{selector} lost an admitted operation.");

        Console.WriteLine(
            $"relocation_interruption_evidence selector={selector}"
            + $" unit_kind={unitKind}"
            + (executionMode is null
                ? string.Empty
                : $" execution_mode={executionMode}")
            + $" duration_seconds={duration:F6}"
            + " source_last_handler_to_target_first_handler_or_reply_gap_ms="
            + terminal.MaxServiceInterruptionMilliseconds.ToString("F3")
            + " loss=0 duplicate=0"
            + $" request_count={result.RequestSucceeded}"
            + $" one_way_count={result.OneWaySucceeded}");

        if (unit == ServiceUnit.PerActorSpot
            && profile != AdapterProfile.Small)
        {
            var unexpectedKind = profile == AdapterProfile.SlowCapture
                ? "unexpected_spot_slow_capture"
                : "unexpected_spot_slow_restore";
            var evidence = (await context.GetEvidenceAsync(context.NodeA))
                .Concat(await context.GetEvidenceAsync(context.NodeB))
                .Concat(await context.GetEvidenceAsync(context.NodeC));
            ZlinkStreamAssert.Ensure(
                !evidence.Any(item =>
                    item.Scenario == selector
                    && item.Kind == unexpectedKind),
                $"{selector} invoked a Spot state adapter even though "
                + "PerActor Spot shells use RecreateOnRelocation.");
        }
    }

    private enum ServiceUnit
    {
        PerActorActor,
        PerActorSpot,
        InstanceSpot
    }

    private enum AdapterProfile
    {
        Small,
        SlowCapture,
        SlowRestore
    }
}
