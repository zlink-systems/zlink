// Verifies SM-D3 Entry And User Spot Session Relay behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmD3EntryAndUserSpotSessionRelayScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA, string sessionAStreamEndpoint)
    {
        var entryActorId = $"actor-sm-d3-entry-{Guid.NewGuid():N}";
        var userSpotRid = $"spot-sm-d3-user-{Guid.NewGuid():N}";
        var userActorId = $"actor-sm-d3-user-{Guid.NewGuid():N}";
        await using var entry = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await entry.Connect.Async();
        await entry.Request(new AuthReq(entryActorId, "entry bind"))
            .PacketName("AuthReq").Async<AuthRes>();

        var entryPushed = entry.WaitFor<ActorPushNotify>().Async().AsTask();
        var entryReply = await entry.Request(new ActorPushReq("entry-push"))
            .PacketName("ActorPushReq")
            .Async<ActorPingRes>();
        var entryNotify = await entryPushed;
        ZlinkStreamAssert.Ensure(entryReply.ActorId == entryActorId, "SM-D3 entry bind actor mismatch.");
        ZlinkStreamAssert.Ensure(entryReply.NodeRid == "play-a", "SM-D3 entry bind node mismatch.");
        ZlinkStreamAssert.Ensure(entryNotify.Payload.ActorId == entryActorId, "SM-D3 entry push actor mismatch.");
        ZlinkStreamAssert.Ensure(entryNotify.Payload.Value == "entry-push", "SM-D3 entry push value mismatch.");

        await using var user = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await user.Connect.Async();
        await user.Request(new UserSpotAuthReq(userSpotRid, userActorId, "user bind"))
            .PacketName("UserSpotAuthReq").Async<AuthRes>();
        await playA.Post("/spot/create").Body(new CreateSpotReq(userSpotRid)).Async<CreateSpotRes>();
        await user.Request(new JoinUserSpotActorReq(userSpotRid, userActorId))
            .PacketName("JoinUserSpotActorReq").Async<JoinUserSpotActorRes>();

        var userPushed = user.WaitFor<ActorPushNotify>().Async().AsTask();
        var userReply = await user.Request(new ActorPingReq("user-relay"))
            .PacketName("UserActorPingReq")
            .Async<ActorPingRes>();
        var userPushReply = await user.Request(new ActorPushReq("user-push"))
            .PacketName("UserActorPushReq")
            .Async<ActorPingRes>();
        var userNotify = await userPushed;
        ZlinkStreamAssert.Ensure(userReply.ActorId == userActorId, "SM-D3 user bind actor mismatch.");
        ZlinkStreamAssert.Ensure(userReply.NodeRid == "play-a", "SM-D3 user bind node mismatch.");
        ZlinkStreamAssert.Ensure(userReply.SpotRid == userSpotRid, "SM-D3 user bind spot mismatch.");
        ZlinkStreamAssert.Ensure(userReply.Value == "user-relay", "SM-D3 user relay value mismatch.");
        ZlinkStreamAssert.Ensure(userPushReply.ActorId == userActorId, "SM-D3 user push reply actor mismatch.");
        ZlinkStreamAssert.Ensure(userNotify.Payload.ActorId == userActorId, "SM-D3 user push actor mismatch.");
        ZlinkStreamAssert.Ensure(userNotify.Payload.Value == "user-push", "SM-D3 user push value mismatch.");

        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([
                $"spot-actor-joined|rid=play-a|spot={userSpotRid}|actor={userActorId}",
                $"actor-ping|rid=play-a|actor={userActorId}|spot={userSpotRid}|value=user-relay"
            ]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains(
                $"spot-actor-joined|rid=play-a|spot={userSpotRid}|actor={userActorId}",
                StringComparison.Ordinal))
            && evidence.Any(line => line.Contains(
                $"actor-ping|rid=play-a|actor={userActorId}|spot={userSpotRid}|value=user-relay",
                StringComparison.Ordinal)),
            "SM-D3 expected user spot bind and relay evidence.");

        Console.WriteLine("operation SpotService.sm-d3 passed");
    }
}
