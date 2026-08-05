using Zlink.Framework.Runtime.Backend.DotNet.Mappings;

namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

// RouteMesh 10.0.0 STREAM seam. Raw framed I/O stays on IStreamSocket; the
// bound-actor plane (bind/unbind/relay) moved onto IStreamSessionService, created
// from the owning MeshNode. Actor identity is resolved to a full ActorRef via the
// session bindings table so the framework seam can keep its actor-id-keyed shape.
internal sealed class ZLinkBackendStreamSocketWrapper : IZLinkBackendStreamSocket
{
    private readonly IStreamSocket _socket;
    private readonly IMeshNode _node;
    private readonly ZLinkMeshCompletionTable? _completions;
    private readonly bool _ownsNode;
    private readonly object _sendGate = new();
    private readonly object _sessionGate = new();
    private IStreamSessionService? _session;
    private bool _sessionStarted;

    // completions is the shared MeshNode's completion table (non-null when the
    // node is the framework's single MeshNode); the dispatch pump resolves the
    // StreamBind/StreamUnbind completions this wrapper awaits. ownsNode is false
    // for the shared node so it is not disposed here.
    public ZLinkBackendStreamSocketWrapper(
        IStreamSocket socket,
        IMeshNode node,
        ZLinkMeshCompletionTable? completions,
        bool ownsNode)
    {
        _socket = socket;
        _node = node;
        _completions = completions;
        _ownsNode = ownsNode;
    }

    internal IStreamSocket NativeSocket => _socket;

    public IZLinkBackendSocketPoller CreateReceivePoller() =>
        ZLinkBackendSocketPoller.Create(_socket);

    public void ApplySocketConfig(IZLinkSocketConfig config)
    {
        ZLinkBackendSocketOptionsMapper.Apply(_socket.Options, config);
        // Core uses -1 as the explicit unlimited value. The Framework's
        // StreamNode value 0 must not leave a binding-specific default in use.
        if (config.MaxMessageSize == 0)
            _socket.Options.MaxMessageSize = -1;
    }

    public string GetLastEndpoint() => _socket.Options.LastEndpoint;

    private IStreamSessionService Session()
    {
        if (_session is { } existing) return existing;
        lock (_sessionGate)
        {
            if (_session is null)
            {
                _session = _node.CreateStreamSessionService(_socket);
                _session.Start();
                _sessionStarted = true;
            }

            return _session;
        }
    }

    public void Bind(string endpoint)
    {
        _socket.Bind(endpoint);
    }

    public void SetChannelName(string channelName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(channelName);
    }

    public void SetTlsServer(string certPath, string keyPath, bool requireClientCert)
    {
        _socket.SetTlsServer(certPath, keyPath, requireClientCert);
    }

    public bool RecvPart(
        out RoutingId? sourceRoutingId,
        out Message? part,
        out bool hasMore,
        RecvFlags flags = RecvFlags.None) =>
        _socket.RecvPart(out sourceRoutingId, out part, out hasMore, flags);

    public void OnSendReady(Action handler)
    {
        _socket.OnSendReady(handler);
    }

    public bool Send(RoutingId routingId, Message payload, SendFlags flags)
    {
        lock (_sendGate)
            return _socket.Send(routingId).Message(payload).Flags(flags).Submit();
    }

    public bool Send(RoutingId routingId, IReadOnlyList<Message> parts, SendFlags flags)
    {
        lock (_sendGate)
            return _socket.Send(routingId).Messages(parts).Flags(flags).Submit();
    }

    public void DisconnectPeer(RoutingId routingId)
    {
        _socket.DisconnectRid(routingId);
    }

    public async ValueTask BindActorAsync(
        RoutingId sessionRid,
        ZLinkBackendActorRef actor,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        // Core marks a session live from its connected observer event, which is
        // asynchronous to packet delivery — a bind triggered by the session's
        // first packet can outrun it and see NotConnected. Retry within the
        // bind timeout; the liveness event is milliseconds behind the packet.
        var deadline = DateTime.UtcNow
                       + (timeout > TimeSpan.Zero ? timeout : TimeSpan.FromSeconds(5));
        while (true)
        {
            var submit = Session().BindActor(
                sessionRid, ToNativeActor(actor), out var operationId, timeout);
            if (submit != SubmitResult.NotConnected || DateTime.UtcNow >= deadline)
            {
                await AwaitOperationAsync(submit, operationId, cancellationToken)
                    .ConfigureAwait(false);
                return;
            }

            await Task.Delay(TimeSpan.FromMilliseconds(10), cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private ActorRef ToNativeActor(ZLinkBackendActorRef actor)
    {
        if (_node is not ZLinkManagedMeshNode managed)
            throw new InvalidOperationException(
                "Actor binding requires the Framework managed MeshNode.");
        return actor.ToNative(managed.MeshName);
    }

    public ValueTask UnbindActorAsync(
        RoutingId sessionRid,
        string actorId,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var session = Session();
        foreach (var binding in session.Bindings(sessionRid))
            if (string.Equals(binding.Actor.ActorId, actorId, StringComparison.Ordinal))
            {
                var submit = session.UnbindActor(
                    sessionRid, binding.Actor, binding.BindingGeneration,
                    out var operationId, timeout);
                return AwaitOperationAsync(submit, operationId, cancellationToken);
            }

        return ValueTask.CompletedTask;
    }

    // Awaits a MeshNode operation (StreamBind/StreamUnbind) to terminal
    // completion via the shared node's completion table so bind/unbind are
    // observably complete (spec 31 §7 — binding update runs on the infrastructure
    // claim). When no shared completion table is available (standalone minted
    // node) the submit result is the only signal and the operation is not awaited.
    private async ValueTask AwaitOperationAsync(
        SubmitResult submit,
        MeshOperationId operationId,
        CancellationToken cancellationToken)
    {
        if (submit != SubmitResult.Ok)
            throw new ZlinkSubmitException((ZlinkSubmitException.ErrorCode)(int)submit);
        if (_completions is null || operationId == default) return;

        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _completions.Register(operationId, (record, parts) =>
        {
            ZLinkMessageParts.DisposeAll(parts);
            var result = ZLinkMeshCompletionTable.MapResult(
                record.TerminalResult, record.FailureErrno);
            if (result == RequestResult.Ok)
                completion.TrySetResult();
            else
                completion.TrySetException(
                    new ZlinkRequestException((ZlinkRequestException.ErrorCode)(int)result));
        });
        await using (cancellationToken.Register(() => completion.TrySetCanceled())
                         .ConfigureAwait(false))
            await completion.Task.ConfigureAwait(false);
    }

    public bool SendBoundActor(
        RoutingId sessionRid,
        string actorId,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        var session = Session();
        foreach (var binding in session.Bindings(sessionRid))
            if (string.Equals(binding.Actor.ActorId, actorId, StringComparison.Ordinal))
                lock (_sendGate)
                    return session.SendToActor(sessionRid, binding.Actor, parts, flags)
                        == SubmitResult.Ok;

        return false;
    }

    public async ValueTask DisposeAsync()
    {
        IStreamSessionService? session;
        lock (_sessionGate)
        {
            session = _sessionStarted ? _session : null;
            _session = null;
        }

        if (session is not null)
            await session.DisposeAsync().ConfigureAwait(false);
        await _socket.DisposeAsync().ConfigureAwait(false);

        // The shared framework MeshNode is owned by its spot node runtime; only a
        // standalone minted node is disposed here.
        if (_ownsNode)
            await _node.DisposeAsync().ConfigureAwait(false);
    }
}
