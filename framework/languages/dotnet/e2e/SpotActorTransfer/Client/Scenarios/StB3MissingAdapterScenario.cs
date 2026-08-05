// Verifies ST-B3 Missing Adapter behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StB3MissingAdapterScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-no-adapter-{Guid.NewGuid():N}";
        var spotId = $"spot-no-adapter-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeB, spotId);
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeNoAdapter, 31);

        var join = await context.JoinAsync(context.NodeA, actorId, new JoinTargetReq("ST-B3", spotId));
        ZlinkStreamAssert.Ensure(join.Accepted, "ST-B3 join was rejected.");

        var probe = await context.ProbeAsync(context.NodeB, actorId, new ProbeReq("ST-B3", "after-default-empty-transfer"));
        ZlinkStreamAssert.Ensure(SpotActorTransferScenarioContext.IsNode(probe.NodeRid, "actor-b"), $"ST-B3 probe expected actor-b, got {probe.NodeRid}.");
        ZlinkStreamAssert.Ensure(probe.StateVersion == 0, $"ST-B3 default empty target state expected 0, got {probe.StateVersion}.");
        await context.WaitEvidenceAsync(context.NodeA, [
            $"transfer|{actorId}|transfer_out_empty_default|no-adapter",
            $"transfer|{actorId}|leave|31"
        ]);
        await context.WaitEvidenceAsync(context.NodeB, [
            $"transfer|{actorId}|transfer_in_empty_default|actor-factory",
            $"transfer|{actorId}|joined|{spotId}:0",
            $"ST-B3|{actorId}|packet_handler|after-default-empty-transfer"
        ]);
    }
}
