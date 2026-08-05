// Verifies SM-B4 Remote Actor Request Reply behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmB4RemoteActorRequestReplyScenario
{
    public static async Task RunAsync(ZLinkHttpClient playB, string sessionAStreamEndpoint)
    {
        var actorId = $"actor-sm-b4-remote-{Guid.NewGuid():N}";
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
        await client.Request(new AuthReq(actorId, "remote actor request"))
            .PacketName("AuthReq")
            .Async<AuthRes>();
        var reply = await client.Request(new ActorPingReq("sm-b4"))
            .PacketName("ActorPingReq")
            .Async<ActorPingRes>();

        ZlinkStreamAssert.Ensure(
            reply.NodeRid.StartsWith("play-b-", StringComparison.Ordinal),
            "SM-B4 remote actor request reached the wrong node.");
        var expectedEvidence = new[] { "actor-ping|rid=play-b-", $"|actor={actorId}" };
        var evidence = (await playB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.Any(line =>
                line.Contains("actor-ping|rid=play-b-", StringComparison.Ordinal)
                && line.Contains($"|actor={actorId}", StringComparison.Ordinal)),
            "SM-B4 evidence did not include remote actor request.");
        Console.WriteLine("operation SpotService.sm-b4 passed");
    }
}
