namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelRuntimeBundle : IAsyncDisposable
{
    private readonly HashSet<string> _autoConnections = new(StringComparer.Ordinal);
    private readonly SemaphoreSlim _connectionGate = new(1, 1);
    private readonly object _disposeGate = new();
    private readonly HashSet<string> _manualConnections = new(StringComparer.Ordinal);
    private int _disposed;
    private long _autoConnectAttemptCount;
    private long _autoDisconnectAttemptCount;
    private Task? _disposeTask;
    private IDisposable? _manualConnectionAttachment;

    public ZLinkChannelRuntimeBundle(
        IZLinkBackendSocket socket,
        ZLinkAsyncSubmitter? submitter = null,
        RoutingId localRid = default,
        string? socketRole = null,
        ZLinkClientServerServerIdentity? clientServerServer = null,
        ZLinkFanoutPublisherIdentity? fanoutPublisher = null)
    {
        Socket = socket;
        Submitter = submitter;
        LocalRid = localRid.Size > 0 ? localRid.ToString() : null;
        SocketRole = socketRole;
        ClientServerServer = clientServerServer;
        FanoutPublisher = fanoutPublisher;
    }

    public IZLinkBackendSocket Socket { get; }

    public ZLinkAsyncSubmitter? Submitter { get; }

    public string? LocalRid { get; }

    public string? SocketRole { get; }

    internal ZLinkClientServerServerIdentity? ClientServerServer { get; }

    internal ZLinkFanoutPublisherIdentity? FanoutPublisher { get; }

    public SemaphoreSlim ReceiveGate { get; } = new(1, 1);

    internal long AutoConnectAttemptCount =>
        Volatile.Read(ref _autoConnectAttemptCount);

    internal long AutoDisconnectAttemptCount =>
        Volatile.Read(ref _autoDisconnectAttemptCount);

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

    public void ConnectManual(IZLinkBackendConnectableSocket socket, string endpoint)
    {
        _connectionGate.Wait();
        try
        {
            ThrowIfDisposed();
            Acquire(_manualConnections, endpoint, () => socket.Connect(endpoint), true);
        }
        finally
        {
            _connectionGate.Release();
        }
    }

    public void DisconnectManual(IZLinkBackendConnectableSocket socket, string endpoint)
    {
        _connectionGate.Wait();
        try
        {
            ThrowIfDisposed();
            Release(_manualConnections, endpoint, () => socket.Disconnect(endpoint), true);
        }
        finally
        {
            _connectionGate.Release();
        }
    }

    public bool ConnectAuto(IZLinkBackendConnectableSocket socket, string endpoint)
    {
        _connectionGate.Wait();
        try
        {
            if (Volatile.Read(ref _disposed) != 0) return false;
            return Acquire(
                _autoConnections,
                endpoint,
                () =>
                {
                    Interlocked.Increment(ref _autoConnectAttemptCount);
                    socket.Connect(endpoint);
                },
                false);
        }
        finally
        {
            _connectionGate.Release();
        }
    }

    public bool DisconnectAuto(IZLinkBackendConnectableSocket socket, string endpoint)
    {
        _connectionGate.Wait();
        try
        {
            if (Volatile.Read(ref _disposed) != 0) return false;
            return Release(
                _autoConnections,
                endpoint,
                () =>
                {
                    Interlocked.Increment(ref _autoDisconnectAttemptCount);
                    socket.Disconnect(endpoint);
                },
                false);
        }
        finally
        {
            _connectionGate.Release();
        }
    }

    private bool Acquire(HashSet<string> source, string endpoint, Action connect, bool throwOnFailure)
    {
        var alreadyOwned = _manualConnections.Contains(endpoint) || _autoConnections.Contains(endpoint);
        if (!source.Add(endpoint) || alreadyOwned) return true;
        try
        {
            connect();
            return true;
        }
        catch when (!throwOnFailure)
        {
            source.Remove(endpoint);
            return false;
        }
        catch
        {
            source.Remove(endpoint);
            throw;
        }
    }

    private bool Release(HashSet<string> source, string endpoint, Action disconnect, bool throwOnFailure)
    {
        if (!source.Remove(endpoint)) return true;
        if (_manualConnections.Contains(endpoint) || _autoConnections.Contains(endpoint)) return true;
        try
        {
            disconnect();
            return true;
        }
        catch when (!throwOnFailure)
        {
            source.Add(endpoint);
            return false;
        }
        catch
        {
            source.Add(endpoint);
            throw;
        }
    }

    private void ThrowIfDisposed()
    {
        if (Volatile.Read(ref _disposed) != 0)
            throw new ObjectDisposedException(nameof(ZLinkChannelRuntimeBundle));
    }

}
