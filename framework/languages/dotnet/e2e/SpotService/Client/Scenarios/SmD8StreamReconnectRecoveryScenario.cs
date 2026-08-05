// Verifies SM-D8 Stream Reconnect Recovery behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmD8StreamReconnectRecoveryScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        string sessionAStreamEndpoint)
    {
        const string actorId = "actor-sm-d8-reconnect";

        await using var first = CreateConnector(sessionAStreamEndpoint);
        await first.Connect.Async();
        var firstAuth = await first.Request(new AuthReq(actorId, "stream reconnect"))
            .PacketName("AuthReq")
            .Async<AuthRes>();

        var owner = firstAuth.NodeRid.StartsWith("play-a-", StringComparison.Ordinal)
            ? playA
            : playB;
        var pending = first.Request(new SlowActorPingReq("before-disconnect", 1000))
            .PacketName("SlowActorPingReq")
            .Timeout(TimeSpan.FromSeconds(10))
            .Async<ActorPingRes>()
            .AsTask();
        var started = $"actor-slow-ping-started|rid={firstAuth.NodeRid}|actor={actorId}";
        var evidence = (await owner.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([started]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
                evidence.Any(line => line.Contains(started, StringComparison.Ordinal)
                                     && line.Contains("value=before-disconnect", StringComparison.Ordinal)),
            "SM-D8 slow request was not accepted before disconnect.");
        await first.Close.Async();
        await ZlinkStreamAssert.ExpectFailureAsync(
            async _ => await pending,
            nameof(ZlinkStreamErrorCode.Disconnected));

        await using var second = CreateConnector(sessionAStreamEndpoint);
        await second.Connect.Async();
        var secondAuth = await second.Request(new AuthReq(actorId, "stream reconnect"))
            .PacketName("AuthReq")
            .Async<AuthRes>();

        var resumed = await second.Request(new ActorPingReq("after-reconnect"))
            .PacketName("ActorPingReq")
            .Async<ActorPingRes>();
        ZlinkStreamAssert.Ensure(resumed.ActorId == actorId, "SM-D8 reconnected actor mismatch.");
        ZlinkStreamAssert.Ensure(
            resumed.NodeRid == firstAuth.NodeRid
            && secondAuth.NodeRid == firstAuth.NodeRid,
            "SM-D8 reconnected node mismatch.");
        ZlinkStreamAssert.Ensure(resumed.Value == "after-reconnect", "SM-D8 reconnected value mismatch.");
        Console.WriteLine("operation SpotService.sm-d8 passed");
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
}
