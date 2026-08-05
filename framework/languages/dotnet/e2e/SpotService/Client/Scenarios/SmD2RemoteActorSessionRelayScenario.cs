// Verifies SM-D2 Remote Actor Session Relay behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmD2RemoteActorSessionRelayScenario
{
    public static async Task RunAsync(ZLinkHttpClient sessionA, string sessionAStreamEndpoint)
    {
        var control = (await sessionA.Post("/channel/control-ping/play-b")
            .Body(new ControlPingReq("sm-d2-play-b-ready"))
            .Async<ControlPingRes>()).Body;
        ZlinkStreamAssert.Ensure(control.NodeRid == "play-b", "SM-D2 control route target mismatch.");
        await using var remote = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(10),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await remote.Connect.Async();
        await remote.Request(new AuthReq("actor-sm-d2", "remote relay"))
            .PacketName("AuthReq").Async<AuthRes>();
        var remotePushed = remote.WaitFor<ActorPushNotify>().Async().AsTask();
        var remoteReply = await remote.Request(new ActorPushReq("push-remote"))
            .PacketName("ActorPushReq").Async<ActorPingRes>();
        var remoteNotify = await remotePushed;
        ZlinkStreamAssert.Ensure(remoteReply.ActorId == "actor-sm-d2", "SM-D2 actor reply mismatch.");
        ZlinkStreamAssert.Ensure(
            remoteReply.NodeRid.StartsWith("play-b-", StringComparison.Ordinal),
            "SM-D2 remote node mismatch.");
        ZlinkStreamAssert.Ensure(remoteNotify.Payload.ActorId == "actor-sm-d2", "SM-D2 push actor mismatch.");
        ZlinkStreamAssert.Ensure(remoteNotify.Payload.Value == "push-remote", "SM-D2 push value mismatch.");

        Console.WriteLine("operation SpotService.sm-d2 passed");
    }
}
