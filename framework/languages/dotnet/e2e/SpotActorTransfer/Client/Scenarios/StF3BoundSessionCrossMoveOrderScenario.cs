// Verifies ST-F3 Bound Session Cross Move Order behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StF3BoundSessionCrossMoveOrderScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-bound-order-{Guid.NewGuid():N}";
        var spotId = $"spot-bound-order-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeB, spotId, "delay-joined");
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeStateful, 103);
        var oldRef = await context.GetActorRefAsync(context.NodeA, actorId);
        await using var bound = await context.ConnectAndBindAsync(context.Options.NodeAStreamEndpoint, "ST-F3", oldRef);

        var joinTask = context.JoinAsync(context.NodeA, actorId, new JoinTargetReq("ST-F3", spotId));
        await context.WaitEvidenceAsync(context.NodeB, [$"ST-F3|{actorId}|joined_wait|{spotId}"]);
        await bound.Send(new HandoffPacket("ST-F3", "S1")).Async();
        await bound.Send(new HandoffPacket("ST-F3", "S2")).Async();
        await context.WaitRuntimeEvidenceAsync(context.NodeB,
            $"handoff_backlog actor={actorId} arrival=1");
        await context.ReleaseJoinedGateAsync(context.NodeB, spotId);
        // Submit at the completion boundary so the packets may hit either the
        // source capture or the rebound target route. The actor queue must still
        // observe the pre-cutover backlog first.
        await bound.Send(new HandoffPacket("ST-F3", "S3")).Async();
        await bound.Send(new HandoffPacket("ST-F3", "S4")).Async();
        ZlinkStreamAssert.Ensure((await joinTask).Accepted, "ST-F3 transfer was rejected.");
        await context.AssertEvidenceOrderAsync(context.NodeB, actorId, "handoff_packet", ["S1", "S2", "S3", "S4"]);
    }
}
