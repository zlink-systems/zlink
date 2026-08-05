// Verifies SM-D6 Bound Session Push Targeting behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace SpotService.Client.Scenarios;

internal static class SmD6BoundSessionPushTargetingScenario
{
    public static async Task RunAsync(
        string sessionAStreamEndpoint,
        string sessionBStreamEndpoint,
        Func<Task>? prepareShadowPlacement = null)
    {
        IZlinkStreamConnector? bound = null;
        IZlinkStreamConnector? unbound = null;
        try
        {
            bound = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(sessionAStreamEndpoint),
                ConnectTimeout = TimeSpan.FromSeconds(5),
                RequestTimeout = TimeSpan.FromSeconds(10),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                MaxReceivedMessages = 1024
            });
            await bound.Connect.Async();
        await bound.Request(new AuthReq("actor-sm-d6", "bound"))
                .PacketName("AuthReq")
                .Async<AuthRes>();

            if (prepareShadowPlacement is not null)
                await prepareShadowPlacement();

            unbound = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(sessionBStreamEndpoint),
                ConnectTimeout = TimeSpan.FromSeconds(5),
                RequestTimeout = TimeSpan.FromSeconds(10),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                MaxReceivedMessages = 1024
            });
            await unbound.Connect.Async();
        await unbound.Request(new AuthReq("actor-sm-d6-shadow", "unbound"))
                .PacketName("AuthReq")
                .Async<AuthRes>();

            var activeBound = bound;
            var activeUnbound = unbound;
            var pushed = activeBound.WaitFor<ActorPushNotify>().Async().AsTask();
            await activeBound.Request(new ActorPushReq("push-bound-only"))
                .PacketName("ActorPushReq")
                .Async<ActorPingRes>();
            var notify = await pushed;
            ZlinkStreamAssert.Ensure(notify.Payload.ActorId == "actor-sm-d6", "SM-D6 push actor mismatch.");
            ZlinkStreamAssert.Ensure(notify.Payload.Value == "push-bound-only", "SM-D6 push value mismatch.");
            await activeUnbound.ExpectNone<ActorPushNotify>()
                .Within(TimeSpan.FromMilliseconds(200))
                .Async();
        }
        finally
        {
            if (bound is not null) await bound.DisposeAsync();
            if (unbound is not null) await unbound.DisposeAsync();
        }

        Console.WriteLine("operation SpotService.sm-d6 passed");
    }
}
