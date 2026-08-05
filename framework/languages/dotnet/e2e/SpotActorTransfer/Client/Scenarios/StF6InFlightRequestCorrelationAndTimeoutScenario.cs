// Verifies ST-F6 In Flight Request Correlation And Timeout behavior.
using SpotActorTransfer.Client.Support;
using SpotActorTransfer.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace SpotActorTransfer.Client.Scenarios;

internal static class StF6InFlightRequestCorrelationAndTimeoutScenario
{
    public static async Task RunAsync(SpotActorTransferScenarioContext context)
    {
        await RunInFlightRequestCorrelationAsync(context);
        await RunInFlightRequestTimeoutAsync(context);
    }

    private static async Task RunInFlightRequestCorrelationAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-inflight-req-{Guid.NewGuid():N}";
        var spotId = $"spot-inflight-req-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeB, spotId, "delay-joined");
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeStateful, 106);
        var joinTask = context.JoinAsync(context.NodeA, actorId, new JoinTargetReq("ST-F6", spotId));
        await context.WaitEvidenceAsync(context.NodeB, [$"ST-F6|{actorId}|joined_wait|{spotId}"]);
        var requestTask = context.ProbeFromNodeAsync(
            context.NodeA,
            actorId,
            new ProbeReq("ST-F6", "correlated-reply"),
            TimeSpan.FromSeconds(5));
        await context.WaitRuntimeEvidenceAsync(context.NodeB,
            $"handoff_backlog actor={actorId} arrival=0");
        await context.ReleaseJoinedGateAsync(context.NodeB, spotId);

        ZlinkStreamAssert.Ensure((await joinTask).Accepted, "ST-F6 correlation transfer was rejected.");
        var response = await requestTask;
        ZlinkStreamAssert.Ensure(response.Succeeded
            && response.Reply is { } reply
            && SpotActorTransferScenarioContext.IsNode(reply.NodeRid, "actor-b"),
            $"ST-F6 reply did not correlate to the original caller: {response.ErrorKind}");
        ZlinkStreamAssert.Ensure(response.Reply?.Marker == "correlated-reply", "ST-F6 correlated reply marker mismatch.");
        await context.WaitEvidenceAsync(context.NodeB, [$"ST-F6|{actorId}|packet_handler|correlated-reply"]);
    }

    private static async Task RunInFlightRequestTimeoutAsync(SpotActorTransferScenarioContext context)
    {
        var actorId = $"actor-inflight-req-timeout-{Guid.NewGuid():N}";
        var spotId = $"spot-inflight-req-timeout-{Guid.NewGuid():N}";
        await context.CreateSpotAsync(context.NodeB, spotId, "delay-joined");
        await context.CreateActorAsync(context.NodeA, actorId, SpotActorTransferNames.ActorTypeStateful, 107);
        var joinTask = context.JoinAsync(context.NodeA, actorId, new JoinTargetReq("ST-F6", spotId));
        await context.WaitEvidenceAsync(context.NodeB, [$"ST-F6|{actorId}|joined_wait|{spotId}"]);
        var requestTask = context.ProbeFromNodeAsync(
            context.NodeA,
            actorId,
            new ProbeReq("ST-F6", "late-reply"),
            TimeSpan.FromMilliseconds(250));
        var timeout = await requestTask;
        ZlinkStreamAssert.Ensure(
            !timeout.Succeeded
            && timeout.ErrorKind == nameof(ZLinkFrameworkErrorKind.DeadlineExceeded),
            $"ST-F6 expected DeadlineExceeded, got '{timeout.ErrorKind}'.");

        await context.ReleaseJoinedGateAsync(context.NodeB, spotId);
        ZlinkStreamAssert.Ensure((await joinTask).Accepted, "ST-F6 timeout transfer was rejected.");
        await context.WaitEvidenceAsync(context.NodeB, [
            $"ST-F6|{actorId}|packet_handler|late-reply",
            $"ST-F6|{actorId}|late_reply_created|late-reply"
        ]);
    }
}
