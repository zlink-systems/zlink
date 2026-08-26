using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamSessionSerialExecutor : IAsyncDisposable
{
    private readonly ZLinkSerialExecutionQueue _queue;
    private readonly CancellationTokenSource _stopSource = new();
    private readonly ZLinkStateLane _lane = new();
    private Task? _disposeTask;
    private Task _stopCancellation = Task.CompletedTask;
    private bool _stopSourceDisposed;
    private bool _stopSourceFinalizing;

    public ZLinkStreamSessionSerialExecutor(
        object executionOwner,
        IZLinkRuntimeFailureReporter errorSink)
    {
        _queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, _stopSource.Token, executionOwner),
            errorSink,
            _stopSource.Token);
    }

    public ValueTask DisposeAsync()
    {
        return new ValueTask(AwaitStateLane(_lane.RunAsync(
            () => _disposeTask ??= StartDisposeCore())));
    }

    private Task StartDisposeCore()
    {
        if (ExecutionContext.IsFlowSuppressed())
            return Task.Run(DisposeCoreAsync, CancellationToken.None);

        using (ExecutionContext.SuppressFlow())
            return Task.Run(DisposeCoreAsync, CancellationToken.None);
    }

    private async Task DisposeCoreAsync()
    {
        RequestStop();
        await _queue.DisposeAsync().ConfigureAwait(false);
        var cancellation = await _lane.RunAsync(() =>
        {
            _stopSourceFinalizing = true;
            return _stopCancellation;
        }).ConfigureAwait(false);
        try
        {
            await cancellation.ConfigureAwait(false);
        }
        finally
        {
            await _lane.RunAsync(() =>
            {
                if (!_stopSourceDisposed)
                {
                    _stopSource.Dispose();
                    _stopSourceDisposed = true;
                }
            }).ConfigureAwait(false);
        }
    }

    public void RequestStop()
    {
        _queue.Complete();
    }

    public void ForceStop()
    {
        _queue.Complete();
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_stopSourceDisposed || _stopSourceFinalizing || _stopSource.IsCancellationRequested) return;
            _stopCancellation = _stopSource.CancelAsync();
        }));
    }

    public bool EnqueueInfrastructure(Func<ValueTask> work)
    {
        return _queue.TryPostNext(_ => work(), out _);
    }

    public void CloseApplicationAdmission() =>
        _queue.CloseApplicationAdmission();

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

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();
}
