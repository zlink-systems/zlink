namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkSerialWorkItem
{
    private readonly Func<CancellationToken, ValueTask> _callback;
    private readonly Action? _relocationRelease;
    private readonly Func<ReadOnlyMemory<byte>>? _acceptedPayloadFactory;
    private readonly object? _acceptedPayloadGate;
    private bool _acceptedPayloadCreated;
    private ReadOnlyMemory<byte> _acceptedPayload;

    private readonly TaskCompletionSource? _completion;
    private Action? _terminalRelease;
    private int _terminalReleased;

    public ZLinkSerialWorkItem(
        Func<CancellationToken, ValueTask> callback,
        Action? relocationRelease = null,
        bool previousOwnerMessageFollow = false,
        ZLinkSerialWorkLane lane = ZLinkSerialWorkLane.Application,
        ulong acceptedSequence = 0,
        ReadOnlyMemory<byte> acceptedPayload = default,
        Func<ReadOnlyMemory<byte>>? acceptedPayloadFactory = null,
        bool reservationHeld = true,
        Func<bool>? ready = null
    )
    {
        _completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
        _callback = callback;
        _relocationRelease = relocationRelease;
        PreviousOwnerMessageFollow = previousOwnerMessageFollow;
        Lane = lane;
        AcceptedSequence = acceptedSequence;
        _acceptedPayload = acceptedPayload;
        _acceptedPayloadFactory = acceptedPayloadFactory;
        _acceptedPayloadGate = acceptedPayloadFactory is null ? null : new object();
        _acceptedPayloadCreated = acceptedPayloadFactory is null;
        ReservationHeld = reservationHeld;
        Ready = ready;
    }

    internal ZLinkSerialWorkItem(ZLinkSerialExecutionQueue? mailbox)
    {
        _callback = static _ => ValueTask.CompletedTask;
        Mailbox = mailbox;
    }

    public Task Completion =>
        _completion?.Task
        ?? throw new InvalidOperationException("Mailbox readiness has no payload completion.");

    internal ZLinkSerialWorkItem? Next { get; set; }

    // Publication links belong to the publisher and the acquiring cursor;
    // Next belongs exclusively to the execution gate's mutable prefix.
    internal ZLinkSerialWorkItem? PublicationNext;
    internal ZLinkSerialExecutionQueue? Mailbox { get; }
    internal Func<Action, ValueTask>? DispatchTerminal { get; set; }
    internal Action? PreparePublication { get; set; }
    internal Action? RejectPublication { get; set; }
    internal ZLinkSerialWorkItem? LifecycleOwner { get; set; }
    internal ZLinkSerialWorkItem? ReadyContinuation { get; set; }
    internal object? AcceptedState { get; set; }

    public ulong AcceptedSequence { get; }
    public ReadOnlyMemory<byte> AcceptedPayload
    {
        get
        {
            if (_acceptedPayloadFactory is null)
                return _acceptedPayload;
            lock (_acceptedPayloadGate!)
            {
                if (!_acceptedPayloadCreated)
                {
                    _acceptedPayload = _acceptedPayloadFactory();
                    _acceptedPayloadCreated = true;
                }
                return _acceptedPayload;
            }
        }
    }
    public bool IsAccepted => AcceptedSequence != 0;
    public bool PreviousOwnerMessageFollow { get; }
    public ZLinkSerialWorkLane Lane { get; }

    internal bool ReservationHeld { get; }
    internal Func<bool>? Ready { get; }

    internal void BindTerminalRelease(Action release)
    {
        ArgumentNullException.ThrowIfNull(release);
        if (Interlocked.CompareExchange(ref _terminalRelease, release, null) is not null)
            throw new InvalidOperationException(
                "ZLink serial work already has a terminal release callback."
            );
    }

    internal void AddTerminalRelease(Action release)
    {
        ArgumentNullException.ThrowIfNull(release);
        var current =
            Volatile.Read(ref _terminalRelease)
            ?? throw new InvalidOperationException("The work item has no terminal owner.");
        if (
            Volatile.Read(ref _terminalReleased) != 0
            || Interlocked.CompareExchange(
                ref _terminalRelease,
                () =>
                {
                    try
                    {
                        release();
                    }
                    finally
                    {
                        current();
                    }
                },
                current
            ) != current
        )
            throw new InvalidOperationException("The work item terminal release has changed.");
    }

    public ZLinkAcceptedWorkRecord CreateAcceptedRecord()
    {
        if (!IsAccepted)
            throw new InvalidOperationException(
                "Only accepted work can create a relocation record."
            );
        return new ZLinkAcceptedWorkRecord(AcceptedSequence, AcceptedPayload);
    }

    public void ReleaseForRelocation(Action<Exception> onUnhandledException)
    {
        try
        {
            _relocationRelease?.Invoke();
            ReleaseTerminal();
            _completion!.TrySetResult();
        }
        catch (Exception exception)
        {
            ReleaseTerminal();
            _completion!.TrySetException(exception);
            _ = _completion!.Task.Exception;
            onUnhandledException(exception);
        }
    }

    public async ValueTask<ZLinkSerialWorkItemResult> InvokeAsync(
        Action<Exception> onUnhandledException,
        CancellationToken cancellationToken,
        ZLinkSerialTurn turn
    )
    {
        Task? callbackTask = null;
        try
        {
            using var turnScope = ZLinkSerialTurn.Push(turn);
            var operation = _callback(cancellationToken);
            if (operation.IsCompletedSuccessfully)
            {
                ReleaseTerminal();
                _completion!.TrySetResult();
                return ZLinkSerialWorkItemResult.Completed;
            }

            callbackTask = operation.AsTask();
            turn.BindOwnerTask(callbackTask);
            var bounded = callbackTask.WaitAsync(cancellationToken);
            var completed = await Task.WhenAny(bounded, turn.Suspended).ConfigureAwait(false);
            if (ReferenceEquals(completed, turn.Suspended))
            {
                _ = CompleteAfterSuspensionAsync(bounded, callbackTask, onUnhandledException);
                return ZLinkSerialWorkItemResult.Suspended;
            }

            await bounded.ConfigureAwait(false);
            ReleaseTerminal();
            _completion!.TrySetResult();
            return ZLinkSerialWorkItemResult.Completed;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            if (callbackTask is not null)
                await CompleteAfterCancellationAsync(
                        callbackTask,
                        cancellationToken,
                        onUnhandledException
                    )
                    .ConfigureAwait(false);
            else
            {
                ReleaseTerminal();
                _completion!.TrySetCanceled(cancellationToken);
            }
            return ZLinkSerialWorkItemResult.Completed;
        }
        catch (Exception ex)
        {
            ReleaseTerminal();
            _completion!.TrySetException(ex);
            _ = _completion!.Task.Exception;
            onUnhandledException(ex);
            return ZLinkSerialWorkItemResult.Completed;
        }
    }

    private async Task CompleteAfterCancellationAsync(
        Task callbackTask,
        CancellationToken cancellationToken,
        Action<Exception> onUnhandledException,
        bool suspended = false
    )
    {
        try
        {
            await callbackTask.ConfigureAwait(false);
        }
        catch (OperationCanceledException) { }
        catch (Exception exception)
        {
            onUnhandledException(exception);
        }
        finally
        {
            if (suspended)
                await ReleaseSuspendedTerminalAsync().ConfigureAwait(false);
            else
                ReleaseTerminal();
            _completion!.TrySetCanceled(cancellationToken);
        }
    }

    private async Task CompleteAfterSuspensionAsync(
        Task boundedTask,
        Task callbackTask,
        Action<Exception> onUnhandledException
    )
    {
        try
        {
            await boundedTask.ConfigureAwait(false);
            await ReleaseSuspendedTerminalAsync().ConfigureAwait(false);
            _completion!.TrySetResult();
        }
        catch (OperationCanceledException cancellation)
        {
            await CompleteAfterCancellationAsync(
                    callbackTask,
                    cancellation.CancellationToken,
                    onUnhandledException,
                    suspended: true
                )
                .ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            await ReleaseSuspendedTerminalAsync().ConfigureAwait(false);
            _completion!.TrySetException(ex);
            _ = _completion!.Task.Exception;
            if (ex is not OperationCanceledException)
                onUnhandledException(ex);
        }
    }

    private void ReleaseTerminal()
    {
        if (Interlocked.Exchange(ref _terminalReleased, 1) != 0)
            return;
        (
            Volatile.Read(ref _terminalRelease)
            ?? throw new InvalidOperationException(
                "ZLink serial work has no terminal release callback."
            )
        )();
    }

    private ValueTask ReleaseSuspendedTerminalAsync() =>
        DispatchTerminal is { } dispatch ? dispatch(ReleaseTerminal) : ReleaseTerminalInline();

    private ValueTask ReleaseTerminalInline()
    {
        ReleaseTerminal();
        return ValueTask.CompletedTask;
    }
}

internal enum ZLinkSerialWorkLane
{
    Application = 0,
    Lifecycle = 1,
}

internal enum ZLinkSerialWorkItemResult
{
    Completed,
    Suspended,
}

internal sealed record ZLinkAcceptedWorkRecord(ulong AcceptedSequence, ReadOnlyMemory<byte> Payload)
{
    public ZLinkAcceptedWorkRecord Snapshot()
    {
        return new ZLinkAcceptedWorkRecord(AcceptedSequence, Payload.ToArray());
    }
}
