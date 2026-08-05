// Verifies Spot one-way and request Message Follow through stale routes.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StI4SpotMessageFollowMatrixScenario
{
    public static async Task RunAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-I4-SPOT-FOLLOW";
        var prefix = $"spot-message-follow-{Guid.NewGuid():N}";
        await context.SetExclusivePlacementAsync(context.NodeA);
        RelocationBulkSpotCreateRes created;
        try
        {
            created = await context.CreateBulkSpotsAsync(
                context.NodeC,
                new RelocationBulkSpotCreateReq(
                    scenario,
                    prefix,
                    Count: 1,
                    ApplicationStateBytes: 4 * 1024,
                    InstanceSpot: false,
                    MaxConcurrency: 1));
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync();
        }
        var spotId = created.SpotIds[0];
        var source = context.NodeForRid(created.NodeRids[0]);

        var oneWayOperation = $"spot-one-way-{Guid.NewGuid():N}";
        var requestOperation = $"spot-request-{Guid.NewGuid():N}";
        Console.WriteLine(
            "message_follow_case_evidence case=MF-SO-FOLLOW phase=started");
        Console.WriteLine(
            "message_follow_case_evidence case=MF-SR-FOLLOW phase=started");
        await context.ArmExternalTransportDeliveryAsync(
            oneWayOperation,
            oneWayOperation);
        await context.ArmExternalTransportDeliveryAsync(
            requestOperation,
            requestOperation);
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var oneWay = context.SendSpotWorkloadAsync(
            context.NodeD,
            new RelocationWorkloadCallReq(
                spotId,
                scenario,
                Sequence: 1,
                oneWayOperation,
                now,
                now + 30_000,
                TimeoutMilliseconds: 30_000));
        var request = context.RequestSpotWorkloadAsync(
            context.NodeD,
            new RelocationWorkloadCallReq(
                spotId,
                scenario,
                Sequence: 2,
                requestOperation,
                now,
                now + 30_000,
                TimeoutMilliseconds: 30_000));
        await context.WaitExternalTransportDeliveryAsync(oneWayOperation);
        await context.WaitExternalTransportDeliveryAsync(requestOperation);

        await context.SetExclusivePlacementAsync(context.NodeB);
        RelocateHostRes relocation;
        try
        {
            relocation = await context.RelocateAsync(
                source,
                TimeSpan.FromSeconds(30));
        }
        finally
        {
            await context.RestoreDefaultPlacementAsync(source);
        }
        ZlinkStreamAssert.Ensure(
            string.Equals(
                relocation.Outcome,
                "Relocated",
                StringComparison.OrdinalIgnoreCase),
            $"{scenario} relocation failed: {relocation.Reason}.");

        await context.ReleaseExternalTransportDeliveryAsync(oneWayOperation);
        await context.ReleaseExternalTransportDeliveryAsync(requestOperation);
        await oneWay;
        var reply = await request;
        ZlinkStreamAssert.Ensure(
            reply.OperationId == requestOperation
            && reply.Sequence == 2
            && SpotActorTransferScenarioContext.IsNode(
                reply.NodeRid,
                "actor-b"),
            $"{scenario} request reply route/correlation changed.");
        var targetEvidence = await context.WaitEvidenceAsync(
            context.NodeB,
            [
                $"{scenario}|{spotId}|workload_one_way|sequence=1;operation={oneWayOperation};",
                $"{scenario}|{spotId}|workload_request|sequence=2;operation={requestOperation};"
            ]);
        EnsureExactlyOnce(
            targetEvidence,
            scenario,
            spotId,
            "workload_one_way",
            oneWayOperation);
        EnsureExactlyOnce(
            targetEvidence,
            scenario,
            spotId,
            "workload_request",
            requestOperation);
        var sourceEvidence = await context.GetEvidenceAsync(source);
        ZlinkStreamAssert.Ensure(
            !sourceEvidence.Any(item =>
                item.Scenario == scenario
                && item.ActorId == spotId
                && (item.Value.Contains(
                        $"operation={oneWayOperation}",
                        StringComparison.Ordinal)
                    || item.Value.Contains(
                        $"operation={requestOperation}",
                        StringComparison.Ordinal))),
            $"{scenario} source application handler processed followed work.");
        Console.WriteLine(
            "message_follow_case_evidence case=MF-SO-FOLLOW"
            + " phase=completed terminal_count=1"
            + " current_owner_handler_count=1"
            + " previous_owner_handler_count=0");
        Console.WriteLine(
            "message_follow_case_evidence case=MF-SR-FOLLOW"
            + " phase=completed terminal_count=1"
            + " current_owner_handler_count=1"
            + " previous_owner_handler_count=0"
            + " reply_correlation=preserved");
    }

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
            $"{scenario} operation '{operationId}' was not handled once.");
}
