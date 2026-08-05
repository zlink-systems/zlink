using LocationMessaging.Shared;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Support;

internal static class ProviderEvidence
{
    public static async Task<string[]> WaitFromEitherAsync(
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB,
        string contains,
        int timeoutMilliseconds = 10000)
    {
        using var cancellation = new CancellationTokenSource();
        var request = new EvidenceWaitReq(contains, timeoutMilliseconds);
        var pending = new List<Task<string[]>>
        {
            WaitAsync(providerA, request, cancellation.Token),
            WaitAsync(providerB, request, cancellation.Token)
        };
        var failures = new List<Exception>();

        while (pending.Count > 0)
        {
            var completed = await Task.WhenAny(pending);
            pending.Remove(completed);
            try
            {
                var evidence = await completed;
                cancellation.Cancel();
                await ObserveCanceledLosersAsync(pending, cancellation.Token);
                return evidence;
            }
            catch (Exception error)
            {
                failures.Add(error);
            }
        }

        throw new AggregateException("Neither provider returned matching evidence.", failures);
    }

    private static async Task<string[]> WaitAsync(
        ZLinkHttpClient provider,
        EvidenceWaitReq request,
        CancellationToken cancellationToken)
    {
        return (await provider.Post("/evidence/wait")
            .Body(request)
            .Async<string[]>(cancellationToken)).Body;
    }

    private static async Task ObserveCanceledLosersAsync(
        IEnumerable<Task<string[]>> pending,
        CancellationToken cancellationToken)
    {
        foreach (var loser in pending)
        {
            try
            {
                await loser;
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
            }
            catch
            {
                // The first successful evidence defines this operation. Observe a racing loser
                // failure without replacing a result already returned by the other provider.
            }
        }
    }
}
