// Verifies Message Follow correlation, deadlines, and expiry through TCP.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StI5MessageFollowSafetyScenario
{
    public static async Task RunAsync(
        SpotActorTransferScenarioContext context)
    {
        const string scenario = "ST-I5";
        var actorId = $"actor-message-follow-safety-{Guid.NewGuid():N}";
        var spotId = $"spot-message-follow-safety-{Guid.NewGuid():N}";
        var created = await context.CreateActorAsync(
            context.NodeA,
            actorId,
            SpotActorTransferNames.ActorTypeStateful,
            stateVersion: 505,
            applicationStateBytes: 4 * 1024);
        var source = context.NodeForRid(created.NodeRid);
        var (target, targetPrefix) =
            context.OtherActorNode(created.NodeRid);
        var firstCaller = context.ThirdActorNode(source, target);
        var secondCaller = context.NodeD;
        var targetSpot = await context.CreateSpotAsync(target, spotId);

        var correlationAGate = Guid.NewGuid().ToString("N");
        var correlationBGate = Guid.NewGuid().ToString("N");
        var deadlineGate = Guid.NewGuid().ToString("N");
        var expiredGate = Guid.NewGuid().ToString("N");
        var replyDeadlineGate = Guid.NewGuid().ToString("N");
        var heldReplyGate = Guid.NewGuid().ToString("N");
        var correlationA = $"correlation-a-{correlationAGate}";
        var correlationB = $"correlation-b-{correlationBGate}";
        var correlationAReply = $"correlation-a-reply-{correlationAGate}";
        var correlationBReply = $"correlation-b-reply-{correlationBGate}";
        var deadlineMarker = $"deadline-{deadlineGate}";
        var expiredMarker = $"expired-{expiredGate}";
        var replyDeadlineMarker =
            $"reply-backpressure-deadline-{replyDeadlineGate}";
        var heldReplyMarker = $"held-reply-{heldReplyGate}";
        Console.WriteLine(
            "message_follow_case_evidence case=MF-CORR phase=started");

        var first = await StartHeldRequestAsync(
            context,
            firstCaller,
            actorId,
            scenario,
            correlationA,
            correlationAReply,
            correlationAGate,
            TimeSpan.FromSeconds(15));
        var deadline = await StartHeldRequestAsync(
            context,
            firstCaller,
            actorId,
            scenario,
            deadlineMarker,
            null,
            deadlineGate,
            TimeSpan.FromSeconds(5));
        var expired = await StartHeldRequestAsync(
            context,
            firstCaller,
            actorId,
            scenario,
            expiredMarker,
            null,
            expiredGate,
            TimeSpan.FromSeconds(15));
        var second = await StartHeldRequestAsync(
            context,
            secondCaller,
            actorId,
            scenario,
            correlationB,
            correlationBReply,
            correlationBGate,
            TimeSpan.FromSeconds(15));
        await context.ArmExternalTransportDeliveryAsync(
            replyDeadlineGate,
            replyDeadlineMarker);
        await context.ArmExternalTransportDeliveryAsync(
            heldReplyGate,
            string.Empty,
            afterGateId: replyDeadlineGate);
        var replyDeadline = context.ProbeFromNodeAsync(
            secondCaller,
            actorId,
            new ProbeReq(
                scenario,
                replyDeadlineMarker,
                heldReplyMarker),
            TimeSpan.FromSeconds(2));
        await context.WaitExternalTransportDeliveryAsync(replyDeadlineGate);

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
        var messageFollowExpiry =
            DateTimeOffset.UtcNow + TimeSpan.FromSeconds(8);

        // The requests use two physical peer connections. Releasing B before
        // A proves correlation without relying on a production operation-ID
        // hook or attempting impossible reordering within one TCP stream.
        await context.ReleaseExternalTransportDeliveryAsync(correlationBGate);
        var secondReply = await second;
        await context.ReleaseExternalTransportDeliveryAsync(correlationAGate);
        var firstReply = await first;
        var replies = new[] { firstReply, secondReply };
        ZlinkStreamAssert.Ensure(
            replies[0].Succeeded
            && replies[1].Succeeded
            && replies[0].Reply?.Marker == correlationAReply
            && replies[1].Reply?.Marker == correlationBReply
            && replies.All(result =>
                result.Reply is not null
                && SpotActorTransferScenarioContext.IsNode(
                    result.Reply.NodeRid,
                    targetPrefix)),
            $"{scenario} correlation crossed between followed requests.");

        await context.ReleaseExternalTransportDeliveryAsync(
            replyDeadlineGate);
        await context.WaitExternalTransportDeliveryAsync(heldReplyGate);
        var heldReplyDeadlineResult = await replyDeadline;
        ZlinkStreamAssert.Ensure(
            !heldReplyDeadlineResult.Succeeded
            && heldReplyDeadlineResult.ErrorKind
                == nameof(TimeoutException),
            $"{scenario} held reply did not preserve the original deadline: "
            + heldReplyDeadlineResult.ErrorKind);
        await context.ReleaseExternalTransportDeliveryAsync(
            heldReplyGate,
            requireForwarded: false);
        await Task.Delay(TimeSpan.FromMilliseconds(250));
        ZlinkStreamAssert.Ensure(
            !heldReplyDeadlineResult.Succeeded
            && heldReplyDeadlineResult.ErrorKind
                == nameof(TimeoutException),
            $"{scenario} late TCP reply changed the timeout terminal.");

        await context.ReleaseExternalTransportDeliveryAsync(deadlineGate);
        var deadlineResult = await deadline;
        ZlinkStreamAssert.Ensure(
            !deadlineResult.Succeeded
            && deadlineResult.ErrorKind == nameof(TimeoutException),
            $"{scenario} followed request extended its deadline: "
            + deadlineResult.ErrorKind);
        var deadlineEvidence = await context.WaitEvidenceAsync(
            target,
            [$"{scenario}|{actorId}|packet_handler|{deadlineMarker}"]);
        ZlinkStreamAssert.Ensure(
            deadlineEvidence.Count(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "packet_handler"
                && item.Value == deadlineMarker) == 1,
            $"{scenario} deadline handler did not execute exactly once.");

        // Message Follow lasts seven seconds in this topology. Exceeding that
        // duration must reject the held stale route before application dispatch.
        var untilExpiry = messageFollowExpiry - DateTimeOffset.UtcNow;
        if (untilExpiry > TimeSpan.Zero)
            await Task.Delay(untilExpiry);
        await context.ReleaseExternalTransportDeliveryAsync(expiredGate);
        var expiredResult = await expired;
        ZlinkStreamAssert.Ensure(
            !expiredResult.Succeeded
            && expiredResult.ErrorKind == "Unavailable",
            $"{scenario} expired delivery was not unavailable: "
            + expiredResult.ErrorKind);

        var targetEvidence = await context.GetEvidenceAsync(target);
        foreach (var marker in new[]
                 {
                     correlationA,
                     correlationB,
                     replyDeadlineMarker,
                     deadlineMarker
                 })
        {
            ZlinkStreamAssert.Ensure(
                targetEvidence.Count(item =>
                    item.Scenario == scenario
                    && item.ActorId == actorId
                    && item.Kind == "packet_handler"
                    && item.Value == marker) == 1,
                $"{scenario} request '{marker}' was not handled once.");
        }
        ZlinkStreamAssert.Ensure(
            !targetEvidence.Any(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "packet_handler"
                && item.Value == expiredMarker),
            $"{scenario} expired request reached the target handler.");
        var sourceEvidence = await context.GetEvidenceAsync(source);
        ZlinkStreamAssert.Ensure(
            !sourceEvidence.Any(item =>
                item.Scenario == scenario
                && item.ActorId == actorId
                && item.Kind == "packet_handler"),
            $"{scenario} source handler processed followed work.");

        foreach (var gate in new[]
                 {
                     correlationAGate,
                     correlationBGate,
                     deadlineGate,
                     expiredGate,
                     replyDeadlineGate
                 })
        {
            ZlinkStreamAssert.Ensure(
                (await context.GetExternalTransportDeliveryAsync(gate))
                    .ReleasedCount == 1,
                $"{scenario} external gate '{gate}' was not released once.");
        }
        ZlinkStreamAssert.Ensure(
            (await context.GetExternalTransportDeliveryAsync(heldReplyGate))
                .Released,
            $"{scenario} held reply gate was not released after timeout.");
        Console.WriteLine(
            "message_follow_case_evidence case=MF-CORR"
            + " phase=completed terminal_count=2"
            + " current_owner_handler_count=2"
            + " previous_owner_handler_count=0"
            + " reply_correlation=preserved");

        // Duplicate, generation, loop, hop and bounded-capacity cases remain
        // separate gaps because a black-box TCP fixture must not synthesize
        // private route frames.
    }

    private static async Task<Task<NodeActorProbeRes>> StartHeldRequestAsync(
        SpotActorTransferScenarioContext context,
        ZLinkHttpClient caller,
        string actorId,
        string scenario,
        string marker,
        string? replyMarker,
        string gateId,
        TimeSpan timeout)
    {
        await context.ArmExternalTransportDeliveryAsync(
            gateId,
            marker);
        var request = context.ProbeFromNodeAsync(
            caller,
            actorId,
            new ProbeReq(scenario, marker, replyMarker),
            timeout);
        await context.WaitExternalTransportDeliveryAsync(gateId);
        return request;
    }
}
