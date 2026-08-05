// Verifies ST-F4 Message Follow relay and expiry rejection.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StF4MessageFollowThenRejectScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-F4";
        var actorId = $"actor-message-follow-{Guid.NewGuid():N}";
        var spotId = $"spot-message-follow-{Guid.NewGuid():N}";
        var created = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            104);
        var source = context.NodeForRid(created.NodeRid);
        var (target, _) = context.OtherActorNode(created.NodeRid);
        var caller = context.NodeD;
        var targetSpot = await context.CreateSpotAsync(target, spotId);

        var g1 = Guid.NewGuid().ToString("N");
        var g2 = Guid.NewGuid().ToString("N");
        var g1Marker = $"G1-{g1}";
        var g2Marker = $"G2-{g2}";
        await context.ArmExternalTransportDeliveryAsync(g1, g1Marker);
        await context.ArmExternalTransportDeliveryAsync(g2, g2Marker);
        var g1Delivery = context.SendFromNodeAsync(
            caller,
            actorId,
            new HandoffPacket(scenario, g1Marker));
        var g2Delivery = context.ProbeFromNodeAsync(
            caller,
            actorId,
            new ProbeReq(scenario, g2Marker),
            TimeSpan.FromSeconds(15));
        await context.WaitExternalTransportDeliveryAsync(g1);
        await context.WaitExternalTransportDeliveryAsync(g2);

        ZlinkStreamAssert.Ensure(
            (await context.JoinAsync(
                source,
                actorId,
                new JoinTargetReq(scenario, spotId))).Accepted,
            "ST-F4 relocation was rejected.");
        await context.WaitEvidenceAsync(
            target,
            [$"{scenario}|{actorId}|success_reply|{spotId}"]);
        _ = await context.WaitActorOwnerAsync(
            target,
            actorId,
            targetSpot.NodeRid);

        await context.ReleaseExternalTransportDeliveryAsync(g1);
        await g1Delivery;
        await context.WaitEvidenceAsync(
            target,
            [$"{scenario}|{actorId}|handoff_packet|{g1Marker}"]);
        var deliveredEvidence = await context.GetEvidenceAsync(target);
        ZlinkStreamAssert.Ensure(
            deliveredEvidence.Count(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "handoff_packet"
                && item.Value == g1Marker) == 1,
            "ST-F4 duration-bounded delivery was not handled exactly once.");
        ZlinkStreamAssert.Ensure(
            !(await context.GetEvidenceAsync(source)).Any(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "handoff_packet"
                && item.Value == g1Marker),
            "ST-F4 source application handler processed followed work.");

        // This topology configures a seven-second Message Follow duration.
        // The application-level assertion below proves expiry without reading
        // runtime markers or private route state.
        await Task.Delay(TimeSpan.FromSeconds(8));
        await context.ReleaseExternalTransportDeliveryAsync(g2);
        var stale = await g2Delivery;
        ZlinkStreamAssert.Ensure(!stale.Succeeded && stale.ErrorKind == "Unavailable",
            $"ST-F4 expected Unavailable after Message Follow expiry, got '{stale.ErrorKind}'.");
        ZlinkStreamAssert.Ensure(
            !(await context.GetEvidenceAsync(target)).Any(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "packet_handler"
                && item.Value == g2Marker),
            "ST-F4 expired request reached the target application handler.");
        ZlinkStreamAssert.Ensure(
            !(await context.GetEvidenceAsync(source)).Any(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "packet_handler"
                && item.Value == g2Marker),
            "ST-F4 expired request reached the source application handler.");
        ZlinkStreamAssert.Ensure(
            (await context.GetExternalTransportDeliveryAsync(g1))
                .ReleasedCount == 1
            && (await context.GetExternalTransportDeliveryAsync(g2))
                .ReleasedCount == 1,
            "ST-F4 did not release each pre-resolved delivery exactly once.");
    }
}
