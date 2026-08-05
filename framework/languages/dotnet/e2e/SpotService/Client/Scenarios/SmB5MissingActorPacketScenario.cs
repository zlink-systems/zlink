// Verifies SM-B5 Missing Actor Packet behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmB5MissingActorPacketScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA, string sessionAStreamEndpoint)
    {
        var actorId = $"actor-sm-b5-missing-{Guid.NewGuid():N}";
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
            "SM-B5 evidence mismatch.");
        Console.WriteLine("operation SpotService.sm-b5 passed");
    }
}
