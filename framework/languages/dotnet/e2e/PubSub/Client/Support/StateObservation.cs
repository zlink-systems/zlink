namespace PubSub.Client.Support;

internal static class StateObservation
{
    private static readonly TimeSpan ReadinessTimeout = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan ReadinessPollInterval = TimeSpan.FromMilliseconds(100);

    public static async Task WaitUntilAsync(
        Func<Task<bool>> condition,
        string failureMessage,
        TimeSpan? timeout = null)
    {
        using var timeoutSource = new CancellationTokenSource(timeout ?? ReadinessTimeout);
        try
        {
            while (true)
            {
                if (await condition()) return;

                await Task.Delay(ReadinessPollInterval, timeoutSource.Token);
            }
        }
        catch (OperationCanceledException) when (timeoutSource.IsCancellationRequested)
        {
            throw new TimeoutException(failureMessage);
        }
    }
}
