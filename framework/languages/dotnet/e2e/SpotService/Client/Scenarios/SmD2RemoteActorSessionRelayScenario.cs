// Verifies SM-D2 Local And Remote Actor Session Relay behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmD2RemoteActorSessionRelayScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient sessionA,
        string sessionAStreamEndpoint,
        Func<Task> prepareLocalActor,
        Func<Task> prepareRemoteActor)
    {
        await prepareLocalActor();
        await VerifyLocalActorRelayAsync(sessionA, sessionAStreamEndpoint);

        await prepareRemoteActor();
        await VerifyRemoteActorRelayAsync(sessionA, sessionAStreamEndpoint);

        Console.WriteLine("operation SpotService.sm-d2 passed");
    }

    private static async Task VerifyLocalActorRelayAsync(
        ZLinkHttpClient sessionA,
        string sessionAStreamEndpoint)
    {
        var control = (await sessionA.Post("/channel/control-ping/play-a")
            .Body(new ControlPingReq("sm-d2-play-a-ready"))
            .Async<ControlPingRes>()).Body;
        ZlinkStreamAssert.Ensure(control.NodeRid == "play-a", "SM-D2 local control route target mismatch.");
        await using var local = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(10),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await local.Connect.Async();
        await local.Request(new AuthReq("actor-sm-d2-local", "local relay"))
            .PacketName("AuthReq").Async<AuthRes>();
        var pushed = local.WaitFor<ActorPushNotify>().Async().AsTask();
        var reply = await local.Request(new ActorPushReq("push-local"))
            .PacketName("ActorPushReq").Async<ActorPingRes>();
        var notify = await pushed;
        ZlinkStreamAssert.Ensure(reply.ActorId == "actor-sm-d2-local", "SM-D2 local actor reply mismatch.");
        ZlinkStreamAssert.Ensure(notify.Payload.ActorId == "actor-sm-d2-local", "SM-D2 local push actor mismatch.");
        ZlinkStreamAssert.Ensure(notify.Payload.Value == "push-local", "SM-D2 local push value mismatch.");
    }

    private static async Task VerifyRemoteActorRelayAsync(
        ZLinkHttpClient sessionA,
        string sessionAStreamEndpoint)
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
    }
}
