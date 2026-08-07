namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelRuntimeBundle : IAsyncDisposable
{
    private readonly Action<string>? _connect;
    private readonly SemaphoreSlim _connectionGate = new(1, 1);
    private readonly Action<string>? _disconnect;
    private readonly object _disposeGate = new();
    private readonly HashSet<string> _manualConnections = new(StringComparer.Ordinal);
    private int _disposed;
    private Task? _disposeTask;
    private IDisposable? _manualConnectionAttachment;

    public ZLinkChannelRuntimeBundle(
        IAsyncDisposable socket,
        Action<string>? connect = null,
        Action<string>? disconnect = null,
        ZLinkAsyncSubmitter? submitter = null,
        RoutingId localRid = default,
        string? socketRole = null,
        ZLinkClientServerServerIdentity? clientServerServer = null,
        ZLinkFanoutPublisherIdentity? fanoutPublisher = null)
    {
        Socket = socket;
        _connect = connect;
        _disconnect = disconnect;
        Submitter = submitter;
        LocalRid = localRid.Size > 0 ? localRid.ToString() : null;
        SocketRole = socketRole;
        ClientServerServer = clientServerServer;
        FanoutPublisher = fanoutPublisher;
    }

    public IAsyncDisposable Socket { get; }

    public ZLinkAsyncSubmitter? Submitter { get; }

    public string? LocalRid { get; }

    public string? SocketRole { get; }

    internal ZLinkClientServerServerIdentity? ClientServerServer { get; }

    internal ZLinkFanoutPublisherIdentity? FanoutPublisher { get; }

    public SemaphoreSlim ReceiveGate { get; } = new(1, 1);

    public ValueTask DisposeAsync()
    {
        Task task;
        TaskCompletionSource? start = null;
        lock (_disposeGate)
        {
            if (_disposeTask is null)
            {
                Volatile.Write(ref _disposed, 1);
                start = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                _disposeTask = DisposeCoreAsync(start.Task);
            }
            task = _disposeTask;
        }
        start?.TrySetResult();
        return new ValueTask(task);
    }

    private async Task DisposeCoreAsync(Task started)
    {
        await started.ConfigureAwait(false);
        var failures = new ZLinkFailureCollector();
        failures.Capture(DetachManualConnections);
        if (Submitter is not null)
            await failures.CaptureAsync(Submitter.DisposeAsync).ConfigureAwait(false);

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
    }

    internal void OwnManualConnectionAttachment(IDisposable attachment)
    {
        ArgumentNullException.ThrowIfNull(attachment);
        IDisposable? previous = null;
        var dispose = false;
        lock (_disposeGate)
        {
            if (Volatile.Read(ref _disposed) != 0)
                dispose = true;
            else
            {
                previous = _manualConnectionAttachment;
                _manualConnectionAttachment = attachment;
            }
        }
        previous?.Dispose();
        if (!dispose) return;
        attachment.Dispose();
        throw new ObjectDisposedException(nameof(ZLinkChannelRuntimeBundle));
    }

    private void DetachManualConnections()
    {
        IDisposable? attachment;
        lock (_disposeGate)
        {
            attachment = _manualConnectionAttachment;
            _manualConnectionAttachment = null;
        }
        attachment?.Dispose();
    }

    public void ConnectManual(string endpoint)
    {
        _connectionGate.Wait();
        try
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
        finally
        {
            _connectionGate.Release();
        }
    }

    private void ThrowIfDisposed()
    {
        if (Volatile.Read(ref _disposed) != 0)
            throw new ObjectDisposedException(nameof(ZLinkChannelRuntimeBundle));
    }

}
