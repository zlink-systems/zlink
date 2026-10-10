namespace ZLink.Framework.Perf;

public static class ServerDrivenStreams
{
    public static async Task RunRequestsAsync(
        Measurement measurement,
        int streamCount,
        Func<int, Task> operation
    )
    {
        var stream = 0;
        while (measurement.CanIssue)
        {
            _ = operation(stream);
            if (++stream == streamCount)
            {
                stream = 0;
                await Task.Yield();
            }
        }
    }

    public static Task RunTerminalStreamsAsync(
        Measurement measurement,
        int streamCount,
        Func<int, Task> operation
    ) =>
        Task.WhenAll(
            Enumerable
                .Range(0, streamCount)
                .Select(stream => RunTerminalStreamAsync(measurement, operation, stream))
        );

    private static async Task RunTerminalStreamAsync(
        Measurement measurement,
        Func<int, Task> operation,
        int stream
    )
    {
        while (measurement.CanIssue)
            await operation(stream).ConfigureAwait(false);
    }
}
