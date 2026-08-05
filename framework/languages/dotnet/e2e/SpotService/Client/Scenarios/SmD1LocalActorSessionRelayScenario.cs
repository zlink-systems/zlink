// Verifies SM-D1 Local Actor Session Relay behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmD1LocalActorSessionRelayScenario
{
    public static async Task RunAsync(ZLinkHttpClient sessionA, string sessionAStreamEndpoint)
    {
        var control = (await sessionA.Post("/channel/control-ping/play-a")
            .Body(new ControlPingReq("sm-d1-play-a-ready"))
            .Async<ControlPingRes>()).Body;
        ZlinkStreamAssert.Ensure(control.NodeRid == "play-a", "SM-D1 control route target mismatch.");
        await using var bound = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(10),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await bound.Connect.Async();
        await bound.Request(new AuthReq("actor-sm-d1", "local relay"))
            .PacketName("AuthReq").Async<AuthRes>();
        var pushed = bound.WaitFor<ActorPushNotify>().Async().AsTask();
        var reply = await bound.Request(new ActorPushReq("push-local"))
            .PacketName("ActorPushReq").Async<ActorPingRes>();
        var notify = await pushed;
        ZlinkStreamAssert.Ensure(reply.ActorId == "actor-sm-d1", "SM-D1 actor reply mismatch.");
        ZlinkStreamAssert.Ensure(notify.Payload.ActorId == "actor-sm-d1", "SM-D1 push actor mismatch.");
        ZlinkStreamAssert.Ensure(notify.Payload.Value == "push-local", "SM-D1 push value mismatch.");

        Console.WriteLine("operation SpotService.sm-d1 passed");
    }
}
