// Verifies ST-B4 Empty State Transfer behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StB4EmptyStateTransferScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-empty-state-{Guid.NewGuid():N}";
        var spotId = $"spot-empty-state-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeB, spotId);
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeEmptyState, 41);

        var join = await context.JoinAsync(context.NodeA, actorId, new JoinTargetReq("ST-B4", spotId));
        ZlinkStreamAssert.Ensure(join.Accepted, "ST-B4 join was rejected.");

        var probe = await context.ProbeAsync(context.NodeB, actorId, new ProbeReq("ST-B4", "after-empty-state-transfer"));
        ZlinkStreamAssert.Ensure(SpotActorTransferScenarioContext.IsNode(probe.NodeRid, "actor-b"), $"ST-B4 probe expected actor-b, got {probe.NodeRid}.");
        ZlinkStreamAssert.Ensure(probe.StateVersion == 41, $"ST-B4 loaded target state expected 41, got {probe.StateVersion}.");

        await context.WaitEvidenceAsync(context.NodeA, [
            $"transfer|{actorId}|transfer_out_empty|custom-adapter",
            $"transfer|{actorId}|leave|41"
        ]);
        await context.WaitEvidenceAsync(context.NodeB, [
            $"transfer|{actorId}|transfer_in_empty|custom-adapter",
            $"transfer|{actorId}|joined|{spotId}:0",
            $"transfer|{actorId}|domain_state_loaded|{actorId}",
            $"ST-B4|{actorId}|packet_handler|after-empty-state-transfer"
        ]);
    }
}
