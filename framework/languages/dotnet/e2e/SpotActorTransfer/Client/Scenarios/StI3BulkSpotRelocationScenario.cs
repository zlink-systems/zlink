// Verifies bulk Spot relocation throughput, interruption, and service continuity bounds.
using System.Diagnostics;
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StI3BulkSpotRelocationScenario
{
    internal static Task RunInstanceAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(context, instanceSpot: true);

    internal static Task RunSpotWideAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(context, instanceSpot: false);

    private static async Task RunAsync(
        SpotActorTransferScenarioContext context,
        bool instanceSpot)
    {
        var countVariable = instanceSpot
            ? "ZLINK_E2E_ST_I3_INSTANCE_COUNT"
            : "ZLINK_E2E_ST_I3_SPOTWIDE_COUNT";
        var canonicalCount = instanceSpot ? 1_000 : 100;
        var spotCount = RelocationWorkloadEnvironment.Count(
            countVariable,
            canonicalCount);
        var actorsPerSpot = instanceSpot
            ? 0
            : RelocationWorkloadEnvironment.Count(
                "ZLINK_E2E_ST_I3_ACTORS_PER_SPOT",
                100);
        var baselineDuration =
            RelocationWorkloadEnvironment.Duration(
                "ZLINK_E2E_RELOCATION_BASELINE_SECONDS",
                60);
        var rate = RelocationWorkloadEnvironment.Rate(
            "ZLINK_E2E_RELOCATION_RATE",
            200);
        var setupConcurrency =
            RelocationWorkloadEnvironment.Count(
                "ZLINK_E2E_RELOCATION_SETUP_CONCURRENCY",
                64);
        var relocationDeadline =
            RelocationWorkloadEnvironment.Duration(
                "ZLINK_E2E_RELOCATION_DEADLINE_SECONDS",
                300);
        var label = instanceSpot ? "instance" : "spotwide";
        var runId = Guid.NewGuid().ToString("N");

        var controlSpotId =
            $"st-i3-{label}-control-spot-{runId}";
        var controlActorPrefix =
            $"st-i3-{label}-control-actor-{runId}";
        await context.SetExclusivePlacementAsync(context.NodeC);
        try
        {
            _ = await context.CreatePayloadUserSpotAsync(
                context.NodeC,
                controlSpotId,
                new RelocationPayloadSpotReq(
                    $"ST-I3-{label}-control",
                    4 * 1024));
            _ = await context.CreateBulkActorsAsync(
                context.NodeC,
                new RelocationBulkActorCreateReq(
                    $"ST-I3-{label}-control",
                    controlActorPrefix,
                    SpotActorTransferNames.ActorTypeNoAdapter,
                    1,
                    0,
                    setupConcurrency));
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }
        var controlActorId =
            controlActorPrefix + "-000000";

        RelocationBulkSpotCreateRes moving;
        await context.SetExclusivePlacementAsync(context.NodeA);
        try
        {
            moving = await context.CreateBulkSpotsAsync(
                context.NodeA,
                new RelocationBulkSpotCreateReq(
                    $"ST-I3-{label}",
                    $"st-i3-{label}-{runId}",
                    spotCount,
                    instanceSpot ? 64 * 1024 : 1024 * 1024,
                    instanceSpot,
                    MaxConcurrency: setupConcurrency,
                    ActorsPerSpot: actorsPerSpot,
                    ActorApplicationStateBytes:
                        instanceSpot ? 0 : 64 * 1024));
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }
        ZlinkStreamAssert.Ensure(
            moving.SpotIds.Length == spotCount
            && moving.NodeRids.All(rid =>
                SpotActorTransferScenarioContext.IsNode(
                    rid,
                    "actor-a"))
            && (instanceSpot
                ? moving.ActorIds.Length == 0
                : moving.ActorIds.Length
                  == spotCount * actorsPerSpot),
            $"ST-I3 {label} bulk was not created on source actor-a.");
        var initialLocations =
            await context.GetRelocationLocationsAsync(
                context.NodeB,
                moving.ActorIds,
                moving.SpotIds);

        var baselineActor = new RelocationBulkWorkload(
            context,
            $"ST-I3-{label}-control-actor-baseline",
            "actor",
            [controlActorId],
            rate);
        var baselineSpot = new RelocationBulkWorkload(
            context,
            $"ST-I3-{label}-control-spot-baseline",
            "spot",
            [controlSpotId],
            rate);
        var baseline = await Task.WhenAll(
            baselineActor.RunAsync(baselineDuration),
            baselineSpot.RunAsync(baselineDuration));
        await Task.WhenAll(
            baselineActor.WaitForInitialEvidenceAsync(
                TimeSpan.FromSeconds(5)),
            baselineSpot.WaitForInitialEvidenceAsync(
                TimeSpan.FromSeconds(5)));
        foreach (var result in baseline)
        {
            await RelocationBulkWorkloadVerification.VerifyAsync(
                context,
                result);
            RelocationBulkWorkloadVerification.Report(result);
        }

        using var relocationTraffic = new CancellationTokenSource();
        var spotFlowWatermark = await RelocationBulkWorkloadVerification
            .CaptureSpotFlowWatermarkAsync(context);
        var relocationActor = new RelocationBulkWorkload(
            context,
            $"ST-I3-{label}-control-actor-relocation",
            "actor",
            [controlActorId],
            rate);
        var relocationSpot = new RelocationBulkWorkload(
            context,
            $"ST-I3-{label}-control-spot-relocation",
            "spot",
            [controlSpotId],
            rate);
        var movingSpot = new RelocationBulkWorkload(
            context,
            $"ST-I3-{label}-moving-spot-relocation",
            "spot",
            moving.SpotIds,
            rate);
        RelocationBulkWorkload? movingActor = null;
        var traffic = new List<Task<RelocationBulkWorkloadResult>>
        {
            relocationActor.RunAsync(
                TimeSpan.FromMinutes(6),
                relocationTraffic.Token),
            relocationSpot.RunAsync(
                TimeSpan.FromMinutes(6),
                relocationTraffic.Token),
            movingSpot.RunAsync(
                TimeSpan.FromMinutes(6),
                relocationTraffic.Token)
        };
        if (moving.ActorIds.Length > 0)
        {
            movingActor = new RelocationBulkWorkload(
                context,
                $"ST-I3-{label}-moving-actor-relocation",
                "actor",
                moving.ActorIds,
                rate);
            traffic.Add(
                movingActor.RunAsync(
                    TimeSpan.FromMinutes(6),
                    relocationTraffic.Token));
        }

        var relocationWatch = new Stopwatch();
        RelocateHostRes? relocation = null;
        RelocationBulkWorkloadResult[]? during = null;
        try
        {
            try
            {
                var initialEvidence = new List<Task>
                {
                    relocationActor.WaitForInitialEvidenceAsync(
                        TimeSpan.FromSeconds(5)),
                    relocationSpot.WaitForInitialEvidenceAsync(
                        TimeSpan.FromSeconds(5)),
                    movingSpot.WaitForInitialEvidenceAsync(
                        TimeSpan.FromSeconds(5))
                };
                if (movingActor is not null)
                    initialEvidence.Add(
                        movingActor.WaitForInitialEvidenceAsync(
                            TimeSpan.FromSeconds(5)));
                await Task.WhenAll(initialEvidence);
            }
            catch (OperationCanceledException error)
            {
                throw new InvalidOperationException(
                    $"ST-I3 {label} relocation traffic did not produce "
                    + "bounded initial request and one-way handler "
                    + "evidence.",
                    error);
            }

            relocationWatch.Start();
            relocation = await context.RelocateAsync(
                context.NodeA,
                relocationDeadline);
            relocationWatch.Stop();
            var postRelocationEvidence = new List<Task>
            {
                relocationActor.WaitForAdditionalEvidenceAsync(
                    TimeSpan.FromSeconds(5)),
                relocationSpot.WaitForAdditionalEvidenceAsync(
                    TimeSpan.FromSeconds(5)),
                movingSpot.WaitForAdditionalEvidenceAsync(
                    TimeSpan.FromSeconds(5))
            };
            if (movingActor is not null)
                postRelocationEvidence.Add(
                    movingActor.WaitForAdditionalEvidenceAsync(
                        TimeSpan.FromSeconds(5)));
            await Task.WhenAll(postRelocationEvidence);
        }
        finally
        {
            relocationWatch.Stop();
            relocationTraffic.Cancel();
            during = await Task.WhenAll(traffic);
        }

        var completedRelocation = relocation
            ?? throw new InvalidOperationException(
                $"ST-I3 {label} relocation did not produce a terminal "
                + "result.");
        var completedTraffic = during
            ?? throw new InvalidOperationException(
                $"ST-I3 {label} workload tasks did not produce terminal "
                + "results.");

        ZlinkStreamAssert.Ensure(
            completedRelocation.Outcome == "Relocated"
            && completedRelocation.State == "Relocated",
            $"ST-I3 {label} host relocation did not reach "
            + $"Relocated: {completedRelocation.Outcome}/"
            + $"{completedRelocation.Reason}/"
            + completedRelocation.State);
        foreach (var result in completedTraffic)
        {
            await RelocationBulkWorkloadVerification.VerifyAsync(
                context,
                result);
            RelocationBulkWorkloadVerification.Report(result);
        }
        RelocationBulkWorkloadVerification.VerifyContinuity(
            baseline[0],
            completedTraffic[0]);
        RelocationBulkWorkloadVerification.VerifyContinuity(
            baseline[1],
            completedTraffic[1]);

        var elapsed = relocationWatch.Elapsed.TotalSeconds;
        var unitsPerSecond = spotCount / elapsed;
        var canonical =
            spotCount == canonicalCount
            && (instanceSpot || actorsPerSpot == 100)
            && baselineDuration == TimeSpan.FromSeconds(60)
            && rate == 200;
        var terminalSummary = await RelocationBulkWorkloadVerification
            .VerifyRelocationTerminalsAsync(
                context,
                initialLocations,
                moving.ActorIds,
                moving.SpotIds,
                completedTraffic,
                requireSpotWideAggregatePublication:
                    !instanceSpot,
                spotFlowWatermark);
        Console.WriteLine(
            $"ST-I3 profile={label}"
            + $" unit_count={spotCount}"
            + $" participant_count="
            + (spotCount + moving.ActorIds.Length)
            + $" completed={terminalSummary.CompletedUnits}"
            + $" verified_participants="
            + terminalSummary.VerifiedParticipants
            + $" max_service_interruption_ms="
            + terminalSummary.MaxServiceInterruptionMilliseconds
                .ToString("F3")
            + $" service_unit_slo_missed="
            + terminalSummary.ServiceUnitSloMissed
            + $" safe_aborted={terminalSummary.SafeAborted}"
            + $" blocked={terminalSummary.Blocked}"
            + $" elapsed_seconds={elapsed:F3}"
            + $" units_per_second={unitsPerSecond:F2}"
            + $" canonical_profile={canonical}"
            + " diagnostic_blockers="
            + (instanceSpot
                ? "interruption,encoded_bytes_per_second,"
                  + "payload_latency,cpu,rss,store_bytes"
                : "spotwide_pre_post_visibility,interruption,"
                  + "encoded_bytes_per_second,payload_latency,"
                  + "cpu,rss,store_bytes"));
        if (canonical)
        {
            var elapsedLimit = instanceSpot ? 150 : 180;
            var rateLimit = instanceSpot ? 8 : 1;
            ZlinkStreamAssert.Ensure(
                elapsed <= elapsedLimit
                && unitsPerSecond >= rateLimit
                && terminalSummary.ServiceUnitSloMissed == 0
                && terminalSummary.MaxServiceInterruptionMilliseconds
                   <= 1_000,
                $"ST-I3 {label} workload_slo_missed.");
        }
    }
}
