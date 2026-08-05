namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkSerialWorkItem
{
    private readonly Func<CancellationToken, ValueTask> _callback;
    private readonly Action? _relocationRelease;
    private readonly Func<ReadOnlyMemory<byte>>? _acceptedPayloadFactory;
    private readonly object _acceptedPayloadGate = new();
    private bool _acceptedPayloadCreated;
    private ReadOnlyMemory<byte> _acceptedPayload;

    private readonly TaskCompletionSource _completion =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public ZLinkSerialWorkItem(
        Func<CancellationToken, ValueTask> callback,
        Action? relocationRelease = null,
        bool previousOwnerMessageFollow = false,
        ZLinkSerialWorkLane lane = ZLinkSerialWorkLane.Application,
        long accountingBytes = ZLinkSerialExecutionQueue.WorkItemFixedCostBytes,
        ulong acceptedSequence = 0,
        ReadOnlyMemory<byte> acceptedPayload = default,
        Func<ReadOnlyMemory<byte>>? acceptedPayloadFactory = null)
    {
        _callback = callback;
        _relocationRelease = relocationRelease;
        PreviousOwnerMessageFollow = previousOwnerMessageFollow;
        Lane = lane;
        AccountingBytes = accountingBytes > 0
            ? accountingBytes
            : throw new ArgumentOutOfRangeException(nameof(accountingBytes));
        AcceptedSequence = acceptedSequence;
        _acceptedPayload = acceptedPayload;
        _acceptedPayloadFactory = acceptedPayloadFactory;
        _acceptedPayloadCreated = acceptedPayloadFactory is null;
    }

    public Task Completion => _completion.Task;

    public ulong AcceptedSequence { get; }
    public ReadOnlyMemory<byte> AcceptedPayload
    {
        get
        {
            if (_acceptedPayloadFactory is null)
                return _acceptedPayload;
            lock (_acceptedPayloadGate)
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
    public long AccountingBytes { get; }

    public ZLinkAcceptedWorkRecord CreateAcceptedRecord()
    {
        if (!IsAccepted)
            throw new InvalidOperationException(
                "Only accepted work can create a relocation record.");
        return new ZLinkAcceptedWorkRecord(AcceptedSequence, AcceptedPayload);
    }

    public void ReleaseForRelocation(Action<Exception> onUnhandledException)
    {
        try
        {
            _relocationRelease?.Invoke();
            _completion.TrySetResult();
        }
        catch (Exception exception)
        {
            _completion.TrySetException(exception);
            _ = _completion.Task.Exception;
            onUnhandledException(exception);
        }
    }

    public async ValueTask<ZLinkSerialWorkItemResult> InvokeAsync(
        Action<Exception> onUnhandledException,
        CancellationToken cancellationToken,
        ZLinkSerialTurn turn)
    {
        Task? callbackTask = null;
        try
        {
            using var turnScope = ZLinkSerialTurn.Push(turn);
            var operation = _callback(cancellationToken);
            if (operation.IsCompletedSuccessfully)
            {
                _completion.TrySetResult();
                return ZLinkSerialWorkItemResult.Completed;
            }

            callbackTask = operation.AsTask();
            turn.BindOwnerTask(callbackTask);
            var bounded = callbackTask.WaitAsync(cancellationToken);
            var completed = await Task.WhenAny(bounded, turn.Suspended).ConfigureAwait(false);
            if (ReferenceEquals(completed, turn.Suspended))
            {
                _ = CompleteAfterSuspensionAsync(
                    bounded,
                    callbackTask,
                    onUnhandledException);
                return ZLinkSerialWorkItemResult.Suspended;
            }

            await bounded.ConfigureAwait(false);
            _completion.TrySetResult();
            return ZLinkSerialWorkItemResult.Completed;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            if (callbackTask is not null)
                await CompleteAfterCancellationAsync(
                        callbackTask,
                        cancellationToken,
                        onUnhandledException)
                    .ConfigureAwait(false);
            else
                _completion.TrySetCanceled(cancellationToken);
            return ZLinkSerialWorkItemResult.Completed;
        }
        catch (Exception ex)
        {
            _completion.TrySetException(ex);
            _ = _completion.Task.Exception;
            onUnhandledException(ex);
            return ZLinkSerialWorkItemResult.Completed;
        }
    }

    private async Task CompleteAfterCancellationAsync(
        Task callbackTask,
        CancellationToken cancellationToken,
        Action<Exception> onUnhandledException)
    {
        try
        {
            await callbackTask.ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception exception)
        {
            onUnhandledException(exception);
        }
        finally
        {
            _completion.TrySetCanceled(cancellationToken);
        }
    }

    private async Task CompleteAfterSuspensionAsync(
        Task boundedTask,
        Task callbackTask,
        Action<Exception> onUnhandledException)
    {
        try
        {
            await boundedTask.ConfigureAwait(false);
            _completion.TrySetResult();
        }
        catch (OperationCanceledException cancellation)
        {
            await CompleteAfterCancellationAsync(
                    callbackTask,
                    cancellation.CancellationToken,
                    onUnhandledException)
                .ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            _completion.TrySetException(ex);
            _ = _completion.Task.Exception;
            if (ex is not OperationCanceledException) onUnhandledException(ex);
        }
    }
}

internal enum ZLinkSerialWorkLane
{
    Application = 0,
    Lifecycle = 1
}

internal enum ZLinkSerialWorkItemResult
{
    Completed,
    Suspended
}

internal sealed record ZLinkAcceptedWorkRecord(
    ulong AcceptedSequence,
    ReadOnlyMemory<byte> Payload)
{
    public ZLinkAcceptedWorkRecord Snapshot()
    {
        return new ZLinkAcceptedWorkRecord(
            AcceptedSequence,
            Payload.ToArray());
    }
}
