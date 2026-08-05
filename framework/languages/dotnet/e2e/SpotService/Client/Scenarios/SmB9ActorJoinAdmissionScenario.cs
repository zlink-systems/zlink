// Verifies SM-B9 Actor Join Admission behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

// Verifies that user spot actor admission accepts valid joins and returns a
// classified rejection without creating a joined user-spot actor.
internal static class SmB9ActorJoinAdmissionScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        string sessionAStreamEndpoint)
    {
        try
        {
            await SetPlacementWeightsAsync(playA, playB, 100, 0);
            await VerifyAdmissionAsync(
                playA,
                sessionAStreamEndpoint,
                "play-a",
                $"spot-sm-b9-local-{Guid.NewGuid():N}",
                $"actor-sm-b9-local-{Guid.NewGuid():N}");
            await SetPlacementWeightsAsync(playA, playB, 0, 100);
            await VerifyAdmissionAsync(
                playB,
                sessionAStreamEndpoint,
                "play-b",
                $"spot-sm-b9-remote-{Guid.NewGuid():N}",
                $"actor-sm-b9-remote-{Guid.NewGuid():N}");
            Console.WriteLine("operation SpotService.sm-b9 passed");
        }
        finally
        {
            await SetPlacementWeightsAsync(playA, playB, 100, 100);
        }
    }

    private static async Task VerifyAdmissionAsync(
        ZLinkHttpClient owner,
        string sessionAStreamEndpoint,
        string nodeRid,
        string spotRid,
        string actorId)
    {
        await owner.Post("/spot/create")
            .Body(new CreateSpotReq(spotRid))
            .Async<CreateSpotRes>();

        await using var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(10),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await client.Connect.Async();
        await client.Request(new AuthReq(actorId, $"admission {nodeRid}"))
            .PacketName("AuthReq")
            .Async<AuthRes>();

        var allowed = await client.Request(new JoinAdmittedUserSpotActorReq(spotRid, actorId, true, "allowed"))
            .PacketName("JoinAdmittedUserSpotActorReq")
            .Async<JoinAdmittedUserSpotActorRes>();
        ZlinkStreamAssert.Ensure(allowed.Accepted, "SM-B9 allowed join was rejected.");
        ZlinkStreamAssert.Ensure(allowed.SpotRid == spotRid, "SM-B9 allowed join spot mismatch.");

        // The request reply is the admission result. The second binding must
        // start only after the actor's public join-completion evidence, so it
        // cannot race the first actor's entry-to-user-spot handoff.
        await owner.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([
                $"spot-actor-joined|rid={nodeRid}|spot={spotRid}|actor={actorId}",
                $"actor-join-completed|rid={nodeRid}|actor={actorId}|spot={spotRid}"
            ]))
            .Async<string[]>();

        var rejectedActorId = $"{actorId}-rejected";
        await client.Request(new AuthReq(rejectedActorId, $"rejected {nodeRid}"))
            .PacketName("AuthReq")
            .Async<AuthRes>();
        var rejected = await client.Request(new JoinAdmittedUserSpotActorReq(
                spotRid,
                rejectedActorId,
                false,
                "capacity"))
            .PacketName("JoinAdmittedUserSpotActorReq")
            .Metadata(SpotServiceNames.ActorIdMetadata, rejectedActorId)
            .Async<JoinAdmittedUserSpotActorRes>();
        ZlinkStreamAssert.Ensure(!rejected.Accepted, "SM-B9 rejected join was accepted.");
        ZlinkStreamAssert.Ensure(rejected.ErrorKind == "ActorJoinRejected", "SM-B9 rejection was not classified.");

        var evidence = (await owner.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([
                $"spot-actor-join-admitted|rid={nodeRid}|spot={spotRid}|actor={actorId}|reason=allowed",
                $"spot-actor-joined|rid={nodeRid}|spot={spotRid}|actor={actorId}",
                $"spot-actor-join-rejected|rid={nodeRid}|spot={spotRid}|actor={rejectedActorId}|reason=capacity"
            ]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.All(line =>
                !line.Contains($"spot-actor-joined|rid={nodeRid}|spot={spotRid}|actor={rejectedActorId}",
                    StringComparison.Ordinal)),
            "SM-B9 rejected actor was joined to the user spot.");
    }

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
        // Placement updates are published through the shared location view.
        // Let both nodes observe the new values before creating the next
        // object; otherwise the local/remote variant can select a stale
        // candidate and report evidence on the other process.
        await Task.Delay(TimeSpan.FromSeconds(2));
    }
}
