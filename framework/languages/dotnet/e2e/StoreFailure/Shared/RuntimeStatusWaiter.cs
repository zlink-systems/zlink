using System.Diagnostics;

namespace StoreFailure.Shared;

public static class RuntimeStatusWaiter
{
    public static async Task<RuntimeStatusRes?> WaitAsync(
        Func<CancellationToken, ValueTask<RuntimeStatusRes>> readStatus,
        RuntimeStatusWaitReq request,
        CancellationToken cancellationToken)
    {
        var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 60000));
        var elapsed = Stopwatch.StartNew();
        while (elapsed.Elapsed < timeout)
        {
            var status = await readStatus(cancellationToken);
            if (Matches(status, request)) return status;

            await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken);
        }

        return null;
    }

    private static bool Matches(RuntimeStatusRes status, RuntimeStatusWaitReq request)
    {
        return (request.StoreHealthy is null || status.StoreHealthy == request.StoreHealthy)
               && (request.OwnerLeaseHealthy is null
                   || status.OwnerLeaseHealthy == request.OwnerLeaseHealthy)
               && (!request.RequireLastRefresh || status.LastRefreshAt is not null)
               && (request.LastRefreshAfter is null
                   || status.LastRefreshAt > request.LastRefreshAfter);
    }
}
