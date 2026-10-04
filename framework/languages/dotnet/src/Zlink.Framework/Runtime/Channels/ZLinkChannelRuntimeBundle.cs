using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelRuntimeBundle : IAsyncDisposable
{
    private readonly Action<string>? _connect;
    private readonly Action<string>? _disconnect;
    private readonly ZLinkStateLane _lane = new();
    private readonly HashSet<string> _manualConnections = new(StringComparer.Ordinal);
    private Task? _disposeTask;
    private IDisposable? _manualConnectionAttachment;
    private IAsyncDisposable? _receiveFlowRegistration;

    public ZLinkChannelRuntimeBundle(
        IAsyncDisposable socket,
        Action<string>? connect = null,
        Action<string>? disconnect = null,
        RoutingId localRid = default,
        string? socketRole = null,
        ZLinkClientServerServerIdentity? clientServerServer = null,
        ZLinkFanoutPublisherIdentity? fanoutPublisher = null,
        IAsyncDisposable? receiveFlowRegistration = null
    )
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

    public ValueTask DisposeAsync() =>
        new(ZLinkRuntimeTaskRunner.RunDisposal(ref _disposeTask, DisposeCoreAsync));

    private async Task DisposeCoreAsync()
    {
        var failures = new ZLinkFailureCollector();
        IDisposable? attachment = null;
        await failures
            .CaptureAsync(async () =>
            {
                attachment = await _lane
                    .RunAsync(DetachManualConnectionsCore)
                    .ConfigureAwait(false);
            })
            .ConfigureAwait(false);
        failures.Capture(() => attachment?.Dispose());
        await failures.CaptureAsync(DetachReceiveFlowAsync).ConfigureAwait(false);
        await failures.CaptureAsync(Socket.DisposeAsync).ConfigureAwait(false);
        failures.ThrowIfAny();
        ReceiveGate.Dispose();
    }

    internal void OwnManualConnectionAttachment(IDisposable attachment)
    {
        ArgumentNullException.ThrowIfNull(attachment);
        var (previous, dispose) = AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (Volatile.Read(ref _disposeTask) is not null)
                    return ((IDisposable?)null, true);
                var replaced = _manualConnectionAttachment;
                _manualConnectionAttachment = attachment;
                return (replaced, false);
            })
        );
        previous?.Dispose();
        if (!dispose)
            return;
        attachment.Dispose();
        throw new ObjectDisposedException(nameof(ZLinkChannelRuntimeBundle));
    }

    private IDisposable? DetachManualConnectionsCore()
    {
        var attachment = _manualConnectionAttachment;
        _manualConnectionAttachment = null;
        return attachment;
    }

    private ValueTask DetachReceiveFlowAsync() =>
        ZLinkReceiveFlowController.DisposeRegistrationAsync(
            Interlocked.Exchange(ref _receiveFlowRegistration, null)
        );

    public void ConnectManual(string endpoint)
    {
        AwaitStateLane(_lane.RunAsync(() => ConnectManualCore(endpoint)));
    }

    public void DisconnectManual(string endpoint)
    {
        AwaitStateLane(_lane.RunAsync(() => DisconnectManualCore(endpoint)));
    }

    private void ConnectManualCore(string endpoint)
    {
        ThrowIfDisposed();
        if (!_manualConnections.Add(endpoint))
            return;
        try
        {
            (
                _connect
                ?? throw new InvalidOperationException(
                    "This channel socket does not support connections."
                )
            )(endpoint);
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
        if (!_manualConnections.Remove(endpoint))
            return;
        try
        {
            (
                _disconnect
                ?? throw new InvalidOperationException(
                    "This channel socket does not support disconnections."
                )
            )(endpoint);
        }
        catch
        {
            _manualConnections.Add(endpoint);
            throw;
        }
    }

    private void ThrowIfDisposed()
    {
        if (Volatile.Read(ref _disposeTask) is not null)
            throw new ObjectDisposedException(nameof(ZLinkChannelRuntimeBundle));
    }
}
