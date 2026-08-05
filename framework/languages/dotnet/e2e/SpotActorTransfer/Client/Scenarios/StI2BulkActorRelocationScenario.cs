// Verifies bulk Actor relocation throughput, interruption, and service continuity bounds.
using System.Diagnostics;
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StI2BulkActorRelocationScenario
{
    internal static Task RunRecreateOnRelocationAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(context, recreate: true);

    internal static Task RunPreserveStateWithAsync(
        SpotActorTransferScenarioContext context) =>
        RunAsync(context, recreate: false);

    private static async Task RunAsync(
        SpotActorTransferScenarioContext context,
        bool recreate)
    {
        const int canonicalRecreateCount = 10_000;
        const int canonicalPreserveStateCount = 1_000;
        var count = RelocationWorkloadEnvironment.Count(
            recreate
                ? "ZLINK_E2E_ST_I2_RECREATE_COUNT"
                : "ZLINK_E2E_ST_I2_SNAPSHOT_COUNT",
            recreate
                ? canonicalRecreateCount
                : canonicalPreserveStateCount);
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
        var movingTrafficEnabled =
            RelocationWorkloadEnvironment.Enabled(
                "ZLINK_E2E_RELOCATION_MOVING_TRAFFIC",
                true);
        var profile = recreate
            ? "recreate_on_relocation"
            : "preserve_state_with";
        var runId = Guid.NewGuid().ToString("N");

        var controlSpotId = $"st-i2-control-spot-{runId}";
        var controlActorId = $"st-i2-control-actor-{runId}";
        await context.SetExclusivePlacementAsync(context.NodeC);
        try
        {
            _ = await context.CreatePayloadUserSpotAsync(
                context.NodeC,
                controlSpotId,
                new RelocationPayloadSpotReq("ST-I2-control", 4 * 1024));
            _ = await context.CreateBulkActorsAsync(
                context.NodeC,
                new RelocationBulkActorCreateReq(
                    "ST-I2-control",
                    controlActorId,
                    SpotActorTransferNames.ActorTypeNoAdapter,
                    1,
                    0,
                    setupConcurrency));
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }
        controlActorId += "-000000";

        RelocationBulkActorCreateRes moving;
        await context.SetExclusivePlacementAsync(context.NodeA);
        try
        {
            moving = await context.CreateBulkActorsAsync(
                context.NodeA,
                new RelocationBulkActorCreateReq(
                    $"ST-I2-{profile}",
                    $"st-i2-{profile}-{runId}",
                    recreate
                        ? SpotActorTransferNames.ActorTypeNoAdapter
                        : SpotActorTransferNames.ActorTypeStateful,
                    count,
                    recreate ? 0 : 64 * 1024,
                    setupConcurrency));
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }

        EnsureOwnedBySource(moving, profile);
        var movingActorIds = moving.ActorIds;
        var initialLocations =
            await context.GetRelocationLocationsAsync(
                context.NodeB,
                movingActorIds,
                []);

        var baselineActor = new RelocationBulkWorkload(
            context,
            "ST-I2-control-actor-baseline",
            "actor",
            [controlActorId],
            rate);
        var baselineSpot = new RelocationBulkWorkload(
            context,
            "ST-I2-control-spot-baseline",
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
            "ST-I2-control-actor-relocation",
            "actor",
            [controlActorId],
            rate);
        var relocationSpot = new RelocationBulkWorkload(
            context,
            "ST-I2-control-spot-relocation",
            "spot",
            [controlSpotId],
            rate);
        var movingActors = new RelocationBulkWorkload(
            context,
            "ST-I2-moving-actor-relocation",
            "actor",
            movingActorIds,
            rate);
        var trafficTasks = new List<Task<RelocationBulkWorkloadResult>>
        {
            relocationActor.RunAsync(
                TimeSpan.FromMinutes(6),
                relocationTraffic.Token),
            relocationSpot.RunAsync(
                TimeSpan.FromMinutes(6),
                relocationTraffic.Token)
        };
        if (movingTrafficEnabled)
            trafficTasks.Add(
                movingActors.RunAsync(
                    TimeSpan.FromMinutes(6),
                    relocationTraffic.Token));

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
                        TimeSpan.FromSeconds(5))
                };
                if (movingTrafficEnabled)
                    initialEvidence.Add(
                        movingActors.WaitForInitialEvidenceAsync(
                            TimeSpan.FromSeconds(5)));
                await Task.WhenAll(initialEvidence);
            }
            catch (OperationCanceledException error)
            {
                throw new InvalidOperationException(
                    "ST-I2 relocation traffic did not produce bounded "
                    + "initial request and one-way handler evidence.",
                    error);
            }

            relocationWatch.Start();
            relocation = await context.RelocateAsync(
                context.NodeA,
                relocationDeadline);
            relocationWatch.Stop();
            var postTerminalEvidence = new List<Task>
            {
                relocationActor.WaitForAdditionalEvidenceAsync(
                    TimeSpan.FromSeconds(5)),
                relocationSpot.WaitForAdditionalEvidenceAsync(
                    TimeSpan.FromSeconds(5))
            };
            if (movingTrafficEnabled)
                postTerminalEvidence.Add(
                    movingActors.WaitForAdditionalEvidenceAsync(
                        TimeSpan.FromSeconds(5)));
            await Task.WhenAll(postTerminalEvidence);
        }
        finally
        {
            relocationWatch.Stop();
            relocationTraffic.Cancel();
            during = await Task.WhenAll(trafficTasks);
        }

        var completedRelocation = relocation
            ?? throw new InvalidOperationException(
                "ST-I2 relocation did not produce a terminal result.");
        var completedTraffic = during
            ?? throw new InvalidOperationException(
                "ST-I2 workload tasks did not produce terminal results.");

        ZlinkStreamAssert.Ensure(
            completedRelocation.Outcome == "Relocated"
            && completedRelocation.State == "Relocated",
            "ST-I2 host relocation did not reach Relocated: "
            + $"{completedRelocation.Outcome}/"
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
        var canonicalShape =
            count == (recreate
                ? canonicalRecreateCount
                : canonicalPreserveStateCount)
            && baselineDuration == TimeSpan.FromSeconds(60)
            && rate == 200;
        ZlinkStreamAssert.Ensure(
            !canonicalShape || movingTrafficEnabled,
            $"ST-I2 {profile} canonical run requires moving-target "
            + "request and one-way traffic.");
        var canonical = canonicalShape && movingTrafficEnabled;
        var terminalSummary = await RelocationBulkWorkloadVerification
            .VerifyRelocationTerminalsAsync(
                context,
                initialLocations,
                movingActorIds,
                [],
                completedTraffic,
                requireSpotWideAggregatePublication: false,
                spotFlowWatermark);
        var unitsPerSecond = count / elapsed;
        Console.WriteLine(
            $"ST-I2 profile={profile}"
            + $" unit_count={count}"
            + $" completed={terminalSummary.CompletedUnits}"
            + $" verified_participants="
            + terminalSummary.VerifiedParticipants
            + $" safe_aborted={terminalSummary.SafeAborted}"
            + $" blocked={terminalSummary.Blocked}"
            + $" elapsed_seconds={elapsed:F3}"
            + $" units_per_second={unitsPerSecond:F2}"
            + $" canonical_profile={canonical}"
            + " diagnostic_blockers="
            + "interruption,encoded_bytes_per_second,"
            + "payload_latency,cpu,rss,store_bytes");
        if (canonical)
        {
            var elapsedLimit = recreate ? 180 : 90;
            var rateLimit = recreate ? 64 : 16;
            ZlinkStreamAssert.Ensure(
                elapsed <= elapsedLimit
                && unitsPerSecond >= rateLimit,
                $"ST-I2 {profile} workload_slo_missed.");
        }
    }

    private static void EnsureOwnedBySource(
        RelocationBulkActorCreateRes result,
        string kind)
    {
        ZlinkStreamAssert.Ensure(
            result.ActorIds.Length == result.NodeRids.Length
            && result.NodeRids.All(rid =>
                SpotActorTransferScenarioContext.IsNode(
                    rid,
                    "actor-a")),
            $"ST-I2 {kind} bulk was not placed on source actor-a.");
    }
}
