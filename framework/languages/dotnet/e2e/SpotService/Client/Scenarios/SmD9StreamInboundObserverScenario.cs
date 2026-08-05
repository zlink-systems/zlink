// Verifies SM-D9 Stream Inbound Observer behavior.
using System.Collections.Concurrent;
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace SpotService.Client.Scenarios;

internal static class SmD9StreamInboundObserverScenario
{
    public static async Task RunAsync(string sessionAStreamEndpoint)
    {
        var observed = new ConcurrentQueue<string>();
        await using var stream = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        stream.ObserveInbound((observation, cancellationToken) =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            observed.Enqueue(observation.Name);
            return ValueTask.CompletedTask;
        });
        await stream.Connect.Async();
        await stream.Request(new AuthReq("actor-sm-d9", "observer"))
            .PacketName("AuthReq").Async<AuthRes>();
        await stream.Request(new ActorPingReq("observer-1"))
            .PacketName("ActorPingReq").Async<ActorPingRes>();
        await stream.Request(new ActorPingReq("observer-2"))
            .PacketName("ActorPingReq").Async<ActorPingRes>();
        ZlinkStreamAssert.Ensure(observed.Count >= 2, "SM-D9 inbound observer did not observe stream replies.");

        Console.WriteLine("operation SpotService.sm-d9 passed");
    }
}
