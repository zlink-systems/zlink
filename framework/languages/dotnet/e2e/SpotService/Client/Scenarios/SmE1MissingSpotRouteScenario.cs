// Verifies SM-E1 Missing Spot Route behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmE1MissingSpotRouteScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA, string sessionAStreamEndpoint)
    {
        await VerifyActorMissingHandlerAsync(playA, sessionAStreamEndpoint);
        await VerifySpotMissingHandlerAsync(playA);
        Console.WriteLine("operation SpotService.sm-e1 passed");
    }

    private static async Task VerifyActorMissingHandlerAsync(
        ZLinkHttpClient playA,
        string sessionAStreamEndpoint)
    {
        var actorId = $"actor-sm-e1-missing-{Guid.NewGuid():N}";
        ZlinkStreamAssert.Ensure(!string.IsNullOrWhiteSpace(sessionAStreamEndpoint),
            "session-a stream endpoint is required.");
        await using var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await client.Connect.Async();
        await client.Request(new AuthReq(actorId, "missing handler actor"))
            .PacketName("AuthReq")
            .Async<AuthRes>();
        await ZlinkStreamAssert.ExpectFailureAsync(
            async cancellationToken => _ = await client.Request(new ActorPingReq("missing-handler"))
                .PacketName("MissingActorReq")
                .Timeout(TimeSpan.FromSeconds(2))
                .Async<ActorPingRes>(cancellationToken),
            nameof(ZlinkStreamErrorCode.RemoteError));
        var expectedEvidence = new[]
            { "dispatch-error|surface=SpotActor|reason=HandlerMissing|action=ReplyError|packet=MissingActorReq" };
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line => line.Contains(expected, StringComparison.Ordinal))),
            "SM-E1 missing actor handler evidence mismatch.");
    }

    private static async Task VerifySpotMissingHandlerAsync(ZLinkHttpClient playA)
    {
        var spotRid = $"spot-sm-e1-{Guid.NewGuid():N}";
        var created = (await playA.Post("/spot/create")
            .Body(new CreateSpotReq(spotRid))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(created.SpotRid == spotRid && created.NodeRid == "play-a",
            "SM-E1 spot was not created on play-a.");
        var missingRequest = (await playA.Post("/spot/missing-handler/request")
            .Body(new SpotMissingHandlerReq(spotRid))
            .Async<SpotMissingHandlerRes>()).Body;
        ZlinkStreamAssert.Ensure(missingRequest.Failed, "SM-E1 missing handler request did not fail.");
        var missingCommand = (await playA.Post("/spot/missing-handler/command")
            .Body(new SpotMissingCommandReq(spotRid, "missing-command"))
            .Async<SpotMissingCommandRes>()).Body;
        ZlinkStreamAssert.Ensure(missingCommand.Sent, "SM-E1 missing handler command was not sent.");
        var expectedEvidence = new[]
        {
            "dispatch-error|surface=SpotRoute|reason=HandlerMissing|action=ReplyError|packet=MissingSpotReq",
            "dispatch-error|surface=SpotRoute|reason=HandlerMissing|action=Drop|packet=MissingSpotMsg"
        };
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line => line.Contains(expected, StringComparison.Ordinal))),
            "SM-E1 missing handler evidence mismatch.");
    }
}
