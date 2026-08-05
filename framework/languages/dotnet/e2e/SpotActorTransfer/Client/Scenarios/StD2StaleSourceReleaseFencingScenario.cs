// Verifies ST-D2 Stale Source Release Fencing behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StD2StaleSourceReleaseFencingScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-stale-release-{Guid.NewGuid():N}";
        var spotId = $"spot-stale-release-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeB, spotId);
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeStateful, 81);
        await context.ArmCleanupGateAsync(context.NodeA, actorId, "ST-D2");

        var join = await context.JoinAsync(context.NodeA, actorId, new JoinTargetReq("ST-D2", spotId));
        ZlinkStreamAssert.Ensure(join.Accepted, "ST-D2 join was rejected.");
        await context.AllowCleanupAttemptAsync(context.NodeA, actorId, "ST-D2");
        await context.WaitEvidenceAsync(context.NodeA, [
            $"ST-D2|{actorId}|source_cleanup_attempt|"
        ]);
        var before = await context.GetActorRefWithEvidenceAsync(
            context.NodeB, actorId, "ST-D2", "location_snapshot_before_cleanup");
        ZlinkStreamAssert.Ensure(SpotActorTransferScenarioContext.IsNode(before.NodeRid, "actor-b"), $"ST-D2 target ref expected actor-b, got {before.NodeRid}.");
        var beforeCleanupProbe = await context.ProbeAsync(
            context.NodeB,
            actorId,
            new ProbeReq("ST-D2", "before-stale-cleanup-release"));
        ZlinkStreamAssert.Ensure(SpotActorTransferScenarioContext.IsNode(beforeCleanupProbe.NodeRid, "actor-b"),
            $"ST-D2 pre-cleanup probe expected actor-b, got {beforeCleanupProbe.NodeRid}.");

        await context.ReleaseCleanupGateAsync(context.NodeA, actorId, "ST-D2");
        await context.WaitEvidenceAsync(context.NodeA, [
            $"ST-D2|{actorId}|source_cleanup_completed|"
        ]);
        var after = await context.GetActorRefWithEvidenceAsync(
            context.NodeB, actorId, "ST-D2", "location_snapshot_after_cleanup");
        ZlinkStreamAssert.Ensure(SpotActorTransferScenarioContext.IsNode(after.NodeRid, "actor-b"), $"ST-D2 target ref changed after delayed cleanup: {after.NodeRid}.");
        ZlinkStreamAssert.Ensure(after.Generation == before.Generation,
            $"ST-D2 generation changed after delayed cleanup. before={before.Generation}, after={after.Generation}");
        var probe = await context.ProbeAsync(context.NodeB, actorId, new ProbeReq("ST-D2", "after-stale-cleanup-window"));
        ZlinkStreamAssert.Ensure(SpotActorTransferScenarioContext.IsNode(probe.NodeRid, "actor-b"), $"ST-D2 probe expected actor-b, got {probe.NodeRid}.");
    }
}
