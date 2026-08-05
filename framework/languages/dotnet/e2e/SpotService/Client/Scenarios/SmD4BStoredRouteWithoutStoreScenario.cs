// Verifies SM-D4B stored route dispatch without Location Store reads.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmD4BStoredRouteWithoutStoreScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient gateway,
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        ZLinkHttpClient sessionAHttp,
        string sessionAStreamEndpoint,
        string sessionBStreamEndpoint,
        string[] transportProxyAdmins)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var actorId = $"actor-sm-d4b-{suffix}";
        var sessionACompanionId = $"actor-sm-d4b-a-companion-{suffix}";
        var sessionBCompanionId = $"actor-sm-d4b-b-companion-{suffix}";

        try
        {
            await SetPlacementWeightsAsync(playA, playB, 100, 0);
            await Task.Delay(TimeSpan.FromSeconds(2));
            await using var sessionA = CreateConnector(sessionAStreamEndpoint);
            await using var sessionB = CreateConnector(sessionBStreamEndpoint);
            await sessionA.Connect.Async();
            await sessionB.Connect.Async();

            await BindPairAsync(sessionA, actorId, sessionACompanionId);
            await ConfigureProbeAsync(
                sessionAHttp,
                [actorId, sessionACompanionId],
                blocked: true);

            var valid = await PingAsync(sessionA, actorId, "stored-route");
            ZlinkStreamAssert.Ensure(
                valid.ActorId == actorId
                && (valid.NodeRid.StartsWith("play-a-", StringComparison.Ordinal)
                    || valid.NodeRid.StartsWith("play-b-", StringComparison.Ordinal)),
                "SM-D4B valid stored route selected a different Actor.");

            var pushed = sessionA.WaitFor<ActorPushNotify>()
                .Where(message => message.Payload.ActorId == actorId)
                .Async()
                .AsTask();
            var pushReply = await sessionA.Request(new ActorPushReq("stored-push"))
                .PacketName("ActorPushReq")
                .Metadata(SpotServiceNames.ActorIdMetadata, actorId)
                .Async<ActorPingRes>();
            var push = await pushed;
            ZlinkStreamAssert.Ensure(
                pushReply.ActorId == actorId
                && push.Payload.ActorId == actorId
                && push.Payload.Value == "stored-push",
                "SM-D4B bound push did not use the stored Actor route.");

            _ = await PingAsync(
                sessionA,
                sessionACompanionId,
                "companion-before-rebind");
            await AssertNoMatchingReadsAsync(sessionAHttp, "valid stored route");

            await VerifyStoreBlockedMessageFollowAsync(
                gateway,
                playA,
                playB,
                sessionAHttp,
                sessionA,
                actorId,
                sessionACompanionId,
                suffix,
                valid.NodeRid,
                transportProxyAdmins);

            await ConfigureProbeAsync(sessionAHttp, [], blocked: false);
            await BindPairAsync(sessionB, actorId, sessionBCompanionId);
            await ConfigureProbeAsync(
                sessionAHttp,
                [actorId, sessionACompanionId],
                blocked: true);

            var stale = await sessionA
                .Request(new StaleBindingProbeReq(actorId, "stale-route"))
                .PacketName("StaleBindingProbeReq")
                .Async<StaleBindingProbeRes>();
            ZlinkStreamAssert.Ensure(
                stale.ActorId == actorId
                && stale.RelayRejected
                && stale.DisconnectCompleted
                && string.Equals(
                    stale.ErrorKind,
                    ZLinkFrameworkErrorKind.InvalidOperation.ToString(),
                    StringComparison.Ordinal),
                "SM-D4B stale binding did not terminate with the typed stale result.");

            var companion = await PingAsync(
                sessionA,
                sessionACompanionId,
                "companion-after-rebind",
                packetName: "UserActorPingReq");
            ZlinkStreamAssert.Ensure(
                companion.ActorId == sessionACompanionId,
                "SM-D4B stale Actor handling changed the companion binding.");
            await AssertNoMatchingReadsAsync(sessionAHttp, "stale stored route");

            Console.WriteLine("operation SpotService.sm-d4b passed");
        }
        finally
        {
            await ConfigureProbeAsync(sessionAHttp, [], blocked: false);
            await SetPlacementWeightsAsync(playA, playB, 100, 100);
        }
    }

    private static async Task VerifyStoreBlockedMessageFollowAsync(
        ZLinkHttpClient gateway,
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        ZLinkHttpClient sessionAHttp,
        IZlinkStreamConnector session,
        string actorId,
        string expiredActorId,
        string suffix,
        string sourceNodeRid,
        IReadOnlyCollection<string> transportProxyAdmins)
    {
        var activeGate = $"sm-d4b-active-{suffix}";
        var expiredGate = $"sm-d4b-expired-{suffix}";
        var activeMarker = $"active-message-follow-{suffix}";
        var expiredMarker = $"expired-message-follow-{suffix}";
        var sourceIsPlayA = sourceNodeRid.StartsWith(
            "play-a-",
            StringComparison.Ordinal);
        await SetPlacementWeightsAsync(
            playA,
            playB,
            sourceIsPlayA ? 0 : 100,
            sourceIsPlayA ? 100 : 0);
        await Task.Delay(TimeSpan.FromSeconds(2));
        var targetSpotId = $"spot-sm-d4b-message-follow-{suffix}";
        var targetSpot = (await gateway.Post("/spot/get-or-create")
            .Body(new CreateSpotReq(targetSpotId))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(
            targetSpot.NodeRid.StartsWith(
                sourceIsPlayA ? "play-b-" : "play-a-",
                StringComparison.Ordinal),
            "SM-D4B Message Follow target Spot was not placed on the other owner.");
        var targetOwner = sourceIsPlayA ? playB : playA;
        var targetOwnerName = sourceIsPlayA ? "play-b" : "play-a";

        // Resolve both routes before blocking the Store. The Session runtime
        // retains each positive route for 15 seconds, so the calls below use
        // the exact old owner after relocation without a transport test hook.
        await ConfigureProbeAsync(
            sessionAHttp,
            [actorId, expiredActorId],
            blocked: false);
        foreach (var (prewarmActorId, marker) in new[]
                 {
                     (actorId, $"prewarm-{activeMarker}"),
                     (expiredActorId, $"prewarm-{expiredMarker}")
                 })
        {
            var prewarm = (await sessionAHttp.Post("/actor/request")
                .Body(new ActorRequestReq(
                    prewarmActorId,
                    marker,
                    DelayMilliseconds: 0,
                    TimeoutMilliseconds: 20000))
                .Async<ActorRequestRes>()).Body;
            ZlinkStreamAssert.Ensure(
                prewarm.Succeeded
                && prewarm.Reply?.NodeRid == sourceNodeRid,
                $"SM-D4B failed to cache the old route for '{prewarmActorId}'.");
        }
        await ConfigureProbeAsync(
            sessionAHttp,
            [actorId, expiredActorId],
            blocked: true);

        foreach (var relocatedActorId in new[] { actorId, expiredActorId })
        {
            var join = (await gateway.Post("/actor/join-spot")
                .Body(new JoinUserSpotActorReq(
                    targetSpotId,
                    relocatedActorId))
                .Async<JoinUserSpotActorRes>()).Body;
            ZlinkStreamAssert.Ensure(
                join.Accepted && join.SpotRid == targetSpotId,
                $"SM-D4B Actor '{relocatedActorId}' relocation was not accepted.");
        }
        await targetOwner.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([
                $"spot-actor-admitted|rid={targetOwnerName}|spot={targetSpotId}|actor={actorId}",
                $"spot-actor-admitted|rid={targetOwnerName}|spot={targetSpotId}|actor={expiredActorId}"
            ]))
            .Async<string[]>();

        var activeRequest = await sessionAHttp.Post("/actor/request")
            .Body(new ActorRequestReq(
                actorId,
                activeMarker,
                DelayMilliseconds: 0,
                TimeoutMilliseconds: 20000))
            .Async<ActorRequestRes>();
        var followed = activeRequest.Body;
        ZlinkStreamAssert.Ensure(
            followed.Succeeded
            && followed.Reply?.ActorId == actorId
            && followed.Reply.NodeRid.StartsWith(
                sourceIsPlayA ? "play-b-" : "play-a-",
                StringComparison.Ordinal)
            && followed.Reply.SpotRid == targetSpotId,
            "SM-D4B active Message Follow did not reach the relocated Actor.");
        var storedAfterRelocation = await PingAsync(
            session,
            actorId,
            "stored-route-after-relocation",
            packetName: "UserActorPingReq");
        ZlinkStreamAssert.Ensure(
            storedAfterRelocation.NodeRid.StartsWith(
                sourceIsPlayA ? "play-b-" : "play-a-",
                StringComparison.Ordinal),
            "SM-D4B stored Session route did not commit to the target owner.");
        await AssertNoMatchingReadsAsync(
            sessionAHttp,
            "active Message Follow");

        // The runner sets a seven-second Message Follow duration for this
        // selector. The second Actor still has its independently cached old
        // route when Message Follow expires.
        await Task.Delay(TimeSpan.FromSeconds(8));
        var expired = (await sessionAHttp.Post("/actor/request")
            .Body(new ActorRequestReq(
                expiredActorId,
                expiredMarker,
                DelayMilliseconds: 0,
                TimeoutMilliseconds: 20000))
            .Async<ActorRequestRes>()).Body;
        ZlinkStreamAssert.Ensure(
            !expired.Succeeded
            && string.Equals(
                expired.ErrorKind,
                ZLinkFrameworkErrorKind.Unavailable.ToString(),
                StringComparison.Ordinal),
            $"SM-D4B expired Message Follow returned '{expired.ErrorKind}'.");
        await AssertNoMatchingReadsAsync(
            sessionAHttp,
            "expired Message Follow");

        var targetEvidence = (await targetOwner.Get("/evidence")
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            targetEvidence.All(entry =>
                !entry.Contains(expiredMarker, StringComparison.Ordinal)),
            "SM-D4B expired Message Follow reached the target Actor handler.");
    }

    private static async Task ConfigureProbeAsync(
        ZLinkHttpClient client,
        string[] actorIds,
        bool blocked)
    {
        var snapshot = (await client
                .Post("/location-store/read-probe/configure")
                .Body(new LocationStoreReadProbeReq(actorIds, blocked))
                .Async<LocationStoreReadProbeSnapshot>())
            .Body;
        ZlinkStreamAssert.Ensure(
            snapshot.Blocked == blocked
            && snapshot.MatchingReads == 0,
            "SM-D4B Location Store probe did not reset at the requested boundary.");
    }

    private static async Task AssertNoMatchingReadsAsync(
        ZLinkHttpClient client,
        string phase)
    {
        var snapshot = (await client
                .Get("/location-store/read-probe")
                .Async<LocationStoreReadProbeSnapshot>())
            .Body;
        ZlinkStreamAssert.Ensure(
            snapshot.Blocked && snapshot.MatchingReads == 0,
            $"SM-D4B observed {snapshot.MatchingReads} Actor authority Store reads during {phase}.");
    }

    private static async Task BindPairAsync(
        IZlinkStreamConnector session,
        string actorId,
        string companionId)
    {
        var result = await session
            .Request(new MultiBindReq(actorId, companionId))
            .PacketName("MultiBindReq")
            .Async<MultiBindRes>();
        ZlinkStreamAssert.Ensure(
            result.BoundCount == 2,
            "SM-D4B expected two current bindings.");
    }

    private static async Task<ActorPingRes> PingAsync(
        IZlinkStreamConnector session,
        string actorId,
        string value,
        string packetName = "ActorPingReq") =>
        await session.Request(new ActorPingReq(value))
            .PacketName(packetName)
            .Metadata(SpotServiceNames.ActorIdMetadata, actorId)
            .Async<ActorPingRes>();

    private static async Task SetPlacementWeightsAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        int playAWeight,
        int playBWeight)
    {
        await playA.Post("/placement-weight")
            .Body(new PlacementWeightReq(playAWeight))
            .Async<PlacementWeightRes>();
        await playB.Post("/placement-weight")
            .Body(new PlacementWeightReq(playBWeight))
            .Async<PlacementWeightRes>();
    }

    private static async Task ArmGateAsync(
        IEnumerable<string> admins,
        string gateId,
        string marker)
    {
        foreach (var admin in admins)
        {
            using var client = CreateAdminClient(admin);
            await client.Post("/arm")
                .Body(new TransportGateArm(gateId, marker))
                .AsyncRaw();
        }
    }

    private static async Task WaitCapturedAsync(
        IReadOnlyCollection<string> admins,
        string gateId)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        do
        {
            var snapshot = await SnapshotGateAsync(admins, gateId);
            if (snapshot.CapturedCount == 1)
                return;
            ZlinkStreamAssert.Ensure(
                snapshot.CapturedCount == 0,
                $"SM-D4B gate '{gateId}' captured more than one delivery.");
            await Task.Delay(25);
        } while (DateTimeOffset.UtcNow < deadline);

        throw new TimeoutException(
            $"SM-D4B gate '{gateId}' did not capture a delivery.");
    }

    private static async Task ReleaseGateAsync(
        IEnumerable<string> admins,
        string gateId)
    {
        foreach (var admin in admins)
        {
            using var client = CreateAdminClient(admin);
            await client.Post("/release")
                .Query("gateId", gateId)
                .AsyncRaw();
        }
    }

    private static async Task<TransportGateSnapshot> SnapshotGateAsync(
        IEnumerable<string> admins,
        string gateId)
    {
        var captured = 0;
        var released = 0;
        foreach (var admin in admins)
        {
            using var client = CreateAdminClient(admin);
            var snapshot = (await client.Get("/snapshot")
                    .Query("gateId", gateId)
                    .Async<TransportGateSnapshot>())
                .Body;
            captured += snapshot.CapturedCount;
            released += snapshot.ReleasedCount;
        }
        return new TransportGateSnapshot(gateId, captured, released);
    }

    private static ZLinkHttpClient CreateAdminClient(string endpoint) =>
        ZLinkHttpClient.Create(endpoint)
            .Timeout(TimeSpan.FromSeconds(10))
            .Build();

    private static IZlinkStreamConnector CreateConnector(string endpoint) =>
        ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(20),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });

    private sealed record TransportGateArm(string GateId, string Marker);

    private sealed record TransportGateSnapshot(
        string GateId,
        int CapturedCount,
        int ReleasedCount);
}
