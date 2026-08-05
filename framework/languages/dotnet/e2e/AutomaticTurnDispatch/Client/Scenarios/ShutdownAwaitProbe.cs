// Verifies shutdown cancels an outstanding await without hanging the host.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;

internal static class ShutdownAwaitProbe
{
    public static async Task RunWaitAsync(ClientOptions options)
    {
        await using var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(options.SessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(60),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await client.Connect.Async();

        var failure = await ZlinkStreamAssert.ExpectFailureAsync(
            async cancellationToken => _ = await client.Request(
                    new AwaitShutdownScenarioReq(options.RequestId, options.SpotRid, 30_000))
                .Timeout(TimeSpan.FromSeconds(90))
                .Async<AwaitShutdownScenarioRes>(cancellationToken));
        ZlinkStreamAssert.Ensure(
            failure.Code is ZlinkStreamErrorCode.Disconnected or ZlinkStreamErrorCode.RemoteError,
            $"TD-F5 expected Disconnected or RemoteError, got {failure.Code}.");
        Console.WriteLine("automatic-turn-dispatch shutdown wait result=passed");
    }

    public static async Task RunRecoveryAsync(ClientOptions options)
    {
        await using var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(options.SessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(60),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await client.Connect.Async();

        var result = await client.Request(new AwaitShutdownRecoveryReq(options.RequestId, options.SpotRid))
            .Timeout(TimeSpan.FromSeconds(45))
            .Async<AwaitShutdownRecoveryRes>();
        ZlinkStreamAssert.Ensure(result.Operation == "await.e3-shutdown-recovery", "TD-F5 recovery operation mismatch.");
        ZlinkStreamAssert.Ensure(result.SpotRid == options.SpotRid, "TD-F5 recovery spot rid mismatch.");
        ZlinkStreamAssert.Ensure(
            result.Evidence.Any(line => line.Contains($"request={options.RequestId}", StringComparison.Ordinal)
                                        && line.Contains("marker=shutdown-recovery-probe", StringComparison.Ordinal)),
            "TD-F5 recovery probe marker missing.");

        Console.WriteLine("automatic-turn-dispatch shutdown recovery result=passed");
    }

}
