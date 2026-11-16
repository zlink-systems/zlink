using Systems.Zlink.Stream.Connector.Contracts;

namespace Zlink.Framework.Tests.Common;

internal static class StreamConnectionHarness
{
    internal static async Task<IZlinkStreamConnector[]> ConnectAsync(
        List<IZlinkStreamConnector> connectors,
        int sessionCount,
        ZlinkStreamConnectorOptions options
    )
    {
        using var connectConcurrency = new SemaphoreSlim(128);
        var connected = await Task.WhenAll(
            Enumerable
                .Range(0, sessionCount)
                .Select(async _ =>
                {
                    await connectConcurrency.WaitAsync();
                    try
                    {
                        var connector = ZlinkStreamConnectorFactory.Create(options);
                        lock (connectors)
                            connectors.Add(connector);
                        await connector.Connect.Async();
                        return connector;
                    }
                    finally
                    {
                        connectConcurrency.Release();
                    }
                })
        );
        return connected;
    }
}
