namespace Zlink.Framework.Runtime.Execution;

/// <summary>
///     Runs an asynchronous external operation without submitting work to the
///     bounded CPU pool. The terminator alone decides whether the current Spot
///     turn is retained or released.
/// </summary>
internal sealed class ZLinkIoWorkerCall<TResult>(
    CancellationToken runtimeStopToken,
    Func<CancellationToken, ValueTask<TResult>> work,
    IZLinkRuntimeFailureReporter errorSink) : IZLinkWorkerCall<TResult>
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
        EnsureSingleTerminator();
        ZLinkUnawaitedSubmit.Observe(
            ObserveAsync(cancellationToken),
            "I/O worker submit",
            errorSink);
    }

    public ValueTask<TResult> Yield(CancellationToken cancellationToken = default)
    {
        EnsureSingleTerminator();
        var turn = ZLinkApplicationExecutionContext.RequireYieldTurn("I/O worker");
        if (!ReferenceEquals(turn, _turn))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "I/O worker Yield must execute in the callback turn that created the call.");
        return turn.YieldFrameworkCallAsync(ExecuteAsync, cancellationToken);
    }

    private async ValueTask<TResult> ExecuteAsync(CancellationToken cancellationToken)
    {
        using var stopSource = CancellationTokenSource.CreateLinkedTokenSource(
            runtimeStopToken,
            cancellationToken);
        try
        {
            var operation = work(stopSource.Token).AsTask();
            return _timeout is { } timeout
                ? await operation.WaitAsync(timeout, stopSource.Token).ConfigureAwait(false)
                : await operation.WaitAsync(stopSource.Token).ConfigureAwait(false);
        }
        catch (TimeoutException ex)
        {
            stopSource.Cancel();
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                "I/O worker call timed out.",
                ZLinkRetryAdvice.DoNotRetry,
                ex);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException) when (runtimeStopToken.IsCancellationRequested)
        {
            throw new OperationCanceledException(
                "I/O worker call was canceled because the framework runtime stopped.");
        }
        catch (Exception ex) when (ex is not ZLinkFrameworkException)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                "I/O worker call failed.",
                ZLinkRetryAdvice.DoNotRetry,
                ex);
        }
    }

    private async ValueTask ObserveAsync(CancellationToken cancellationToken)
    {
        _ = await ExecuteAsync(cancellationToken).ConfigureAwait(false);
    }

    private void EnsureSingleTerminator()
    {
        if (Interlocked.Exchange(ref _terminated, 1) != 0)
            throw new InvalidOperationException(
                "I/O worker call already has a terminator. Call Async or Yield once.");
    }
}
