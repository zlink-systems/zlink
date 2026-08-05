// Verifies public Deferred Join snapshot and Actor queue barrier semantics.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StH1DeferredJoinBarrierScenario
{
    public static async Task RunAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-H1";
        var actorId = $"actor-deferred-join-{Guid.NewGuid():N}";
        var spotId = $"spot-deferred-join-{Guid.NewGuid():N}";
        var created = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            stateVersion: 701,
            applicationStateBytes: 4 * 1024);
        var source = context.NodeForRid(created.NodeRid);
        var (target, targetPrefix) =
            context.OtherActorNode(created.NodeRid);
        var targetSpot = await context.CreateSpotAsync(
            target,
            spotId,
            mode: "delay-joined");

        var join = context.JoinAsync(
            source,
            actorId,
            new JoinTargetReq(scenario, spotId));
        await context.WaitEvidenceAsync(
            source,
            [$"{scenario}|{actorId}|defer_registered|{spotId}"]);

        var marker = $"barrier-payload-{Guid.NewGuid():N}";
        await context.SendFromNodeAsync(
            context.NodeD,
            actorId,
            new HandoffPacket(scenario, marker));

        await Task.Delay(TimeSpan.FromMilliseconds(100));
        ZlinkStreamAssert.Ensure(
            !(await context.GetEvidenceAsync(target)).Any(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "admission"),
            $"{scenario} target I/O started before the Actor handler terminal.");

        var registration = await join;
        ZlinkStreamAssert.Ensure(
            registration.Accepted,
            $"{scenario} registration response was rejected.");
        await context.WaitEvidenceAsync(
            source,
            [$"{scenario}|{actorId}|defer_handler_completed|{spotId}"]);
        await context.WaitEvidenceAsync(
            target,
            [$"{scenario}|{actorId}|joined_wait|{spotId}"]);

        await Task.Delay(TimeSpan.FromMilliseconds(200));
        foreach (var node in new[] { source, target })
        {
            ZlinkStreamAssert.Ensure(
                !(await context.GetEvidenceAsync(node)).Any(item =>
                    item.Scenario == scenario
                    && item.ActorId == actorId
                    && item.Kind == "handoff_packet"
                    && item.Value == marker),
                $"{scenario} queued Actor send crossed the Join barrier.");
        }

        var released = await context.ReleaseJoinedGateAsync(target, spotId);
        ZlinkStreamAssert.Ensure(
            released.Released,
            $"{scenario} target joined gate was not released.");
        await context.WaitEvidenceAsync(
            target,
            [
                $"{scenario}|{actorId}|joined_released|{spotId}",
                $"{scenario}|{actorId}|success_reply|{spotId}",
                $"{scenario}|{actorId}|handoff_packet|{marker}"
            ]);
        _ = await context.WaitActorOwnerAsync(
            target,
            actorId,
            targetSpot.NodeRid);

        var targetEvidence = await context.GetEvidenceAsync(target);
        ZlinkStreamAssert.Ensure(
            targetEvidence.Count(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "handoff_packet"
                && item.Value == marker) == 1,
            $"{scenario} queued Actor send was not handled exactly once.");
        ZlinkStreamAssert.Ensure(
            !(await context.GetEvidenceAsync(source)).Any(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "handoff_packet"
                && item.Value == marker),
            $"{scenario} queued Actor send ran on the previous owner.");
        ZlinkStreamAssert.Ensure(
            !targetEvidence.Any(item =>
                item.Scenario == "ST-H1-MUTATED"
                || item.Value == "mutated-target"),
            $"{scenario} target observed the request object after Defer.");
        ZlinkStreamAssert.Ensure(
            SpotActorTransferScenarioContext.IsNode(
                targetSpot.NodeRid,
                targetPrefix),
            $"{scenario} target Spot was not placed on the selected target node.");
        EnsureEvidenceOrder(
            targetEvidence,
            scenario,
            actorId,
            ("success_reply", spotId),
            ("handoff_packet", marker));
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
            for (var candidate = 0; candidate < evidence.Count; candidate++)
            {
                var item = evidence[candidate];
                if (item.Scenario == scenario
                    && item.ActorId == actorId
                    && item.Kind == kind
                    && item.Value.StartsWith(valuePrefix, StringComparison.Ordinal))
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
