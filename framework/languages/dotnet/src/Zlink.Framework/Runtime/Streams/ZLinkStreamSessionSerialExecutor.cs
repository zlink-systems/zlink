namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamSessionSerialExecutor : IAsyncDisposable
{
    private readonly ZLinkSerialExecutionQueue _queue;
    private readonly CancellationTokenSource _stopSource = new();
    private readonly object _stopGate = new();
    private readonly object _disposeGate = new();
    private Task? _disposeTask;
    private Task _stopCancellation = Task.CompletedTask;
    private bool _stopSourceDisposed;
    private bool _stopSourceFinalizing;

    public ZLinkStreamSessionSerialExecutor(
        object executionOwner,
        IZLinkRuntimeFailureReporter errorSink,
        int capacity = 4096)
    {
        _queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, _stopSource.Token, executionOwner),
            errorSink,
            _stopSource.Token,
            capacity);
    }

    public ValueTask DisposeAsync()
    {
        lock (_disposeGate)
            return new ValueTask(_disposeTask ??= DisposeCoreAsync());
    }

    private async Task DisposeCoreAsync()
    {
        RequestStop();
        await _queue.DisposeAsync().ConfigureAwait(false);
        Task cancellation;
        lock (_stopGate)
        {
            _stopSourceFinalizing = true;
            cancellation = _stopCancellation;
        }
        try
        {
            await cancellation.ConfigureAwait(false);
        }
        finally
        {
            lock (_stopGate)
            {
                if (!_stopSourceDisposed)
                {
                    _stopSource.Dispose();
                    _stopSourceDisposed = true;
                }
            }
        }
    }

    public void RequestStop()
    {
        _queue.Complete();
    }

    public void ForceStop()
    {
        _queue.Complete();
        lock (_stopGate)
        {
            if (_stopSourceDisposed || _stopSourceFinalizing || _stopSource.IsCancellationRequested) return;
            _stopCancellation = _stopSource.CancelAsync();
        }
    }

    public bool EnqueueInfrastructure(Func<ValueTask> work)
    {
        return _queue.TryPostNext(_ => work(), out _);
    }

    public ZLinkSerialPostAdmission EnqueueApplication(
        Func<CancellationToken, ValueTask> work)
    {
        return _queue.TryPostApplicationWithAdmission(work, out _);
    }

    public ZLinkSerialPostAdmission EnqueueControl(
        Func<CancellationToken, ValueTask> work)
    {
        return _queue.TryPostNextWithAdmission(work, out _);
    }

    public bool EnqueueFinal(Func<ValueTask> work)
    {
        return _queue.TryPostFinal(_ => work(), out _);
    }
}
