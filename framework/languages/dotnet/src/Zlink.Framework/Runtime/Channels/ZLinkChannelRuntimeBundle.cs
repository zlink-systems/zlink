using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelRuntimeBundle : IAsyncDisposable
{
    private readonly Action<string>? _connect;
    private readonly SemaphoreSlim _connectionGate = new(1, 1);
    private readonly Action<string>? _disconnect;
    private readonly ZLinkStateLane _lane = new();
    private readonly HashSet<string> _manualConnections = new(StringComparer.Ordinal);
    private int _disposed;
    private Task? _disposeTask;
    private IDisposable? _manualConnectionAttachment;
    private IDisposable? _receiveFlowRegistration;

    public ZLinkChannelRuntimeBundle(
        IAsyncDisposable socket,
        Action<string>? connect = null,
        Action<string>? disconnect = null,
        RoutingId localRid = default,
        string? socketRole = null,
        ZLinkClientServerServerIdentity? clientServerServer = null,
        ZLinkFanoutPublisherIdentity? fanoutPublisher = null,
        IDisposable? receiveFlowRegistration = null)
    {
        Socket = socket;
        _connect = connect;
        _disconnect = disconnect;
        LocalRid = localRid.Size > 0 ? localRid.ToString() : null;
        SocketRole = socketRole;
        ClientServerServer = clientServerServer;
        FanoutPublisher = fanoutPublisher;
        _receiveFlowRegistration = receiveFlowRegistration;
    }

    public IAsyncDisposable Socket { get; }

    public string? LocalRid { get; }

    public string? SocketRole { get; }

    internal ZLinkClientServerServerIdentity? ClientServerServer { get; }

    internal ZLinkFanoutPublisherIdentity? FanoutPublisher { get; }

    public SemaphoreSlim ReceiveGate { get; } = new(1, 1);

    public ValueTask DisposeAsync()
    {
        var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        if (Interlocked.CompareExchange(ref _disposed, 1, 0) != 0)
        {
            var spinner = new SpinWait();
            Task? task;
            while ((task = Volatile.Read(ref _disposeTask)) is null)
                spinner.SpinOnce();
            return new ValueTask(task);
        }

        Volatile.Write(ref _disposeTask, completion.Task);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        StartDisposeCore(started.Task, completion);
        started.TrySetResult();
        return new ValueTask(completion.Task);
    }

    private void StartDisposeCore(Task started, TaskCompletionSource completion)
    {
        if (ExecutionContext.IsFlowSuppressed())
        {
            _ = DisposeCoreAsync(started, completion);
            return;
        }

        using (ExecutionContext.SuppressFlow())
            _ = DisposeCoreAsync(started, completion);
    }

    private async Task DisposeCoreAsync(Task started, TaskCompletionSource completion)
    {
        try
        {
            await started.ConfigureAwait(false);
            var failures = new ZLinkFailureCollector();
            IDisposable? attachment = null;
            await failures.CaptureAsync(async () =>
            {
                attachment = await _lane.RunAsync(DetachManualConnectionsCore)
                    .ConfigureAwait(false);
            }).ConfigureAwait(false);
            failures.Capture(() => attachment?.Dispose());
            failures.Capture(DetachReceiveFlow);
            await _connectionGate.WaitAsync().ConfigureAwait(false);
            try
            {
                await failures.CaptureAsync(Socket.DisposeAsync).ConfigureAwait(false);
            }
            finally
            {
                _connectionGate.Release();
            }
            failures.Capture(_connectionGate.Dispose);
            failures.Capture(ReceiveGate.Dispose);
            failures.ThrowIfAny();
            completion.TrySetResult();
        }
        catch (Exception error)
        {
            completion.TrySetException(error);
        }
    }

    internal void OwnManualConnectionAttachment(IDisposable attachment)
    {
        ArgumentNullException.ThrowIfNull(attachment);
        var (previous, dispose) = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (Volatile.Read(ref _disposed) != 0)
                return ((IDisposable?)null, true);
            var replaced = _manualConnectionAttachment;
            _manualConnectionAttachment = attachment;
            return (replaced, false);
        }));
        previous?.Dispose();
        if (!dispose) return;
        attachment.Dispose();
        throw new ObjectDisposedException(nameof(ZLinkChannelRuntimeBundle));
    }

    private IDisposable? DetachManualConnectionsCore()
    {
        var attachment = _manualConnectionAttachment;
        _manualConnectionAttachment = null;
        return attachment;
    }

    private void DetachReceiveFlow() =>
        Interlocked.Exchange(ref _receiveFlowRegistration, null)?.Dispose();

    public void ConnectManual(string endpoint)
    {
        _connectionGate.Wait();
        try
        {
            AwaitStateLane(_lane.RunAsync(() => ConnectManualCore(endpoint)));
        }
        finally
        {
            _connectionGate.Release();
        }
    }

    public void DisconnectManual(string endpoint)
    {
        _connectionGate.Wait();
        try
        {
            AwaitStateLane(_lane.RunAsync(() => DisconnectManualCore(endpoint)));
        }
        finally
        {
            _connectionGate.Release();
        }
    }

    private void ConnectManualCore(string endpoint)
    {
        ThrowIfDisposed();
        if (!_manualConnections.Add(endpoint)) return;
        try
        {
            (_connect ?? throw new InvalidOperationException(
                "This channel socket does not support connections."))(endpoint);
        }
        catch
        {
            _manualConnections.Remove(endpoint);
            throw;
        }
    }

    private void DisconnectManualCore(string endpoint)
    {
        ThrowIfDisposed();
        if (!_manualConnections.Remove(endpoint)) return;
        try
        {
            (_disconnect ?? throw new InvalidOperationException(
                "This channel socket does not support disconnections."))(endpoint);
        }
        catch
        {
            _manualConnections.Add(endpoint);
            throw;
        }
    }

    private void ThrowIfDisposed()
    {
        if (Volatile.Read(ref _disposed) != 0)
            throw new ObjectDisposedException(nameof(ZLinkChannelRuntimeBundle));
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

}
