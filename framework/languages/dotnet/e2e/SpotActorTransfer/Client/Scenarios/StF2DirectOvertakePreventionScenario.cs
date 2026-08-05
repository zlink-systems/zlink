// Verifies ST-F2 Direct Overtake Prevention behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StF2DirectOvertakePreventionScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-inflight-overtake-{Guid.NewGuid():N}";
        var spotId = $"spot-inflight-overtake-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeB, spotId, "delay-joined");
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeStateful, 102);
        var joinTask = context.JoinAsync(context.NodeA, actorId, new JoinTargetReq("ST-F2", spotId));
        await context.WaitEvidenceAsync(context.NodeB, [$"ST-F2|{actorId}|joined_wait|{spotId}"]);
        foreach (var marker in new[] { "B1", "B2" })
            await context.SendFromNodeAsync(
                context.NodeA,
                actorId,
                new HandoffPacket("ST-F2", marker));
        // The authority is committed before this call, so the global Actor
        // ID send arrives at target ingress and must wait behind the imported
        // backlog. The source only performs Message Follow relay.
        await context.WaitRuntimeEvidenceAsync(context.NodeB,
            $"handoff_backlog actor={actorId} arrival=1");
        await context.ReleaseJoinedGateAsync(context.NodeB, spotId);
        ZlinkStreamAssert.Ensure((await joinTask).Accepted, "ST-F2 transfer was rejected.");
        await context.SendFromNodeAsync(
            context.NodeB,
            actorId,
            new HandoffPacket("ST-F2", "D1"));
        await context.AssertEvidenceOrderAsync(context.NodeB, actorId, "handoff_packet", ["B1", "B2", "D1"]);
    }
}
