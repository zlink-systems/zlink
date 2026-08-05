// Verifies SM-D5 physical disconnect all-settled fan-out and exact-binding dedupe.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmD5ExplicitDisconnectNotificationScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient gateway,
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        ZLinkHttpClient sessionA,
        string sessionAStreamEndpoint)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var failingLocalActorId = $"actor-sm-d5-fail-{suffix}";
        var racingRemoteActorId = $"actor-sm-d5-race-{suffix}";

        try
        {
            // Create one Actor on the Session host and one on a remote Play host.
            // Binding later resolves the already-published exact ActorRef values.
            await SetWeightsAsync(playA, playB, sessionA, 0, 0, 100);
            var localBefore = await EnsureActorAsync(
                gateway,
                failingLocalActorId);
            ZlinkStreamAssert.Ensure(
                localBefore.NodeRid.StartsWith("session-a-", StringComparison.Ordinal),
                $"SM-D5 expected local Actor on session-a, got '{localBefore.NodeRid}'.");

            await SetWeightsAsync(playA, playB, sessionA, 100, 0, 0);
            var remoteBefore = await EnsureActorAsync(
                gateway,
                racingRemoteActorId);
            ZlinkStreamAssert.Ensure(
                remoteBefore.NodeRid.StartsWith("play-a-", StringComparison.Ordinal),
                $"SM-D5 expected remote Actor on play-a, got '{remoteBefore.NodeRid}'.");

            await using var client = CreateConnector(sessionAStreamEndpoint);
            await client.Connect.Async();
            var bound = await client
                .Request(new MultiBindReq(failingLocalActorId, racingRemoteActorId))
                .PacketName("MultiBindReq")
                .Async<MultiBindRes>();
            ZlinkStreamAssert.Ensure(
                bound.BoundCount == 2,
                "SM-D5 expected the fixed disconnect snapshot to contain two Actors.");

            await ConfigureStoreProbeAsync(
                sessionA,
                [failingLocalActorId, racingRemoteActorId],
                blocked: true);

            // Hold the logical callback inside the Actor turn, then close the same
            // physical connection. The exact binding identity must deduplicate the
            // logical and automatic notifications.
            var logicalNotification = client
                .Request(new NotifyBoundActorDisconnectedReq(racingRemoteActorId))
                .PacketName("NotifyBoundActorDisconnectedReq")
                .Async<NotifyBoundActorDisconnectedRes>();
            await WaitForEvidenceAsync(
                playA,
                $"entry-disconnect-started|rid=play-a|actor={racingRemoteActorId}");
            await client.Close.Async();

            try
            {
                _ = await logicalNotification;
            }
            catch
            {
                // Closing the transport may prevent the logical reply from reaching
                // the caller. The Actor callback and cleanup evidence are authoritative.
            }

            var remoteEvidence = await WaitForEvidenceAsync(
                playA,
                $"entry-disconnected|rid=play-a|actor={racingRemoteActorId}");
            var localEvidence = await WaitForEvidenceAsync(
                sessionA,
                $"entry-disconnected|rid=session-a|actor={failingLocalActorId}");

            ZlinkStreamAssert.Ensure(
                Count(remoteEvidence, racingRemoteActorId) == 1,
                "SM-D5 logical/physical race invoked the remote Actor callback more than once.");
            ZlinkStreamAssert.Ensure(
                Count(localEvidence, failingLocalActorId) == 1,
                "SM-D5 callback failure was retried or skipped instead of settling once.");

            var blockedProbe = (await sessionA.Get("/location-store/read-probe")
                .Async<LocationStoreReadProbeSnapshot>()).Body;
            ZlinkStreamAssert.Ensure(
                blockedProbe.MatchingReads == 0,
                $"SM-D5 physical cleanup performed {blockedProbe.MatchingReads} Location Store reads.");

            await ConfigureStoreProbeAsync(
                sessionA,
                [failingLocalActorId, racingRemoteActorId],
                blocked: false);
            var localAfter = await CaptureRefAsync(gateway, failingLocalActorId);
            var remoteAfter = await CaptureRefAsync(gateway, racingRemoteActorId);
            ZlinkStreamAssert.Ensure(
                SameIdentity(localBefore, localAfter)
                && SameIdentity(remoteBefore, remoteAfter),
                "SM-D5 physical disconnect changed Actor identity, generation, or membership owner.");
            ZlinkStreamAssert.Ensure(
                localEvidence.All(line =>
                    !line.Contains(
                        $"entry-left|rid=session-a|actor={failingLocalActorId}",
                        StringComparison.Ordinal))
                && remoteEvidence.All(line =>
                    !line.Contains(
                        $"entry-left|rid=play-a|actor={racingRemoteActorId}",
                        StringComparison.Ordinal)),
                "SM-D5 physical disconnect removed Actor membership.");

            Console.WriteLine("operation SpotService.sm-d5 passed");
        }
        finally
        {
            await ConfigureStoreProbeAsync(
                sessionA,
                [failingLocalActorId, racingRemoteActorId],
                blocked: false);
            await SetWeightsAsync(playA, playB, sessionA, 100, 100, 0);
        }
    }

    private static IZlinkStreamConnector CreateConnector(string endpoint) =>
        ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });

    private static async Task<EnsureActorRes> EnsureActorAsync(
        ZLinkHttpClient gateway,
        string actorId) =>
        (await gateway.Post("/actor/get-or-create")
            .Body(new EnsureActorReq(actorId, actorId))
            .Async<EnsureActorRes>()).Body;

    private static async Task<ActorRefRes> CaptureRefAsync(
        ZLinkHttpClient gateway,
        string actorId) =>
        (await gateway.Post("/actor/capture-ref")
            .Body(new ActorRefReq(actorId))
            .Async<ActorRefRes>()).Body;

    private static async Task ConfigureStoreProbeAsync(
        ZLinkHttpClient sessionA,
        string[] actorIds,
        bool blocked)
    {
        _ = await sessionA.Post("/location-store/read-probe/configure")
            .Body(new LocationStoreReadProbeReq(actorIds, blocked))
            .Async<LocationStoreReadProbeSnapshot>();
    }

    private static async Task<string[]> WaitForEvidenceAsync(
        ZLinkHttpClient host,
        string marker) =>
        (await host.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([marker], TimeoutMilliseconds: 15000))
            .Async<string[]>()).Body;

    private static int Count(IEnumerable<string> evidence, string actorId) =>
        evidence.Count(line =>
            line.Contains("entry-disconnected|", StringComparison.Ordinal)
            && line.Contains($"actor={actorId}", StringComparison.Ordinal));

    private static bool SameIdentity(EnsureActorRes before, ActorRefRes after) =>
        before.ActorId == after.ActorId
        && before.NodeRid == after.NodeRid
        && before.Generation == after.Generation;

    private static async Task SetWeightsAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        ZLinkHttpClient sessionA,
        int playAWeight,
        int playBWeight,
        int sessionAWeight)
    {
        await playA.Post("/placement-weight")
            .Body(new PlacementWeightReq(playAWeight))
            .Async<PlacementWeightRes>();
        await playB.Post("/placement-weight")
            .Body(new PlacementWeightReq(playBWeight))
            .Async<PlacementWeightRes>();
        await sessionA.Post("/placement-weight")
            .Body(new PlacementWeightReq(sessionAWeight))
            .Async<PlacementWeightRes>();
        await Task.Delay(TimeSpan.FromSeconds(1));
    }
}
