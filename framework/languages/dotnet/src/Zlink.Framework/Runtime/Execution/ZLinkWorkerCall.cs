namespace Zlink.Framework.Runtime.Execution;

/// <summary>
///     Fluent CPU worker call. The work delegate runs on a pool thread. The
///     <c>Async()</c> keeps the current framework turn, while <c>Yield()</c>
///     releases it and resumes through the serial queue. A late completion
///     after a timeout is dropped.
/// </summary>
internal sealed class ZLinkWorkerCall<TResult>(
    ZLinkWorkerPool pool,
    Func<CancellationToken, TResult> work,
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
            "CPU worker submit",
            errorSink);
    }

    public ValueTask<TResult> Yield(CancellationToken cancellationToken = default)
    {
        EnsureSingleTerminator();
        var turn = ZLinkApplicationExecutionContext.RequireYieldTurn("CPU worker");
        if (!ReferenceEquals(turn, _turn))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "CPU worker Yield must execute in the callback turn that created the call.");
        return turn.YieldFrameworkCallAsync(ExecuteAsync, cancellationToken);
    }

    private ValueTask<TResult> ExecuteAsync(CancellationToken cancellationToken)
    {
        var completion = new TaskCompletionSource<TResult>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        Start(
            result => completion.TrySetResult(result),
            error => completion.TrySetException(error),
            token => completion.TrySetCanceled(token),
            cancellationToken);
        return new ValueTask<TResult>(completion.Task);
    }

    private async ValueTask ObserveAsync(CancellationToken cancellationToken)
    {
        _ = await ExecuteAsync(cancellationToken).ConfigureAwait(false);
    }

    private void Start(
        Action<TResult> complete,
        Action<Exception> fail,
        Action<CancellationToken> cancel,
        CancellationToken callerToken)
    {
        var execution = new Execution(work, complete, fail, cancel, _timeout);
        execution.Start(pool, callerToken);
    }

    private void EnsureSingleTerminator()
    {
        if (Interlocked.Exchange(ref _terminated, 1) != 0)
            throw new InvalidOperationException(
                "CPU worker call already has a terminator. Call Async or Yield once.");
    }

    private sealed class Execution(
        Func<CancellationToken, TResult> work,
        Action<TResult> complete,
        Action<Exception> fail,
        Action<CancellationToken> cancel,
        TimeSpan? timeout)
    {
        private readonly object _admissionGate = new();
        private readonly Action<CancellationToken> _cancel = cancel;
        private readonly Action<TResult> _complete = complete;
        private readonly Action<Exception> _fail = fail;
        private CancellationToken _callerToken;
        private CancellationTokenRegistration _callerRegistration;
        private int _settled;
        private CancellationTokenRegistration _timeoutRegistration;
        private CancellationTokenSource? _timeoutSource;
        private CancellationTokenSource? _workTokenSource;

        public void Start(ZLinkWorkerPool pool, CancellationToken callerToken)
        {
            Bind(pool, callerToken);
            if (!TrySubmit(pool, out var submitResult))
            {
                Cleanup();
                return;
            }

            switch (submitResult)
            {
                case ZLinkWorkerSubmitResult.Accepted:
                    break;
                case ZLinkWorkerSubmitResult.Full:
                    FailQueueFull();
                    break;
                case ZLinkWorkerSubmitResult.Stopped:
                    FailStopped();
                    break;
                default:
                    throw new ArgumentOutOfRangeException();
            }
        }

        private void Bind(ZLinkWorkerPool pool, CancellationToken callerToken)
        {
            _workTokenSource = CancellationTokenSource.CreateLinkedTokenSource(
                pool.ShutdownToken,
                callerToken);
            if (timeout is { } timeoutValue)
            {
                _timeoutSource = new CancellationTokenSource(timeoutValue);
                _timeoutRegistration = _timeoutSource.Token.Register(FailTimedOut);
            }

            if (callerToken.CanBeCanceled)
            {
                _callerToken = callerToken;
                _callerRegistration = callerToken.Register(
                    static state =>
                    {
                        var execution = (Execution)state!;
                        execution.CancelBeforeOrAfterAdmission(execution._callerToken);
                    },
                    this);
            }
        }

        private bool TrySubmit(
            ZLinkWorkerPool pool,
            out ZLinkWorkerSubmitResult submitResult)
        {
            lock (_admissionGate)
            {
                if (Volatile.Read(ref _settled) != 0)
                {
                    submitResult = default;
                    return false;
                }

                submitResult = pool.TrySubmit(Run, FailStopped);
                return true;
            }
        }

        public void Run(CancellationToken shutdownToken)
        {
            _ = shutdownToken;
            try
            {
                using var linked = _timeoutSource is null
                    ? null
                    : CancellationTokenSource.CreateLinkedTokenSource(
                        _workTokenSource!.Token,
                        _timeoutSource.Token);
                var result = work(linked?.Token ?? _workTokenSource!.Token);
                TrySettle(static (self, state) => self._complete((TResult)state!), this, result);
            }
            catch (Exception ex)
            {
                TrySettle(static (self, state) => self._fail(
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.InternalFailure,
                            "Worker call failed.",
                            ZLinkRetryAdvice.DoNotRetry,
                            (Exception)state!)),
                    this,
                    ex);
            }
            finally
            {
                Cleanup();
            }
        }

        public void FailQueueFull()
        {
            TrySettle(static (self, _) => self._fail(
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.CapacityExceeded,
                        "Worker queue is full.",
                        ZLinkRetryAdvice.RetryAfterBackoff)),
                this);
            Cleanup();
        }

        public void FailStopped()
        {
            TrySettle(static (self, _) => self._fail(
                    new OperationCanceledException(
                        "Worker call was canceled because the framework runtime stopped.")),
                this);
            Cleanup();
        }

        private void FailTimedOut()
        {
            lock (_admissionGate)
            {
                TrySettle(static (self, _) => self._fail(
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DeadlineExceeded,
                            "Worker call timed out.",
                            ZLinkRetryAdvice.DoNotRetry)),
                    this);
            }
        }

        private void TrySettle(
            Action<Execution, object?> settle,
            Execution self,
            object? state = null)
        {
            if (Interlocked.Exchange(ref _settled, 1) != 0)
                // Late completion after timeout/cancellation: drop the result.
                return;

            settle(self, state);
        }

        private void Cleanup()
        {
            _timeoutRegistration.Dispose();
            _callerRegistration.Dispose();
            _timeoutSource?.Dispose();
            _workTokenSource?.Dispose();
        }

        private void CancelBeforeOrAfterAdmission(CancellationToken cancellationToken)
        {
            lock (_admissionGate)
            {
                TrySettle(
                    static (self, state) => self._cancel((CancellationToken)state!),
                    this,
                cancellationToken);
            }
        }
    }
}
