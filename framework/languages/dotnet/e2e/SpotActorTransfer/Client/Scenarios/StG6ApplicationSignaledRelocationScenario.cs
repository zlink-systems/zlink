// Verifies ApplicationSignaled safe-point completion and same-turn fencing.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StG6ApplicationSignaledRelocationScenario
{
    public static async Task RunAsync(
        SpotActorTransferScenarioContext context)
    {
        await VerifyContinuedWithoutRelocationAsync(context);
        await VerifyContinuedAfterPrecommitAbortAsync(context);
        await VerifyRelocatedOnTargetAsync(context);
        await VerifyDefaultNoOpAsync(context);
        await VerifySameTurnRejectionsAsync(context);
    }

    private static async Task VerifyContinuedWithoutRelocationAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-G6-NO-RELOCATION";
        var created = await CreateAsync(
            context,
            scenario,
            SpotActorTransferNames.RelocationReadyUserSpotType,
            context.NodeA);
        var signal = await context.SignalRelocationReadyAsync(
            context.NodeC,
            new RelocationWorkloadReadyCallReq(
                created.SpotIds[0],
                scenario));
        ZlinkStreamAssert.Ensure(
            signal.Deferred,
            $"{scenario} did not register the safe-point boundary.");
        var source = context.NodeForRid(created.NodeRids[0]);
        _ = await context.WaitEvidenceAsync(
            source,
            [$"{scenario}|{created.SpotIds[0]}|relocation_ready_completed|Continued:"]);
        var reply = await SubmitProbeAsync(
            context,
            created.SpotIds[0],
            scenario);
        var evidence = await context.GetEvidenceAsync(source);
        EnsureCallbackBeforeProbe(evidence, scenario, created.SpotIds[0]);
        ZlinkStreamAssert.Ensure(
            reply.NodeRid == signal.NodeRid,
            $"{scenario} changed owner without relocation.");
        await context.ClosePayloadSpotAsync(
            context.NodeC,
            created.SpotIds[0]);
    }

    private static async Task VerifyContinuedAfterPrecommitAbortAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-G6-PRECOMMIT-ABORT";
        var created = await CreateAsync(
            context,
            scenario,
            SpotActorTransferNames.RelocationReadyUserSpotType,
            context.NodeA);
        var source = context.NodeForRid(created.NodeRids[0]);
        await context.SetExclusivePlacementAsync(context.NodeB);
        try
        {
            var relocation = context.RelocateAsync(
                source,
                TimeSpan.FromSeconds(30));
            await Task.Delay(200);
            _ = await context.SignalRelocationReadyAsync(
                context.NodeC,
                new RelocationWorkloadReadyCallReq(
                    created.SpotIds[0],
                    scenario));
            var terminal = await relocation;
            ZlinkStreamAssert.Ensure(
                !string.Equals(
                    terminal.Outcome,
                    "Relocated",
                    StringComparison.OrdinalIgnoreCase),
                $"{scenario} ignored the injected precommit failure.");
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }

        _ = await context.WaitEvidenceAsync(
            source,
            [$"{scenario}|{created.SpotIds[0]}|relocation_ready_completed|Continued:"]);
        _ = await SubmitProbeAsync(context, created.SpotIds[0], scenario);
        var evidence = await context.GetEvidenceAsync(source);
        EnsureCallbackBeforeProbe(evidence, scenario, created.SpotIds[0]);
        await context.ClosePayloadSpotAsync(
            context.NodeC,
            created.SpotIds[0]);
    }

    private static async Task VerifyRelocatedOnTargetAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-G6-RELOCATED";
        var created = await CreateAsync(
            context,
            scenario,
            SpotActorTransferNames.RelocationReadyUserSpotType,
            context.NodeA);
        var source = context.NodeForRid(created.NodeRids[0]);
        await context.SetExclusivePlacementAsync(context.NodeB);
        RelocateHostRes terminal;
        try
        {
            var relocation = context.RelocateAsync(
                source,
                TimeSpan.FromSeconds(30));
            await Task.Delay(200);
            _ = await context.SignalRelocationReadyAsync(
                context.NodeC,
                new RelocationWorkloadReadyCallReq(
                    created.SpotIds[0],
                    scenario));
            terminal = await relocation;
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync(source);
        }
        ZlinkStreamAssert.Ensure(
            string.Equals(
                terminal.Outcome,
                "Relocated",
                StringComparison.OrdinalIgnoreCase),
            $"{scenario} relocation failed: {terminal.Reason}.");
        var target = context.NodeB;
        _ = await context.WaitEvidenceAsync(
            target,
            [$"{scenario}|{created.SpotIds[0]}|relocation_ready_completed|Relocated:"]);
        var reply = await SubmitProbeAsync(
            context,
            created.SpotIds[0],
            scenario);
        var evidence = await context.GetEvidenceAsync(target);
        EnsureCallbackBeforeProbe(evidence, scenario, created.SpotIds[0]);
        ZlinkStreamAssert.Ensure(
            SpotActorTransferScenarioContext.IsNode(
                reply.NodeRid,
                "actor-b"),
            $"{scenario} callback/probe did not run on the target owner.");
        await context.ClosePayloadSpotAsync(
            context.NodeC,
            created.SpotIds[0]);
    }

    private static async Task VerifyDefaultNoOpAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-G6-DEFAULT-NOOP";
        var created = await CreateAsync(
            context,
            scenario,
            SpotActorTransferNames.RelocationReadyDefaultUserSpotType,
            context.NodeA);
        var signal = await context.SignalRelocationReadyAsync(
            context.NodeC,
            new RelocationWorkloadReadyCallReq(
                created.SpotIds[0],
                scenario));
        var reply = await SubmitProbeAsync(
            context,
            created.SpotIds[0],
            scenario);
        ZlinkStreamAssert.Ensure(
            signal.Deferred && reply.WithinDeadline,
            $"{scenario} default callback blocked the next Spot turn.");
        await context.ClosePayloadSpotAsync(
            context.NodeC,
            created.SpotIds[0]);
    }

    private static async Task VerifySameTurnRejectionsAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-G6-SAME-TURN-NEGATIVE";
        var created = await CreateAsync(
            context,
            scenario,
            SpotActorTransferNames.RelocationReadyUserSpotType,
            context.NodeA);
        var signal = await context.SignalRelocationReadyAsync(
            context.NodeC,
            new RelocationWorkloadReadyCallReq(
                created.SpotIds[0],
                scenario,
                DeferTwice: true,
                StartFrameworkOperationAfterDefer: true));
        ZlinkStreamAssert.Ensure(
            signal.Deferred
            && signal.SecondDeferRejected
            && signal.FrameworkOperationRejected,
            $"{scenario} did not reject the second Defer and subsequent "
            + "Framework operation.");
        _ = await context.WaitEvidenceAsync(
            context.NodeForRid(created.NodeRids[0]),
            [$"{scenario}|{created.SpotIds[0]}|relocation_ready_completed|Continued:"]);
        await context.ClosePayloadSpotAsync(
            context.NodeC,
            created.SpotIds[0]);
    }

    private static async Task<RelocationBulkSpotCreateRes> CreateAsync(
        SpotActorTransferScenarioContext context,
        string scenario,
        string spotType,
        Zlink.HttpClient.ZLinkHttpClient owner)
    {
        await context.SetExclusivePlacementAsync(owner);
        try
        {
            return await context.CreateBulkSpotsAsync(
                context.NodeC,
                new RelocationBulkSpotCreateReq(
                    scenario,
                    $"st-g6-{Guid.NewGuid():N}",
                    Count: 1,
                    ApplicationStateBytes: 0,
                    InstanceSpot: false,
                    MaxConcurrency: 1,
                    SpotType: spotType));
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }
    }

    private static async Task<RelocationWorkloadReply> SubmitProbeAsync(
        SpotActorTransferScenarioContext context,
        string spotId,
        string scenario)
    {
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        return await context.RequestSpotWorkloadAsync(
            context.NodeC,
            new RelocationWorkloadCallReq(
                spotId,
                scenario,
                Sequence: 1,
                OperationId: Guid.NewGuid().ToString("N"),
                SentUnixTimeMilliseconds: now,
                AbsoluteDeadlineUnixTimeMilliseconds: now + 10_000,
                TimeoutMilliseconds: 10_000));
    }

    private static void EnsureCallbackBeforeProbe(
        IReadOnlyList<ActorEvidence> evidence,
        string scenario,
        string spotId)
    {
        var callback = evidence.FindIndex(item =>
            item.Scenario == scenario
            && item.ActorId == spotId
            && item.Kind == "relocation_ready_completed");
        var probe = evidence.FindIndex(item =>
            item.Scenario == scenario
            && item.ActorId == spotId
            && item.Kind == "workload_request");
        ZlinkStreamAssert.Ensure(
            callback >= 0 && probe >= 0 && callback < probe,
            $"{scenario} admitted the next Spot job before readiness completion.");
    }

    private static int FindIndex(
        this IReadOnlyList<ActorEvidence> evidence,
        Func<ActorEvidence, bool> predicate)
    {
        for (var index = 0; index < evidence.Count; index++)
            if (predicate(evidence[index]))
                return index;
        return -1;
    }
}
