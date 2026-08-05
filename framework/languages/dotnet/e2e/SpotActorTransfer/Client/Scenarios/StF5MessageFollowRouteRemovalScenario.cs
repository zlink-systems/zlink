// Verifies ST-F5 Message Follow route removal.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StF5MessageFollowRouteRemovalScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-F5";
        var actorId = $"actor-message-follow-chain-{Guid.NewGuid():N}";
        var firstSpot = $"spot-message-follow-chain-one-{Guid.NewGuid():N}";
        var finalSpot = $"spot-message-follow-chain-final-{Guid.NewGuid():N}";
        var created = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            105);
        var source = context.NodeForRid(created.NodeRid);
        var (firstTarget, _) = context.OtherActorNode(created.NodeRid);
        var finalTarget = context.ThirdActorNode(source, firstTarget);
        var caller = context.NodeD;
        var firstSpotRef = await context.CreateSpotAsync(
            firstTarget,
            firstSpot);
        var finalSpotRef = await context.CreateSpotAsync(
            finalTarget,
            finalSpot);

        var chainOperation = Guid.NewGuid().ToString("N");
        var expiredOperation = Guid.NewGuid().ToString("N");
        var chainMarker = $"chain-to-final-{chainOperation}";
        var expiredMarker = $"after-route-removal-{expiredOperation}";
        await context.ArmExternalTransportDeliveryAsync(
            chainOperation, chainMarker);
        await context.ArmExternalTransportDeliveryAsync(
            expiredOperation, expiredMarker);
        var chained = context.SendFromNodeAsync(
            caller,
            actorId,
            new HandoffPacket(scenario, chainMarker));
        await context.WaitExternalTransportDeliveryAsync(chainOperation);
        var expired = context.ProbeFromNodeAsync(
            caller,
            actorId,
            new ProbeReq(scenario, expiredMarker),
            TimeSpan.FromSeconds(15));
        await context.WaitExternalTransportDeliveryAsync(expiredOperation);

        ZlinkStreamAssert.Ensure((await context.JoinAsync(
                source,
                actorId,
                new JoinTargetReq(scenario, firstSpot))).Accepted,
            "ST-F5 first transfer was rejected.");
        await context.WaitEvidenceAsync(
            firstTarget,
            [$"{scenario}|{actorId}|success_reply|{firstSpot}"]);
        _ = await context.WaitActorOwnerAsync(
            firstTarget,
            actorId,
            firstSpotRef.NodeRid);
        ZlinkStreamAssert.Ensure((await context.JoinAsync(
                firstTarget,
                actorId,
                new JoinTargetReq(scenario, finalSpot))).Accepted,
            "ST-F5 chained transfer was rejected.");
        await context.WaitEvidenceAsync(
            finalTarget,
            [$"{scenario}|{actorId}|success_reply|{finalSpot}"]);

        await context.ReleaseExternalTransportDeliveryAsync(chainOperation);
        await chained;
        await context.WaitEvidenceAsync(
            finalTarget,
            [$"{scenario}|{actorId}|handoff_packet|{chainMarker}"]);
        _ = await context.WaitActorOwnerAsync(
            finalTarget,
            actorId,
            finalSpotRef.NodeRid);
        var finalEvidence = await context.GetEvidenceAsync(finalTarget);
        ZlinkStreamAssert.Ensure(
            finalEvidence.Count(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "handoff_packet"
                && item.Value == chainMarker) == 1,
            "ST-F5 multi-hop delivery was not handled exactly once.");
        foreach (var previousOwner in new[] { source, firstTarget })
        {
            ZlinkStreamAssert.Ensure(
                !(await context.GetEvidenceAsync(previousOwner)).Any(item =>
                    item.Scenario == scenario
                    && item.ActorId == actorId
                    && item.Kind == "handoff_packet"
                    && item.Value == chainMarker),
                "ST-F5 previous-owner application handler processed followed work.");
        }

        // Expiry is verified through the public terminal result. Route table
        // cleanup itself has no public observation surface.
        await Task.Delay(TimeSpan.FromSeconds(8));
        await context.ReleaseExternalTransportDeliveryAsync(expiredOperation);
        var stale = await expired;
        ZlinkStreamAssert.Ensure(!stale.Succeeded && stale.ErrorKind == "Unavailable",
            $"ST-F5 expected Unavailable after Message Follow route removal, got '{stale.ErrorKind}'.");
        foreach (var node in new[] { source, firstTarget, finalTarget })
        {
            ZlinkStreamAssert.Ensure(
                !(await context.GetEvidenceAsync(node)).Any(item =>
                    item.Scenario == scenario
                    && item.ActorId == actorId
                    && item.Kind == "packet_handler"
                    && item.Value == expiredMarker),
                "ST-F5 expired request reached an application handler.");
        }
        ZlinkStreamAssert.Ensure(
            (await context.GetExternalTransportDeliveryAsync(chainOperation))
                .ReleasedCount == 1
            && (await context.GetExternalTransportDeliveryAsync(
                expiredOperation)).ReleasedCount == 1,
            "ST-F5 did not release each pre-resolved operation exactly once.");
    }
}
