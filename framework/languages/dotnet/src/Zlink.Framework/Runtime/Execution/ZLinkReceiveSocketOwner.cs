namespace Zlink.Framework.Runtime.Execution;

/// <summary>
/// Closes a socket after its logical receive source has stopped and left the process poller.
/// </summary>
internal sealed class ZLinkReceiveSocketOwner(Func<ValueTask> close)
{
    private const int NotStarted = 0;
    private const int Receiving = 1;
    private const int Released = 2;

    private readonly TaskCompletionSource _receiveEnded = new(
        TaskCreationOptions.RunContinuationsAsynchronously
    );
    private int _state;
    private Task? _ownerClose;
    private Task? _close;

    /// <summary>Completes once the receive loop has returned and run its close.</summary>
    internal Task ReceiveEnded => _receiveEnded.Task;

    /// <summary>
    /// Admits one logical receive lifetime through the runner. Its readiness turns use
    /// the process receive role; source removal precedes the asynchronous socket close.
    /// </summary>
    internal void Start(
        ZLinkRuntimeTaskRunner runner,
        string name,
        Func<CancellationToken, ValueTask> receive
    )
    {
        if (Interlocked.CompareExchange(ref _state, Receiving, NotStarted) != NotStarted)
            return;
        if (
            runner.TryRunDetached(
                name,
                async token =>
                {
                    try
                    {
                        await receive(token).ConfigureAwait(false);
                    }
                    finally
                    {
                        Task closing;
                        try
                        {
                            closing = close().AsTask();
                        }
                        catch (Exception failure)
                        {
                            closing = Task.FromException(failure);
                        }
                        Volatile.Write(ref _ownerClose, closing);
                        try
                        {
                            await closing.ConfigureAwait(false);
                        }
                        finally
                        {
                            Release();
                        }
                    }
                }
            )
        )
            return;
        Release();
    }

    /// <summary>
    /// Waits for the receive loop to close the socket. Closes it here when no loop started, and
    /// retries here after a failed close.
    /// </summary>
    internal async ValueTask CloseAsync()
    {
        if (Interlocked.CompareExchange(ref _state, Released, NotStarted) == NotStarted)
            _receiveEnded.TrySetResult();
        await _receiveEnded.Task.ConfigureAwait(false);
        if (Interlocked.Exchange(ref _ownerClose, null) is { } ownerClose)
        {
            if (!ownerClose.IsFaulted)
                Volatile.Write(ref _close, ownerClose);
            await ownerClose.ConfigureAwait(false);
            return;
        }
        await ZLinkRuntimeTaskRunner
            .RunDisposal(ref _close, () => close().AsTask())
            .ConfigureAwait(false);
    }

    private void Release()
    {
        Volatile.Write(ref _state, Released);
        _receiveEnded.TrySetResult();
    }
}
