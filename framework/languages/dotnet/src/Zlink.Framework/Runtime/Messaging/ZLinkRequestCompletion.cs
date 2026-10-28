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
    private readonly Service.ZLinkServiceLiveness? _receiveAdmission;
    private readonly Action? _onCanceled;
    private readonly CancellationTokenRegistration _stopCancellationRegistration;
    private readonly TaskCompletionSource<TResult> _completion = new(
        TaskCreationOptions.RunContinuationsAsynchronously
    );

    internal ZLinkRequestCompletion(
        CancellationToken callerCancellation,
        CancellationToken stopCancellation = default,
        Action<TResult>? discardResult = null,
        Action? onCanceled = null,
        Service.ZLinkServiceLiveness? receiveAdmission = null
    )
    {
        _discardResult = discardResult;
        _receiveAdmission = receiveAdmission;
        _onCanceled = onCanceled;
        _callerCancellationRegistration = RegisterCancellation(callerCancellation);
        _stopCancellationRegistration = RegisterShutdown(stopCancellation);
    }

    internal Task<TResult> Task => _completion.Task;

    internal void Complete(TResult result)
    {
        if (!_completion.TrySetResult(result))
        {
            _receiveAdmission?.RecordReceived();
            _discardResult?.Invoke(result);
        }
    }

    internal void Cancel(CancellationToken cancellationToken)
    {
        if (_completion.TrySetCanceled(cancellationToken))
            _onCanceled?.Invoke();
    }

    internal void Fail(Exception exception)
    {
        _completion.TrySetException(exception);
    }

    public void Dispose()
    {
        _callerCancellationRegistration.Dispose();
        _stopCancellationRegistration.Dispose();
    }

    private CancellationTokenRegistration RegisterCancellation(CancellationToken cancellationToken)
    {
        return cancellationToken.Register(
            static (state, token) => ((ZLinkRequestCompletion<TResult>)state!).Cancel(token),
            this
        );
    }

    private CancellationTokenRegistration RegisterShutdown(CancellationToken cancellationToken)
    {
        return cancellationToken.Register(
            static state =>
            {
                var completion = (ZLinkRequestCompletion<TResult>)state!;
                completion.Fail(
                    ZLinkRequestFailureMapper.CreateShutdownRequestException(
                        "ZLink request was interrupted by runtime shutdown."
                    )
                );
            },
            this
        );
    }
}
