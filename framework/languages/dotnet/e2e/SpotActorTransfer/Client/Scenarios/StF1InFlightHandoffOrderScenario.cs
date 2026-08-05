// Verifies ST-F1 In Flight Handoff Order behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StF1InFlightHandoffOrderScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-inflight-order-{Guid.NewGuid():N}";
        var spotId = $"spot-inflight-order-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeB, spotId, "delay-joined");
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeStateful, 101);
        var joinTask = context.JoinAsync(context.NodeA, actorId, new JoinTargetReq("ST-F1", spotId));
        await context.WaitEvidenceAsync(context.NodeB, [$"ST-F1|{actorId}|joined_wait|{spotId}"]);
        foreach (var marker in new[] { "P1", "P2", "P3" })
            await context.SendFromNodeAsync(
                context.NodeA,
                actorId,
                new HandoffPacket("ST-F1", marker));
        // After authority commit, the public Actor ID route follows the
        // target. The target ingress owns the backlog while OnJoinedActorAsync
        // is waiting; the source only records the Message Follow relay.
        await context.WaitRuntimeEvidenceAsync(context.NodeB,
            $"handoff_backlog actor={actorId} arrival=2");
        var sourceEvidence = await context.GetEvidenceAsync(context.NodeA);
        ZlinkStreamAssert.Ensure(
            !sourceEvidence.Any(item => SpotActorTransferScenarioContext.EvidenceText(item)
                .Contains($"ST-F1|{actorId}|handoff_packet|", StringComparison.Ordinal)),
            "ST-F1 packet ran on the source node.");

        await context.ReleaseJoinedGateAsync(context.NodeB, spotId);
        ZlinkStreamAssert.Ensure((await joinTask).Accepted, "ST-F1 transfer was rejected.");
        await context.AssertEvidenceOrderAsync(context.NodeB, actorId, "handoff_packet", ["P1", "P2", "P3"]);
    }
}
