namespace Zlink.Framework.Runtime.Execution;

/// <summary>
/// Owns the close of a socket that a blocking receive loop uses.
/// </summary>
/// <remarks>
/// <para>
/// The receive loop runs on a dedicated runner thread, and that thread closes the socket when the
/// loop returns. A stopping component signals the loop's own stop and awaits
/// <see cref="CloseAsync"/>; it never closes the socket while the loop can still be inside a Core
/// call on it (Core returns <c>EBUSY</c> for that close).
/// </para>
/// <para>
/// When no receive loop started, no thread receives on the socket and <see cref="CloseAsync"/>
/// closes it on the caller. The same holds for a retry after the loop's close failed: the loop
/// has returned, so the next close runs on the caller. Callers serialize their
/// <see cref="CloseAsync"/> calls through their own disposal.
/// </para>
/// </remarks>
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
    /// Starts <paramref name="receive"/> on a dedicated thread of <paramref name="runner"/>. Does
    /// nothing when the socket is already released; when the runner refuses new work, no thread
    /// receives and <see cref="CloseAsync"/> closes the socket on its caller.
    /// </summary>
    internal void Start(
        ZLinkRuntimeTaskRunner runner,
        string name,
        Action<CancellationToken> receive
    )
    {
        if (Interlocked.CompareExchange(ref _state, Receiving, NotStarted) != NotStarted)
            return;
        if (
            runner.TryRunLongRunning(
                name,
                token =>
                {
                    try
                    {
                        receive(token);
                    }
                    finally
                    {
                        Volatile.Write(ref _ownerClose, CloseOnOwner());
                        Release();
                    }
                },
                out _
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

    private Task CloseOnOwner()
    {
        try
        {
            close().AsTask().GetAwaiter().GetResult();
            return Task.CompletedTask;
        }
        catch (Exception failure)
        {
            return Task.FromException(failure);
        }
    }

    private void Release()
    {
        Volatile.Write(ref _state, Released);
        _receiveEnded.TrySetResult();
    }
}
