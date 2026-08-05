// Verifies OBS-A2 Error Flow behavior.
using ObservabilityOps.Client.Support;
using Systems.Zlink.Stream.Connector.Contracts;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsA2ErrorFlowScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        await using var connector = await context.ConnectAsync();
        await ZlinkStreamAssert.ExpectFailureAsync(
            async cancellationToken =>
                _ = await connector
                    .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 0xff, 0x00 }))
                    .PacketName("ObservabilityMissingPacket")
                    .Async(cancellationToken),
            nameof(ZlinkStreamErrorCode.RemoteError));
        await connector.Close.Async();
        Console.WriteLine("scenario OBS-A2 passed");
    }
}
