// Verifies SM-B7 Actor Lifecycle Order behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmB7ActorLifecycleOrderScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA, string sessionAStreamEndpoint)
    {
        var actorId = $"actor-sm-b7-order-{Guid.NewGuid():N}";
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
        await client.Request(new AuthReq(actorId, "order"))
            .PacketName("AuthReq")
            .Async<AuthRes>();
        var replies = new[]
        {
            await client.Request(new ActorPingReq("order-1"))
                .PacketName("ActorPingReq").Async<ActorPingRes>(),
            await client.Request(new ActorPingReq("order-2"))
                .PacketName("ActorPingReq").Async<ActorPingRes>()
        };
        ZlinkStreamAssert.Ensure(
            replies[0].Value == "order-1" && replies[0].Seen == 1
                                          && replies[1].Value == "order-2" && replies[1].Seen == 2,
            "SM-B7 stream replies did not preserve actor packet order.");
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(["actor-ping|rid=play-a-", $"|actor={actorId}", "value=order-2|seen=2"]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.Any(line =>
                line.Contains("actor-ping|rid=play-a-", StringComparison.Ordinal)
                && line.Contains($"|actor={actorId}", StringComparison.Ordinal)
                && line.Contains("value=order-2|seen=2", StringComparison.Ordinal)),
            "SM-B7 evidence did not include ordered second request.");
        Console.WriteLine("operation SpotService.sm-b7 passed");
    }
}
