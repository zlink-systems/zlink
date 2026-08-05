// Verifies SM-D15 Gateway Actor Session Push behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

// Verifies a cross-role chain: gateway HTTP request -> actor request ->
// bound-session push -> client stream receive.
internal static class SmD15GatewayActorSessionPushScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient gateway,
        string sessionAStreamEndpoint)
    {
        var actorId = $"actor-sm-d15-{Guid.NewGuid():N}";
        await using var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(10),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await client.Connect.Async();
        await client.Request(new AuthReq(actorId, "d15 push chain"))
            .PacketName("AuthReq")
            .Async<AuthRes>();
        await client.Request(new ActorPingReq("d15-bind-probe"))
            .PacketName("ActorPingReq")
            .Async<ActorPingRes>();

        var marker = $"sm-d15-{Guid.NewGuid():N}";
        var pushed = client.WaitFor<ActorPushNotify>().Async().AsTask();
        var result = (await gateway.Post("/actor/push")
            .Body(new ActorPushByActorReq(actorId, marker))
            .Async<ActorPushByActorRes>()).Body;
        var notify = await pushed;

        ZlinkStreamAssert.Ensure(result.Delivered, $"SM-D15 gateway actor push failed: {result.ErrorKind}");
        ZlinkStreamAssert.Ensure(notify.Payload.ActorId == actorId, "SM-D15 push actor mismatch.");
        ZlinkStreamAssert.Ensure(notify.Payload.Value == marker, "SM-D15 push marker mismatch.");

        await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([
                $"actor-push|rid=play-a|actor={actorId}",
                $"|value={marker}"
            ]))
            .Async<string[]>();
        await gateway.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([
                $"actor-push-delivered|rid=gateway|actor={actorId}|value={marker}|node=play-a"
            ]))
            .Async<string[]>();
        Console.WriteLine("operation SpotService.sm-d15 passed");
    }
}
