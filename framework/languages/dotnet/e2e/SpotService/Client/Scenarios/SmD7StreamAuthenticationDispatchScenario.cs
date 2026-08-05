// Verifies SM-D7 Stream Authentication Dispatch behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace SpotService.Client.Scenarios;

internal static class SmD7StreamAuthenticationDispatchScenario
{
    public static async Task RunAsync(string sessionAStreamEndpoint)
    {
        await using var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await client.Connect.Async();
        var authReply = await client.Request(new AuthReq("actor-sm-d7", "stream auth"))
            .PacketName("AuthReq").Async<AuthRes>();
        ZlinkStreamAssert.Ensure(authReply.ActorId == "actor-sm-d7", "SM-D7 auth reply actor mismatch.");
        var reply = await client.Request(new ActorPingReq("auth-ok"))
            .PacketName("ActorPingReq").Async<ActorPingRes>();
        ZlinkStreamAssert.Ensure(reply.ActorId == "actor-sm-d7", "SM-D7 relay actor mismatch.");
        ZlinkStreamAssert.Ensure(reply.Value == "auth-ok", "SM-D7 relay value mismatch.");

        Console.WriteLine("operation SpotService.sm-d7 passed");
    }
}
