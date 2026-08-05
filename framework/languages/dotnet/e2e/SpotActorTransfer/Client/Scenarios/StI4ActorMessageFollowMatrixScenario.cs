// Verifies Actor Message Follow with process-external TCP delivery gates.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StI4ActorMessageFollowMatrixScenario
{
    public static async Task RunAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-I4";
        var actorId = $"actor-message-follow-matrix-{Guid.NewGuid():N}";
        var spotId = $"spot-message-follow-matrix-{Guid.NewGuid():N}";
        var created = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            stateVersion: 404,
            applicationStateBytes: 4 * 1024);
        var source = context.NodeForRid(created.NodeRid);
        var (target, targetPrefix) =
            context.OtherActorNode(created.NodeRid);
        var caller = context.NodeD;
        var targetSpot = await context.CreateSpotAsync(target, spotId);

        var baselineGate = Guid.NewGuid().ToString("N");
        var baselineMarker = $"actor-one-way-source-baseline-{baselineGate}";
        await context.ArmExternalTransportDeliveryAsync(
            baselineGate,
            baselineMarker);
        var baseline = context.SendFromNodeAsync(
            caller,
            actorId,
            new HandoffPacket(scenario, baselineMarker));
        await context.WaitExternalTransportDeliveryAsync(baselineGate);
        await context.ReleaseExternalTransportDeliveryAsync(baselineGate);
        await baseline;
        await context.WaitEvidenceAsync(
            source,
            [$"{scenario}|{actorId}|handoff_packet|{baselineMarker}"]);

        var oneWayGate = Guid.NewGuid().ToString("N");
        var requestGate = Guid.NewGuid().ToString("N");
        var replyGate = Guid.NewGuid().ToString("N");
        var oneWayMarker = $"actor-one-way-follow-{oneWayGate}";
        var requestMarker = $"actor-request-follow-{requestGate}";
        var replyMarker = $"actor-request-reply-{replyGate}";
        Console.WriteLine(
            "message_follow_case_evidence case=MF-AO-FOLLOW phase=started");
        Console.WriteLine(
            "message_follow_case_evidence case=MF-AR-FOLLOW phase=started");
        await context.ArmExternalTransportDeliveryAsync(
            oneWayGate,
            oneWayMarker);
        await context.ArmExternalTransportDeliveryAsync(
            requestGate,
            requestMarker);
        await context.ArmExternalTransportDeliveryAsync(
            replyGate,
            string.Empty,
            afterGateId: requestGate);

        var oneWay = context.SendFromNodeAsync(
            caller,
            actorId,
            new HandoffPacket(scenario, oneWayMarker));
        var request = context.ProbeFromNodeAsync(
            caller,
            actorId,
            new ProbeReq(scenario, requestMarker, replyMarker),
            TimeSpan.FromSeconds(10));
        await context.WaitExternalTransportDeliveryAsync(oneWayGate);
        await context.WaitExternalTransportDeliveryAsync(requestGate);

        ZlinkStreamAssert.Ensure(
            (await context.JoinAsync(
                source,
                actorId,
                new JoinTargetReq(scenario, spotId))).Accepted,
            $"{scenario} relocation was rejected.");
        await context.WaitEvidenceAsync(
            target,
            [$"{scenario}|{actorId}|success_reply|{spotId}"]);
        _ = await context.WaitActorOwnerAsync(
            target,
            actorId,
            targetSpot.NodeRid);

        await context.ReleaseExternalTransportDeliveryAsync(oneWayGate);
        await context.ReleaseExternalTransportDeliveryAsync(requestGate);
        await oneWay;
        await context.WaitExternalTransportDeliveryAsync(replyGate);
        await Task.Delay(TimeSpan.FromMilliseconds(200));
        ZlinkStreamAssert.Ensure(
            !request.IsCompleted,
            $"{scenario} request completed while its TCP reply was held.");
        await context.ReleaseExternalTransportDeliveryAsync(replyGate);
        var result = await request;
        ZlinkStreamAssert.Ensure(
            result.Succeeded && result.Reply is not null,
            $"{scenario} followed request failed: {result.ErrorKind}.");
        var reply = result.Reply!;
        ZlinkStreamAssert.Ensure(
            SpotActorTransferScenarioContext.IsNode(
                reply.NodeRid,
                targetPrefix),
            $"{scenario} request reached unexpected owner '{reply.NodeRid}'.");
        ZlinkStreamAssert.Ensure(
            reply.StateVersion == 404,
            $"{scenario} request lost restored Actor state.");
        ZlinkStreamAssert.Ensure(
            reply.Marker == replyMarker,
            $"{scenario} request reply marker changed.");

        var targetEvidence = await context.WaitEvidenceAsync(
            target,
            [
                $"{scenario}|{actorId}|handoff_packet|{oneWayMarker}",
                $"{scenario}|{actorId}|packet_handler|{requestMarker}"
            ]);
        ZlinkStreamAssert.Ensure(
            targetEvidence.Count(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "handoff_packet"
                && item.Value == oneWayMarker) == 1,
            $"{scenario} one-way was not handled exactly once.");
        ZlinkStreamAssert.Ensure(
            targetEvidence.Count(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "packet_handler"
                && item.Value == requestMarker) == 1,
            $"{scenario} request was not handled exactly once.");
        ZlinkStreamAssert.Ensure(
            !(await context.GetEvidenceAsync(source)).Any(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && (item.Kind == "handoff_packet"
                    || item.Kind == "packet_handler")
                && (item.Value == oneWayMarker
                    || item.Value == requestMarker)),
            $"{scenario} source application handler processed followed work.");
        foreach (var gate in new[] { oneWayGate, requestGate, replyGate })
        {
            ZlinkStreamAssert.Ensure(
                (await context.GetExternalTransportDeliveryAsync(gate))
                    .ReleasedCount == 1,
                $"{scenario} external gate '{gate}' was not released once.");
        }
        Console.WriteLine(
            "message_follow_case_evidence case=MF-AO-FOLLOW"
            + " phase=completed terminal_count=1"
            + " current_owner_handler_count=1"
            + " previous_owner_handler_count=0");
        Console.WriteLine(
            "message_follow_case_evidence case=MF-AR-FOLLOW"
            + " phase=completed terminal_count=1"
            + " current_owner_handler_count=1"
            + " previous_owner_handler_count=0"
            + " reply_correlation=preserved");
    }
}
