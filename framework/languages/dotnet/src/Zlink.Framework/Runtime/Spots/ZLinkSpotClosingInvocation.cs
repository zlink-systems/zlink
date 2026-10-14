namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotClosingInvocation
{
    public static async ValueTask InvokeAsync(
        Func<ZLinkSpotClosingContext, CancellationToken, ValueTask> callback,
        ZLinkSpotCloseReason reason,
        DateTimeOffset deadline
    )
    {
        ArgumentNullException.ThrowIfNull(callback);
        var remaining = deadline - DateTimeOffset.UtcNow;
        using var cleanupSource = new CancellationTokenSource();
        if (remaining <= TimeSpan.Zero)
            cleanupSource.Cancel();
        else
            cleanupSource.CancelAfter(remaining);
        try
        {
            var pending = callback(
                new ZLinkSpotClosingContext(reason, deadline),
                cleanupSource.Token
            );
            if (pending.IsCompleted)
                await pending.ConfigureAwait(false);
            else
                await pending.AsTask().WaitAsync(cleanupSource.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException error)
            when (cleanupSource.IsCancellationRequested
                && error.CancellationToken == cleanupSource.Token
            )
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                "SPOT closing reached the host teardown end."
            )
            {
                Origin = ZLinkErrorOrigin.Framework,
            };
        }
    }
}
