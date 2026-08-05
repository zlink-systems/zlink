// Verifies SM-D13 Heartbeat Request behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace SpotService.Client.Scenarios;

internal static class SmD13HeartbeatRequestScenario
{
    public static async Task RunAsync(string sessionAStreamEndpoint)
    {
        var disconnected = new TaskCompletionSource<ZlinkStreamDisconnected>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        await using var stream = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions
            {
                Enabled = true,
                Interval = TimeSpan.FromMilliseconds(200),
                Timeout = TimeSpan.FromSeconds(2)
            },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        stream.Disconnected += (message, _) =>
        {
            disconnected.TrySetResult(message);
            return ValueTask.CompletedTask;
        };
        await stream.Connect.Async();
        await stream.Request(new AuthReq("actor-sm-d13", "heartbeat"))
            .PacketName("AuthReq").Async<AuthRes>();
        var alive = await stream.Request(new ActorPingReq("heartbeat-alive"))
            .PacketName("ActorPingReq").Async<ActorPingRes>();
        ZlinkStreamAssert.Ensure(alive.ActorId == "actor-sm-d13", "SM-D13 normal heartbeat session failed.");
        Console.WriteLine("spot-service sm-d13 heartbeat-stop armed");
        var stopped = await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(10));
        ZlinkStreamAssert.Ensure(
            stopped.CloseReason is ZlinkStreamCloseReason.HeartbeatTimeout or ZlinkStreamCloseReason.TransportError
            && !stream.IsConnected,
            $"SM-D13 expected heartbeat loss to disconnect the connector, actual {stopped.CloseReason}.");

        Console.WriteLine("operation SpotService.sm-d13 passed");
    }
}
