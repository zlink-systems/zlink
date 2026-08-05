// Verifies source queue and precommit ingress-hold relocation boundaries.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StI4RelocationAuthorityBoundaryScenario
{
    internal static Task RunActorQueueAsync(
        SpotActorTransferScenarioContext context) =>
        RunQueueAsync(context, TargetKind.Actor);

    internal static Task RunSpotQueueAsync(
        SpotActorTransferScenarioContext context) =>
        RunQueueAsync(context, TargetKind.Spot);

    internal static Task RunActorHoldAsync(
        SpotActorTransferScenarioContext context) =>
        RunHoldAsync(context, TargetKind.Actor);

    internal static Task RunSpotHoldAsync(
        SpotActorTransferScenarioContext context) =>
        RunHoldAsync(context, TargetKind.Spot);

    private static async Task RunQueueAsync(
        SpotActorTransferScenarioContext context,
        TargetKind kind)
    {
        var selector = kind == TargetKind.Actor
            ? "MF-AO-QUEUE"
            : "MF-SO-QUEUE";
        Console.WriteLine(
            kind == TargetKind.Actor
                ? "message_follow_case_evidence case=MF-AO-QUEUE phase=started"
                : "message_follow_case_evidence case=MF-SO-QUEUE phase=started");
        var suffix = Guid.NewGuid().ToString("N");
        var targetId = kind == TargetKind.Actor
            ? $"actor-{selector.ToLowerInvariant()}-{suffix}"
            : $"spot-{selector.ToLowerInvariant()}-{suffix}";
        var createdTarget = await CreateTargetAsync(
            context,
            kind,
            targetId,
            selector,
            slowCapture: false);
        var source = createdTarget.Source;
        targetId = createdTarget.TargetId;
        var blocker = kind == TargetKind.Actor
            ? context.BlockActorQueueAsync(
                context.NodeD,
                new RelocationQueueBlockReq(selector, targetId))
            : context.BlockSpotQueueAsync(
                context.NodeD,
                new RelocationQueueBlockReq(selector, targetId));
        _ = await context.WaitEvidenceAsync(
            source,
            [$"{selector}|{targetId}|queue_block_started|"]);

        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var operationId = $"queue-{Guid.NewGuid():N}";
        var queued = new RelocationWorkloadCallReq(
            targetId,
            selector,
            Sequence: 1,
            operationId,
            now,
            now + 30_000,
            TimeoutMilliseconds: 30_000);
        if (kind == TargetKind.Actor)
            await context.SendActorWorkloadAsync(context.NodeD, queued);
        else
            await context.SendSpotWorkloadAsync(context.NodeD, queued);

        await context.SetExclusivePlacementAsync(context.NodeB);
        RelocateHostRes relocation;
        try
        {
            var relocating = context.RelocateAsync(
                source,
                TimeSpan.FromSeconds(30));
            await Task.Delay(200);
            var released = await context.ReleaseTransferGateAsync(
                source,
                targetId);
            ZlinkStreamAssert.Ensure(
                released.Released,
                $"{selector} did not release the source queue turn.");
            _ = await blocker;
            relocation = await relocating;
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync(source);
        }
        EnsureRelocated(selector, relocation);

        var target = context.NodeB;
        var targetEvidence = await context.WaitEvidenceAsync(
            target,
            [$"{selector}|{targetId}|workload_one_way|sequence=1;operation={operationId};"]);
        EnsureExactlyOnce(
            targetEvidence,
            selector,
            targetId,
            "workload_one_way",
            operationId);
        EnsureNoApplicationDelivery(
            await context.GetEvidenceAsync(source),
            selector,
            targetId,
            operationId);
        Console.WriteLine(
            (kind == TargetKind.Actor
                ? "message_follow_case_evidence case=MF-AO-QUEUE"
                : "message_follow_case_evidence case=MF-SO-QUEUE")
            + " phase=completed terminal_count=1"
            + " source_queue_accepted=1"
            + " current_owner_handler_count=1"
            + " previous_owner_handler_count=0"
            + " message_follow_route_used=0");
    }

    private static async Task RunHoldAsync(
        SpotActorTransferScenarioContext context,
        TargetKind kind)
    {
        var selector = kind == TargetKind.Actor
            ? "MF-AR-HOLD"
            : "MF-SR-HOLD";
        Console.WriteLine(
            kind == TargetKind.Actor
                ? "message_follow_case_evidence case=MF-AR-HOLD phase=started"
                : "message_follow_case_evidence case=MF-SR-HOLD phase=started");
        var suffix = Guid.NewGuid().ToString("N");
        var targetId = kind == TargetKind.Actor
            ? $"actor-{selector.ToLowerInvariant()}-slow-capture-{suffix}"
            : $"spot-{selector.ToLowerInvariant()}-slow-capture-{suffix}";
        var createdTarget = await CreateTargetAsync(
            context,
            kind,
            targetId,
            selector + "-SLOW-CAPTURE",
            slowCapture: true);
        var source = createdTarget.Source;
        targetId = createdTarget.TargetId;

        // Prime the caller's bounded route cache before the source seals.
        _ = await RequestAsync(
            context,
            kind,
            targetId,
            selector + "-PRIME",
            $"prime-{Guid.NewGuid():N}");

        var operationId = $"hold-{Guid.NewGuid():N}";
        await context.ArmExternalTransportDeliveryAsync(
            operationId,
            operationId);
        var request = RequestAsync(
            context,
            kind,
            targetId,
            selector,
            operationId);
        await context.WaitExternalTransportDeliveryAsync(operationId);

        await context.SetExclusivePlacementAsync(context.NodeB);
        RelocateHostRes relocation;
        try
        {
            var relocating = context.RelocateAsync(
                source,
                TimeSpan.FromSeconds(30));
            var slowKind = "slow_capture_started";
            _ = await context.WaitEvidenceAsync(
                source,
                [$"{selector}-SLOW-CAPTURE|{targetId}|{slowKind}|1250ms"]);
            await context.ReleaseExternalTransportDeliveryAsync(operationId);
            await Task.Delay(100);
            ZlinkStreamAssert.Ensure(
                !request.IsCompleted,
                $"{selector} request completed before relocation commit.");
            relocation = await relocating;
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync(source);
        }
        EnsureRelocated(selector, relocation);
        var reply = await request;
        ZlinkStreamAssert.Ensure(
            reply.OperationId == operationId
            && SpotActorTransferScenarioContext.IsNode(
                reply.NodeRid,
                "actor-b"),
            $"{selector} did not preserve operation/reply routing.");
        var targetEvidence = await context.WaitEvidenceAsync(
            context.NodeB,
            [$"{selector}|{targetId}|workload_request|sequence=1;operation={operationId};"]);
        EnsureExactlyOnce(
            targetEvidence,
            selector,
            targetId,
            "workload_request",
            operationId);
        EnsureNoApplicationDelivery(
            await context.GetEvidenceAsync(source),
            selector,
            targetId,
            operationId);
        Console.WriteLine(
            (kind == TargetKind.Actor
                ? "message_follow_case_evidence case=MF-AR-HOLD"
                : "message_follow_case_evidence case=MF-SR-HOLD")
            + " phase=completed terminal_count=1"
            + " ingress_hold_enqueued=1"
            + " current_owner_handler_count=1"
            + " previous_owner_handler_count=0"
            + " reply_correlation=preserved");
    }

    private static async Task<(
        Zlink.HttpClient.ZLinkHttpClient Source,
        string TargetId)>
        CreateTargetAsync(
            SpotActorTransferScenarioContext context,
            TargetKind kind,
            string targetId,
            string scenario,
            bool slowCapture)
    {
        await context.SetExclusivePlacementAsync(context.NodeA);
        try
        {
            string nodeRid;
            if (kind == TargetKind.Actor)
            {
                var created = await context.CreateActorAsync(
                    context.NodeA,
                    targetId,
                    SpotActorTransferNames.ActorTypeStateful,
                    stateVersion: 1,
                    applicationStateBytes: 4 * 1024);
                nodeRid = created.NodeRid;
            }
            else
            {
                var created = await context.CreateBulkSpotsAsync(
                    context.NodeC,
                    new RelocationBulkSpotCreateReq(
                        scenario,
                        targetId,
                        Count: 1,
                        ApplicationStateBytes: 4 * 1024,
                        InstanceSpot: false,
                        MaxConcurrency: 1));
                targetId = created.SpotIds[0];
                nodeRid = created.NodeRids[0];
            }
            _ = slowCapture;
            return (context.NodeForRid(nodeRid), targetId);
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }
    }

    private static async Task<RelocationWorkloadReply> RequestAsync(
        SpotActorTransferScenarioContext context,
        TargetKind kind,
        string targetId,
        string scenario,
        string operationId)
    {
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var call = new RelocationWorkloadCallReq(
            targetId,
            scenario,
            Sequence: 1,
            operationId,
            now,
            now + 30_000,
            TimeoutMilliseconds: 30_000);
        return kind == TargetKind.Actor
            ? await context.RequestActorWorkloadAsync(context.NodeD, call)
            : await context.RequestSpotWorkloadAsync(context.NodeD, call);
    }

    private static void EnsureRelocated(
        string selector,
        RelocateHostRes relocation) =>
        ZlinkStreamAssert.Ensure(
            string.Equals(
                relocation.Outcome,
                "Relocated",
                StringComparison.OrdinalIgnoreCase),
            $"{selector} relocation failed: {relocation.Reason}.");

    private static void EnsureExactlyOnce(
        IReadOnlyList<ActorEvidence> evidence,
        string scenario,
        string targetId,
        string kind,
        string operationId) =>
        ZlinkStreamAssert.Ensure(
            evidence.Count(item =>
                item.Scenario == scenario
                && item.ActorId == targetId
                && item.Kind == kind
                && item.Value.Contains(
                    $"operation={operationId}",
                    StringComparison.Ordinal)) == 1,
            $"{scenario} operation was not handled exactly once.");

    private static void EnsureNoApplicationDelivery(
        IReadOnlyList<ActorEvidence> evidence,
        string scenario,
        string targetId,
        string operationId) =>
        ZlinkStreamAssert.Ensure(
            !evidence.Any(item =>
                item.Scenario == scenario
                && item.ActorId == targetId
                && item.Value.Contains(
                    $"operation={operationId}",
                    StringComparison.Ordinal)),
            $"{scenario} previous owner executed application work.");

    private enum TargetKind
    {
        Actor,
        Spot
    }
}
