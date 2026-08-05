namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotClosingInvocation
{
    public static async ValueTask InvokeAsync(
        Func<ZLinkSpotClosingContext, CancellationToken, ValueTask> callback,
        ZLinkSpotCloseReason reason,
        DateTimeOffset deadline)
    {
        ArgumentNullException.ThrowIfNull(callback);
        var remaining = deadline - DateTimeOffset.UtcNow;
        if (remaining <= TimeSpan.Zero)
            throw new OperationCanceledException(
                "The SPOT closing deadline elapsed before the lifecycle callback started.");

        using var cleanupSource = new CancellationTokenSource(remaining);
        await callback(
                new ZLinkSpotClosingContext(reason, deadline),
                cleanupSource.Token)
            .ConfigureAwait(false);
    }
}
