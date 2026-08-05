namespace Zlink.Framework.AspNetCore;

internal sealed class ZLinkSpotHttpExecutionScheduler : IZLinkHttpExecutionScheduler
{
    public IZLinkHttpExecutionTurn? Capture()
    {
        return ZLinkApplicationExecutionContext.Current is { YieldAllowed: true }
               && ZLinkSerialTurn.Current is { } turn
            ? new ZLinkSpotHttpExecutionTurn(turn)
            : null;
    }

    private sealed class ZLinkSpotHttpExecutionTurn(ZLinkSerialTurn turn) : IZLinkHttpExecutionTurn
    {
        public ValueTask<TResult> YieldAsync<TResult>(
            Func<CancellationToken, ValueTask<TResult>> operation,
            CancellationToken cancellationToken = default)
        {
            ArgumentNullException.ThrowIfNull(operation);
            return turn.YieldFrameworkCallAsync(operation, cancellationToken);
        }

        public void Post(Action callback)
        {
            ArgumentNullException.ThrowIfNull(callback);
            if (!turn.TryPost(_ =>
                {
                    callback();
                    return ValueTask.CompletedTask;
                }))
                turn.ReportError(
                    new ObjectDisposedException(
                        nameof(ZLinkSerialExecutionQueue),
                        "HTTP callback completion could not enter the framework execution queue."));
        }

        public void ReportError(Exception exception)
        {
            ArgumentNullException.ThrowIfNull(exception);
            turn.ReportError(exception);
        }
    }
}
