// Verifies SM-D14 Tls Stream Validation behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace SpotService.Client.Scenarios;

internal static class SmD14TlsStreamValidationScenario
{
    public static async Task RunAsync(string sessionATlsStreamEndpoint)
    {
        await using var strict = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionATlsStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024,
            SkipServerCertificateValidation = false
        });
        await ZlinkStreamAssert.ExpectFailureAsync(
            cancellationToken => strict.Connect.Async(cancellationToken),
            nameof(ZlinkStreamErrorCode.TlsValidationFailed));

        await using var tls = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionATlsStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024,
            SkipServerCertificateValidation = true
        });
        await tls.Connect.Async();
        await tls.Request(new AuthReq("actor-sm-d14-tls", "stream tls"))
            .PacketName("AuthReq")
            .Async<AuthRes>();

        var pushed = tls.WaitFor<ActorPushNotify>().Async().AsTask();
        var reply = await tls.Request(new ActorPushReq("tls-push"))
            .PacketName("ActorPushReq")
            .Async<ActorPingRes>();
        var notify = await pushed;
        ZlinkStreamAssert.Ensure(reply.ActorId == "actor-sm-d14-tls", "SM-D14 TLS actor reply mismatch.");
        ZlinkStreamAssert.Ensure(reply.NodeRid == "play-a", "SM-D14 TLS actor node mismatch.");
        ZlinkStreamAssert.Ensure(notify.Payload.ActorId == "actor-sm-d14-tls", "SM-D14 TLS push actor mismatch.");
        ZlinkStreamAssert.Ensure(notify.Payload.Value == "tls-push", "SM-D14 TLS push payload mismatch.");

        Console.WriteLine("operation SpotService.sm-d14 passed");
    }
}
