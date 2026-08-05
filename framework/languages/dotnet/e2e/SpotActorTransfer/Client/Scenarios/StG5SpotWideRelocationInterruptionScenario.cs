// Verifies SpotWide relocation interruption, payload, ordering, and service continuity bounds.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StG5SpotWideRelocationInterruptionScenario
{
    private const int ActorStateBytes = 64 * 1024;

    internal static Task RunActors10Async(
        SpotActorTransferScenarioContext context) =>
        RunAsync(context, 10, canonicalGate: false);

    internal static Task RunActors100Async(
        SpotActorTransferScenarioContext context) =>
        RunAsync(context, 100, canonicalGate: true);

    private static async Task RunAsync(
        SpotActorTransferScenarioContext context,
        int actorCount,
        bool canonicalGate)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var profile = $"ACTORS-{actorCount}";
        var selector = $"ST-G5-SPOT-WIDE-{profile}";
        var spotIdPrefix = $"st-g5-spot-wide-{actorCount}-{suffix}";
        var setupConcurrency = RelocationWorkloadEnvironment.Count(
            "ZLINK_E2E_RELOCATION_SETUP_CONCURRENCY",
            64);
        var trafficRate = RelocationWorkloadEnvironment.Rate(
            "ZLINK_E2E_ST_G5_SPOT_WIDE_RATE",
            20);
        var relocationDeadline = RelocationWorkloadEnvironment.Duration(
            "ZLINK_E2E_RELOCATION_DEADLINE_SECONDS",
            300);

        RelocationBulkSpotCreateRes created;
        await context.SetExclusivePlacementAsync(context.NodeA);
        try
        {
            created = await context.CreateBulkSpotsAsync(
                context.NodeA,
                new RelocationBulkSpotCreateReq(
                    selector,
                    spotIdPrefix,
                    Count: 1,
                    ApplicationStateBytes: ActorStateBytes,
                    InstanceSpot: false,
                    MaxConcurrency: setupConcurrency,
                    ActorsPerSpot: actorCount,
                    PerActor: false,
                    ActorApplicationStateBytes: ActorStateBytes));
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }

        ZlinkStreamAssert.Ensure(
            created.SpotIds.Length == 1
            && created.ActorIds.Length == actorCount
            && created.NodeRids.Length == 1
            && SpotActorTransferScenarioContext.IsNode(
                created.NodeRids[0],
                "actor-a"),
            $"{selector} did not create one SpotWide User Spot with "
            + $"{actorCount} member Actors on actor-a.");

        var initialLocations = await context.GetRelocationLocationsAsync(
            context.NodeB,
            created.ActorIds,
            created.SpotIds);
        ZlinkStreamAssert.Ensure(
            initialLocations.Count == actorCount + 1
            && initialLocations.All(location =>
                SpotActorTransferScenarioContext.IsNode(
                    location.NodeRid,
                    "actor-a")),
            $"{selector} initial participant inventory was incomplete "
            + "or was not owned by actor-a.");

        await context.SetExclusivePlacementAsync(context.NodeB);
        var preSpotPrime = new RelocationBulkWorkload(
            context,
            selector + "-TO-SPOT-PRE",
            "spot",
            created.SpotIds,
            trafficRate,
            context.NodeC);
        var preActorPrime = new RelocationBulkWorkload(
            context,
            selector + "-TO-ACTOR-PRE",
            "actor",
            created.ActorIds,
            trafficRate,
            context.NodeC);
        var preTraffic = await Task.WhenAll(
            preSpotPrime.PrimeAllTargetsAsync(setupConcurrency),
            preActorPrime.PrimeAllTargetsAsync(setupConcurrency));
        foreach (var result in preTraffic)
            await RelocationBulkWorkloadVerification.VerifyAsync(
                context,
                result);
        await Task.WhenAll(
            context.ResetRelocationBlobMeasurementsAsync(context.NodeA),
            context.ResetRelocationBlobMeasurementsAsync(context.NodeB),
            context.ResetRelocationBlobMeasurementsAsync(context.NodeC));

        using var trafficCancellation = new CancellationTokenSource();
        var flowWatermark =
            await RelocationBulkWorkloadVerification
                .CaptureSpotFlowWatermarkAsync(context);
        var spotScenario = selector + "-TO-SPOT";
        var actorScenario = selector + "-TO-ACTOR";
        var spotTraffic = new RelocationBulkWorkload(
            context,
            spotScenario,
            "spot",
            created.SpotIds,
            trafficRate,
            context.NodeC,
            preservePerKindSubmissionOrder: true);
        var actorTraffic = new RelocationBulkWorkload(
            context,
            actorScenario,
            "actor",
            created.ActorIds,
            trafficRate,
            context.NodeC,
            preservePerKindSubmissionOrder: true);
        var trafficTasks = new[]
        {
            spotTraffic.RunAsync(
                TimeSpan.FromMinutes(6),
                trafficCancellation.Token),
            actorTraffic.RunAsync(
                TimeSpan.FromMinutes(6),
                trafficCancellation.Token)
        };

        RelocateHostRes? relocation = null;
        RelocationBulkWorkloadResult[]? traffic = null;
        try
        {
            await Task.WhenAll(
                spotTraffic.WaitForInitialEvidenceAsync(
                    TimeSpan.FromSeconds(10)),
                actorTraffic.WaitForInitialEvidenceAsync(
                    TimeSpan.FromSeconds(10)));
            await Task.Delay(TimeSpan.FromSeconds(6));
            EnsureBaselineHasNoTimeouts(
                selector,
                spotTraffic.Snapshot(),
                actorTraffic.Snapshot());
            relocation = await context.RelocateAsync(
                context.NodeA,
                relocationDeadline);
            await Task.WhenAll(
                spotTraffic.WaitForAdditionalEvidenceAsync(
                    TimeSpan.FromSeconds(10)),
                actorTraffic.WaitForAdditionalEvidenceAsync(
                    TimeSpan.FromSeconds(10)));
        }
        finally
        {
            trafficCancellation.Cancel();
            traffic = await Task.WhenAll(trafficTasks);
        }

        var completedRelocation = relocation
            ?? throw new InvalidOperationException(
                $"{selector} did not produce a relocation terminal.");
        var completedTraffic = traffic
            ?? throw new InvalidOperationException(
                $"{selector} traffic did not produce terminal evidence.");
        ZlinkStreamAssert.Ensure(
            completedRelocation.Outcome == "Relocated"
            && completedRelocation.State == "Relocated",
            $"{selector} relocation did not complete: "
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
        var postSpotPrime = new RelocationBulkWorkload(
            context,
            selector + "-TO-SPOT-POST",
            "spot",
            created.SpotIds,
            trafficRate,
            context.NodeC);
        var postActorPrime = new RelocationBulkWorkload(
            context,
            selector + "-TO-ACTOR-POST",
            "actor",
            created.ActorIds,
            trafficRate,
            context.NodeC);
        var postTraffic = await Task.WhenAll(
            postSpotPrime.PrimeAllTargetsAsync(setupConcurrency),
            postActorPrime.PrimeAllTargetsAsync(setupConcurrency));
        foreach (var result in postTraffic)
            await RelocationBulkWorkloadVerification.VerifyAsync(
                context,
                result);

        var terminal = await RelocationBulkWorkloadVerification
            .VerifyRelocationTerminalsAsync(
                context,
                initialLocations,
                created.ActorIds,
                created.SpotIds,
                completedTraffic.Concat(postTraffic).ToArray(),
                requireSpotWideAggregatePublication: true,
                flowWatermark);
        ZlinkStreamAssert.Ensure(
            terminal.CompletedUnits == 1
            && terminal.VerifiedParticipants == actorCount + 1,
            $"{selector} did not verify the complete SpotWide service unit.");
        await VerifyApplicationStateSizesAsync(
            context,
            selector,
            created.SpotIds[0],
            created.ActorIds);
        var phaseTiming = await ReadPhaseTimingAsync(
            context,
            selector,
            created.SpotIds[0],
            created.ActorIds);

        var measurements = await context.WaitRelocationInterruptionAsync(
            context.NodeA,
            "user_spot",
            1,
            30_000,
            "spot_wide");
        var interruptionSeconds = measurements[^1].DurationSeconds;
        if (canonicalGate)
            ZlinkStreamAssert.Ensure(
                interruptionSeconds <= 1,
                $"{selector} canonical interruption was "
                + $"{interruptionSeconds:F6}s.");

        var gapMilliseconds = await ReadApplicationObservedGapAsync(
            context,
            new HashSet<string>(
                [spotScenario, actorScenario],
                StringComparer.Ordinal),
            created.SpotIds.Concat(created.ActorIds).ToHashSet(
                StringComparer.Ordinal));
        ZlinkStreamAssert.Ensure(
            gapMilliseconds <= 1_000 || !canonicalGate,
            $"{selector} canonical source-last to target-first gap was "
            + $"{gapMilliseconds} ms.");

        Console.WriteLine(
            $"required_gate=spotwide_service_continuity status=passed"
            + $" selector={selector}"
            + $" requests={completedTraffic.Sum(static item => item.RequestSucceeded)}"
            + $" one_way={completedTraffic.Sum(static item => item.OneWaySucceeded)}"
            + $" application_gap_ms={gapMilliseconds}"
            + " loss=0 duplicate=0 actor_fifo=preserved");
        Console.WriteLine(
            "required_gap=spot_message_follow_stale_physical_route"
            + " status=not_proven"
            + " reason=no_public_stale_physical_route_release");
        Console.WriteLine(
            $"relocation_interruption_evidence selector={selector}"
            + " unit_kind=user_spot"
            + " execution_mode=spot_wide"
            + $" actor_count={actorCount}"
            + $" actor_state_bytes={ActorStateBytes}"
            + $" spot_state_bytes={ActorStateBytes}"
            + $" duration_seconds={interruptionSeconds:F6}"
            + $" capture_callback_ms={phaseTiming.CaptureCallbacksMilliseconds}"
            + $" store_io_ms={phaseTiming.StoreIoMilliseconds}"
            + $" restore_callback_ms={phaseTiming.RestoreCallbacksMilliseconds}"
            + $" max_concurrent_store_io={phaseTiming.MaxConcurrentStoreIo}"
            + $" store_operations={phaseTiming.StoreOperations}"
            + $" encoded_store_bytes={phaseTiming.EncodedStoreBytes}"
            + $" largest_encoded_blob_bytes={phaseTiming.LargestEncodedBlobBytes}"
            + $" application_state_bytes="
            + $"{(long)(actorCount + 1) * ActorStateBytes}"
            + $" opaque_store_overhead_bytes="
            + $"{Math.Max(
                0,
                phaseTiming.EncodedStoreBytes
                - (long)(actorCount + 1) * ActorStateBytes)}"
            + " source_last_handler_to_target_first_handler_or_reply_gap_ms="
            + gapMilliseconds
            + " loss=0 duplicate=0 actor_fifo=preserved"
            + " message_follow=not_proven"
            + " message_follow_blocker="
            + "no_public_stale_physical_route_release"
            + $" canonical_gate={canonicalGate}"
            + $" diagnostic_blocked={terminal.Blocked != 0}");
    }

    private static void EnsureBaselineHasNoTimeouts(
        string selector,
        params RelocationBulkWorkloadResult[] results)
    {
        foreach (var result in results)
            ZlinkStreamAssert.Ensure(
                result.RequestFailed == 0
                && result.OneWayFailed == 0,
                $"{selector} pre-relocation baseline overloaded "
                + $"{result.TargetKind}: request_failed="
                + $"{result.RequestFailed}, one_way_failed="
                + result.OneWayFailed);
    }

    private static async Task<RelocationPhaseTiming> ReadPhaseTimingAsync(
        SpotActorTransferScenarioContext context,
        string selector,
        string spotId,
        IReadOnlyCollection<string> actorIds)
    {
        var participantIds = actorIds
            .Append(spotId)
            .ToHashSet(StringComparer.Ordinal);
        var evidence = (await Task.WhenAll(
                context.GetEvidenceAsync(context.NodeA),
                context.GetEvidenceAsync(context.NodeB),
                context.GetEvidenceAsync(context.NodeC)))
            .SelectMany(static items => items)
            .Where(item => participantIds.Contains(item.ActorId))
            .ToArray();
        var captureStarts = evidence
            .Where(item =>
                item.Kind is "application_capture_started"
                    or "spot_application_capture_started"
                && (item.Scenario == selector
                    || item.Kind == "application_capture_started"))
            .Select(static item => item.ObservedUnixTimeMilliseconds)
            .ToArray();
        var captureCompletions = evidence
            .Where(item =>
                item.Kind is "application_payload"
                    or "spot_application_payload"
                && (item.Scenario == selector
                    || item.Kind == "application_payload"))
            .Select(static item => item.ObservedUnixTimeMilliseconds)
            .ToArray();
        var restoreStarts = evidence
            .Where(item =>
                item.Kind is "application_restore_started"
                    or "spot_application_restore_started")
            .Select(static item => item.ObservedUnixTimeMilliseconds)
            .ToArray();
        var restoreCompletions = evidence
            .Where(item =>
                item.Kind is "application_state_restored"
                    or "spot_application_state_restored")
            .Select(static item => item.ObservedUnixTimeMilliseconds)
            .ToArray();
        ZlinkStreamAssert.Ensure(
            captureStarts.Length == participantIds.Count
            && captureCompletions.Length == participantIds.Count
            && restoreStarts.Length == participantIds.Count
            && restoreCompletions.Length == participantIds.Count,
            $"{selector} callback timing evidence was incomplete.");

        var store = (await Task.WhenAll(
                context.GetRelocationBlobMeasurementsAsync(context.NodeA),
                context.GetRelocationBlobMeasurementsAsync(context.NodeB),
                context.GetRelocationBlobMeasurementsAsync(context.NodeC)))
            .SelectMany(static items => items)
            .ToArray();
        var puts = store
            .Where(static item => item.Operation == "put")
            .ToArray();
        var reads = store
            .Where(static item => item.Operation == "read")
            .ToArray();
        ZlinkStreamAssert.Ensure(
            store.Length > participantIds.Count
            && store.Any(static item =>
                item.ActiveOperationCount >= 2),
            $"{selector} did not observe participant-parallel Store I/O.");
        ZlinkStreamAssert.Ensure(
            puts.Length > 0
            && puts.All(put => reads.Any(read =>
                read.OpaqueReferenceSha256 == put.OpaqueReferenceSha256
                && read.EncodedBytes == put.EncodedBytes
                && read.PayloadSha256 == put.PayloadSha256)),
            $"{selector} did not read every opaque Store blob back with "
            + "the same encoded size and checksum.");

        return new RelocationPhaseTiming(
            captureCompletions.Max() - captureStarts.Min(),
            store.Max(static item => item.CompletedUnixTimeMilliseconds)
            - store.Min(static item => item.StartedUnixTimeMilliseconds),
            restoreCompletions.Max() - restoreStarts.Min(),
            store.Max(static item => item.ActiveOperationCount),
            store.Length,
            puts.Sum(static item => (long)item.EncodedBytes),
            puts.Max(static item => item.EncodedBytes));
    }

    private static async Task VerifyApplicationStateSizesAsync(
        SpotActorTransferScenarioContext context,
        string selector,
        string spotId,
        IReadOnlyCollection<string> actorIds)
    {
        var evidence = (await Task.WhenAll(
                context.GetEvidenceAsync(context.NodeA),
                context.GetEvidenceAsync(context.NodeB),
                context.GetEvidenceAsync(context.NodeC)))
            .SelectMany(static items => items)
            .ToArray();
        var expectedBytes = $"bytes={ActorStateBytes}";
        ZlinkStreamAssert.Ensure(
            evidence.Any(item =>
                item.Scenario == selector
                && item.ActorId == spotId
                && item.Kind == "spot_application_payload"
                && item.Value.Contains(
                    expectedBytes,
                    StringComparison.Ordinal))
            && evidence.Any(item =>
                item.ActorId == spotId
                && item.Kind == "spot_application_state_restored"
                && item.Value.Contains(
                    expectedBytes,
                    StringComparison.Ordinal)),
            $"{selector} did not capture and restore the Spot's "
            + $"{ActorStateBytes}-byte application state.");
        var restoredActors = evidence
            .Where(item =>
                actorIds.Contains(item.ActorId, StringComparer.Ordinal)
                && item.Kind == "application_state_restored"
                && item.Value.Contains(
                    expectedBytes,
                    StringComparison.Ordinal))
            .Select(static item => item.ActorId)
            .ToHashSet(StringComparer.Ordinal);
        ZlinkStreamAssert.Ensure(
            restoredActors.Count == actorIds.Count
            && actorIds.All(restoredActors.Contains),
            $"{selector} did not restore {ActorStateBytes}-byte application "
            + "state for every member Actor.");
    }

    private static async Task<long> ReadApplicationObservedGapAsync(
        SpotActorTransferScenarioContext context,
        IReadOnlySet<string> scenarios,
        IReadOnlySet<string> targetIds)
    {
        var evidence = (await Task.WhenAll(
                context.GetEvidenceAsync(context.NodeA),
                context.GetEvidenceAsync(context.NodeB),
                context.GetEvidenceAsync(context.NodeC)))
            .SelectMany(static items => items)
            .Where(item =>
                scenarios.Contains(item.Scenario)
                && targetIds.Contains(item.ActorId)
                && item.Kind is "workload_request" or "workload_one_way")
            .ToArray();
        var sourceLast = evidence
            .Where(item => SpotActorTransferScenarioContext.IsNode(
                RelocationBulkWorkload.ParseHandlerOwner(item.Value),
                "actor-a"))
            .Select(static item => item.ObservedUnixTimeMilliseconds)
            .DefaultIfEmpty(-1)
            .Max();
        var targetFirst = evidence
            .Where(item => !SpotActorTransferScenarioContext.IsNode(
                RelocationBulkWorkload.ParseHandlerOwner(item.Value),
                "actor-a"))
            .Select(static item => item.ObservedUnixTimeMilliseconds)
            .DefaultIfEmpty(-1)
            .Min();
        ZlinkStreamAssert.Ensure(
            sourceLast >= 0 && targetFirst >= 0,
            "SpotWide traffic did not observe both source and target "
            + "handler evidence.");
        ZlinkStreamAssert.Ensure(
            targetFirst >= sourceLast,
            "SpotWide target handler evidence preceded the last source "
            + "handler completion.");
        return targetFirst - sourceLast;
    }

    private sealed record RelocationPhaseTiming(
        long CaptureCallbacksMilliseconds,
        long StoreIoMilliseconds,
        long RestoreCallbacksMilliseconds,
        int MaxConcurrentStoreIo,
        int StoreOperations,
        long EncodedStoreBytes,
        int LargestEncodedBlobBytes);
}
