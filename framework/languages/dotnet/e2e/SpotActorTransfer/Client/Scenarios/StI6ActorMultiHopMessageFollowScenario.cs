// Verifies Actor Message Follow across two relocations through external TCP.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StI6ActorMultiHopMessageFollowScenario
{
    public static async Task RunAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-I6";
        var actorId = $"actor-message-follow-multi-hop-{Guid.NewGuid():N}";
        var firstSpotId = $"spot-message-follow-hop-one-{Guid.NewGuid():N}";
        var secondSpotId = $"spot-message-follow-hop-two-{Guid.NewGuid():N}";
        var created = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            stateVersion: 606,
            applicationStateBytes: 4 * 1024);
        var source = context.NodeForRid(created.NodeRid);
        var (firstTarget, _) = context.OtherActorNode(created.NodeRid);
        var secondTarget = context.ThirdActorNode(source, firstTarget);
        var secondTargetPrefix =
            ReferenceEquals(secondTarget, context.NodeA)
                ? "actor-a"
                : ReferenceEquals(secondTarget, context.NodeB)
                    ? "actor-b"
                    : "actor-c";

        var firstSpot = await context.CreateSpotAsync(firstTarget, firstSpotId);
        var secondSpot = await context.CreateSpotAsync(secondTarget, secondSpotId);
        var gateId = Guid.NewGuid().ToString("N");
        var marker = $"multi-hop-request-{gateId}";
        await context.ArmExternalTransportDeliveryAsync(
            gateId,
            marker);
        var delayedRequest = context.ProbeFromNodeAsync(
            context.NodeD,
            actorId,
            new ProbeReq(scenario, marker),
            TimeSpan.FromSeconds(15));
        await context.WaitExternalTransportDeliveryAsync(gateId);

        ZlinkStreamAssert.Ensure(
            (await context.JoinAsync(
                source,
                actorId,
                new JoinTargetReq(scenario, firstSpotId))).Accepted,
            $"{scenario} first relocation was rejected.");
        await context.WaitEvidenceAsync(
            firstTarget,
            [$"{scenario}|{actorId}|success_reply|{firstSpotId}"]);
        _ = await context.WaitActorOwnerAsync(
            firstTarget,
            actorId,
            firstSpot.NodeRid);
        ZlinkStreamAssert.Ensure(
            (await context.JoinAsync(
                firstTarget,
                actorId,
                new JoinTargetReq(scenario, secondSpotId))).Accepted,
            $"{scenario} second relocation was rejected.");
        await context.WaitEvidenceAsync(
            secondTarget,
            [$"{scenario}|{actorId}|success_reply|{secondSpotId}"]);
        _ = await context.WaitActorOwnerAsync(
            secondTarget,
            actorId,
            secondSpot.NodeRid);

        await context.ReleaseExternalTransportDeliveryAsync(gateId);
        var delayedResult = await delayedRequest;
        ZlinkStreamAssert.Ensure(
            delayedResult.Succeeded && delayedResult.Reply is not null,
            $"{scenario} delayed request failed: {delayedResult.ErrorKind}.");
        var reply = delayedResult.Reply!;
        ZlinkStreamAssert.Ensure(
            SpotActorTransferScenarioContext.IsNode(
                reply.NodeRid,
                secondTargetPrefix),
            $"{scenario} multi-hop request reached '{reply.NodeRid}'.");
        ZlinkStreamAssert.Ensure(
            reply.StateVersion == 606,
            $"{scenario} multi-hop relocation lost Actor state.");

        var targetEvidence = await context.WaitEvidenceAsync(
            secondTarget,
            [$"{scenario}|{actorId}|packet_handler|{marker}"]);
        ZlinkStreamAssert.Ensure(
            targetEvidence.Count(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "packet_handler"
                && item.Value == marker) == 1,
            $"{scenario} multi-hop request was not handled exactly once.");
        foreach (var previousOwner in new[] { source, firstTarget })
        {
            ZlinkStreamAssert.Ensure(
                !(await context.GetEvidenceAsync(previousOwner)).Any(item =>
                    item.Scenario == scenario
                    && item.ActorId == actorId
                    && item.Kind == "packet_handler"
                    && item.Value == marker),
                $"{scenario} previous owner processed followed work.");
        }
        ZlinkStreamAssert.Ensure(
            (await context.GetExternalTransportDeliveryAsync(gateId))
                .ReleasedCount == 1,
            $"{scenario} delayed request was not released exactly once.");
    }
}
