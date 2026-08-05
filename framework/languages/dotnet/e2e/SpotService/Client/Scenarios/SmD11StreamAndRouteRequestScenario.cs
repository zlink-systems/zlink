// Verifies SM-D11 Stream And Route Request behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmD11StreamAndRouteRequestScenario
{
    public static async Task RunAsync(ZLinkHttpClient sessionA, string sessionAStreamEndpoint)
    {
        await using (var stream = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        }))
        {
            await stream.Connect.Async();
        await stream.Request(new AuthReq("actor-sm-d11", "stream and channel"))
                .PacketName("AuthReq").Async<AuthRes>();
            var streamReply = await stream.Request(new ActorPingReq("stream-side"))
                .PacketName("ActorPingReq")
                .Async<ActorPingRes>();
            ZlinkStreamAssert.Ensure(streamReply.ActorId == "actor-sm-d11", "SM-D11 stream request actor mismatch.");
        }

        var channelReply = (await sessionA.Post("/channel/control-ping/play-a")
            .Body(new ControlPingReq("channel-side"))
            .Async<ControlPingRes>()).Body;
        ZlinkStreamAssert.Ensure(channelReply.NodeRid == "play-a", "SM-D11 channel request node mismatch.");
        ZlinkStreamAssert.Ensure(channelReply.Value == "channel-side", "SM-D11 channel reply value mismatch.");
        Console.WriteLine("operation SpotService.sm-d11 passed");
    }
}
