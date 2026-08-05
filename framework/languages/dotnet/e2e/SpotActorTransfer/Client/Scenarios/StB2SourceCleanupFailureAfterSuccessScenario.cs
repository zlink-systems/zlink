// Verifies ST-B2 target recovery after committed source cleanup loss.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StB2SourceCleanupFailureAfterSuccessScenario
{
    public static async Task RunAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-B2";
        var actorId = $"actor-cleanup-after-success-{Guid.NewGuid():N}";
        var spotId = $"spot-cleanup-after-success-{Guid.NewGuid():N}";
        var created = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            stateVersion: 22,
            applicationStateBytes: 4 * 1024);
        var source = context.NodeForRid(created.NodeRid);
        ZlinkStreamAssert.Ensure(
            ReferenceEquals(source, context.NodeA),
            $"{scenario} source must be the process killed by the fixture.");
        var (target, targetPrefix) =
            context.OtherActorNode(created.NodeRid);
        var spot = await context.CreateSpotAsync(target, spotId);
        ZlinkStreamAssert.Ensure(
            SpotActorTransferScenarioContext.IsNode(
                spot.NodeRid,
                targetPrefix),
            $"{scenario} target Spot was not placed on the selected "
            + "remote node.");
        var registration = await context.JoinAsync(
            source,
            actorId,
            new JoinTargetReq(scenario, spotId));
        ZlinkStreamAssert.Ensure(
            registration.Accepted,
            $"{scenario} deferred Join registration was rejected.");
        await context.WaitEvidenceAsync(source, [
            $"{scenario}|{actorId}|source_cleanup_wait|entry-spot-leave"
        ]);
        await context.WaitEvidenceAsync(target, [
            $"transfer|{actorId}|transfer_in|22",
            $"transfer|{actorId}|joined|{spotId}:22"
        ]);
        // Source OnLeaveActorAsync is a one-way notification. Target Join
        // completion and application dispatch must not wait for the source
        // cleanup callback to release its application gate.
        var targetBeforeLoss = await context.WaitEvidenceAsync(target, [
            $"{scenario}|{actorId}|success_reply|{spotId}"
        ]);
        ZlinkStreamAssert.Ensure(
            targetBeforeLoss.Count(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "success_reply") == 1,
            $"{scenario} Join completion was not delivered exactly once "
            + "before source cleanup loss.");

        await context.CrashNodeAAndWaitUnavailableAsync();

        var owner = await context.WaitActorOwnerAsync(
            target,
            actorId,
            spot.NodeRid);
        ZlinkStreamAssert.Ensure(
            owner.Generation == created.Generation,
            $"{scenario} ObjectGeneration changed from "
            + $"{created.Generation} to {owner.Generation}.");

        var afterLoss = await context.ProbeAsync(
            context.NodeD,
            actorId,
            new ProbeReq(
                scenario,
                "after-source-cleanup-loss"));
        ZlinkStreamAssert.Ensure(
            afterLoss.NodeRid == spot.NodeRid,
            $"{scenario} target ownership was lost after source shutdown: "
            + $"{afterLoss.NodeRid}.");
        ZlinkStreamAssert.Ensure(
            afterLoss.StateVersion == 22,
            $"{scenario} state changed after source cleanup loss: "
            + $"{afterLoss.StateVersion}.");
        ZlinkStreamAssert.Ensure(
            afterLoss.SpotId == spotId,
            $"{scenario} Actor was restored in unexpected Spot "
            + $"'{afterLoss.SpotId}'.");
        var targetAfterCrash = await context.GetEvidenceAsync(target);
        ZlinkStreamAssert.Ensure(
            targetAfterCrash.Count(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "success_reply") == 1,
            $"{scenario} Join completion was replayed after source process loss.");
        var targetAfterLoss = await context.WaitEvidenceAsync(target, [
            $"{scenario}|{actorId}|packet_handler|after-source-cleanup-loss"
        ]);
        ZlinkStreamAssert.Ensure(
            targetAfterLoss.Count(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "packet_handler"
                && item.Value == "after-source-cleanup-loss") == 1,
            $"{scenario} post-recovery request was not handled exactly "
            + "once.");
        EnsureEvidenceOrder(
            targetAfterLoss,
            scenario,
            actorId,
            ("joined", $"{spotId}:22"),
            ("success_reply", spotId),
            ("packet_handler", "after-source-cleanup-loss"));
    }

    private static void EnsureEvidenceOrder(
        IReadOnlyList<ActorEvidence> evidence,
        string scenario,
        string actorId,
        params (string Kind, string ValuePrefix)[] expected)
    {
        var previous = -1;
        foreach (var (kind, valuePrefix) in expected)
        {
            var index = -1;
            for (var candidate = 0;
                 candidate < evidence.Count;
                 candidate++)
            {
                var item = evidence[candidate];
                if (item.ActorId == actorId
                    && item.Kind == kind
                    && item.Value.StartsWith(
                        valuePrefix,
                        StringComparison.Ordinal)
                    && (item.Scenario == scenario
                        || item.Scenario == "transfer"))
                {
                    index = candidate;
                    break;
                }
            }

            ZlinkStreamAssert.Ensure(
                index > previous,
                $"{scenario} evidence order is invalid at '{kind}'.");
            previous = index;
        }
    }
}
