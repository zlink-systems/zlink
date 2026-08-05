namespace Zlink.Framework.Runtime.Messaging;

/// <summary>
/// Separates the lifetime of an accepted native request from the caller waiting
/// for its result. Cancellation ends only the caller wait; a later native result
/// is still observed and released through the configured discard policy.
/// </summary>
internal sealed class ZLinkRequestCompletion<TResult> : IDisposable
{
    private readonly CancellationTokenRegistration _callerCancellationRegistration;
    private readonly Action<TResult>? _discardResult;
    private readonly Action? _onCanceled;
    private readonly CancellationTokenRegistration _stopCancellationRegistration;
    private readonly TaskCompletionSource<TResult> _completion =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly CancellationTokenSource? _timeoutSource;
    private readonly CancellationTokenRegistration _timeoutRegistration;

    internal ZLinkRequestCompletion(
        CancellationToken callerCancellation,
        CancellationToken stopCancellation = default,
        Action<TResult>? discardResult = null,
        Action? onCanceled = null)
    {
        _discardResult = discardResult;
        _onCanceled = onCanceled;
        _callerCancellationRegistration = RegisterCancellation(callerCancellation);
        _stopCancellationRegistration = RegisterShutdown(stopCancellation);
    }

    internal ZLinkRequestCompletion(
        CancellationToken callerCancellation,
        TimeSpan timeout,
        string timeoutMessage,
        Action<TResult>? discardResult = null)
        : this(
            callerCancellation,
            default,
            timeout,
            timeoutMessage,
            discardResult)
    {
    }

    internal ZLinkRequestCompletion(
        CancellationToken callerCancellation,
        CancellationToken stopCancellation,
        TimeSpan timeout,
        string timeoutMessage,
        Action<TResult>? discardResult = null,
        Action? onCanceled = null,
        Func<string, Exception>? timeoutExceptionFactory = null)
        : this(callerCancellation, stopCancellation, discardResult, onCanceled)
    {
        _timeoutSource = new CancellationTokenSource();
        _timeoutRegistration = _timeoutSource.Token.Register(
            static state =>
            {
                var timeoutState = (TimeoutState<TResult>)state!;
                timeoutState.Completion.TrySetException(
                    timeoutState.ExceptionFactory?.Invoke(timeoutState.Message)
                    ?? new TimeoutException(timeoutState.Message));
            },
            new TimeoutState<TResult>(_completion, timeoutMessage, timeoutExceptionFactory));
        _timeoutSource.CancelAfter(timeout);
    }

    internal Task<TResult> Task => _completion.Task;

    internal void Complete(TResult result)
    {
        if (!_completion.TrySetResult(result)) _discardResult?.Invoke(result);
    }

    internal void Cancel(CancellationToken cancellationToken)
    {
        if (_completion.TrySetCanceled(cancellationToken)) _onCanceled?.Invoke();
    }

    internal void Fail(Exception exception)
    {
        _completion.TrySetException(exception);
    }

    public void Dispose()
    {
        _callerCancellationRegistration.Dispose();
        _stopCancellationRegistration.Dispose();
        _timeoutRegistration.Dispose();
        _timeoutSource?.Dispose();
    }

    private CancellationTokenRegistration RegisterCancellation(CancellationToken cancellationToken)
    {
        return cancellationToken.Register(
            static state =>
            {
                var cancellation = (CancellationState<TResult>)state!;
                cancellation.Completion.Cancel(cancellation.Token);
            },
            new CancellationState<TResult>(this, cancellationToken));
    }

    private CancellationTokenRegistration RegisterShutdown(CancellationToken cancellationToken)
    {
        return cancellationToken.Register(
            static state =>
            {
                var completion = (ZLinkRequestCompletion<TResult>)state!;
                completion.Fail(
                    ZLinkRequestFailureMapper.CreateShutdownRequestException(
                        "ZLink request was interrupted by runtime shutdown."));
            },
            this);
    }

    private sealed record CancellationState<T>(
        ZLinkRequestCompletion<T> Completion,
        CancellationToken Token);

    private sealed record TimeoutState<T>(
        TaskCompletionSource<T> Completion,
        string Message,
        Func<string, Exception>? ExceptionFactory);
}
