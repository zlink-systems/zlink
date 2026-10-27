namespace Zlink.Framework.Runtime.Execution;

/// <summary>
///     Runs an asynchronous external operation without submitting work to the
///     bounded CPU pool. The terminator alone decides whether the current Spot
///     turn is retained or released.
/// </summary>
internal sealed class ZLinkIoWorkerCall<TResult>(
    CancellationToken runtimeStopToken,
    Func<CancellationToken, ValueTask<TResult>> work
) : IZLinkWorkerCall<TResult>
{
    private readonly ZLinkSerialTurn? _turn = ZLinkSerialTurn.Current;
    private int _terminated;
    private TimeSpan? _timeout;

    public IZLinkWorkerCall<TResult> Timeout(TimeSpan timeout)
    {
        ZLinkRequestTimeoutValidation.Validate(timeout, nameof(timeout));
        _timeout = timeout;
        return this;
    }

    public ValueTask<TResult> Async(CancellationToken cancellationToken = default)
    {
        EnsureSingleTerminator();
        return ExecuteAsync(cancellationToken);
    }

    public void Submit(CancellationToken cancellationToken = default)
    {
        ZLinkApplicationExecutionContext.EnsureBlockingSubmitAllowed();
        _ = Async(cancellationToken).AsTask().GetAwaiter().GetResult();
    }

    public ValueTask<TResult> Yield(CancellationToken cancellationToken = default)
    {
        EnsureSingleTerminator();
        var turn = ZLinkApplicationExecutionContext.RequireYieldTurn(_turn, "I/O worker");
        return turn.YieldFrameworkCallAsync(ExecuteAsync, cancellationToken);
    }

    private async ValueTask<TResult> ExecuteAsync(CancellationToken cancellationToken)
    {
        using var deadlineSource = _timeout is { } timeout
            ? new CancellationTokenSource(timeout)
            : null;
        using var stopSource = CancellationTokenSource.CreateLinkedTokenSource(
            runtimeStopToken,
            cancellationToken,
            deadlineSource?.Token ?? CancellationToken.None
        );
        try
        {
            stopSource.Token.ThrowIfCancellationRequested();
            var operation = work(stopSource.Token).AsTask();
            return await operation.WaitAsync(stopSource.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException ex) when (cancellationToken.IsCancellationRequested)
        {
            throw new OperationCanceledException(ex.Message, ex, cancellationToken);
        }
        catch (OperationCanceledException) when (runtimeStopToken.IsCancellationRequested)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                "I/O worker call was stopped because the framework runtime stopped."
            );
        }
        catch (OperationCanceledException ex) when (deadlineSource?.IsCancellationRequested == true)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                "I/O worker call timed out.",
                ZLinkRetryAdvice.DoNotRetry,
                ex
            );
        }
        catch (Exception ex) when (ex is not ZLinkFrameworkException)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                "I/O worker call failed.",
                ZLinkRetryAdvice.DoNotRetry,
                ex
            );
        }
    }

    private void EnsureSingleTerminator()
    {
        if (Interlocked.Exchange(ref _terminated, 1) != 0)
            throw new InvalidOperationException(
                "I/O worker call already has a terminator. Call Async or Yield once."
            );
    }
}
