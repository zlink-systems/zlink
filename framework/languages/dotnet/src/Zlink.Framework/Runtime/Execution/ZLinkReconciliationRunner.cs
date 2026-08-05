namespace Zlink.Framework.Runtime.Execution;

/// <summary>
/// Owns the retry, terminal-failure and shutdown mechanics shared by durable
/// actor reconciliation operations. Callers supply only their domain operation,
/// terminal exception classification and operation-specific failure reporting.
/// </summary>
internal static class ZLinkReconciliationRunner
{
    private static readonly TimeSpan DefaultRetryDelay = TimeSpan.FromMilliseconds(100);

    internal static async ValueTask RunAsync(
        Func<CancellationToken, ValueTask> operation,
        Action<Exception> reportRetry,
        CancellationToken cancellationToken,
        Predicate<Exception>? isTerminal = null,
        TimeSpan? retryDelay = null)
    {
        await RunAsync(
                async token =>
                {
                    await operation(token).ConfigureAwait(false);
                    return true;
                },
                reportRetry,
                cancellationToken,
                isTerminal,
                retryDelay)
            .ConfigureAwait(false);
    }

    internal static async ValueTask<TResult> RunAsync<TResult>(
        Func<CancellationToken, ValueTask<TResult>> operation,
        Action<Exception> reportRetry,
        CancellationToken cancellationToken,
        Predicate<Exception>? isTerminal = null,
        TimeSpan? retryDelay = null)
    {
        var delay = retryDelay ?? DefaultRetryDelay;
        if (delay < TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(retryDelay));

        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                return await operation(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception exception)
            {
                if (isTerminal?.Invoke(exception) == true) throw;
                reportRetry(exception);
            }

            await Task.Delay(delay, cancellationToken).ConfigureAwait(false);
        }
    }
}
