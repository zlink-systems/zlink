// Verifies SM-G1 crash isolation and both explicit application recovery paths.
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmG1BoundActorCrashRecoveryScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playAHttp,
        ZLinkHttpClient playBHttp,
        ZLinkHttpClient gateway,
        string sessionAStreamEndpoint,
        string sessionBStreamEndpoint)
    {
        const string crashedActorId = "actor-sm-g1-crash";
        const string survivorActorId = "actor-sm-g1-survivor";
        await using var ownerSession = await ConnectAsync(sessionAStreamEndpoint);
        await using var survivorSession = await ConnectAsync(sessionBStreamEndpoint);
        await SetPlacementWeightAsync(playAHttp, 100);
        await SetPlacementWeightAsync(playBHttp, 0);
        await WaitForPlacementAsync(gateway, "play-a");
        await ownerSession.Request(new AuthReq(crashedActorId, "snapshot-v1"))
            .PacketName("AuthReq").Async<AuthRes>();
        await SetPlacementWeightAsync(playAHttp, 0);
        await SetPlacementWeightAsync(playBHttp, 100);
        await WaitForPlacementAsync(gateway, "play-b");
        await survivorSession.Request(new AuthReq(survivorActorId, "survivor"))
            .PacketName("AuthReq").Async<AuthRes>();
        await SetPlacementWeightAsync(playAHttp, 100);
        await SetPlacementWeightAsync(playBHttp, 0);

        var oldRef = await CaptureRefAsync(gateway, crashedActorId);
        var inFlight = gateway.Post("/actor/request")
            .Body(new ActorRequestReq(oldRef.ActorId, "in-flight-1", 10000, 3000))
            .Async<ActorRequestRes>().AsTask();
        await playAHttp.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([
                $"actor-slow-ping-started|rid=play-a|actor={crashedActorId}|value=in-flight-1"
            ])).AsyncRaw();
        Console.WriteLine("spot-service sm-g1 crash-1-ready");

        var inFlightOutcome = (await inFlight).Body;
        EnsureInFlightFailure(inFlightOutcome, "first crash");
        await EnsureSurvivorAsync(
            survivorSession,
            survivorActorId,
            "during-first-crash");
        await WaitMissingAsync(gateway, crashedActorId);
        await ZlinkStreamAssert.ExpectFailureAsync(async cancellationToken =>
        {
            _ = await ownerSession.Request(new ActorPingReq("crashed-session"))
                .PacketName("ActorPingReq")
                .Async<ActorPingRes>(cancellationToken);
        });
        await EnsureRequestErrorAsync(
            gateway,
            crashedActorId,
            "before-restart",
            "NotFound");
        await EnsureSurvivorAsync(survivorSession, survivorActorId, "after-first-crash");

        Console.WriteLine("spot-service sm-g1 restart-1-ready");
        await gateway.Post("/node/wait-ready")
            .Body(new NodeReadinessWaitReq("play-a", 15000)).Async<NodeReadinessWaitRes>();
        await WaitForPlacementAsync(gateway, "play-a");
        var restarted = (await gateway.Post("/actor/get-or-create")
            .Body(new EnsureActorReq(crashedActorId, "snapshot-v1"))
            .Async<EnsureActorRes>()).Body;
        ZlinkStreamAssert.Ensure(IsNode(restarted.NodeRid, "play-a"),
            "SM-G1 replacement placement did not select the only positive-weight node.");
        var restartedRef = await CaptureRefAsync(gateway, crashedActorId);
        ZlinkStreamAssert.Ensure(restartedRef.Generation != oldRef.Generation,
            "SM-G1 same-rid restart reused the previous actor generation.");
        await EnsureStaleRefAsync(gateway, oldRef);
        await EnsureSuccessAsync(gateway, restartedRef, "restart-follow-up", "play-a");
        await using var restartedSession = await ConnectAsync(sessionAStreamEndpoint);
        var restartedAuth = await restartedSession
            .Request(new AuthReq(crashedActorId, "snapshot-v1"))
            .PacketName("AuthReq")
            .Async<AuthRes>();
        ZlinkStreamAssert.Ensure(
            IsNode(restartedAuth.NodeRid, "play-a"),
            "SM-G1 explicit rebind did not select the restarted play-a owner.");
        var restartedSessionReply = await restartedSession
            .Request(new ActorPingReq("restart-rebound"))
            .PacketName("ActorPingReq")
            .Async<ActorPingRes>();
        ZlinkStreamAssert.Ensure(
            IsNode(restartedSessionReply.NodeRid, "play-a"),
            "SM-G1 explicit rebind did not restore messaging on restarted play-a.");

        var secondInFlight = gateway.Post("/actor/request")
            .Body(new ActorRequestReq(
                restartedRef.ActorId,
                "in-flight-2",
                10000,
                3000))
            .Async<ActorRequestRes>().AsTask();
        await playAHttp.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([
                $"actor-slow-ping-started|rid=play-a|actor={crashedActorId}|value=in-flight-2"
            ])).AsyncRaw();
        Console.WriteLine("spot-service sm-g1 crash-2-ready");
        EnsureInFlightFailure((await secondInFlight).Body, "second crash");
        await EnsureSurvivorAsync(
            survivorSession,
            survivorActorId,
            "during-second-crash");
        await WaitMissingAsync(gateway, crashedActorId);
        await EnsureRequestErrorAsync(
            gateway,
            crashedActorId,
            "before-play-b-recovery",
            "NotFound");

        await SetPlacementWeightAsync(playBHttp, 100);
        await gateway.Post("/node/wait-ready")
            .Body(new NodeReadinessWaitReq("play-b", 15000)).Async<NodeReadinessWaitRes>();
        await WaitForPlacementAsync(gateway, "play-b");
        var recovered = (await gateway.Post("/actor/get-or-create")
            .Body(new EnsureActorReq(crashedActorId, "snapshot-v1"))
            .Async<EnsureActorRes>()).Body;
        ZlinkStreamAssert.Ensure(IsNode(recovered.NodeRid, "play-b"),
            "SM-G1 recovery placement did not select the only positive-weight node.");
        var recoveredRef = await CaptureRefAsync(gateway, crashedActorId);
        ZlinkStreamAssert.Ensure(IsNode(recoveredRef.NodeRid, "play-b"),
            "SM-G1 application recovery did not place the actor on play-b.");
        await EnsureStaleRefAsync(gateway, restartedRef);
        await EnsureSuccessAsync(gateway, recoveredRef, "play-b-follow-up", "play-b");
        await using var rebound = await ConnectAsync(sessionBStreamEndpoint);
        await rebound.Request(new AuthReq(crashedActorId, "snapshot-v1"))
            .PacketName("AuthReq").Async<AuthRes>();
        var reboundReply = await rebound.Request(new ActorPingReq("rebound"))
            .PacketName("ActorPingReq").Async<ActorPingRes>();
        ZlinkStreamAssert.Ensure(IsNode(reboundReply.NodeRid, "play-b"),
            "SM-G1 explicit rebind did not restore messaging on play-b.");
        await EnsureSurvivorAsync(survivorSession, survivorActorId, "after-second-crash");
        Console.WriteLine("operation SpotService.sm-g1 passed");
    }

    private static async Task<IZlinkStreamConnector> ConnectAsync(string endpoint)
    {
        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint),
            RequestTimeout = TimeSpan.FromSeconds(10),
            Heartbeat = new ZlinkStreamHeartbeatOptions
            {
                // Heartbeat detects physical loss. Mid-crash ActorPing requests
                // above keep the application session active during lease expiry.
                Enabled = true,
                Interval = TimeSpan.FromSeconds(1),
                Timeout = TimeSpan.FromSeconds(5)
            }
        });
        await connector.Connect.Async();
        return connector;
    }

    private static async Task SetPlacementWeightAsync(
        ZLinkHttpClient client,
        int weight) =>
        await client.Post("/placement-weight")
            .Body(new PlacementWeightReq(weight))
            .Async<PlacementWeightRes>();

    private static async Task WaitForPlacementAsync(
        ZLinkHttpClient gateway,
        string expectedNodePrefix)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(15);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var actorId = $"actor-sm-g1-placement-probe-{Guid.NewGuid():N}";
            var placed = (await gateway.Post("/actor/get-or-create")
                .Body(new EnsureActorReq(actorId, "placement-probe"))
                .Async<EnsureActorRes>()).Body;
            if (IsNode(placed.NodeRid, expectedNodePrefix)) return;
            await Task.Delay(TimeSpan.FromMilliseconds(100));
        }

        throw new TimeoutException(
            $"SM-G1 placement did not converge to '{expectedNodePrefix}'.");
    }

    private static async Task<ActorRefRes> CaptureRefAsync(
        ZLinkHttpClient gateway, string actorId) =>
        (await gateway.Post("/actor/capture-ref")
            .Body(new ActorRefReq(actorId)).Async<ActorRefRes>()).Body;

    private static async Task WaitMissingAsync(
        ZLinkHttpClient gateway,
        string actorId)
    {
        var response = await gateway.Post("/actor/wait-missing")
            .Body(new ActorMissingWaitReq(actorId, 15000))
            .AsyncRaw();
        ZlinkStreamAssert.Ensure(
            response.Status is >= 200 and < 300,
            $"SM-G1 Actor route '{actorId}' did not disappear after owner lease expiry; "
            + $"HTTP status={response.Status}.");
    }

    private static async Task EnsureRequestErrorAsync(
        ZLinkHttpClient gateway,
        string actorId,
        string value,
        string expected)
    {
        var outcome = (await gateway.Post("/actor/request")
            .Body(new ActorRequestReq(actorId, value))
            .Async<ActorRequestRes>()).Body;
        ZlinkStreamAssert.Ensure(!outcome.Succeeded && outcome.ErrorKind == expected,
            $"SM-G1 expected {expected}, got success={outcome.Succeeded} kind={outcome.ErrorKind}.");
    }

    private static async Task EnsureSuccessAsync(
        ZLinkHttpClient gateway,
        ActorRefRes actor,
        string value,
        string nodeRid)
    {
        var outcome = (await gateway.Post("/actor/request")
            .Body(new ActorRequestReq(actor.ActorId, value))
            .Async<ActorRequestRes>()).Body;
        ZlinkStreamAssert.Ensure(
            outcome.Succeeded
            && outcome.Reply is { } reply
            && IsNode(reply.NodeRid, nodeRid),
            $"SM-G1 live ActorRef follow-up failed: {outcome.ErrorKind}.");
    }

    private static async Task EnsureStaleRefAsync(
        ZLinkHttpClient gateway,
        ActorRefRes actor)
    {
        var outcome = (await gateway.Post("/actor/destroy-ref")
            .Body(new ActorRefDestroyReq(actor))
            .Async<ActorRefDestroyRes>()).Body;
        ZlinkStreamAssert.Ensure(
            !outcome.Succeeded
            && !outcome.Destroyed
            && outcome.ErrorKind == "InvalidOperation",
            $"SM-G1 stale ActorRef lifecycle operation returned "
            + $"success={outcome.Succeeded} destroyed={outcome.Destroyed} "
            + $"kind={outcome.ErrorKind}.");
    }

    private static void EnsureInFlightFailure(ActorRequestRes outcome, string phase) =>
        ZlinkStreamAssert.Ensure(!outcome.Succeeded
                                 && outcome.ErrorKind is "Unavailable" or "Timeout",
            $"SM-G1 {phase} in-flight outcome was {outcome.ErrorKind}.");

    private static async Task EnsureSurvivorAsync(
        IZlinkStreamConnector connector, string actorId, string marker)
    {
        var reply = await connector.Request(new ActorPingReq(marker))
            .PacketName("ActorPingReq").Async<ActorPingRes>();
        ZlinkStreamAssert.Ensure(
            reply.ActorId == actorId && IsNode(reply.NodeRid, "play-b"),
            "SM-G1 survivor actor/session was affected by play-a crash.");
    }

    private static bool IsNode(string actualRid, string expectedPrefix) =>
        string.Equals(actualRid, expectedPrefix, StringComparison.Ordinal)
        || actualRid.StartsWith($"{expectedPrefix}-", StringComparison.Ordinal);

}
